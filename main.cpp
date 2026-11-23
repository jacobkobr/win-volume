// win-volume: hotkeys that change the volume of the app in the focused window.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

enum { HK_UP = 1, HK_DOWN };
enum { ID_EXIT = 1, ID_KEY_UP = 101, ID_KEY_DOWN, ID_STEP, ID_SCROLL };
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_WHEEL = WM_APP + 2;
constexpr UINT_PTR OSD_TIMER = 1, HOLD_TIMER = 2;

// Defaults as hotkey-control values (flags in the high byte): Ctrl+Alt+Up / Ctrl+Alt+Down.
constexpr int DEFAULT_UP = MAKEWORD(VK_UP, HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_EXT);
constexpr int DEFAULT_DOWN = MAKEWORD(VK_DOWN, HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_EXT);

static const struct { const wchar_t* name; UINT mods; } SCROLL_OPTIONS[] = {
    {L"Off", 0},
    {L"Ctrl+Alt", MOD_CONTROL | MOD_ALT},
    {L"Alt", MOD_ALT},
    {L"Ctrl+Shift", MOD_CONTROL | MOD_SHIFT},
    {L"Alt+Shift", MOD_ALT | MOD_SHIFT},
};

static std::wstring g_ini;
static int g_dpi = 96;
static HFONT g_font;
static HWND g_main, g_osd, g_popup;
static NOTIFYICONDATAW g_tray = {};
static UINT g_taskbarCreated;
static DWORD g_popupHiddenAt;

static int g_keyUp, g_keyDown, g_stepPercent;
static std::atomic<UINT> g_scrollMods;
static DWORD g_hookThreadId;
static int g_wheelRemainder;
static int g_holdDir, g_holdKey;
static DWORD g_holdStart;

// Sessions of the app being adjusted, kept until the indicator hides so repeated presses are fast.
static DWORD g_cachedPid;
static std::vector<ISimpleAudioVolume*> g_cached;

static std::wstring g_osdName;
static float g_osdLevel;  // negative means the app has no audio session

static int px(int v) { return MulDiv(v, g_dpi, 96); }

// Releases the COM pointer when it goes out of scope.
template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
};

static std::wstring exeName(DWORD pid) {
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return name;
    wchar_t buf[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        name = buf;
        name = name.substr(name.find_last_of(L'\\') + 1);
        for (auto& c : name) c = towlower(c);
    }
    CloseHandle(h);
    return name;
}

// Volume controls for every audio session of the given app, across all output devices.
// Matches by exe name too, since browsers and Electron apps play audio from child processes.
static std::vector<ISimpleAudioVolume*> sessionsFor(DWORD pid, const std::wstring& exe) {
    std::vector<ISimpleAudioVolume*> out;
    Com<IMMDeviceEnumerator> enumerator;
    Com<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
        return out;

    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);
    for (UINT d = 0; d < deviceCount; d++) {
        Com<IMMDevice> device;
        Com<IAudioSessionManager2> manager;
        Com<IAudioSessionEnumerator> sessions;
        if (FAILED(devices->Item(d, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&manager)) ||
            FAILED(manager->GetSessionEnumerator(&sessions)))
            continue;

        int count = 0;
        sessions->GetCount(&count);
        for (int i = 0; i < count; i++) {
            Com<IAudioSessionControl> control;
            Com<IAudioSessionControl2> control2;
            if (FAILED(sessions->GetSession(i, &control)) ||
                FAILED(control->QueryInterface(IID_PPV_ARGS(&control2))))
                continue;

            DWORD sessionPid = 0;
            control2->GetProcessId(&sessionPid);
            if (sessionPid != pid && exeName(sessionPid) != exe) continue;

            ISimpleAudioVolume* volume = nullptr;
            if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&volume)))) out.push_back(volume);
        }
    }
    return out;
}

static void clearCache() {
    for (auto* v : g_cached) v->Release();
    g_cached.clear();
    g_cachedPid = 0;
}

static void showOsd() {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST), &mi);
    RECT wr;
    GetWindowRect(g_osd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2;
    int y = mi.rcWork.bottom - h - px(80);
    SetWindowPos(g_osd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_osd, nullptr, FALSE);
    UpdateWindow(g_osd);
    SetTimer(g_osd, OSD_TIMER, 1200, nullptr);
}

static void adjustFocused(float delta) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != g_cachedPid || g_cached.empty()) {
        clearCache();
        std::wstring exe = exeName(pid);
        if (exe.empty()) return;
        g_cachedPid = pid;
        g_cached = sessionsFor(pid, exe);
        g_osdName = exe.substr(0, exe.find_last_of(L'.'));
    }

    g_osdLevel = -1;
    if (!g_cached.empty()) {
        // The first session is the reference so all of the app's sessions end up the same.
        float level = 0;
        g_cached[0]->GetMasterVolume(&level);
        level = std::clamp(std::round((level + delta) * 100) / 100, 0.0f, 1.0f);
        for (auto* v : g_cached) {
            v->SetMasterVolume(level, nullptr);
            v->SetMute(FALSE, nullptr);
        }
        g_osdLevel = level;
    }
    showOsd();
}

static UINT heldMods() {
    auto down = [](int vk) { return GetAsyncKeyState(vk) < 0; };
    return (down(VK_CONTROL) ? MOD_CONTROL : 0) | (down(VK_MENU) ? MOD_ALT : 0) |
           (down(VK_SHIFT) ? MOD_SHIFT : 0) | (down(VK_LWIN) || down(VK_RWIN) ? MOD_WIN : 0);
}

// Runs on its own thread, so slow audio calls on the main thread never stall the mouse.
static LRESULT CALLBACK mouseHook(int code, WPARAM wp, LPARAM lp) {
    UINT mods = g_scrollMods;
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && mods && heldMods() == mods) {
        auto* info = (MSLLHOOKSTRUCT*)lp;
        PostMessageW(g_main, WM_WHEEL, (WPARAM)(short)HIWORD(info->mouseData), 0);

        // Releasing Alt on its own opens the focused app's menu bar; a dummy key press prevents that.
        if (mods & MOD_ALT) {
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = 0xE8;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI hookThread(void*) {
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(nullptr), 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
    UnhookWindowsHookEx(hook);
    return 0;
}

// The hook only exists while scroll is enabled, so it costs nothing when off.
static void updateScrollHook() {
    if (g_scrollMods && !g_hookThreadId) {
        CloseHandle(CreateThread(nullptr, 0, hookThread, nullptr, 0, &g_hookThreadId));
    } else if (!g_scrollMods && g_hookThreadId) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        g_hookThreadId = 0;
    }
}

// Converts wheel movement into volume steps. Touchpads send small deltas, so the leftover is kept.
static int notches(WPARAM wp) {
    g_wheelRemainder += (int)(INT_PTR)wp;
    int n = g_wheelRemainder / WHEEL_DELTA;
    g_wheelRemainder -= n * WHEEL_DELTA;
    return n;
}

static int readInt(const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(L"settings", key, fallback, g_ini.c_str());
}

static void writeInt(const wchar_t* key, int value) {
    WritePrivateProfileStringW(L"settings", key, std::to_wstring(value).c_str(), g_ini.c_str());
}

static void loadSettings() {
    g_keyUp = readInt(L"volume_up", DEFAULT_UP);
    g_keyDown = readInt(L"volume_down", DEFAULT_DOWN);
    g_stepPercent = std::clamp(readInt(L"step", 5), 1, 25);
    g_scrollMods = readInt(L"scroll_mods", MOD_CONTROL | MOD_ALT);
}

// Hotkey-control flags and RegisterHotKey modifiers use different bits.
static UINT toMods(int hotkey) {
    BYTE f = HIBYTE(hotkey);
    return (f & HOTKEYF_SHIFT ? MOD_SHIFT : 0) | (f & HOTKEYF_CONTROL ? MOD_CONTROL : 0) |
           (f & HOTKEYF_ALT ? MOD_ALT : 0);
}

static void registerHotkeys() {
    bool ok = true;
    // Key repeat is ignored; holding is handled by the hold timer instead.
    if (LOBYTE(g_keyUp)) ok &= RegisterHotKey(g_main, HK_UP, toMods(g_keyUp) | MOD_NOREPEAT, LOBYTE(g_keyUp)) != 0;
    if (LOBYTE(g_keyDown))
        ok &= RegisterHotKey(g_main, HK_DOWN, toMods(g_keyDown) | MOD_NOREPEAT, LOBYTE(g_keyDown)) != 0;
    if (!ok)
        MessageBoxW(nullptr, L"A hotkey is already used by another app. Pick a different one from the tray icon.",
                    L"win-volume", MB_ICONWARNING);
}

static void unregisterHotkeys() {
    UnregisterHotKey(g_main, HK_UP);
    UnregisterHotKey(g_main, HK_DOWN);
}

static void openPopup() {
    // Without this, pressing the current hotkeys in the picker would fire them instead of recording.
    unregisterHotkeys();
    SendDlgItemMessageW(g_popup, ID_KEY_UP, HKM_SETHOTKEY, g_keyUp, 0);
    SendDlgItemMessageW(g_popup, ID_KEY_DOWN, HKM_SETHOTKEY, g_keyDown, 0);
    SetDlgItemInt(g_popup, ID_STEP, g_stepPercent, FALSE);
    int scrollIndex = 0;
    for (int i = 0; i < (int)ARRAYSIZE(SCROLL_OPTIONS); i++)
        if (SCROLL_OPTIONS[i].mods == g_scrollMods) scrollIndex = i;
    SendDlgItemMessageW(g_popup, ID_SCROLL, CB_SETCURSEL, scrollIndex, 0);

    // Bottom-right of the work area, next to the tray.
    MONITORINFO mi = {sizeof(mi)};
    POINT pt;
    GetCursorPos(&pt);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wr;
    GetWindowRect(g_popup, &wr);
    int x = mi.rcWork.right - (wr.right - wr.left) - px(12);
    int y = mi.rcWork.bottom - (wr.bottom - wr.top) - px(12);
    SetWindowPos(g_popup, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_popup);
}

// Settings are saved whenever the popup loses focus.
static void closePopup() {
    int scrollIndex = (int)SendDlgItemMessageW(g_popup, ID_SCROLL, CB_GETCURSEL, 0, 0);
    writeInt(L"volume_up", (int)SendDlgItemMessageW(g_popup, ID_KEY_UP, HKM_GETHOTKEY, 0, 0));
    writeInt(L"volume_down", (int)SendDlgItemMessageW(g_popup, ID_KEY_DOWN, HKM_GETHOTKEY, 0, 0));
    writeInt(L"step", std::clamp((int)GetDlgItemInt(g_popup, ID_STEP, nullptr, FALSE), 1, 25));
    writeInt(L"scroll_mods", SCROLL_OPTIONS[std::max(scrollIndex, 0)].mods);
    ShowWindow(g_popup, SW_HIDE);
    g_popupHiddenAt = GetTickCount();

    loadSettings();
    registerHotkeys();
    updateScrollHook();
}

static LRESULT CALLBACK osdProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        KillTimer(hwnd, OSD_TIMER);
        ShowWindow(hwnd, SW_HIDE);
        clearCache();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        auto fill = [&](RECT r, COLORREF color) {
            HBRUSH b = CreateSolidBrush(color);
            FillRect(dc, &r, b);
            DeleteObject(b);
        };
        fill(rc, RGB(32, 32, 32));

        int pad = px(14);
        RECT name = {pad, pad, rc.right - pad - px(64), rc.bottom};
        RECT value = {pad, pad, rc.right - pad, rc.bottom};
        std::wstring text = g_osdLevel < 0 ? L"No audio" : std::to_wstring(std::lround(g_osdLevel * 100)) + L"%";
        SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 240));
        DrawTextW(dc, g_osdName.c_str(), -1, &name, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(dc, RGB(170, 170, 170));
        DrawTextW(dc, text.c_str(), -1, &value, DT_SINGLELINE | DT_RIGHT);

        RECT track = {pad, rc.bottom - pad - px(4), rc.right - pad, rc.bottom - pad};
        fill(track, RGB(70, 70, 70));
        if (g_osdLevel > 0) {
            RECT bar = track;
            bar.right = track.left + std::lround((track.right - track.left) * g_osdLevel);
            fill(bar, RGB(96, 165, 250));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE && IsWindowVisible(hwnd)) {
        closePopup();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        g_holdDir = wp == HK_UP ? 1 : -1;
        g_holdKey = LOBYTE(wp == HK_UP ? g_keyUp : g_keyDown);
        g_holdStart = GetTickCount();
        adjustFocused(g_holdDir * g_stepPercent / 100.0f);
        SetTimer(hwnd, HOLD_TIMER, 50, nullptr);
        return 0;
    case WM_TIMER: {
        DWORD held = GetTickCount() - g_holdStart;
        if (GetAsyncKeyState(g_holdKey) >= 0) {
            KillTimer(hwnd, HOLD_TIMER);
        } else if (held > 300) {
            // Starts at 1% per tick and speeds up to 3% the longer the key is held.
            int percent = std::min<int>(1 + (held - 300) / 600, 3);
            adjustFocused(g_holdDir * percent / 100.0f);
        }
        return 0;
    }
    case WM_WHEEL: {
        // Merge queued wheel messages so fast scrolling can't build a backlog.
        int n = notches(wp);
        MSG m;
        while (PeekMessageW(&m, hwnd, WM_WHEEL, WM_WHEEL, PM_REMOVE)) n += notches(m.wParam);
        if (n) adjustFocused(n * g_stepPercent / 100.0f);
        return 0;
    }
    case WM_TRAY:
        // Clicking the icon while the popup is open closes it first; don't reopen it right away.
        if (lp == WM_LBUTTONUP && GetTickCount() - g_popupHiddenAt > 300) openPopup();
        if (lp == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);  // needed so the menu closes when clicking elsewhere
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_EXIT) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        PostQuitMessage(0);
        return 0;
    }
    // Explorer restarted, so the tray icon is gone.
    if (msg == g_taskbarCreated) Shell_NotifyIconW(NIM_ADD, &g_tray);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND makeWindow(const wchar_t* cls, WNDPROC proc, DWORD exStyle, DWORD style, int w, int h, HINSTANCE inst) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    return CreateWindowExW(exStyle, cls, L"win-volume", style, 0, 0, w, h, nullptr, nullptr, inst, nullptr);
}

static void buildPopup(HINSTANCE inst) {
    const struct { const wchar_t* label; const wchar_t* cls; int id; DWORD style; } rows[] = {
        {L"Volume up", HOTKEY_CLASSW, ID_KEY_UP, 0},
        {L"Volume down", HOTKEY_CLASSW, ID_KEY_DOWN, 0},
        {L"Step (%)", L"EDIT", ID_STEP, ES_NUMBER | WS_BORDER},
        {L"Scroll wheel", L"COMBOBOX", ID_SCROLL, CBS_DROPDOWNLIST},
    };
    int w = px(250), rowH = px(32), pad = px(12);
    g_popup = makeWindow(L"win-volume-popup", popupProc, WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WS_POPUP | WS_BORDER,
                         w, pad * 2 + rowH * ARRAYSIZE(rows), inst);

    for (int i = 0; i < (int)ARRAYSIZE(rows); i++) {
        int y = pad + rowH * i;
        // A combo box's height includes its dropdown list.
        int h = rows[i].id == ID_SCROLL ? px(200) : px(24);
        HWND label = CreateWindowW(L"STATIC", rows[i].label, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                   pad, y, px(90), px(24), g_popup, nullptr, inst, nullptr);
        HWND input = CreateWindowW(rows[i].cls, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | rows[i].style,
                                   pad + px(94), y, w - pad * 2 - px(96), h, g_popup,
                                   (HMENU)(INT_PTR)rows[i].id, inst, nullptr);
        SendMessageW(label, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(input, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    for (auto& option : SCROLL_OPTIONS)
        SendDlgItemMessageW(g_popup, ID_SCROLL, CB_ADDSTRING, 0, (LPARAM)option.name);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    CreateMutexW(nullptr, FALSE, L"win-volume-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_ini = path;
    g_ini = g_ini.substr(0, g_ini.find_last_of(L'.')) + L".ini";

    g_main = makeWindow(L"win-volume", mainProc, 0, 0, 0, 0, inst);
    buildPopup(inst);

    // Click-through, never takes focus, hidden from alt-tab.
    int osdW = px(240), osdH = px(58);
    g_osd = makeWindow(L"win-volume-osd", osdProc,
                       WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                       WS_POPUP, osdW, osdH, inst);
    SetLayeredWindowAttributes(g_osd, 0, 235, LWA_ALPHA);
    SetWindowRgn(g_osd, CreateRoundRectRgn(0, 0, osdW + 1, osdH + 1, px(12), px(12)), FALSE);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_main;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(g_tray.szTip, L"win-volume", ARRAYSIZE(g_tray.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_tray);

    loadSettings();
    registerHotkeys();
    updateScrollHook();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Gives the popup tab navigation between its fields.
        if (!IsDialogMessageW(g_popup, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    clearCache();
    CoUninitialize();
    return 0;
}

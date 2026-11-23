// win-volume: hotkeys that change the volume of the app in the focused window.

#include "audio.h"
#include "osd.h"
#include "popup.h"
#include "scroll.h"
#include "settings.h"
#include "ui.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>

enum { HK_UP = 1, HK_DOWN };
constexpr UINT WM_TRAY = WM_APP + 1, WM_WHEEL = WM_APP + 2, WM_POPUP_CLOSED = WM_APP + 3;
constexpr UINT_PTR HOLD_TIMER = 1;
constexpr UINT ID_EXIT = 1;

static HWND g_main;
static NOTIFYICONDATAW g_tray = {};
static UINT g_taskbarCreated;
static int g_holdDir, g_holdKey, g_wheelRemainder;
static DWORD g_holdStart;

static void adjust(float delta) {
    std::wstring name;
    float level;
    if (!planVolumeChange(delta, name, level)) return;
    // Indicator first: apps with many audio sessions (like Discord) take a moment to apply.
    showOsd(name, level);
    applyVolumeChange();
}

// Hotkey-control flags and RegisterHotKey modifiers use different bits.
static UINT toMods(int hotkey) {
    BYTE f = HIBYTE(hotkey);
    return (f & HOTKEYF_SHIFT ? MOD_SHIFT : 0) | (f & HOTKEYF_CONTROL ? MOD_CONTROL : 0) |
           (f & HOTKEYF_ALT ? MOD_ALT : 0);
}

static void registerHotkeys() {
    // Key repeat is ignored; holding is handled by the hold timer instead.
    bool ok = true;
    for (auto [id, key] : {std::pair{HK_UP, g_settings.keyUp}, std::pair{HK_DOWN, g_settings.keyDown}})
        if (LOBYTE(key)) ok &= RegisterHotKey(g_main, id, toMods(key) | MOD_NOREPEAT, LOBYTE(key)) != 0;
    if (!ok)
        MessageBoxW(nullptr, L"A hotkey is already used by another app. Pick a different one from the tray icon.",
                    L"win-volume", MB_ICONWARNING);
}

static void unregisterHotkeys() {
    UnregisterHotKey(g_main, HK_UP);
    UnregisterHotKey(g_main, HK_DOWN);
}

// Converts wheel movement into volume steps. Touchpads send small deltas, so the leftover is kept.
static int notches(WPARAM wp) {
    g_wheelRemainder += (int)(INT_PTR)wp;
    int n = g_wheelRemainder / WHEEL_DELTA;
    g_wheelRemainder -= n * WHEEL_DELTA;
    return n;
}

static void showTrayMenu(HWND hwnd) {
    applyMenuTheme();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);  // needed so the menu closes when clicking elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        g_holdDir = wp == HK_UP ? 1 : -1;
        g_holdKey = LOBYTE(wp == HK_UP ? g_settings.keyUp : g_settings.keyDown);
        g_holdStart = GetTickCount();
        adjust(g_holdDir * g_settings.stepPercent / 100.0f);
        SetTimer(hwnd, HOLD_TIMER, 50, nullptr);
        return 0;
    case WM_TIMER: {
        DWORD held = GetTickCount() - g_holdStart;
        if (GetAsyncKeyState(g_holdKey) >= 0) {
            KillTimer(hwnd, HOLD_TIMER);
        } else if (held > 300) {
            // Starts at 1% per tick and speeds up to 3% the longer the key is held.
            int percent = std::min<int>(1 + (held - 300) / 600, 3);
            adjust(g_holdDir * percent / 100.0f);
        }
        return 0;
    }
    case WM_WHEEL: {
        // Merge queued wheel messages so fast scrolling can't build a backlog.
        int n = notches(wp);
        MSG m;
        while (PeekMessageW(&m, hwnd, WM_WHEEL, WM_WHEEL, PM_REMOVE)) n += notches(m.wParam);
        if (n) adjust(n * g_settings.stepPercent / 100.0f);
        return 0;
    }
    case WM_TRAY:
        // Without unregistering, pressing the current hotkeys in the popup would fire them instead of recording.
        if (lp == WM_LBUTTONUP && openPopup()) unregisterHotkeys();
        if (lp == WM_RBUTTONUP) showTrayMenu(hwnd);
        return 0;
    case WM_POPUP_CLOSED:
        registerHotkeys();
        setScrollHook(g_settings.scrollMods, hwnd, WM_WHEEL);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_EXIT) DestroyWindow(hwnd);
        return 0;
    case WM_SETTINGCHANGE:
        // Light/dark switched, so recolor the tray icon.
        if (lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
            DestroyIcon(g_tray.hIcon);
            g_tray.hIcon = makeTrayIcon();
            Shell_NotifyIconW(NIM_MODIFY, &g_tray);
        }
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

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    CreateMutexW(nullptr, FALSE, L"win-volume-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    initUi();
    loadSettings();

    g_main = makeWindow(L"win-volume", mainProc, 0, 0, 0, 0);
    createOsd();
    createPopup(g_main, WM_POPUP_CLOSED);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_main;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = makeTrayIcon();
    lstrcpynW(g_tray.szTip, L"win-volume", ARRAYSIZE(g_tray.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_tray);

    registerHotkeys();
    setScrollHook(g_settings.scrollMods, g_main, WM_WHEEL);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!popupDialogMessage(msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    setScrollHook(0, nullptr, 0);
    releaseAudio();
    CoUninitialize();
    return 0;
}

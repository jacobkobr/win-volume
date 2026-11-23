#include "popup.h"
#include "settings.h"
#include "ui.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <string>

enum { ID_TITLE = 100, ID_KEY_UP, ID_KEY_DOWN, ID_STEP, ID_SCROLL };

static const struct { const wchar_t* name; UINT mods; } SCROLL_OPTIONS[] = {
    {L"Off", 0},
    {L"Ctrl + Alt", MOD_CONTROL | MOD_ALT},
    {L"Alt", MOD_ALT},
    {L"Ctrl + Shift", MOD_CONTROL | MOD_SHIFT},
    {L"Alt + Shift", MOD_ALT | MOD_SHIFT},
};

static HWND g_popup, g_notify;
static UINT g_closedMsg;
static DWORD g_hiddenAt;
static HBRUSH g_bgBrush, g_fieldBrush;
static COLORREF g_bg, g_field, g_text, g_subtext;

// Key boxes keep their hotkey in GWLP_USERDATA and show it as text.
static void setKeyBox(HWND box, int hotkey) {
    SetWindowLongPtrW(box, GWLP_USERDATA, hotkey);
    BYTE f = HIBYTE(hotkey);
    std::wstring text;
    if (f & HOTKEYF_CONTROL) text += L"Ctrl + ";
    if (f & HOTKEYF_ALT) text += L"Alt + ";
    if (f & HOTKEYF_SHIFT) text += L"Shift + ";
    if (LOBYTE(hotkey)) {
        // The extended bit makes arrow keys read "Up" instead of "Num 8".
        LONG scan = MapVirtualKeyW(LOBYTE(hotkey), MAPVK_VK_TO_VSC) << 16 | (f & HOTKEYF_EXT ? 1 << 24 : 0);
        wchar_t name[32] = {};
        GetKeyNameTextW(scan, name, 32);
        text += name;
    } else {
        text = L"None";
    }
    SetWindowTextW(box, text.c_str());
}

static int keyBoxValue(int id) { return (int)GetWindowLongPtrW(GetDlgItem(g_popup, id), GWLP_USERDATA); }

// Records whatever combination is pressed while the box has focus. Backspace clears it.
static LRESULT CALLBACK keyBoxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU || wp == VK_LWIN || wp == VK_RWIN) return 0;
        BYTE flags = (GetKeyState(VK_SHIFT) < 0 ? HOTKEYF_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? HOTKEYF_CONTROL : 0) |
                     (GetKeyState(VK_MENU) < 0 ? HOTKEYF_ALT : 0) | (lp & (1 << 24) ? HOTKEYF_EXT : 0);
        setKeyBox(hwnd, wp == VK_BACK && !(flags & ~HOTKEYF_EXT) ? 0 : MAKEWORD(wp, flags));
        return 0;
    }
    if (msg == WM_CHAR || msg == WM_SYSCHAR) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Matches the Windows light/dark app setting; checked each time the popup opens.
static void applyTheme() {
    BOOL dark = systemDark();
    g_bg = dark ? RGB(43, 43, 43) : RGB(249, 249, 249);
    g_field = dark ? RGB(56, 56, 56) : RGB(255, 255, 255);
    g_text = dark ? RGB(240, 240, 240) : RGB(26, 26, 26);
    g_subtext = dark ? RGB(190, 190, 190) : RGB(90, 90, 90);

    DeleteObject(g_bgBrush);
    DeleteObject(g_fieldBrush);
    g_bgBrush = CreateSolidBrush(g_bg);
    g_fieldBrush = CreateSolidBrush(g_field);
    SetClassLongPtrW(g_popup, GCLP_HBRBACKGROUND, (LONG_PTR)g_bgBrush);

    DwmSetWindowAttribute(g_popup, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // "DarkMode_CFD" is the theme Explorer uses for its dark edit boxes and dropdowns.
    for (HWND child = GetWindow(g_popup, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SetWindowTheme(child, dark ? L"DarkMode_CFD" : nullptr, nullptr);
    InvalidateRect(g_popup, nullptr, TRUE);
}

static void closePopup() {
    int scroll = (int)SendDlgItemMessageW(g_popup, ID_SCROLL, CB_GETCURSEL, 0, 0);
    g_settings.keyUp = keyBoxValue(ID_KEY_UP);
    g_settings.keyDown = keyBoxValue(ID_KEY_DOWN);
    g_settings.stepPercent = std::clamp((int)GetDlgItemInt(g_popup, ID_STEP, nullptr, FALSE), 1, 25);
    g_settings.scrollMods = SCROLL_OPTIONS[std::max(scroll, 0)].mods;
    saveSettings();

    ShowWindow(g_popup, SW_HIDE);
    g_hiddenAt = GetTickCount();
    PostMessageW(g_notify, g_closedMsg, 0, 0);
}

static LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(hwnd)) closePopup();
        return 0;
    case WM_CTLCOLORSTATIC: {
        // Read-only edits (the key boxes) also come through here.
        int id = GetDlgCtrlID((HWND)lp);
        if (id != ID_KEY_UP && id != ID_KEY_DOWN) {
            SetTextColor((HDC)wp, id == ID_TITLE ? g_text : g_subtext);
            SetBkColor((HDC)wp, g_bg);
            return (LRESULT)g_bgBrush;
        }
    }
    [[fallthrough]];
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, g_text);
        SetBkColor((HDC)wp, g_field);
        return (LRESULT)g_fieldBrush;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void createPopup(HWND notify, UINT closedMsg) {
    g_notify = notify;
    g_closedMsg = closedMsg;

    const struct { const wchar_t* label; const wchar_t* cls; int id; DWORD style; } rows[] = {
        {L"Volume up", L"EDIT", ID_KEY_UP, ES_READONLY | WS_BORDER},
        {L"Volume down", L"EDIT", ID_KEY_DOWN, ES_READONLY | WS_BORDER},
        {L"Step (%)", L"EDIT", ID_STEP, ES_NUMBER | WS_BORDER},
        {L"Scroll wheel", L"COMBOBOX", ID_SCROLL, CBS_DROPDOWNLIST},
    };
    int n = ARRAYSIZE(rows);
    int w = px(290), pad = px(16), rowH = px(34), fieldH = px(24), top = pad + px(34);
    int fieldX = pad + px(104), fieldW = w - pad - fieldX;
    g_popup = makeWindow(L"win-volume-popup", popupProc, WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WS_POPUP,
                         w, top + rowH * (n - 1) + fieldH + pad);
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_popup, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    HINSTANCE inst = GetModuleHandleW(nullptr);
    HWND title = CreateWindowW(L"STATIC", L"win-volume", WS_CHILD | WS_VISIBLE, pad, pad, w - pad * 2, px(22),
                               g_popup, (HMENU)ID_TITLE, inst, nullptr);
    SendMessageW(title, WM_SETFONT, (WPARAM)g_titleFont, TRUE);

    for (int i = 0; i < n; i++) {
        int y = top + rowH * i;
        // A combo box's height includes its dropdown list.
        int h = rows[i].id == ID_SCROLL ? px(200) : fieldH;
        HWND label = CreateWindowW(L"STATIC", rows[i].label, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                   pad, y, fieldX - pad, fieldH, g_popup, nullptr, inst, nullptr);
        HWND input = CreateWindowW(rows[i].cls, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | rows[i].style,
                                   fieldX, y, fieldW, h, g_popup, (HMENU)(INT_PTR)rows[i].id, inst, nullptr);
        SendMessageW(label, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(input, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(input, EM_SETMARGINS, EC_LEFTMARGIN, px(6));
        if (rows[i].id == ID_KEY_UP || rows[i].id == ID_KEY_DOWN) SetWindowSubclass(input, keyBoxProc, 0, 0);
    }
    for (auto& option : SCROLL_OPTIONS)
        SendDlgItemMessageW(g_popup, ID_SCROLL, CB_ADDSTRING, 0, (LPARAM)option.name);
}

bool openPopup() {
    // Clicking the tray icon while the popup is open closes it first; don't reopen it right away.
    if (GetTickCount() - g_hiddenAt < 300) return false;

    applyTheme();
    setKeyBox(GetDlgItem(g_popup, ID_KEY_UP), g_settings.keyUp);
    setKeyBox(GetDlgItem(g_popup, ID_KEY_DOWN), g_settings.keyDown);
    SetDlgItemInt(g_popup, ID_STEP, g_settings.stepPercent, FALSE);
    int scroll = 0;
    for (int i = 0; i < (int)ARRAYSIZE(SCROLL_OPTIONS); i++)
        if (SCROLL_OPTIONS[i].mods == g_settings.scrollMods) scroll = i;
    SendDlgItemMessageW(g_popup, ID_SCROLL, CB_SETCURSEL, scroll, 0);

    // Bottom-right of the work area, above the taskbar.
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
    return true;
}

bool popupDialogMessage(MSG& msg) { return IsDialogMessageW(g_popup, &msg); }

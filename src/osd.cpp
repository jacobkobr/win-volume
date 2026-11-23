#include "osd.h"
#include "ui.h"

#include <windows.h>
#include <cmath>

constexpr UINT_PTR HIDE_TIMER = 1;

static HWND g_osd;
static std::wstring g_name;
static float g_level;

static LRESULT CALLBACK osdProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        KillTimer(hwnd, HIDE_TIMER);
        ShowWindow(hwnd, SW_HIDE);
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
        std::wstring text = g_level < 0 ? L"No audio" : std::to_wstring(std::lround(g_level * 100)) + L"%";
        SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 240));
        DrawTextW(dc, g_name.c_str(), -1, &name, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(dc, RGB(170, 170, 170));
        DrawTextW(dc, text.c_str(), -1, &value, DT_SINGLELINE | DT_RIGHT);

        RECT track = {pad, rc.bottom - pad - px(4), rc.right - pad, rc.bottom - pad};
        fill(track, RGB(70, 70, 70));
        if (g_level > 0) {
            RECT bar = track;
            bar.right = track.left + std::lround((track.right - track.left) * g_level);
            fill(bar, RGB(96, 165, 250));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void createOsd() {
    // Click-through, never takes focus, hidden from alt-tab.
    int w = px(240), h = px(58);
    g_osd = makeWindow(L"win-volume-osd", osdProc,
                       WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                       WS_POPUP, w, h);
    SetLayeredWindowAttributes(g_osd, 0, 235, LWA_ALPHA);
    SetWindowRgn(g_osd, CreateRoundRectRgn(0, 0, w + 1, h + 1, px(12), px(12)), FALSE);
}

void showOsd(const std::wstring& appName, float level) {
    g_name = appName;
    g_level = level;

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
    SetTimer(g_osd, HIDE_TIMER, 1200, nullptr);
}

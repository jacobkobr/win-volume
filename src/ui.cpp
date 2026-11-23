#include "ui.h"

HFONT g_font, g_titleFont;
static int g_dpi = 96;

int px(int v) { return MulDiv(v, g_dpi, 96); }

void initUi() {
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);

    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    g_titleFont = CreateFontIndirectW(&ncm.lfMessageFont);
}

bool systemDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return !light;
}

// Windows has no public API for dark context menus. These undocumented uxtheme exports
// (ordinals 135 and 136) are what Explorer and Notepad use; if they're missing, menus just stay light.
void applyMenuTheme() {
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    HMODULE uxtheme = GetModuleHandleW(L"uxtheme.dll");
    if (!uxtheme) return;
    auto setMode = (SetPreferredAppMode)(void*)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemes)(void*)GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
    if (setMode) setMode(systemDark() ? 2 : 3);  // force dark / force light
    if (flush) flush();
}

HWND makeWindow(const wchar_t* cls, WNDPROC proc, DWORD exStyle, DWORD style, int w, int h) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    return CreateWindowExW(exStyle, cls, L"win-volume", style, 0, 0, w, h, nullptr, nullptr, inst, nullptr);
}

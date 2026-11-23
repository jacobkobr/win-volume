#include "ui.h"

#include <vector>

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

static bool usesLightTheme(const wchar_t* value) {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 value, RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light;
}

bool systemDark() { return !usesLightTheme(L"AppsUseLightTheme"); }

// Draws the mixer glyph from the Windows icon font, like the built-in tray icons.
// White on a dark taskbar, black on a light one.
HICON makeTrayIcon() {
    int size = GetSystemMetrics(SM_CXSMICON);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    DWORD* pixels = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&pixels, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(dc, color);

    // Windows 10 only has Segoe MDL2 Assets, which lacks the mixer glyph, so it gets a plain speaker.
    wchar_t glyph = 0xF4C3;
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                             L"Segoe Fluent Icons");
    HGDIOBJ oldFont = SelectObject(dc, font);
    wchar_t face[LF_FACESIZE];
    GetTextFaceW(dc, LF_FACESIZE, face);
    if (lstrcmpiW(face, L"Segoe Fluent Icons") != 0) {
        SelectObject(dc, oldFont);
        DeleteObject(font);
        glyph = 0xE767;
        font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                           L"Segoe MDL2 Assets");
        SelectObject(dc, font);
    }

    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    // Icon glyphs are drawn in the font's em square, which starts below the internal leading.
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE extent;
    GetTextExtentPoint32W(dc, &glyph, 1, &extent);
    TextOutW(dc, (size - extent.cx) / 2, -tm.tmInternalLeading, &glyph, 1);

    // GDI doesn't write alpha, so the white glyph's brightness becomes the alpha channel.
    DWORD rgb = usesLightTheme(L"SystemUsesLightTheme") ? 0x000000 : 0xFFFFFF;
    for (int i = 0; i < size * size; i++) pixels[i] = (pixels[i] & 0xFF) << 24 | rgb;

    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteDC(dc);

    std::vector<BYTE> maskBits(((size + 15) / 16) * 2 * size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO info = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
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

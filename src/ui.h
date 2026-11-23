#pragma once
#include <windows.h>

extern HFONT g_font, g_titleFont;

void initUi();
int px(int v);  // scales a 96-DPI pixel value to the screen DPI
bool systemDark();
void applyMenuTheme();
HWND makeWindow(const wchar_t* cls, WNDPROC proc, DWORD exStyle, DWORD style, int w, int h);

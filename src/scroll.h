#pragma once
#include <windows.h>

// Turns modifier + mouse wheel into `msg` posted to `target`, with the wheel delta as wParam.
// The wheel event is swallowed so the focused app doesn't also scroll. mods = 0 removes the hook.
void setScrollHook(UINT mods, HWND target, UINT msg);

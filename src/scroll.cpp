#include "scroll.h"

#include <atomic>

static std::atomic<UINT> g_mods;
static HWND g_target;
static UINT g_msg;
static DWORD g_threadId;

static UINT heldMods() {
    auto down = [](int vk) { return GetAsyncKeyState(vk) < 0; };
    return (down(VK_CONTROL) ? MOD_CONTROL : 0) | (down(VK_MENU) ? MOD_ALT : 0) |
           (down(VK_SHIFT) ? MOD_SHIFT : 0) | (down(VK_LWIN) || down(VK_RWIN) ? MOD_WIN : 0);
}

static LRESULT CALLBACK mouseHook(int code, WPARAM wp, LPARAM lp) {
    UINT mods = g_mods;
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && mods && heldMods() == mods) {
        auto* info = (MSLLHOOKSTRUCT*)lp;
        PostMessageW(g_target, g_msg, (WPARAM)(short)HIWORD(info->mouseData), 0);

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

// The hook gets its own thread so slow work on the main thread can never stall the mouse.
static DWORD WINAPI hookThread(void*) {
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(nullptr), 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
    UnhookWindowsHookEx(hook);
    return 0;
}

void setScrollHook(UINT mods, HWND target, UINT msg) {
    g_mods = mods;
    g_target = target;
    g_msg = msg;
    if (mods && !g_threadId) {
        CloseHandle(CreateThread(nullptr, 0, hookThread, nullptr, 0, &g_threadId));
    } else if (!mods && g_threadId) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        g_threadId = 0;
    }
}

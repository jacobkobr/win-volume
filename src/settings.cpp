#include "settings.h"

#include <commctrl.h>
#include <algorithm>
#include <string>

Settings g_settings;

static const std::wstring& iniPath() {
    static const std::wstring path = [] {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring exe = buf;
        return exe.substr(0, exe.find_last_of(L'.')) + L".ini";
    }();
    return path;
}

static int readInt(const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(L"settings", key, fallback, iniPath().c_str());
}

static void writeInt(const wchar_t* key, int value) {
    WritePrivateProfileStringW(L"settings", key, std::to_wstring(value).c_str(), iniPath().c_str());
}

void loadSettings() {
    g_settings.keyUp = readInt(L"volume_up", MAKEWORD(VK_UP, HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_EXT));
    g_settings.keyDown = readInt(L"volume_down", MAKEWORD(VK_DOWN, HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_EXT));
    g_settings.stepPercent = std::clamp(readInt(L"step", 5), 1, 25);
    g_settings.scrollMods = readInt(L"scroll_mods", MOD_CONTROL | MOD_ALT);
}

void saveSettings() {
    writeInt(L"volume_up", g_settings.keyUp);
    writeInt(L"volume_down", g_settings.keyDown);
    writeInt(L"step", g_settings.stepPercent);
    writeInt(L"scroll_mods", g_settings.scrollMods);
}

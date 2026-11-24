#pragma once
#include <windows.h>

// Hotkeys use the hotkey-control format: virtual key in the low byte, HOTKEYF_* flags in the high byte.
struct Settings {
    int keyUp;
    int keyDown;
    int stepPercent;
    UINT scrollMods;  // MOD_* flags, 0 turns scrolling off
    bool startup;
};

extern Settings g_settings;

// Stored in win-volume.ini next to the exe.
void loadSettings();
void saveSettings();

// Adds or removes win-volume from the current user's startup apps.
void applyStartup();

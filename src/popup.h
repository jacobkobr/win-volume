#pragma once
#include <windows.h>

// Settings flyout opened from the tray icon. Saves when it loses focus, then posts closedMsg to notify.
void createPopup(HWND notify, UINT closedMsg);

// Returns false if the popup was just closed by this same click on the tray icon.
bool openPopup();

// Tab navigation between fields; call from the message loop.
bool popupDialogMessage(MSG& msg);

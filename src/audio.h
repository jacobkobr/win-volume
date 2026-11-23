#pragma once
#include <string>

// Changes the volume of the app that owns the foreground window by delta (0-1 scale).
// Returns false if there's no foreground app. level is set to -1 if the app has no audio.
bool adjustFocusedApp(float delta, std::wstring& appName, float& level);

void releaseAudio();

#pragma once
#include <string>

// Works out the new volume for the app that owns the foreground window (delta on a 0-1 scale)
// without applying it, so the indicator can update first. Returns false if there's no foreground app.
// level is set to -1 if the app has no audio.
bool planVolumeChange(float delta, std::wstring& appName, float& level);

// Applies the level from the last planVolumeChange to all of that app's sessions.
void applyVolumeChange();

void releaseAudio();

#pragma once
#include <string>

// Small volume indicator near the bottom of the screen that hides itself after a moment.
void createOsd();
void showOsd(const std::wstring& appName, float level);  // level < 0 shows "No audio"

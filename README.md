# win-volume

Hotkeys that change the volume of the app in the focused window, with a small on-screen indicator.

- Default hotkeys: `Ctrl+Alt+Up` / `Ctrl+Alt+Down`, 5% per press
- Left-click the tray icon to change hotkeys and step size, right-click to exit
- Settings are saved to `win-volume.ini` next to the exe

## Build

```
cmake -S . -B build
cmake --build build --config Release
```

Or with MinGW directly:

```
g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -DNOMINMAX -mwindows -static -s main.cpp -o win-volume.exe -lole32 -lshell32 -luser32 -lgdi32 -lcomctl32
```

To start it with Windows, put a shortcut to the exe in `shell:startup`.

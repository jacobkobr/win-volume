cmake_minimum_required(VERSION 3.20)
project(win-volume CXX RC)

set(CMAKE_CXX_STANDARD 17)

add_executable(win-volume WIN32
    src/main.cpp
    src/audio.cpp
    src/osd.cpp
    src/popup.cpp
    src/scroll.cpp
    src/settings.cpp
    src/ui.cpp
    res/app.rc
)
target_compile_definitions(win-volume PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(win-volume PRIVATE ole32 shell32 user32 gdi32 comctl32 dwmapi uxtheme advapi32)

if(MSVC)
    # The manifest comes from app.rc.
    target_link_options(win-volume PRIVATE /MANIFEST:NO)
elseif(MINGW)
    target_link_options(win-volume PRIVATE -static -s)
endif()

cmake_minimum_required(VERSION 3.20)
project(win-volume CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(win-volume WIN32 main.cpp)
target_compile_definitions(win-volume PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(win-volume PRIVATE ole32 shell32 user32 gdi32 comctl32)

if(MINGW)
    target_link_options(win-volume PRIVATE -static -s)
endif()

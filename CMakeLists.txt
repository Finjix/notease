cmake_minimum_required(VERSION 3.20)

project(Notease LANGUAGES CXX)

add_executable(Notease WIN32
    src/main.cpp
    src/resource.rc
)

target_compile_features(Notease PRIVATE cxx_std_17)
target_compile_definitions(Notease PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_link_libraries(Notease PRIVATE
    advapi32
    dwmapi
    gdi32
    shell32
    user32
    gdiplus
)

if(MSVC)
    target_compile_options(Notease PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

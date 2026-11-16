cmake_minimum_required(VERSION 3.21)
project(MediaCenterUpdater C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Link the C runtime into the program, so the target PC needs nothing else installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT MSVC)
    message(FATAL_ERROR "Media Center Updater builds with the Microsoft C compiler (Visual Studio Build Tools).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Media Center Updater is a 64-bit program. Use the x64 build tools.")
endif()

# A development build asks for no administrator rights and keeps its settings and single-instance
# lock apart from an installed copy. See README.md.
option(MCU_DEV "Build a development copy that runs without administrator rights" OFF)

add_compile_options(/W4 /utf-8 /wd4100 /wd4204 /wd4221)
add_compile_definitions(UNICODE _UNICODE)
if(MCU_DEV)
    add_compile_definitions(MCU_DEV)
endif()

# Everything except the window, shared by the app and the tests.
add_library(core STATIC
    src/util.c
    src/json.c
    src/xml.c
    src/http.c
    src/system.c
    src/models.c
    src/settings.c
    src/coordinator.c
    src/discovery.c
    src/providers.c
    src/selfupdate.c)
target_link_libraries(core PUBLIC winhttp crypt32 bcrypt wintrust shell32 advapi32 user32)

add_executable(MediaCenterUpdater WIN32
    src/main.c
    src/mainwindow.c
    src/ui.c
    src/integration.c
    src/app.rc)
target_include_directories(MediaCenterUpdater PRIVATE src ${CMAKE_SOURCE_DIR})
target_link_libraries(MediaCenterUpdater PRIVATE core comctl32 uxtheme dwmapi gdiplus gdi32)
# The manifest is compiled in from src/app.rc, so the linker must not add its own.
target_link_options(MediaCenterUpdater PRIVATE /MANIFEST:NO)

add_executable(tests Tests/tests.c)
target_link_libraries(tests PRIVATE core)

cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)   # allow MSVC_RUNTIME_LIBRARY selection
project(JusticeLauncher C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static C runtime → the exe needs no VC++ redist on the target machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TP  ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(WV2 ${TP}/webview2/build/native)

if(NOT EXISTS ${WV2}/include/WebView2.h)
    message(FATAL_ERROR "WebView2 SDK not found at ${WV2}. Run bootstrap.ps1 first.")
endif()
if(NOT EXISTS ${TP}/nlohmann/json.hpp)
    message(FATAL_ERROR "nlohmann/json not found. Run bootstrap.ps1 first.")
endif()

# Absolute path to the web frontend (Code/web), baked in at configure time.
# Override at runtime with the JUSTICE_SRC environment variable.
if(NOT DEFINED JUSTICE_SRC_DIR)
    get_filename_component(JUSTICE_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../web ABSOLUTE)
endif()

add_executable(JusticeLauncher WIN32
    src/main.cpp
    src/host.cpp
    src/bridge.cpp
    src/handlers.cpp
    src/http.cpp
    src/util.cpp
    src/auth.cpp
    src/download.cpp
    src/zip.cpp
    src/game.cpp
    src/mods.cpp
    src/extra.cpp
    src/discord.cpp
    src/overlay.cpp
    src/updater.cpp
    src/cors_test.cpp
    ${TP}/miniz/miniz.c
    app.rc
)

target_include_directories(JusticeLauncher PRIVATE
    ${WV2}/include
    ${TP}
    ${TP}/miniz
)

target_link_libraries(JusticeLauncher PRIVATE
    ${WV2}/x64/WebView2LoaderStatic.lib
    Shlwapi Shell32 Ole32 Version Dwmapi Winhttp Crypt32
)

target_compile_definitions(JusticeLauncher PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    JUSTICE_SRC_DIR="${JUSTICE_SRC_DIR}"
)

if(MSVC)
    target_compile_options(JusticeLauncher PRIVATE /EHsc /W3 /utf-8)
endif()

cmake_minimum_required(VERSION 3.20)
project(MinView VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT — produce a single, standalone .exe
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Windows subsystem (no console window)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS /MANIFEST:NO")

# Source files
set(SOURCES
    src/main.cpp
    src/window.cpp
    src/renderer.cpp
    src/decoder.cpp
    src/viewer.cpp
    src/app.cpp
    src/indexer.cpp
    src/metadata.cpp
    src/MinView.rc
)

set(HEADERS
    src/window.h
    src/renderer.h
    src/decoder.h
    src/viewer.h
    src/app.h
    src/indexer.h
    src/metadata.h
)

# Windows libraries (all part of Windows SDK, no external deps)
# d2d1, dwrite, windowscodecs are all built-in
add_executable(MinView WIN32 ${SOURCES} ${HEADERS})

target_link_libraries(MinView
    d2d1
    dwrite
    windowscodecs
    d3d11
    dxgi
    dxguid
    ole32
    comctl32
    comdlg32
    shell32
    shlwapi
    dwmapi
    uxtheme
)

# MSVC warnings
target_compile_options(MinView PRIVATE /W4 /utf-8)

# Disable Windows min/max macros (conflict with std::min/std::max)
target_compile_definitions(MinView PRIVATE NOMINMAX)

# ── Test: dump metadata ───────────────────────────────────────
add_executable(test_meta
    test_meta.cpp
    src/metadata.cpp
)
target_include_directories(test_meta PRIVATE src)
target_compile_options(test_meta PRIVATE /W4 /utf-8)
target_compile_definitions(test_meta PRIVATE NOMINMAX)
target_link_libraries(test_meta PRIVATE
    shlwapi
)
set_target_properties(test_meta PROPERTIES
    WIN32_EXECUTABLE FALSE
)
target_link_options(test_meta PRIVATE /SUBSYSTEM:CONSOLE)

# Enable manifest for DPI awareness
set_target_properties(MinView PROPERTIES
    WIN32_EXECUTABLE TRUE
)

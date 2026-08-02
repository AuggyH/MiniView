cmake_minimum_required(VERSION 3.20)
project(MinView VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MINVIEW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

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
    src/file_operation.cpp
    src/file_operation_windows.cpp
    src/indexer.cpp
    src/metadata.cpp
    src/MinView.rc
)

set(HEADERS
    src/window.h
    src/renderer.h
    src/renderer_state.h
    src/decoder.h
    src/viewer.h
    src/app.h
    src/app_state.h
    src/file_operation.h
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
if(MINVIEW_WARNINGS_AS_ERRORS)
    target_compile_options(MinView PRIVATE /WX)
endif()

# Disable Windows min/max macros (conflict with std::min/std::max)
target_compile_definitions(MinView PRIVATE NOMINMAX)

# ── Test: dump metadata ───────────────────────────────────────
add_executable(test_meta
    test_meta.cpp
    src/metadata.cpp
)
target_include_directories(test_meta PRIVATE src)
target_compile_options(test_meta PRIVATE /W4 /utf-8)
if(MINVIEW_WARNINGS_AS_ERRORS)
    target_compile_options(test_meta PRIVATE /WX)
endif()
target_compile_definitions(test_meta PRIVATE NOMINMAX)
target_link_libraries(test_meta PRIVATE
    shlwapi
)
set_target_properties(test_meta PROPERTIES
    WIN32_EXECUTABLE FALSE
)
target_link_options(test_meta PRIVATE /SUBSYSTEM:CONSOLE)

include(CTest)
if(BUILD_TESTING)
    add_executable(indexer_tests
        tests/indexer_tests.cpp
        src/indexer.cpp
    )
    target_include_directories(indexer_tests PRIVATE src)
    target_compile_options(indexer_tests PRIVATE /W4 /utf-8)
    if(MINVIEW_WARNINGS_AS_ERRORS)
        target_compile_options(indexer_tests PRIVATE /WX)
    endif()
    target_compile_definitions(indexer_tests PRIVATE NOMINMAX)
    set_target_properties(indexer_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(indexer_tests PRIVATE /SUBSYSTEM:CONSOLE)
    add_test(NAME indexer.unit COMMAND indexer_tests)

    add_executable(app_state_tests
        tests/app_state_tests.cpp
    )
    target_include_directories(app_state_tests PRIVATE src)
    target_compile_options(app_state_tests PRIVATE /W4 /utf-8)
    if(MINVIEW_WARNINGS_AS_ERRORS)
        target_compile_options(app_state_tests PRIVATE /WX)
    endif()
    target_compile_definitions(app_state_tests PRIVATE NOMINMAX)
    target_link_libraries(app_state_tests PRIVATE user32)
    set_target_properties(app_state_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(app_state_tests PRIVATE /SUBSYSTEM:CONSOLE)
    add_test(NAME app_state.unit COMMAND app_state_tests)

    add_executable(metadata_tests
        tests/metadata_tests.cpp
        src/metadata.cpp
    )
    target_include_directories(metadata_tests PRIVATE src)
    target_compile_options(metadata_tests PRIVATE /W4 /utf-8)
    if(MINVIEW_WARNINGS_AS_ERRORS)
        target_compile_options(metadata_tests PRIVATE /WX)
    endif()
    target_compile_definitions(metadata_tests PRIVATE NOMINMAX)
    target_link_libraries(metadata_tests PRIVATE shlwapi)
    set_target_properties(metadata_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(metadata_tests PRIVATE /SUBSYSTEM:CONSOLE)
    add_test(NAME metadata.unit COMMAND metadata_tests)

    add_executable(renderer_state_tests
        tests/renderer_state_tests.cpp
    )
    target_include_directories(renderer_state_tests PRIVATE src)
    target_compile_options(renderer_state_tests PRIVATE /W4 /utf-8)
    if(MINVIEW_WARNINGS_AS_ERRORS)
        target_compile_options(renderer_state_tests PRIVATE /WX)
    endif()
    target_compile_definitions(renderer_state_tests PRIVATE NOMINMAX)
    set_target_properties(renderer_state_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(renderer_state_tests PRIVATE /SUBSYSTEM:CONSOLE)
    add_test(NAME renderer_state.unit COMMAND renderer_state_tests)

    add_executable(file_operation_tests
        tests/file_operation_tests.cpp
        src/file_operation.cpp
        src/file_operation_windows.cpp
        src/decoder.cpp
    )
    target_include_directories(file_operation_tests PRIVATE src)
    target_compile_options(file_operation_tests PRIVATE /W4 /utf-8)
    if(MINVIEW_WARNINGS_AS_ERRORS)
        target_compile_options(file_operation_tests PRIVATE /WX)
    endif()
    target_compile_definitions(file_operation_tests PRIVATE
        NOMINMAX
        MINVIEW_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(file_operation_tests PRIVATE
        shell32
        windowscodecs
        ole32
    )
    set_target_properties(file_operation_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(file_operation_tests PRIVATE /SUBSYSTEM:CONSOLE)
    add_test(NAME file_operation.unit COMMAND file_operation_tests)

    add_test(NAME governance.unit
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/governance_tests.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}"
            -CMakeCommand "${CMAKE_COMMAND}"
            -CTestCommand "${CMAKE_CTEST_COMMAND}"
            -BuildDirectory "${CMAKE_BINARY_DIR}"
            -Configuration "$<CONFIG>"
    )
    set(MINVIEW_GOVERNANCE_TEST_TIMEOUT_SECONDS 120)
    set_tests_properties(governance.unit PROPERTIES
        TIMEOUT ${MINVIEW_GOVERNANCE_TEST_TIMEOUT_SECONDS}
    )

    add_executable(indexer_benchmark
        tools/indexer_benchmark.cpp
        src/indexer.cpp
    )
    target_include_directories(indexer_benchmark PRIVATE src)
    target_compile_options(indexer_benchmark PRIVATE /W4 /utf-8)
    if(MINVIEW_WARNINGS_AS_ERRORS)
        target_compile_options(indexer_benchmark PRIVATE /WX)
    endif()
    target_compile_definitions(indexer_benchmark PRIVATE NOMINMAX)
    target_link_libraries(indexer_benchmark PRIVATE psapi)
    set_target_properties(indexer_benchmark PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(indexer_benchmark PRIVATE /SUBSYSTEM:CONSOLE)

    add_executable(wic_format_probe
        tools/wic_format_probe.cpp
        src/decoder.cpp
    )
    target_include_directories(wic_format_probe PRIVATE src)
    target_compile_options(wic_format_probe PRIVATE /W4 /utf-8)
    if(MINVIEW_WARNINGS_AS_ERRORS)
        target_compile_options(wic_format_probe PRIVATE /WX)
    endif()
    target_compile_definitions(wic_format_probe PRIVATE NOMINMAX)
    target_link_libraries(wic_format_probe PRIVATE windowscodecs ole32)
    set_target_properties(wic_format_probe PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(wic_format_probe PRIVATE /SUBSYSTEM:CONSOLE)

    add_executable(hardlink_fixture
        tools/hardlink_fixture.cpp
    )
    target_compile_options(hardlink_fixture PRIVATE /W4 /utf-8)
    if(MINVIEW_WARNINGS_AS_ERRORS)
        target_compile_options(hardlink_fixture PRIVATE /WX)
    endif()
    target_compile_definitions(hardlink_fixture PRIVATE NOMINMAX)
    set_target_properties(hardlink_fixture PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(hardlink_fixture PRIVATE /SUBSYSTEM:CONSOLE)

endif()

# Enable manifest for DPI awareness
set_target_properties(MinView PROPERTIES
    WIN32_EXECUTABLE TRUE
)

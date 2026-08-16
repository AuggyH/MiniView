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
    src/app.cpp
    src/comic_reader_loader.cpp
    src/file_operation.cpp
    src/file_operation_windows.cpp
    src/indexer.cpp
    src/dirwatch.cpp
    src/albumstate.cpp
    src/metadata.cpp
    src/MinView.rc
)

set(HEADERS
    src/window.h
    src/renderer.h
    src/renderer_state.h
    src/open_error.h
    src/decoder.h
    src/app.h
    src/app_state.h
    src/layout.h
    src/comic_reader_loader.h
    src/comic_reader_model.h
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
    psapi
)

# MSVC warnings
target_compile_options(MinView PRIVATE /W4 /utf-8)
if(MINVIEW_WARNINGS_AS_ERRORS)
    target_compile_options(MinView PRIVATE /WX)
endif()

# Disable Windows min/max macros (conflict with std::min/std::max)
target_compile_definitions(MinView PRIVATE NOMINMAX)

include(CTest)
if(BUILD_TESTING)
    function(mv_add_unit_test name)
        add_executable(${name} ${ARGN})
        target_include_directories(${name} PRIVATE src)
        target_compile_options(${name} PRIVATE /W4 /utf-8)
        if(MINVIEW_WARNINGS_AS_ERRORS)
            target_compile_options(${name} PRIVATE /WX)
        endif()
        target_compile_definitions(${name} PRIVATE NOMINMAX)
        set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE FALSE)
        target_link_options(${name} PRIVATE /SUBSYSTEM:CONSOLE)
    endfunction()

    mv_add_unit_test(indexer_tests
        tests/indexer_tests.cpp
        src/indexer.cpp
    )
    add_test(NAME indexer.unit COMMAND indexer_tests)

    mv_add_unit_test(app_state_tests
        tests/app_state_tests.cpp
        src/decoder.cpp
    )
    target_compile_definitions(app_state_tests PRIVATE
        MINVIEW_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(app_state_tests PRIVATE windowscodecs ole32 user32)
    add_test(NAME app_state.unit COMMAND app_state_tests)

    mv_add_unit_test(navstate_tests
        tests/navstate_tests.cpp
        src/indexer.cpp
    )
    add_test(NAME navstate.unit COMMAND navstate_tests)

    mv_add_unit_test(albumstate_tests
        tests/albumstate_tests.cpp
        src/albumstate.cpp
    )
    add_test(NAME albumstate.unit COMMAND albumstate_tests)

    mv_add_unit_test(comic_reader_model_tests
        tests/comic_reader_model_tests.cpp
    )
    add_test(NAME comic_reader_model.unit COMMAND comic_reader_model_tests)

    mv_add_unit_test(comic_reader_loader_tests
        tests/comic_reader_loader_tests.cpp
        src/comic_reader_loader.cpp
        src/decoder.cpp
    )
    target_link_libraries(comic_reader_loader_tests PRIVATE windowscodecs ole32)
    add_test(NAME comic_reader_loader.unit COMMAND comic_reader_loader_tests)

    mv_add_unit_test(decoder_tests
        tests/decoder_tests.cpp
        src/decoder.cpp
    )
    target_compile_definitions(decoder_tests PRIVATE
        MINVIEW_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(decoder_tests PRIVATE windowscodecs ole32)
    add_test(NAME decoder.unit COMMAND decoder_tests)

    mv_add_unit_test(metadata_tests
        tests/metadata_tests.cpp
        src/metadata.cpp
    )
    target_link_libraries(metadata_tests PRIVATE shlwapi)
    add_test(NAME metadata.unit COMMAND metadata_tests)

    mv_add_unit_test(filmstrip_model_tests
        tests/filmstrip_model_tests.cpp
    )
    add_test(NAME filmstrip_model.unit COMMAND filmstrip_model_tests)

    mv_add_unit_test(renderer_state_tests
        tests/renderer_state_tests.cpp
    )
    add_test(NAME renderer_state.unit COMMAND renderer_state_tests)

    mv_add_unit_test(file_operation_tests
        tests/file_operation_tests.cpp
        src/file_operation.cpp
        src/file_operation_windows.cpp
        src/decoder.cpp
    )
    target_compile_definitions(file_operation_tests PRIVATE
        MINVIEW_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(file_operation_tests PRIVATE
        shell32
        windowscodecs
        ole32
    )
    add_test(NAME file_operation.unit COMMAND file_operation_tests)

endif()

# Enable manifest for DPI awareness
set_target_properties(MinView PROPERTIES
    WIN32_EXECUTABLE TRUE
)

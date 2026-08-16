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
    src/thumb_engine.cpp
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
    src/design_tokens.h
    src/fast_image_dims.h
    src/comic_reader_loader.h
    src/comic_reader_model.h
    src/file_operation.h
    src/indexer.h
    src/metadata.h
    src/grid_layout_model.h
    src/selection_remap.h
    src/title_bar_model.h
    src/album_sampler.h
    src/thumb_engine.h
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

    mv_add_unit_test(pipeline_tests
        tests/pipeline_tests.cpp
        src/indexer.cpp
        src/decoder.cpp
    )
    target_compile_definitions(pipeline_tests PRIVATE
        MINVIEW_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(pipeline_tests PRIVATE
        windowscodecs
        ole32
        shlwapi
        shell32
    )
    add_test(NAME pipeline.integration COMMAND pipeline_tests)

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

    mv_add_unit_test(renderer_tests
        tests/renderer_tests.cpp
        src/renderer.cpp
    )
    target_link_libraries(renderer_tests PRIVATE
        d2d1
        dwrite
        windowscodecs
        d3d11
        dxgi
        dxguid
        ole32
        comctl32
        uxtheme
        dwmapi
        user32
    )
    add_test(NAME renderer.unit COMMAND renderer_tests)

    mv_add_unit_test(design_tokens_tests
        tests/design_tokens_tests.cpp
    )
    add_test(NAME design_tokens.unit COMMAND design_tokens_tests)

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

    mv_add_unit_test(grid_layout_model_tests
        tests/grid_layout_model_tests.cpp
    )
    add_test(NAME grid_layout_model.unit COMMAND grid_layout_model_tests)

    mv_add_unit_test(selection_remap_tests
        tests/selection_remap_tests.cpp
        src/indexer.cpp
    )
    add_test(NAME selection_remap.unit COMMAND selection_remap_tests)

    mv_add_unit_test(title_bar_model_tests
        tests/title_bar_model_tests.cpp
    )
    add_test(NAME title_bar_model.unit COMMAND title_bar_model_tests)

    mv_add_unit_test(dirwatch_tests
        tests/dirwatch_tests.cpp
        src/dirwatch.cpp
    )
    target_link_libraries(dirwatch_tests PRIVATE user32)
    add_test(NAME dirwatch.unit COMMAND dirwatch_tests)

    mv_add_unit_test(album_sampler_tests
        tests/album_sampler_tests.cpp
    )
    add_test(NAME album_sampler.unit COMMAND album_sampler_tests)

    mv_add_unit_test(thumb_engine_tests
        tests/thumb_engine_tests.cpp
        src/thumb_engine.cpp
        src/decoder.cpp
    )
    target_link_libraries(thumb_engine_tests PRIVATE windowscodecs ole32)
    add_test(NAME thumb_engine.unit COMMAND thumb_engine_tests)

endif()

# Enable manifest for DPI awareness
set_target_properties(MinView PROPERTIES
    WIN32_EXECUTABLE TRUE
)

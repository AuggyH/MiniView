#include "app.h"
#include <stdexcept>
#include <chrono>
#include <iostream>

namespace mv {

App::App()  = default;
App::~App() = default;

int App::run(const std::wstring& initial_path) {
    // Create window
    if (!m_window.create(L"MinView", 1200, 800))
        throw std::runtime_error("Failed to create window");

    // Register message handler
    m_window.set_message_callback(
        [this](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            return handle_message(hwnd, msg, wp, lp);
        });

    // Initialize Direct2D
    if (!m_renderer.init(m_window.handle()))
        throw std::runtime_error("Failed to init Direct2D renderer");

    // Open initial image if provided
    if (!initial_path.empty()) {
        open_image(initial_path);
    }

    // Main loop
    return m_window.run();
}

LRESULT App::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        uint32_t w = LOWORD(lp);
        uint32_t h = HIWORD(lp);
        if (w > 0 && h > 0) {
            m_renderer.resize(w, h);
        }
        return 0;
    }

    case WM_PAINT: {
        render_frame();
        ValidateRect(hwnd, nullptr);
        return 0;
    }

    case WM_ERASEBKGND:
        // We handle all drawing — suppress default erase to avoid flicker
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0) {
            open_image(path);
        }
        DragFinish(drop);
        return 0;
    }
    }

    return -1; // use default handling
}

void App::open_image(const std::wstring& path) {
    auto t0 = std::chrono::high_resolution_clock::now();

    try {
        // Decode full resolution via WIC
        auto bitmap = m_decoder.decode(path);

        // Upload to GPU at full resolution
        m_renderer.upload_image(bitmap.Get());

        m_current_path = path;
        m_has_image = true;

        auto t2 = std::chrono::high_resolution_clock::now();
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count();

        // Set window title
        size_t pos = path.find_last_of(L"\\/");
        std::wstring filename = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
        uint32_t iw, ih;
        m_renderer.image_size(iw, ih);
        wchar_t title[256];
        swprintf_s(title, L"MinView — %s (%u×%u) [%lldms]",
            filename.c_str(), iw, ih, total_ms);
        SetWindowTextW(m_window.handle(), title);

        m_window.invalidate();
    } catch (const std::exception& e) {
        wchar_t title[512];
        swprintf_s(title, L"MinView — Error: %hs", e.what());
        SetWindowTextW(m_window.handle(), title);
    }
}

void App::render_frame() {
    if (!m_renderer.begin_frame()) return;

    m_renderer.clear();

    if (m_has_image) {
        m_renderer.draw_image();
    }

    m_renderer.end_frame();
}

} // namespace mv

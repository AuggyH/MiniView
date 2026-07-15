#include "app.h"
#include <stdexcept>
#include <chrono>

namespace mv {

App::App()  = default;
App::~App() = default;

int App::run(const std::wstring& initial_path) {
    if (!m_window.create(L"MinView", 1200, 800))
        throw std::runtime_error("Failed to create window");

    m_window.set_message_callback(
        [this](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            return handle_message(hwnd, msg, wp, lp);
        });

    if (!m_renderer.init(m_window.handle()))
        throw std::runtime_error("Failed to init Direct2D renderer");

    if (!initial_path.empty()) {
        open_image(initial_path);
    }

    return m_window.run();
}

LRESULT App::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        uint32_t w = LOWORD(lp);
        uint32_t h = HIWORD(lp);
        if (w > 0 && h > 0) m_renderer.resize(w, h);
        return 0;
    }
    case WM_PAINT: {
        render_frame();
        ValidateRect(hwnd, nullptr);
        return 0;
    }
    case WM_ERASEBKGND:
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0) open_image(path);
        DragFinish(drop);
        return 0;
    }
    }
    return -1;
}

void App::open_image(const std::wstring& path) {
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        auto bitmap = m_decoder.decode(path);
        m_renderer.upload_image(bitmap.Get());
        m_current_path = path;
        m_has_image = true;

        auto t2 = std::chrono::high_resolution_clock::now();
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count();

        // Title: concise (workaround for system hook that truncates custom-class titles)
        size_t pos = path.find_last_of(L"\\/");
        std::wstring filename = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
        uint32_t iw, ih;
        m_renderer.image_size(iw, ih);

        // Use short code prefix to survive title truncation
        std::wstring title = std::to_wstring(iw) + L"x" + std::to_wstring(ih) +
            L" " + std::to_wstring(total_ms) + L"ms " + filename;
        SetWindowTextW(m_window.handle(), title.c_str());

        m_window.invalidate();
    } catch (const std::exception&) {
        SetWindowTextW(m_window.handle(), L"Error loading image");
    }
}

void App::render_frame() {
    if (!m_renderer.begin_frame()) return;
    m_renderer.clear();
    if (m_has_image) m_renderer.draw_image();
    m_renderer.end_frame();
}

} // namespace mv

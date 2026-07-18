#pragma once
#include "window.h"
#include "renderer.h"
#include "decoder.h"
#include "indexer.h"
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <atomic>

namespace mv {

class App {
public:
    App();
    ~App();

    int run(const std::wstring& initial_path = L"");

private:
    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void    open_image(const std::wstring& path);
    void    navigate_to(int idx);
    void    fit_to_window();
    void    zoom_at_center(float factor);
    void    toggle_fullscreen(HWND hwnd);
    void    render_frame();
    void    update_title();

    void    delete_current_file(bool permanent);
    void    open_in_explorer();
    void    copy_to_clipboard();
    void    show_context_menu(HWND hwnd, int x, int y);

    // Preloader
    void    start_preloader();
    void    stop_preloader();
    void    request_preload(const std::wstring& path);
    Microsoft::WRL::ComPtr<IWICBitmapSource> get_preloaded(const std::wstring& path);
    void    preload_neighbors();

    Window     m_window;
    Renderer   m_renderer;
    Decoder    m_decoder;
    ImageIndex m_index;

    std::wstring m_current_path;
    int          m_current_idx = -1;
    bool         m_has_image = false;
    bool         m_fullscreen = false;

    WINDOWPLACEMENT m_saved_placement = {sizeof(WINDOWPLACEMENT)};
    LONG            m_saved_style = 0;
    LONG            m_saved_exstyle = 0;

    bool  m_dragging = false;
    int   m_drag_start_x = 0;
    int   m_drag_start_y = 0;
    float m_drag_offset_x = 0;
    float m_drag_offset_y = 0;

    // Preloader state
    std::thread m_preload_thread;
    std::mutex  m_preload_mutex;
    std::condition_variable m_preload_cv;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IWICBitmapSource>> m_preload_cache;
    std::vector<std::wstring> m_preload_queue;
    std::atomic<bool> m_preload_running{false};
};

} // namespace mv

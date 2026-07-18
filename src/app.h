#pragma once
#include "window.h"
#include "renderer.h"
#include "decoder.h"
#include "indexer.h"
#include "metadata.h"
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

    // Thumb cache entry (public for background thread access)
    struct ThumbEntry {
        Microsoft::WRL::ComPtr<IWICBitmapSource> wic;
        bool loaded = false;
        uint32_t orig_w = 0, orig_h = 0;
        D2D1_COLOR_F dominant_color = {0.10f, 0.10f, 0.12f, 1.0f};
    };

private:
    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void    open_image(const std::wstring& path);
    void    navigate_to(int idx);
    void    fit_to_window();
    void    zoom_at_center(float factor);
    void    toggle_fullscreen(HWND hwnd);
    void    toggle_recursive();
    void    render_frame();
    void    update_title();

    void    delete_current_file(bool permanent);
    void    delete_selected(bool permanent);
    void    open_in_explorer();
    void    show_toolbar_menu(HWND hwnd, int idx, int x, int y);
    void    copy_to_clipboard();
    void    copy_selected();
    void    show_context_menu(HWND hwnd, int x, int y);

    // Preloader
    void    start_preloader();
    void    stop_preloader();
    void    request_preload(const std::wstring& path);
    Microsoft::WRL::ComPtr<IWICBitmapSource> get_preloaded(const std::wstring& path);
    void    preload_neighbors();

    // Grid mode
    void    toggle_grid();
    void    set_sort_mode(SortMode mode);
    void    toggle_thumb_square();
    void    toggle_info();
    bool    has_selection() const;
    void    clear_selection();
    void    select_range(int start, int end);
    bool    grid_click(int x, int y, bool shift, bool ctrl);
    void    grid_navigate(int dir, bool shift);
    void    grid_ensure_visible();
    void    grid_render();
    void    handle_scrollbar_click(HWND hwnd, int mx, int my);
    void    select_item(int idx, bool shift, bool ctrl);
    void    start_thumb_loader();
    void    stop_thumb_loader();
    void    request_thumb(int idx);

    int m_thumb_size = 160;   // decode resolution (WIC)
    int m_thumb_cell  = 160;  // display cell size for column calc
    float m_thumb_zoom = 1.0f; // Ctrl+wheel zoom factor
    int m_thumb_gap_h = 8;    // horizontal gap
    int m_thumb_gap_v = 16;   // vertical gap
    int m_thumb_pad  = 8;     // uniform padding (all sides)
    int m_cell_size  = 168;   // size + gap_h

    Window     m_window;
    Renderer   m_renderer;
    Decoder    m_decoder;
    ImageIndex m_index;

    std::wstring m_current_path;
    int          m_current_idx = -1;
    bool         m_has_image = false;
    bool         m_fullscreen = false;
    bool         m_recursive = false;

    WINDOWPLACEMENT m_saved_placement = {sizeof(WINDOWPLACEMENT)};
    LONG            m_saved_style = 0;
    LONG            m_saved_exstyle = 0;

    bool  m_drag_pending = false;
    int   m_drag_start_x = 0;
    int   m_drag_start_y = 0;

    // Preloader state
    std::thread m_preload_thread;
    std::mutex  m_preload_mutex;
    std::condition_variable m_preload_cv;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IWICBitmapSource>> m_preload_cache;
    std::vector<std::wstring> m_preload_queue;
    std::atomic<bool> m_preload_running{false};

    // Grid state
    bool  m_grid_mode = false;
    int   m_grid_scroll_y = 0;
    int   m_grid_sel = -1;
    int   m_grid_cols = 0;
    int   m_grid_total_rows = 0;
    std::vector<int> m_row_heights;
    bool  m_thumb_square = false;
    bool  m_show_labels = true;
    bool  m_show_info = false;
    bool  m_using_thumb_preview = false;
    bool  m_from_grid = false;  // entered image/fullscreen from grid → Esc returns
    bool  m_temp_preview = false;  // Space quick preview (no title bar)
    int   m_panel_width = 280;
    UINT_PTR m_grid_timer = 0;
    int   m_toolbar_h = 28;
    std::vector<std::wstring> m_toolbar_items = {L"文件", L"查看", L"编辑", L"帮助"};
    int   m_toolbar_active = -1;
    HWND  m_tooltip = nullptr;
    bool  m_scroll_active = false;
    bool  m_scrollbar_dragging = false;  // scrollbar thumb drag active
    bool  m_scrollbar_hover    = false;  // mouse hovering over scrollbar
    int   m_scrollbar_drag_y = 0;        // mouse y when drag started
    int   m_scrollbar_drag_pos = 0;      // scroll position when drag started
    int   m_grid_total_h = 0;            // cached total grid content height
    int   m_grid_scroll_saved = 0;
    int   m_grid_saved_idx = 0;
    ImageMeta m_info_meta;
    std::vector<mv::PanelRegion> m_panel_clickable;
    int m_panel_sel = -1;           // selected clickable index (brief highlight)
    float m_panel_scroll_y = 0;     // side panel scroll offset
    float m_panel_total_h = 0;      // total content height for clamping
    std::wstring m_panel_copied;    // last copied text for toast
    UINT_PTR m_toast_timer = 0;     // toast dismiss timer
    UINT_PTR m_sel_timer = 0;       // selection clear timer

    // Title bar (custom, bypasses system hook on this PC)
    int m_title_h = 40;  // title bar height in DIPs (was 32)
    int m_title_btn_hover = -1;  // 0=min, 1=max, 2=close
    int m_title_btn_press = -1;

    // Multi-select (grid only)
    std::vector<bool> m_selected;
    int  m_sel_anchor = -1;

    // Thumb cache (WIC bitmaps — thread-safe, loaded by background thread)
    std::vector<ThumbEntry> m_thumbs;

    // D2D bitmap cache (main-thread only, populated during render)
    std::unordered_map<int, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> m_thumb_d2d;

    // Thumb loader threads
    std::vector<std::thread> m_thumb_threads;
    std::mutex  m_thumb_mutex;
    std::condition_variable m_thumb_cv;
    std::vector<int> m_thumb_queue;
    std::atomic<bool> m_thumb_running{false};
};

} // namespace mv

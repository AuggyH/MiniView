#pragma once
#include "window.h"
#include "renderer.h"
#include "open_error.h"
#include "decoder.h"
#include "indexer.h"
#include "dirwatch.h"
#include "albumstate.h"
#include "metadata.h"
#include "app_state.h"
#include "file_operation.h"
#include "comic_reader_loader.h"
#include "comic_reader_model.h"
#include "navstate.h"
#include "filmstrip_model.h"
#include "layout.h"
#include "grid_layout_model.h"
#include "thumb_engine.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <deque>
#include <atomic>

namespace mv {

class AppComicPort;

// Async collection-scan job/result (Issue #5 P2). Kept at namespace scope so
// the worker functions can use them.
struct NavScanJob {
    std::wstring path;
    bool recursive = false;
    std::uint64_t generation = 0;
    SortMode sort = SortMode::Name;
    // Real-time refresh: rescan the current collection in place. roots is
    // non-empty for multi-root albums; otherwise the single path is used.
    bool refresh = false;
    std::vector<ScanRoot> roots;
    // Album/favourite collections (Issue #5 P3): paths = explicit image
    // list (fixed favourite album); watch_roots = directories to watch;
    // album_name = breadcrumb label (empty = directory collection).
    std::vector<std::wstring> paths;
    std::vector<ScanRoot> watch_roots;
    std::wstring album_name;
};

struct NavScanResult {
    ImageIndex index;
    std::wstring path;
    bool recursive = false;
    std::uint64_t generation = 0;
    int scan_result = -1;
    bool refresh = false;
    std::vector<ScanRoot> watch_roots;   // rebind the dir watcher
    std::wstring album_name;             // empty = directory collection
};

struct NavTreeJob {
    std::uint64_t node_id = 0;
    std::wstring path;
    std::uint64_t generation = 0;
};

struct NavTreeOutcome {
    std::uint64_t node_id = 0;
    std::uint64_t generation = 0;
    std::vector<NavChildInfo> children;
    int image_count = 0;
    bool ok = false;
    std::wstring error;
};

class App : private DeleteCompositionHost {
public:
    App();
    ~App();

    int run(const std::wstring& initial_path = L"");

    // Recompute DPI-scaled layout members from nominal DIP constants.
    void apply_dpi_layout(float dpi);

private:
    friend class AppComicPort;

    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_command_message(HWND hwnd, WPARAM wp, LPARAM lp);
    LRESULT handle_key_message(HWND hwnd, WPARAM wp, LPARAM lp);
    LRESULT handle_mouse_message(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
    LRESULT handle_paint_message(HWND hwnd);
    LRESULT handle_timer_message(HWND hwnd, WPARAM wp);
    LRESULT handle_async_message(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
    LRESULT handle_window_message(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
    bool    open_image(const std::wstring& path);
    void    show_open_error(OpenInputRoute route);
    void    open_directory(const std::wstring& path);
    void    navigate_to(int idx);
    void    fit_to_window();
    void    zoom_at_center(float factor);
    void    toggle_fullscreen(HWND hwnd);
    void    start_transition(HWND hwnd, bool forward, int request_index = -1);
    void    begin_animation(HWND hwnd);
    bool    enter_grid_image(HWND hwnd, const GridEntryRequest& request);
    void    toggle_recursive();
    void    render_frame();
    bool    synchronize_renderer_generation();
    void    update_title();

    HWND    delete_owner_window() const override;
    DeleteCompositionState capture_delete_state() const override;
    void    remove_delete_indices(const std::vector<int>& indices) override;
    bool    open_delete_successor(const std::wstring& path, int index) override;
    void    set_delete_current_identity(
                const std::wstring& path, int index, bool has_image) override;
    void    set_delete_grid_state(
                bool grid_mode, int grid_selection,
                const std::vector<bool>& selected,
                int selection_anchor) override;
    void    reset_delete_current_bitmap() override;
    void    stop_delete_loader() override;
    void    start_delete_loader() override;
    void    rebuild_delete_thumbnails() override;
    void    clear_delete_thumbnails() override;
    void    reset_delete_grid_cache() override;
    void    ensure_delete_grid_visible() override;
    void    update_delete_title() override;
    void    invalidate_delete_view() override;
    void    open_in_explorer();
    void    show_toolbar_menu(HWND hwnd, int idx, int x, int y);
    std::vector<std::wstring> selected_paths() const;
    std::wstring primary_path() const;
    void    copy_image_data();
    void    copy_file_paths();
    void    create_file_copies();
    void    show_context_menu(HWND hwnd, int x, int y);

    // Async decode pool (Issue #5-P1 lag fix): 3 workers decode the current
    // big image (front of queue, generation-guarded) plus neighbor preloads.
    void    start_async_pool();
    void    stop_async_pool();
    void    start_async_worker(int slot);
    bool    async_slots_active();
    void    request_preload(const std::wstring& path);
    Microsoft::WRL::ComPtr<IWICBitmapSource> get_preloaded(const std::wstring& path);
    void    preload_neighbors();
    void    show_placeholder_thumb(int idx);

    // Continuous comic layout inside large-image mode
    void    toggle_comic_reader();
    bool    leave_comic_reader(bool load_visible_page);
    void    rebuild_comic_pages();
    void    clear_comic_cache();
    void    update_comic_viewport();
    void    adjust_comic_width(float delta);
    void    sync_comic_current();
    ComicControlsRenderInput comic_controls_snapshot() const;
    bool    dispatch_comic_command(ComicAppCommand command);
    bool    start_comic_middle(
                float anchor_x, float anchor_y,
                float pointer_x, float pointer_y, bool anchor_visible);
    void    cancel_comic_auto_scroll(ComicAppCancelTrigger trigger);
    void    reset_comic_controls(ComicAppCancelTrigger trigger);
    bool    start_comic_timer();
    void    stop_comic_timer();
    void    handle_comic_timer(HWND hwnd);
    void    clear_comic_transient();
    void    revalidate_comic_middle_anchor();
    void    finish_comic_scrollbar_drag();
    void    request_comic_pages();
    void    apply_comic_results();
    void    trim_comic_cache();
    void    render_comic_reader(float content_top);

    // Grid mode
    void    toggle_grid();
    void    set_sort_mode(SortMode mode);
    void    toggle_thumb_square();
    bool    toggle_grid_labels();
    void    toggle_info();
    bool    toolbar_visible() const;
    int     visible_panel_width() const;
    int     nav_panel_width() const;
    bool    nav_panel_visible() const;
    void    update_content_viewport(bool refit);
    void    update_panel_data(const std::wstring& path);
    void    start_metadata_loader();
    void    stop_metadata_loader();
    bool    request_metadata(const std::wstring& path);
    void    apply_metadata_result();
    void    apply_metadata(const ImageMeta& meta);
    void    draw_panel(const std::wstring& path, ID2D1Bitmap1* preview,
                uint32_t preview_w, uint32_t preview_h, float top,
                int fallback_count = -1);
    bool    has_selection() const;
    void    clear_selection();
    void    select_range(int start, int end);
    int     grid_hit_test(int x, int y) const;
    bool    grid_click(int x, int y, bool shift, bool ctrl);
    void    grid_navigate(int dir, bool shift);
    void    grid_ensure_visible();
    void    clamp_grid_scroll();
    std::optional<GridTransitionRect> grid_transition_source_rect(int index) const;
    bool    capture_grid_transition_source(int index);
    void    rebuild_grid_layout(int grid_area_width, GridRebuildReason reason);
    void    grid_render();
    void    handle_scrollbar_click(HWND hwnd, int mx, int my);
    void    select_item(int idx, bool shift, bool ctrl);
    void    begin_grid_scroll(HWND hwnd);
    void    finish_grid_scroll();
    void    start_dim_preload();
    void    start_thumb_loader();
    void    stop_thumb_loader();
    bool    thumb_loader_running() const noexcept;
    void    request_thumb(int idx);
    void    trim_thumb_cache(int visible_start, int visible_end);

    // Left navigation panel + breadcrumbs (Issue #5 P2)
    void    toggle_nav_panel();
    void    cycle_nav_focus();
    bool    handle_nav_panel_key(HWND hwnd, WPARAM wp, bool ctrl, bool shift);
    void    switch_collection(const std::wstring& path, bool recursive);
    void    apply_nav_scan_result();
    void    start_nav_workers();
    void    stop_nav_workers();
    void    request_nav_tree_expand(std::uint64_t node_id);
    void    apply_nav_tree_result();
    void    sync_nav_collection();
    void    ensure_nav_root();
    void    render_nav_panel(float x, float y, float w, float h);
    void    render_grid_breadcrumb();
    void    rebuild_nav_breadcrumbs();
    bool    nav_panel_hit_test(int x, int y);
    bool    grid_breadcrumb_hit_test(int x, int y);
    void    nav_panel_mouse_move(int x, int y);
    void    nav_tree_scroll(float delta);
    void    nav_ensure_focus_visible();
    void    reveal_active_collection();

    // Filmstrip (large-image bottom strip, Issue #5 P1)
    bool    filmstrip_showable() const;
    bool    filmstrip_visible() const;
    D2D1_RECT_F filmstrip_rect() const;
    int     filmstrip_hit_test(int x, int y) const;
    void    render_filmstrip();
    void    schedule_filmstrip_hide();
    void    cancel_filmstrip_hide();
    void    note_filmstrip_interaction();
    void    flush_pending_image_before_exit();
    float   filmstrip_reveal_progress() const;
    void    reveal_filmstrip();
    void    hide_filmstrip_animated();
    void    reset_filmstrip_reveal();
    void    update_filmstrip_reveal();

    int m_thumb_size = layout::kThumbSizeDip;  // decode resolution (WIC)
    int m_thumb_cell  = layout::kThumbCellDip;  // display cell size for column calc
    float m_thumb_zoom = 1.0f; // Ctrl+wheel zoom factor
    int m_thumb_gap_h = layout::kThumbGapHDip;  // horizontal gap
    int m_thumb_gap_v = layout::kThumbGapVDip;  // vertical gap
    int m_thumb_pad  = layout::kThumbPadDip;    // uniform padding (all sides)

    Window     m_window;
    Renderer   m_renderer;
    Decoder    m_decoder;
    ImageIndex m_index;
    std::unique_ptr<DeleteComposition> m_delete_composition;

    std::wstring m_current_path;
    std::wstring m_uploaded_path;  // 当前已上传到 GPU 的图片路径
    Microsoft::WRL::ComPtr<IWICBitmapSource> m_current_wic;
    int          m_current_idx = -1;
    bool         m_has_image = false;
    bool         m_fullscreen = false;
    bool         m_recursive = false;
    std::wstring m_open_error;

    WINDOWPLACEMENT m_saved_placement = {sizeof(WINDOWPLACEMENT)};
    LONG            m_saved_style = 0;
    LONG            m_saved_exstyle = 0;

    bool  m_drag_pending = false;
    int   m_drag_start_x = 0;
    int   m_drag_start_y = 0;
    std::vector<std::wstring> m_drag_paths;
    int   m_drag_deferred_select = -1;

    // Real-time collection refresh (Issue #5 P3): watches the active
    // collection's roots and rescans in place on changes.
    DirWatcher m_dir_watcher;
    std::vector<ScanRoot> m_watch_roots;
    void    start_dir_watch();
    void    stop_dir_watch();
    void    request_collection_refresh();
    void    apply_collection_refresh(NavScanResult&& result);

    // Album & favourite collections (Issue #5 P3)
    AlbumStore m_album_store;
    bool       m_album_loaded = false;
    int        m_album_sel = -1;      // selected album index (-1 = none)
    bool       m_fav_selected = false; // fixed favourite album active
    int        m_album_row_hover = -1;
    std::vector<AlbumPanelRow> m_album_rows;
    struct AlbumMenuTarget {
        int album = -1;
        int folder = -1;
        bool folder_row = false;
    } m_album_menu_target;
    void    build_album_rows(std::vector<AlbumPanelRow>& rows);
    // Folder-icon grid (Issue #5 P3c): per-folder sample paths + async
    // tile decode into a small D2D cache.
    std::unordered_map<std::wstring, std::vector<std::wstring>>
        m_folder_samples;
    std::unordered_map<std::wstring,
        std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap>>> m_folder_icon_cache;
    std::vector<FolderIconTiles> m_folder_icon_tiles;  // parallel to rows
    struct FolderIconJob {
        std::wstring folder;
        std::wstring path;
        int tile = 0;
    };
    std::mutex m_folder_icon_mutex;
    std::condition_variable m_folder_icon_cv;
    std::deque<FolderIconJob> m_folder_icon_queue;
    std::unordered_set<std::wstring> m_folder_icon_inflight;
    bool m_folder_icon_stop = false;
    std::thread m_folder_icon_thread;
    void    start_folder_icon_worker();
    void    stop_folder_icon_worker();
    void    rebuild_folder_samples();
    void    enqueue_folder_icons();
    void    handle_folder_icon_ready(LPARAM payload);
    int     album_icon_hit(int x, int y);
    void    toggle_favourite_current();
    bool    primary_is_favourite() const;
    std::wstring m_active_album_name;  // breadcrumb label (empty = dir)
    void    load_album_store();
    void    save_album_store();
    void    open_favourites_collection();
    void    open_album_collection(int index);
    void    toggle_album_folder_view();
    int     album_row_hit(int x, int y);
    void    show_album_row_menu(HWND hwnd, int x, int y);
    void    create_album();
    void    delete_album(int index);
    void    add_folder_to_album(int index);
    void    remove_album_folder(int album, int folder);
    void    move_album_folder(int album, int from, int to);
    void    toggle_album_folder_recursive(int album, int folder);
    std::vector<ScanRoot> album_scan_roots() const;
    std::vector<ScanRoot> album_watch_roots() const;

    struct ComicPageEntry {
        Microsoft::WRL::ComPtr<IWICBitmapSource> wic;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> d2d;
        std::uint32_t source_width = 0;
        std::uint32_t source_height = 0;
        std::uint32_t decoded_width = 0;
        std::uint32_t decoded_height = 0;
        std::size_t estimated_cache_bytes = 0;
        bool failed = false;
    };
    ComicReaderModel m_comic_reader;
    ComicReaderLoader m_comic_loader;
    ComicLruBudget m_comic_lru;
    std::vector<ComicPageEntry> m_comic_pages;
    std::uint64_t m_comic_generation = 1;
    int m_comic_fallback_index = -1;
    UINT_PTR m_comic_timer = 0;
    ULONGLONG m_comic_last_tick_ms = 0;
    ULONGLONG m_comic_transient_until_ms = 0;
    ComicTransientOverlayKind m_comic_transient_kind =
        ComicTransientOverlayKind::None;
    std::wstring m_comic_current_filename;
    std::wstring m_comic_page_badge;
    std::wstring m_comic_transient_text;
    int m_comic_cached_page_index = -1;
    int m_comic_cached_total_pages = 0;
    bool m_comic_scrollbar_dragging = false;
    bool m_comic_scrollbar_hover = false;
    float m_comic_scrollbar_grab_offset_y = 0.0f;
    float m_comic_autoscroll_anchor_x = 0.0f;
    float m_comic_autoscroll_anchor_y = 0.0f;
    float m_comic_autoscroll_pointer_x = 0.0f;
    float m_comic_autoscroll_pointer_y = 0.0f;
    // Render-driven autoscroll advance (QPC): WM_TIMER pacing caused
    // visible judder on cruise and middle-button scrolling.
    LARGE_INTEGER m_comic_advance_last = {};
    // Device-loss recovery: throttled recreate + nested-paint guard.
    bool m_render_busy = false;
    UINT_PTR m_render_retry_timer = 0;
    // Explicit page-width drag slider (mouse-direct, no modifier keys).
    bool m_comic_width_dragging = false;
    void advance_comic_autoscroll_render();
    void set_comic_width_factor_direct(float factor);
    void apply_comic_width_slider_x(
        const ComicControlsLayout& controls, float x);

    // Metadata extraction runs off the UI/render thread.
    std::thread m_metadata_thread;
    // Async decode pool: a fixed set of 3 workers decodes both the current
    // big image (front-of-queue, generation-guarded so stale page flips are
    // dropped) and neighbor preloads. Page-flip storms no longer serialize
    // behind one 300-500ms 4K decode.
    struct AsyncJob {
        std::wstring path;
        ULONGLONG    gen     = 0;
        bool         current = false;  // current-image decode vs preload
        // Identity snapshot from when the current decode was queued: if the
        // decode fails, WM_IMAGE_READY rolls back to these so delete/copy
        // never act on an image that is not actually displayed.
        std::wstring prev_path;
        int          prev_idx = -1;
    };
    struct AsyncSlot {
        std::thread thread;
        ULONGLONG   slot_gen   = 0;   // bumped when the slot is respawned
        bool        busy       = false;
        std::wstring path;            // in-flight job (dedup / diagnostics)
        ULONGLONG   started_ms = 0;
        bool        current    = false;
    };
    // Pool state is heap-shared: each worker holds its own shared_ptr copy,
    // so a worker still stuck in decode() at shutdown (detached) keeps the
    // state alive instead of touching freed App members — app exit never
    // blocks on a frozen decode.
    struct AsyncPoolState {
        std::mutex              mutex;
        std::condition_variable cv;
        std::deque<AsyncJob>    queue;
        std::vector<AsyncSlot>  slots;
        bool        stop = false;
        ULONGLONG   gen  = 0;   // newest current-image generation
        ComPtr<IWICBitmapSource> wic;  // newest decode result
        // Rollback identity of the newest current-image attempt (see AsyncJob).
        std::wstring current_prev_path;
        int          current_prev_idx = -1;
        // Materialized-neighbor cache: workers do the full decode, so a
        // cache hit needs only a GPU upload. Item cap matches the 3+3
        // preload window; the byte budget keeps the 512MiB soft cap intact.
        std::mutex preload_mutex;
        std::unordered_map<std::wstring,
            Microsoft::WRL::ComPtr<IWICBitmapSource>> preload_cache;
        std::deque<std::wstring> preload_order;  // LRU eviction order
        std::uint64_t preload_bytes = 0;         // total materialized bytes
        static constexpr int kPreloadCacheMaxItems = 6;
        // ~4 fully-materialized 5120x3840 items (79MB each). The 512MiB soft
        // budget predates materialized caching; the cache is fixed-size and
        // LRU-evicted (no growth with scrolling) — one constant to dial.
        static constexpr std::uint64_t kPreloadCacheBytes =
            350ULL * 1024ULL * 1024ULL;
    };
    static constexpr int kAsyncWorkers = 3;
    std::shared_ptr<AsyncPoolState> m_async;
    // Watchdog: a decode that never returns (stuck on a pathological file)
    // abandons its slot — the zombie thread is detached (its stale result is
    // dropped via slot generation) and the slot respawns — self-heal ~10s.
    // These two are UI-thread-only bookkeeping for the current image.
    bool       m_async_busy = false;      // current decode in flight
    ULONGLONG m_async_started_ms = 0;
    static constexpr ULONGLONG kAsyncTimeoutMs = 10000;
    void check_async_timeout();
    void advance_transition_animation();
    // An already-materialized image deferred while the filmstrip transition
    // was running (uploaded once the animation completes, so the GPU upload
    // never stalls an animation frame).
    ComPtr<IWICBitmapSource> m_pending_image;
    std::wstring m_pending_path;  // identity check: paged past = stale
    // Progressive-loading placeholder: index whose filmstrip thumbnail is
    // drawn while the full decode is in flight (cleared on upload).
    int m_placeholder_idx = -1;
    std::mutex m_metadata_mutex;
    std::condition_variable m_metadata_cv;
    std::wstring m_metadata_request_path;
    std::wstring m_metadata_result_path;
    ImageMeta m_metadata_result;
    bool m_metadata_request_pending = false;
    bool m_metadata_result_ready = false;
    std::atomic<bool> m_metadata_running{false};

    // Grid state
    bool  m_grid_mode = false;
    int   m_grid_scroll_y = 0;
    int   m_grid_sel = -1;
    int   m_grid_cols = 0;
    int   m_grid_total_rows = 0;
    int   m_last_cached_sel = -1;  // for anim src cache
    std::vector<int> m_row_heights;
    bool  m_thumb_square = false;
    bool  m_show_labels = true;
    bool  m_panel_expanded = false;  // big-image mode: image + filmstrip only
    bool  m_panel_expanded_in_grid = false;  // grid panel state to restore
    bool  m_from_grid = false;  // current image has a grid context → Space/Esc returns
    bool  m_toolbar_revealed = false;
    bool  m_ime_composing = false;

    // Transition animation
    LARGE_INTEGER m_last_anim_tick = {};  // real-time filmstrip animation
    bool m_animating = false;
    float m_anim_t = 0.0f;
    UINT_PTR m_anim_timer = 0;
    ULONGLONG m_anim_start = 0;  // QPC timestamp
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_anim_thumb;
    D2D1_RECT_F m_anim_src = {};
    D2D1_RECT_F m_anim_dst = {};
    bool  m_anim_forward = true;
    float m_anim_iw = 1, m_anim_ih = 1;  // target image size for animation
    // Transition interrupt (Issue #7): a reversed animation draws a
    // snapshot of the interrupted composite frame zooming to the target.
    bool m_anim_reversed = false;
    // Entry transition: the grid frame snapshot that the big-image
    // background covers as its opacity goes 0 -> 100%.
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_anim_grid_snapshot;
    void interrupt_transition(mv::TransitionTrigger trigger, int nav_dir);
    // Draws the transition overlay (background veil + zoom layer) for the
    // current composition; shared by the image and grid render paths so
    // the reversed runs draw identically.
    void draw_transition_overlay();
    void reverse_transition();
    void finish_transition_now();
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> capture_window_frame();

    // Filmstrip state (Issue #5 P1)
    FilmstripModel m_filmstrip;
    bool    m_filmstrip_revealed = false;  // large-image mode: hidden by default
    bool    m_filmstrip_reveal_animating = false;
    bool    m_filmstrip_reveal_forward = false;
    float   m_filmstrip_reveal_raw = 0.0f;  // raw time 0 hidden -> 1 shown
    LARGE_INTEGER m_filmstrip_reveal_start = {};
    UINT_PTR m_filmstrip_timer = 0;        // hide delay (id 8)
    uint64_t m_filmstrip_dimension_generation = 0;

    int   m_panel_width = layout::kPanelWidthDip;
    GridScrollPause m_grid_scroll_pause;
    int   m_toolbar_h = layout::kTitleBarHeightDip;
    std::vector<std::wstring> m_toolbar_items = {L"文件", L"查看", L"编辑", L"帮助"};
    int   m_toolbar_active = -1;
    HWND  m_tooltip = nullptr;
    bool  m_scrollbar_dragging = false;  // scrollbar thumb drag active
    bool  m_scrollbar_hover    = false;  // mouse hovering over scrollbar
    int   m_scrollbar_drag_y = 0;        // mouse y when drag started
    int   m_scrollbar_drag_pos = 0;      // scroll position when drag started
    int   m_grid_total_h = 0;            // cached total grid content height
    std::vector<GridLayoutRow> m_grid_rows;
    std::vector<std::pair<uint32_t, uint32_t>> m_grid_dims;
    std::vector<float> m_grid_item_x;
    std::vector<float> m_grid_item_w;
    int m_grid_layout_width = -1;
    bool m_grid_layout_dirty = true;
    uint64_t m_grid_layout_generation = 0;
    int   m_grid_scroll_saved = 0;
    int   m_grid_saved_idx = 0;
    std::wstring m_panel_path;
    std::vector<std::pair<std::wstring, std::wstring>> m_panel_info;
    std::vector<std::pair<std::wstring, std::wstring>> m_panel_gen;
    std::vector<mv::PanelRegion> m_panel_clickable;
    int m_panel_sel = -1;           // selected clickable index (brief highlight)
    float m_panel_scroll_y = 0;     // side panel scroll offset
    float m_panel_total_h = 0;      // total content height for clamping
    std::wstring m_panel_copied;    // last copied text for toast
    UINT_PTR m_toast_timer = 0;     // toast dismiss timer
    UINT_PTR m_sel_timer = 0;       // selection clear timer

    // Title bar (custom, bypasses system hook on this PC)
    int m_title_h = layout::kTitleBarHeightDip;  // title bar height in DIPs (was 32)
    int m_title_btn_hover = -1;  // 0=min, 1=max, 2=close
    int m_title_btn_press = -1;

    // Multi-select (grid only)
    std::vector<bool> m_selected;
    int  m_sel_anchor = -1;

    // Thumbnail engine: owns the WIC thumb pool/workers/JPEG cache/dimension
    // preload. App keeps only the D2D bitmap cache and GPU uploads; all WIC
    // cache reads go through m_thumb_engine.pool() under the same mutex as
    // the previous m_thumb_pool (engine-owned so stop() can abandon a pool
    // with detached workers and hand App a fresh one).
    ThumbEngine m_thumb_engine;

    // D2D bitmap cache (main-thread only, populated during render)
    std::unordered_map<int, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> m_thumb_d2d;
    std::unordered_map<int, uint64_t> m_thumb_d2d_use;
    uint64_t m_thumb_use_clock = 0;
    uint64_t m_renderer_generation = 0;

    float m_last_thumb_req_scroll = -1.0f;  // for stale-request pruning
    std::wstring m_debounce_path;   // rapid-paging deferred decode target
    int m_debounce_idx = -1;
    ULONGLONG m_last_open_tick = 0; // for rapid-paging detection

    // ── Left navigation panel (Issue #5 P2) ──
    int  m_nav_visible_width = 0;    // DPI-scaled nav panel width (240 DIP)
    int  m_nav_breadcrumb_h = 0;     // DPI-scaled breadcrumb bar height
    int  m_grid_top = 0;             // grid content top = toolbar + breadcrumb bar
    NavPanelState m_nav_panel_state;
    NavSwitchController m_nav_switch;
    CollectionSortMemory m_collection_memory;
    NavTreeModel m_nav_tree;

    // Single path source shared by the panel breadcrumb and the grid
    // breadcrumb (they are two projections of the same active collection).
    std::vector<std::wstring> m_nav_segments;
    std::vector<std::wstring> m_nav_display_segments;  // tail carries [递归]
    NavBreadcrumbLayout m_nav_panel_breadcrumb;  // absolute x (panel space)
    NavBreadcrumbLayout m_nav_grid_breadcrumb;   // absolute x (grid space)
    int m_nav_breadcrumb_hover_panel = -1;
    int m_nav_breadcrumb_hover_grid = -1;
    NavPanelGeometry m_nav_panel_geometry;
    std::vector<NavTreeRow> m_nav_rows;          // visible rows (per frame)
    int m_nav_row_hover = -1;
    std::uint64_t m_nav_highlight_id = 0;        // active collection tree node
    std::uint64_t m_nav_tree_focus_id = 0;       // keyboard focus row
    std::wstring m_nav_synced_key;               // tree already revealed for this dir
    float m_nav_tree_scroll = 0.0f;
    float m_nav_tree_total = 0.0f;

    // Async collection scan (generation-cancelled)
    std::thread m_nav_scan_thread;
    std::mutex m_nav_scan_mutex;
    std::condition_variable m_nav_scan_cv;
    std::atomic<bool> m_nav_scan_running{false};
    bool m_nav_scan_queued = false;
    NavScanJob m_nav_scan_job;
    bool m_nav_scan_ready = false;
    NavScanResult m_nav_scan_result;

    // Async directory-tree enumeration (lazy expand)
    std::thread m_nav_tree_thread;
    std::mutex m_nav_tree_mutex;
    std::condition_variable m_nav_tree_cv;
    std::atomic<bool> m_nav_tree_running{false};
    bool m_nav_tree_queued = false;
    NavTreeJob m_nav_tree_job;
    bool m_nav_tree_outcome_ready = false;
    NavTreeOutcome m_nav_tree_outcome;
};

} // namespace mv

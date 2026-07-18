#include "app.h"
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <ole2.h>

namespace mv {

// ── OLE Drag source helpers ──────────────────────────────────

// Simple IDataObject that holds a single file path as CF_HDROP
class FileDataObject : public IDataObject {
public:
    FileDataObject(const std::wstring& path) : m_ref(1) {
        // Build DROPFILES + path
        int offset = sizeof(DROPFILES);
        int path_bytes = static_cast<int>((path.size() + 1) * sizeof(wchar_t));
        m_data_size = offset + path_bytes;
        m_data = GlobalAlloc(GMEM_MOVEABLE, m_data_size);
        if (m_data) {
            auto* df = static_cast<DROPFILES*>(GlobalLock(m_data));
            df->pFiles = offset;
            df->fWide  = TRUE;
            auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + offset);
            wcscpy_s(dst, path.size() + 1, path.c_str());
            GlobalUnlock(m_data);
        }
    }
    ~FileDataObject() { if (m_data) GlobalFree(m_data); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }

    // IDataObject
    STDMETHODIMP GetData(FORMATETC* fe, STGMEDIUM* sm) override {
        if (!fe || !sm) return E_INVALIDARG;
        if (fe->cfFormat != CF_HDROP || !(fe->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
        sm->tymed = TYMED_HGLOBAL;
        sm->hGlobal = GlobalAlloc(GMEM_MOVEABLE, m_data_size);
        if (!sm->hGlobal) return E_OUTOFMEMORY;
        void* src = GlobalLock(m_data);
        void* dst = GlobalLock(sm->hGlobal);
        memcpy(dst, src, m_data_size);
        GlobalUnlock(sm->hGlobal);
        GlobalUnlock(m_data);
        sm->pUnkForRelease = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return DV_E_FORMATETC; }
    STDMETHODIMP QueryGetData(FORMATETC* fe) override {
        if (!fe) return E_INVALIDARG;
        return (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) ? S_OK : DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return DV_E_FORMATETC; }
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return OLE_S_USEREG; }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ULONG   m_ref;
    HGLOBAL m_data = nullptr;
    SIZE_T  m_data_size = 0;
};

// Minimal IDropSource
class SimpleDropSource : public IDropSource {
public:
    SimpleDropSource() : m_ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ULONG m_ref;
};

// ── Preloader worker (runs on background thread) ─────────────

static void preload_worker(
    std::atomic<bool>& running,
    std::mutex& mtx,
    std::condition_variable& cv,
    std::vector<std::wstring>& queue,
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IWICBitmapSource>>& cache)
{
    // COM must be initialized on every thread that uses WIC
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Decoder decoder;  // each thread gets its own WIC factory
    while (running) {
        std::wstring path;
        {
            std::unique_lock lock(mtx);
            cv.wait(lock, [&] { return !running || !queue.empty(); });
            if (!running) return;
            if (queue.empty()) continue;
            path = std::move(queue.back());
            queue.pop_back();
        }

        try {
            auto bitmap = decoder.decode(path);
            std::lock_guard lock(mtx);
            // Simple LRU: keep at most 6 entries
            if (cache.size() >= 6) cache.clear();
            cache[std::move(path)] = bitmap;
        } catch (...) {
            // decode failed — skip
        }
    }
    CoUninitialize();
}

// ── App lifecycle ────────────────────────────────────────────

App::App()  = default;
App::~App() { stop_preloader(); stop_thumb_loader(); }

int App::run(const std::wstring& initial_path) {
    if (!m_window.create(L"MinView", 1200, 800))
        throw std::runtime_error("Failed to create window");

    m_window.set_message_callback(
        [this](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            return handle_message(hwnd, msg, wp, lp);
        });

    if (!m_renderer.init(m_window.handle()))
        throw std::runtime_error("Failed to init Direct2D renderer");

    start_preloader();

    if (!initial_path.empty()) open_image(initial_path);

    int ret = m_window.run();
    stop_preloader();
    return ret;
}

// ── Message handler ──────────────────────────────────────────

LRESULT App::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_SIZE: {
        uint32_t w = LOWORD(lp), h = HIWORD(lp);
        if (w > 0 && h > 0) m_renderer.resize(w, h);
        return 0;
    }
    case WM_PAINT:
        render_frame();
        ValidateRect(hwnd, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 0;

    case WM_CONTEXTMENU:
        show_context_menu(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSEWHEEL: {
        if (m_grid_mode) {
            float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            m_grid_scroll_y -= static_cast<int>(delta * 60);
            if (m_grid_scroll_y < 0) m_grid_scroll_y = 0;
            m_window.invalidate();
            return 0;
        }
        if (!m_has_image) return 0;
        float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        float factor = (delta > 0) ? 1.15f : 1.0f / 1.15f;

        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);

        uint32_t iw, ih; m_renderer.image_size(iw, ih);
        if (iw == 0 || ih == 0) return 0;

        float old_scale = m_renderer.scale();
        float new_scale = std::clamp(old_scale * factor, 0.01f, 100.0f);
        if (new_scale == old_scale) return 0;

        D2D1_SIZE_U ts = m_renderer.target_size();
        float img_x = (ts.width  - iw * old_scale) / 2.0f;
        float img_y = (ts.height - ih * old_scale) / 2.0f;
        float img_cx = (pt.x - img_x) / old_scale;
        float img_cy = (pt.y - img_y) / old_scale;
        float new_img_x = pt.x - img_cx * new_scale;
        float new_img_y = pt.y - img_cy * new_scale;

        m_renderer.set_scale(new_scale);
        m_renderer.set_offset(
            new_img_x - (ts.width  - iw * new_scale) / 2.0f,
            new_img_y - (ts.height - ih * new_scale) / 2.0f);
        m_window.invalidate();
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (m_grid_mode) {
            grid_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        if (!m_has_image) return -1;
        m_drag_pending = true;
        m_dragging = false;
        m_drag_start_x = GET_X_LPARAM(lp);
        m_drag_start_y = GET_Y_LPARAM(lp);
        m_drag_offset_x = m_renderer.offset_x();
        m_drag_offset_y = m_renderer.offset_y();
        SetCapture(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        if (m_drag_pending) {
            int dx = GET_X_LPARAM(lp) - m_drag_start_x;
            int dy = GET_Y_LPARAM(lp) - m_drag_start_y;
            if (dx*dx + dy*dy >= 16) {  // 4px threshold
                m_drag_pending = false;
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (ctrl && !m_current_path.empty()) {
                    // OLE drag export
                    ReleaseCapture();
                    FileDataObject* data = new FileDataObject(m_current_path);
                    SimpleDropSource* src = new SimpleDropSource();
                    DWORD effect;
                    DoDragDrop(data, src, DROPEFFECT_COPY, &effect);
                    data->Release();
                    src->Release();
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                    return 0;
                }
                // Start pan
                m_dragging = true;
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            }
            return 0;
        }
        if (!m_dragging) return -1;
        m_renderer.set_offset(
            m_drag_offset_x + GET_X_LPARAM(lp) - m_drag_start_x,
            m_drag_offset_y + GET_Y_LPARAM(lp) - m_drag_start_y);
        m_window.invalidate();
        return 0;

    case WM_LBUTTONUP:
        m_drag_pending = false;
        if (!m_dragging) return -1;
        m_dragging = false;
        ReleaseCapture();
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return 0;

    case WM_LBUTTONDBLCLK:
        if (m_grid_mode) {
            if (m_grid_sel >= 0 && m_grid_sel < static_cast<int>(m_index.size())) {
                toggle_grid();  // exit grid
                navigate_to(m_grid_sel);
            }
            return 0;
        }
        fit_to_window();
        m_window.invalidate();
        return 0;

    case WM_KEYDOWN: {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;

        if (ctrl) {
            switch (wp) {
            case '0': case VK_NUMPAD0: fit_to_window(); m_window.invalidate(); return 0;
            case VK_OEM_PLUS: case VK_ADD:   zoom_at_center(1.25f); return 0;
            case VK_OEM_MINUS: case VK_SUBTRACT: zoom_at_center(1.0f/1.25f); return 0;
            case 'C': copy_to_clipboard(); return 0;
            case 'R': toggle_recursive(); return 0;
            }
            return -1;
        }

        switch (wp) {
        case VK_ESCAPE:
            if (m_grid_mode) { toggle_grid(); return 0; }
            if (m_fullscreen) { toggle_fullscreen(hwnd); return 0; }
            DestroyWindow(hwnd); return 0;
        case VK_F11:   toggle_fullscreen(hwnd); return 0;
        case 'G':
            if (!m_index.empty()) toggle_grid();
            return 0;
        case 'N':
            if (!ctrl) { set_sort_mode(SortMode::Name); return 0; }
            return -1;
        case 'D':
            if (!ctrl) { set_sort_mode(SortMode::Date); return 0; }
            return -1;
        case 'S':
            if (!ctrl) { set_sort_mode(SortMode::Size); return 0; }
            return -1;
        case 'R':
            if (!ctrl) { set_sort_mode(SortMode::Random); return 0; }
            return -1;
        case VK_LEFT:
            if (m_grid_mode) { grid_navigate(-1); return 0; }
            navigate_to(m_current_idx - 1); return 0;
        case VK_RIGHT:
            if (m_grid_mode) { grid_navigate(1); return 0; }
            navigate_to(m_current_idx + 1); return 0;
        case VK_UP:
            if (m_grid_mode) { grid_navigate(-m_grid_cols); return 0; }
            return -1;
        case VK_DOWN:
            if (m_grid_mode) { grid_navigate(m_grid_cols); return 0; }
            return -1;
        case VK_HOME:
            if (m_grid_mode) { m_grid_sel = 0; grid_ensure_visible(); m_window.invalidate(); return 0; }
            navigate_to(0); return 0;
        case VK_END:
            if (m_grid_mode) { m_grid_sel = static_cast<int>(m_index.size()) - 1; grid_ensure_visible(); m_window.invalidate(); return 0; }
            navigate_to(static_cast<int>(m_index.size()) - 1); return 0;
        case VK_SPACE: navigate_to(m_current_idx + 1); return 0;
        case VK_BACK:  navigate_to(m_current_idx - 1); return 0;
        case VK_RETURN:
            if (m_grid_mode && m_grid_sel >= 0) { toggle_grid(); navigate_to(m_grid_sel); return 0; }
            return -1;
        case VK_DELETE: delete_current_file(shift); return 0;
        }
        break;
    }

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0) open_image(path);
        DragFinish(drop);
        return 0;
    }

    case WM_COPYDATA: {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (cds && cds->dwData == 1 && cds->lpData) {
            open_image(static_cast<const wchar_t*>(cds->lpData));
        }
        return 0;
    }
    }
    return -1;
}

// ── Image loading ────────────────────────────────────────────

void App::open_image(const std::wstring& path) {
    try {
        // Try preload cache first
        auto cached = get_preloaded(path);
        if (cached) {
            m_renderer.upload_image(cached.Get());
        } else {
            auto bitmap = m_decoder.decode(path);
            m_renderer.upload_image(bitmap.Get());
        }
        m_current_path = path;
        m_has_image = true;

        namespace fs = std::filesystem;
        fs::path p(path);
        std::wstring dir = p.parent_path().wstring();
        if (dir.empty()) dir = L".";

        if (m_index.directory() != dir) {
            m_index.scan(dir, m_recursive);
        }
        m_current_idx = m_index.index_of(path);

        update_title();

        // Preload neighbors in background
        preload_neighbors();

        m_window.invalidate();
    } catch (const std::exception&) {
        SetWindowTextW(m_window.handle(), L"Error loading image");
    }
}

void App::update_title() {
    uint32_t iw, ih; m_renderer.image_size(iw, ih);

    // Show relative path in recursive mode, filename only otherwise
    std::wstring display_name;
    if (m_recursive && m_current_idx >= 0) {
        display_name = m_index.relpath_at(m_current_idx);
    } else {
        size_t pos = m_current_path.find_last_of(L"\\/");
        display_name = (pos != std::wstring::npos)
            ? m_current_path.substr(pos + 1) : m_current_path;
    }

    std::wstring title = std::to_wstring(iw) + L"x" + std::to_wstring(ih) +
        L" [" + std::to_wstring(m_current_idx + 1) + L"/" +
        std::to_wstring(m_index.size()) + L"] " + display_name;
    SetWindowTextW(m_window.handle(), title.c_str());
}

void App::navigate_to(int idx) {
    if (m_index.empty()) return;
    if (idx < 0 || idx >= static_cast<int>(m_index.size())) return;
    const auto& path = m_index.path_at(idx);
    if (path.empty()) return;
    open_image(path);
}

// ── Recursive browse ─────────────────────────────────────────

void App::toggle_recursive() {
    m_recursive = !m_recursive;

    std::wstring dir = m_index.directory();
    if (dir.empty() && !m_current_path.empty()) {
        namespace fs = std::filesystem;
        dir = fs::path(m_current_path).parent_path().wstring();
    }
    if (dir.empty()) return;

    m_index.scan(dir, m_recursive);

    // Re-locate current image in new index
    if (!m_current_path.empty()) {
        m_current_idx = m_index.index_of(m_current_path);
    }
    update_title();
    m_window.invalidate();
}

// ── Sort mode ────────────────────────────────────────────────

void App::set_sort_mode(SortMode mode) {
    if (m_index.empty()) return;
    std::wstring current = m_current_path;
    m_index.sort_by(mode);

    // Re-locate current image
    if (!current.empty()) {
        m_current_idx = m_index.index_of(current);
    }

    // Reset grid if in grid mode (thumbnails need reload)
    if (m_grid_mode) {
        stop_thumb_loader();
        m_thumbs.clear();
        m_thumb_d2d.clear();
        m_grid_sel = m_current_idx >= 0 ? m_current_idx : 0;
        start_thumb_loader();
        grid_ensure_visible();
    }

    update_title();
    m_window.invalidate();
}

// ── Preloader ────────────────────────────────────────────────

void App::start_preloader() {
    m_preload_running = true;
    try {
        m_preload_thread = std::thread(preload_worker,
            std::ref(m_preload_running),
            std::ref(m_preload_mutex),
            std::ref(m_preload_cv),
            std::ref(m_preload_queue),
            std::ref(m_preload_cache));
    } catch (const std::exception&) {
        m_preload_running = false;
        // Preloader failed — app still works, just no preloading
    }
}

void App::stop_preloader() {
    m_preload_running = false;
    m_preload_cv.notify_all();
    if (m_preload_thread.joinable())
        m_preload_thread.join();
}

void App::request_preload(const std::wstring& path) {
    if (path.empty()) return;
    {
        std::lock_guard lock(m_preload_mutex);
        // Don't re-queue if already cached or queued
        if (m_preload_cache.count(path)) return;
        for (auto& q : m_preload_queue)
            if (q == path) return;
        m_preload_queue.push_back(path);
    }
    m_preload_cv.notify_one();
}

Microsoft::WRL::ComPtr<IWICBitmapSource> App::get_preloaded(const std::wstring& path) {
    std::lock_guard lock(m_preload_mutex);
    auto it = m_preload_cache.find(path);
    if (it != m_preload_cache.end()) {
        auto result = it->second;
        m_preload_cache.erase(it);  // consumed
        return result;
    }
    return nullptr;
}

void App::preload_neighbors() {
    // Preload up to 2 ahead, 1 behind
    int total = static_cast<int>(m_index.size());
    for (int offset : {1, 2, -1}) {
        int idx = m_current_idx + offset;
        if (idx >= 0 && idx < total)
            request_preload(m_index.path_at(idx));
    }
}

// ── Delete ───────────────────────────────────────────────────

void App::delete_current_file(bool permanent) {
    if (!m_has_image || m_current_path.empty()) return;

    std::wstring from = m_current_path;
    from.push_back(L'\0'); from.push_back(L'\0');

    SHFILEOPSTRUCTW fos = {};
    fos.wFunc  = FO_DELETE;
    fos.pFrom  = from.c_str();
    fos.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (!permanent) fos.fFlags |= FOF_ALLOWUNDO;

    if (SHFileOperationW(&fos) != 0) return;

    m_index.remove(m_current_idx);

    if (m_index.empty()) {
        m_has_image = false;
        m_current_path.clear();
        m_current_idx = -1;
        SetWindowTextW(m_window.handle(), L"MinView");
        m_window.invalidate();
        return;
    }

    if (m_current_idx >= static_cast<int>(m_index.size()))
        m_current_idx = static_cast<int>(m_index.size()) - 1;

    open_image(m_index.path_at(m_current_idx));
}

// ── Context menu ─────────────────────────────────────────────

void App::show_context_menu(HWND hwnd, int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (m_has_image) {
        AppendMenuW(menu, MF_STRING, 1, L"Open in Explorer");
        AppendMenuW(menu, MF_STRING, 2, L"Copy Image\tCtrl+C");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        // Sort submenu
        HMENU sort_menu = CreatePopupMenu();
        auto sm = m_index.sort_mode();
        AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Name   ? MF_CHECKED : 0), 10, L"By Name\tN");
        AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Date   ? MF_CHECKED : 0), 11, L"By Date\tD");
        AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Size   ? MF_CHECKED : 0), 12, L"By Size\tS");
        AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Random ? MF_CHECKED : 0), 13, L"Random Shuffle\tR");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sort_menu), L"Sort By");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"Delete\tDel");
        AppendMenuW(menu, MF_STRING, 4, L"Delete Permanently\tShift+Del");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        UINT flags = MF_STRING;
        if (m_recursive) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, 6, L"Recursive Browse\tCtrl+R");
    } else {
        AppendMenuW(menu, MF_STRING, 5, L"Open File...");
    }

    if (x == -1 && y == -1) {
        RECT rc; GetClientRect(hwnd, &rc);
        POINT pt = {rc.right / 2, rc.bottom / 2};
        ClientToScreen(hwnd, &pt);
        x = pt.x; y = pt.y;
    }

    int cmd = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        x, y, 0, hwnd, nullptr);

    switch (cmd) {
    case 1: open_in_explorer();         break;
    case 2: copy_to_clipboard();        break;
    case 3: delete_current_file(false); break;
    case 4: delete_current_file(true);  break;
    case 5: {
        // Open File dialog
        OPENFILENAMEW ofn = {};
        wchar_t file[MAX_PATH] = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tiff;*.tif\0All Files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile  = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            open_image(file);
        }
        break;
    }
    case 6: toggle_recursive(); break;
    case 10: set_sort_mode(SortMode::Name);   break;
    case 11: set_sort_mode(SortMode::Date);   break;
    case 12: set_sort_mode(SortMode::Size);   break;
    case 13: set_sort_mode(SortMode::Random); break;
    }

    DestroyMenu(menu);
}

void App::open_in_explorer() {
    if (m_current_path.empty()) return;
    std::wstring args = L"/select," + m_current_path;
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOW);
}

void App::copy_to_clipboard() {
    if (m_current_path.empty()) return;
    if (!OpenClipboard(m_window.handle())) return;
    EmptyClipboard();

    int offset = sizeof(DROPFILES);
    int path_bytes = static_cast<int>((m_current_path.size() + 1) * sizeof(wchar_t));
    int total = offset + path_bytes;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!hMem) { CloseClipboard(); return; }

    auto* df = static_cast<DROPFILES*>(GlobalLock(hMem));
    df->pFiles = offset;
    df->fWide  = TRUE;
    auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + offset);
    wcscpy_s(dst, m_current_path.size() + 1, m_current_path.c_str());
    GlobalUnlock(hMem);

    SetClipboardData(CF_HDROP, hMem);
    CloseClipboard();
}

// ── Fullscreen ───────────────────────────────────────────────

void App::toggle_fullscreen(HWND hwnd) {
    m_fullscreen = !m_fullscreen;

    if (m_fullscreen) {
        GetWindowPlacement(hwnd, &m_saved_placement);
        m_saved_style   = GetWindowLongW(hwnd, GWL_STYLE);
        m_saved_exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);

        LONG ns = m_saved_style & ~(WS_CAPTION | WS_THICKFRAME |
            WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        LONG ne = m_saved_exstyle & ~(WS_EX_DLGMODALFRAME |
            WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

        SetWindowLongW(hwnd, GWL_STYLE,   ns);
        SetWindowLongW(hwnd, GWL_EXSTYLE, ne);

        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(MONITORINFO)};
        GetMonitorInfoW(mon, &mi);

        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right  - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE,   m_saved_style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, m_saved_exstyle);
        SetWindowPlacement(hwnd, &m_saved_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

// ── Zoom ─────────────────────────────────────────────────────

void App::fit_to_window() {
    uint32_t iw, ih; m_renderer.image_size(iw, ih);
    if (iw == 0 || ih == 0) return;
    D2D1_SIZE_U ts = m_renderer.target_size();
    m_renderer.set_scale(std::min(
        static_cast<float>(ts.width)  / iw,
        static_cast<float>(ts.height) / ih));
    m_renderer.set_offset(0, 0);
}

void App::zoom_at_center(float factor) {
    uint32_t iw, ih; m_renderer.image_size(iw, ih);
    if (iw == 0 || ih == 0) return;
    float old_scale = m_renderer.scale();
    float new_scale = std::clamp(old_scale * factor, 0.01f, 100.0f);
    if (new_scale == old_scale) return;

    D2D1_SIZE_U ts = m_renderer.target_size();
    float cx = ts.width / 2.0f, cy = ts.height / 2.0f;
    float img_x = (ts.width  - iw * old_scale) / 2.0f;
    float img_y = (ts.height - ih * old_scale) / 2.0f;
    float img_cx = (cx - img_x) / old_scale;
    float img_cy = (cy - img_y) / old_scale;

    m_renderer.set_scale(new_scale);
    m_renderer.set_offset(
        (cx - img_cx * new_scale) - (ts.width  - iw * new_scale) / 2.0f,
        (cy - img_cy * new_scale) - (ts.height - ih * new_scale) / 2.0f);
    m_window.invalidate();
}

// ── Grid mode ────────────────────────────────────────────────

static void thumb_loader_worker(
    std::atomic<bool>& running,
    std::mutex& mtx,
    std::condition_variable& cv,
    std::vector<int>& queue,
    std::vector<App::ThumbEntry>& thumbs,
    ImageIndex& index)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Decoder decoder;
    while (running) {
        int idx;
        {
            std::unique_lock lock(mtx);
            cv.wait(lock, [&] { return !running || !queue.empty(); });
            if (!running) break;
            if (queue.empty()) continue;
            idx = queue.back();
            queue.pop_back();
        }
        if (idx < 0 || idx >= static_cast<int>(thumbs.size())) continue;
        if (thumbs[idx].loaded) continue;

        try {
            auto wic = decoder.decode_scaled(index.path_at(idx), 320);
            std::lock_guard lock(mtx);
            thumbs[idx].wic = wic;
            thumbs[idx].loaded = true;
        } catch (...) {
            std::lock_guard lock(mtx);
            thumbs[idx].loaded = true;  // mark done even on failure
        }
    }
    CoUninitialize();
}

void App::start_thumb_loader() {
    m_thumb_running = true;
    try {
        m_thumb_thread = std::thread(thumb_loader_worker,
            std::ref(m_thumb_running),
            std::ref(m_thumb_mutex),
            std::ref(m_thumb_cv),
            std::ref(m_thumb_queue),
            std::ref(m_thumbs),
            std::ref(m_index));
    } catch (...) {
        m_thumb_running = false;
    }
}

void App::stop_thumb_loader() {
    m_thumb_running = false;
    m_thumb_cv.notify_all();
    if (m_thumb_thread.joinable())
        m_thumb_thread.join();
}

void App::request_thumb(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_thumbs.size())) return;
    if (m_thumbs[idx].loaded) return;
    {
        std::lock_guard lock(m_thumb_mutex);
        for (int q : m_thumb_queue) if (q == idx) return;
        m_thumb_queue.push_back(idx);
    }
    m_thumb_cv.notify_one();
}

void App::toggle_grid() {
    m_grid_mode = !m_grid_mode;

    if (m_grid_mode) {
        // Enter grid: init thumb cache
        int n = static_cast<int>(m_index.size());
        m_thumbs.clear();
        m_thumbs.resize(n);
        m_thumb_d2d.clear();
        m_grid_scroll_y = 0;
        m_grid_sel = m_current_idx >= 0 ? m_current_idx : 0;
        start_thumb_loader();

        // Request first visible page of thumbnails
        int cols = std::max(1, (static_cast<int>(m_renderer.target_size().width) - THUMB_PAD * 2 + THUMB_GAP) / (THUMB_SIZE + THUMB_GAP));
        m_grid_cols = cols;
        int rows = (static_cast<int>(m_renderer.target_size().height) - THUMB_PAD * 2 + THUMB_GAP) / (THUMB_SIZE + THUMB_GAP);
        for (int i = 0; i < std::min(n, cols * (rows + 2)); ++i)
            request_thumb(i);

        grid_ensure_visible();
        SetWindowTextW(m_window.handle(),
            (L"Grid [" + std::to_wstring(m_index.size()) + L" images]").c_str());
    } else {
        // Exit grid
        stop_thumb_loader();
        m_thumbs.clear();
        m_thumb_d2d.clear();
        update_title();
    }
    m_window.invalidate();
}

void App::grid_click(int x, int y) {
    int col = (x - THUMB_PAD) / (THUMB_SIZE + THUMB_GAP);
    int row = (y - THUMB_PAD + m_grid_scroll_y) / (THUMB_SIZE + THUMB_GAP);
    if (col < 0 || col >= m_grid_cols) return;
    int idx = row * m_grid_cols + col;
    if (idx >= 0 && idx < static_cast<int>(m_index.size())) {
        m_grid_sel = idx;
        m_window.invalidate();
    }
}

void App::grid_navigate(int dir) {
    int total = static_cast<int>(m_index.size());
    if (total == 0) return;
    int next = m_grid_sel + dir;
    if (dir == -1 && m_grid_sel <= 0) return;
    if (dir == 1 && m_grid_sel >= total - 1) return;
    // Handle column edge: -1 wraps same row, +1 wraps same row
    if (dir == -1 && (m_grid_sel % m_grid_cols) == 0) return;
    if (dir == 1 && ((m_grid_sel + 1) % m_grid_cols) == 0) return;
    if (dir == -m_grid_cols && m_grid_sel < m_grid_cols) return;
    if (dir == m_grid_cols && m_grid_sel + m_grid_cols >= total) return;
    if (next < 0) next = 0;
    if (next >= total) next = total - 1;
    m_grid_sel = next;
    grid_ensure_visible();
    m_window.invalidate();
}

void App::grid_ensure_visible() {
    if (m_grid_cols == 0) return;
    int row = m_grid_sel / m_grid_cols;
    int cell_h = THUMB_SIZE + THUMB_GAP;
    int visible_rows = (static_cast<int>(m_renderer.target_size().height) - THUMB_PAD * 2) / cell_h;

    int top_y = row * cell_h;
    int bot_y = top_y + cell_h;
    int view_top = m_grid_scroll_y;
    int view_bot = m_grid_scroll_y + visible_rows * cell_h;

    if (top_y < view_top) m_grid_scroll_y = top_y - THUMB_PAD;
    else if (bot_y > view_bot) m_grid_scroll_y = bot_y - visible_rows * cell_h + THUMB_PAD;
    if (m_grid_scroll_y < 0) m_grid_scroll_y = 0;
}

void App::grid_render() {
    if (!m_renderer.begin_frame()) return;
    m_renderer.clear();

    int total = static_cast<int>(m_index.size());
    int cols = std::max(1, (static_cast<int>(m_renderer.target_size().width) - THUMB_PAD * 2 + THUMB_GAP) / (THUMB_SIZE + THUMB_GAP));
    m_grid_cols = cols;
    int cell = THUMB_SIZE + THUMB_GAP;
    int visible_rows = (static_cast<int>(m_renderer.target_size().height) - THUMB_PAD * 2 + THUMB_GAP) / cell;
    int top_row = m_grid_scroll_y / cell;
    int bot_row = top_row + visible_rows + 1;  // +1 for partial

    // Request thumbs for visible range
    for (int i = top_row * cols; i < std::min(total, (bot_row + 1) * cols); ++i)
        request_thumb(i);

    for (int r = top_row; r <= bot_row; ++r) {
        for (int c = 0; c < cols; ++c) {
            int idx = r * cols + c;
            if (idx >= total) break;

            float x = static_cast<float>(THUMB_PAD + c * cell);
            float y = static_cast<float>(THUMB_PAD + r * cell - m_grid_scroll_y);

            // Check if we have a D2D bitmap cached; if not, create one from WIC
            auto dit = m_thumb_d2d.find(idx);
            if (dit == m_thumb_d2d.end() && idx < static_cast<int>(m_thumbs.size()) && m_thumbs[idx].loaded) {
                auto& te = m_thumbs[idx];
                if (te.wic) {
                    ComPtr<ID2D1Bitmap1> d2d_bmp;
                    HRESULT hr = m_renderer.create_bitmap_from_wic(te.wic.Get(), &d2d_bmp);
                    if (SUCCEEDED(hr) && d2d_bmp) {
                        m_thumb_d2d[idx] = d2d_bmp;
                        dit = m_thumb_d2d.find(idx);
                    }
                }
            }

            bool sel = (idx == m_grid_sel);
            if (dit != m_thumb_d2d.end() && dit->second) {
                // Draw placeholder bg + thumbnail on top
                m_renderer.draw_grid_placeholder(x, y, static_cast<float>(THUMB_SIZE), L"", sel);
                m_renderer.draw_grid_thumbnail(x, y, static_cast<float>(THUMB_SIZE), dit->second.Get());
            } else {
                // Placeholder with filename
                std::wstring name = m_index.path_at(idx);
                size_t pos = name.find_last_of(L"\\/");
                if (pos != std::wstring::npos) name = name.substr(pos + 1);
                m_renderer.draw_grid_placeholder(x, y, static_cast<float>(THUMB_SIZE), name, sel);
            }
        }
    }

    m_renderer.end_frame();
}

void App::render_frame() {
    if (m_grid_mode) {
        grid_render();
        return;
    }
    if (!m_renderer.begin_frame()) {
        // GDI fallback — paint dark background with status text
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_window.handle(), &ps);
        if (hdc) {
            RECT rc = ps.rcPaint;
            HBRUSH bg = CreateSolidBrush(RGB(26, 26, 26));  // #1a1a1a
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(128, 128, 128));
            const wchar_t* msg = m_has_image
                ? L"Loading..." : L"Drag an image here or right-click to open";
            DrawTextW(hdc, msg, -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(m_window.handle(), &ps);
        }
        return;
    }
    m_renderer.clear();
    if (m_has_image) {
        m_renderer.draw_image();
        m_renderer.draw_overlay();
    }
    m_renderer.end_frame();
}

} // namespace mv

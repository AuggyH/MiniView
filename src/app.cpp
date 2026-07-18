#include "app.h"
#include <stdexcept>
#include <Windows.h>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ole2.h>

extern void save_last_dir(const std::wstring& dir);
extern std::wstring get_config_dir();

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


// ── Menu command IDs ─────────────────────────────────────────

enum {
    IDM_OPEN_FILE    = 1001,
    IDM_OPEN_FOLDER  = 1002,
    IDM_EXIT         = 1003,
    IDM_GRID         = 1010,
    IDM_FULLSCREEN   = 1011,
    IDM_RECURSIVE    = 1012,
    IDM_THUMB_SQUARE = 1013,
    IDM_INFO         = 1014,
    IDM_SORT_NAME    = 1020,
    IDM_SORT_DATE    = 1021,
    IDM_SORT_SIZE    = 1022,
    IDM_SORT_RANDOM  = 1023,
    IDM_COPY         = 1030,
    IDM_DELETE       = 1031,
    IDM_DELETE_PERM  = 1032,
    IDM_EXPLORER     = 1033,
    IDM_ABOUT        = 1040,
};

HMENU build_menu_bar() {
    HMENU bar = CreateMenu();

    HMENU file_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, IDM_OPEN_FILE,   L"\u6253\u5F00\u6587\u4EF6...\tCtrl+O");
    AppendMenuW(file_menu, MF_STRING, IDM_OPEN_FOLDER, L"\u6253\u5F00\u6587\u4EF6\u5939...");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file_menu, MF_STRING, IDM_EXIT,        L"\u9000\u51FA\tAlt+F4");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"\u6587\u4EF6(&F)");

    HMENU view_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_STRING, IDM_GRID,        L"\u7F29\u7565\u56FE\u7F51\u683C\tG");
    AppendMenuW(view_menu, MF_STRING, IDM_FULLSCREEN,  L"\u5168\u5C4F\tF11");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);

    HMENU sort_menu = CreatePopupMenu();
    AppendMenuW(sort_menu, MF_STRING, IDM_SORT_NAME,   L"\u6309\u540D\u79F0\tN");
    AppendMenuW(sort_menu, MF_STRING, IDM_SORT_DATE,   L"\u6309\u65E5\u671F\tD");
    AppendMenuW(sort_menu, MF_STRING, IDM_SORT_SIZE,   L"\u6309\u5927\u5C0F\tS");
    AppendMenuW(sort_menu, MF_STRING, IDM_SORT_RANDOM, L"\u968F\u673A\u6253\u4E71\tR");
    AppendMenuW(view_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sort_menu), L"\u6392\u5E8F\u65B9\u5F0F");

    AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view_menu, MF_STRING, IDM_RECURSIVE,    L"\u9012\u5F52\u6D4F\u89C8\tCtrl+R");
    AppendMenuW(view_menu, MF_STRING, IDM_THUMB_SQUARE, L"\u65B9\u5F62\u7F29\u7565\u56FE\tA");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view_menu, MF_STRING, IDM_INFO,         L"\u751F\u6210\u4FE1\u606F\tI");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), L"\u67E5\u770B(&V)");

    HMENU edit_menu = CreatePopupMenu();
    AppendMenuW(edit_menu, MF_STRING, IDM_COPY,        L"\u590D\u5236\tCtrl+C");
    AppendMenuW(edit_menu, MF_STRING, IDM_DELETE,      L"\u5220\u9664\tDel");
    AppendMenuW(edit_menu, MF_STRING, IDM_DELETE_PERM, L"\u6C38\u4E45\u5220\u9664\tShift+Del");
    AppendMenuW(edit_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit_menu, MF_STRING, IDM_EXPLORER,    L"\u5728\u8D44\u6E90\u7BA1\u7406\u5668\u4E2D\u6253\u5F00");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit_menu), L"\u7F16\u8F91(&E)");

    HMENU help_menu = CreatePopupMenu();
    AppendMenuW(help_menu, MF_STRING, IDM_ABOUT, L"\u5173\u4E8E MinView");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), L"\u5E2E\u52A9(&H)");

    return bar;
}
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

    try {
        Decoder decoder;  // each thread gets its own WIC factory
        while (running) {
            std::wstring path;
            {
                std::unique_lock lock(mtx);
                cv.wait(lock, [&] { return !running || !queue.empty(); });
                if (!running) break;
                if (queue.empty()) continue;
                path = std::move(queue.back());
                queue.pop_back();
            }

            try {
                auto bitmap = decoder.decode(path);
                std::lock_guard lock(mtx);
                if (cache.size() >= 6) cache.clear();
                cache[std::move(path)] = bitmap;
            } catch (...) {
                // decode failed — skip
            }
        }
    } catch (...) {
        // Decoder creation failed — thread exits cleanly
    }
    CoUninitialize();
}

// ── App lifecycle ────────────────────────────────────────────

App::App()  = default;
App::~App() { stop_preloader(); stop_thumb_loader(); }

int App::run(const std::wstring& initial_path) {
    // Scale window size by DPI
    float dpi = static_cast<float>(GetDpiForWindow(GetDesktopWindow()));
    int ww = static_cast<int>(1200 * dpi / 96.0f);
    int wh = static_cast<int>(800 * dpi / 96.0f);
    if (!m_window.create(L"MinView", ww, wh))
        throw std::runtime_error("Failed to create window");

    m_window.set_message_callback(
        [this](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            return handle_message(hwnd, msg, wp, lp);
        });

    // Set DPI before renderer init so fonts scale correctly
    m_renderer.set_dpi(dpi, dpi);

    if (!m_renderer.init(m_window.handle()))
        throw std::runtime_error("Failed to init Direct2D renderer");

    // Scale thumbnail cell size by DPI
    float scale = dpi / 96.0f;
    m_thumb_size = static_cast<int>(160 * scale);
    m_thumb_gap  = static_cast<int>(6 * scale);
    m_thumb_pad  = static_cast<int>(0);      // zero padding — grid flush with edges
    m_cell_size  = m_thumb_size + m_thumb_gap;
    m_panel_width = static_cast<int>(280 * scale);
    m_toolbar_h  = static_cast<int>(28 * scale);

    // No native menu bar — custom toolbar drawn via D2D
    SetMenu(m_window.handle(), nullptr);

    start_preloader();

    if (!initial_path.empty()) {
        DWORD attr = GetFileAttributesW(initial_path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            // Directory: scan and enter grid mode
            m_index.scan(initial_path, m_recursive);
            save_last_dir(initial_path);
            if (!m_index.empty()) {
                m_current_idx = 0;
                m_current_path = m_index.path_at(0);
                m_has_image = true;
                toggle_grid();
            }
        } else {
            open_image(initial_path);
        }
    }

    int ret = m_window.run();
    stop_preloader();
    return ret;
}

// ── Message handler ──────────────────────────────────────────

LRESULT App::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN_FILE: {
            OPENFILENAMEW ofn = {};
            wchar_t file[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"\u56FE\u7247\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tiff;*.tif\0\u6240\u6709\u6587\u4EF6\0*.*\0";
            ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) open_image(file);
            return 0;
        }
        case IDM_OPEN_FOLDER: {
            BROWSEINFOW bi = {};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"\u9009\u62E9\u6587\u4EF6\u5939";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t dir[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, dir)) open_image(dir);
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        case IDM_EXIT: DestroyWindow(hwnd); return 0;
        case IDM_GRID: if (!m_index.empty()) toggle_grid(); return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_RECURSIVE: toggle_recursive(); return 0;
        case IDM_THUMB_SQUARE: if (m_grid_mode) toggle_thumb_square(); return 0;
        case IDM_INFO:         toggle_info(); return 0;
        case IDM_SORT_NAME:   set_sort_mode(SortMode::Name);   return 0;
        case IDM_SORT_DATE:   set_sort_mode(SortMode::Date);   return 0;
        case IDM_SORT_SIZE:   set_sort_mode(SortMode::Size);   return 0;
        case IDM_SORT_RANDOM: set_sort_mode(SortMode::Random); return 0;
        case IDM_COPY:
            if (m_grid_mode && has_selection()) copy_selected();
            else copy_to_clipboard();
            return 0;
        case IDM_DELETE:
            if (m_grid_mode && has_selection()) delete_selected(false);
            else delete_current_file(false);
            return 0;
        case IDM_DELETE_PERM:
            if (m_grid_mode && has_selection()) delete_selected(true);
            else delete_current_file(true);
            return 0;
        case IDM_EXPLORER: open_in_explorer(); return 0;
        case IDM_ABOUT:
            MessageBoxW(hwnd,
                L"MinView Native v0.6.0\n\nC++20 / Direct2D + WIC\nGPU \u52A0\u901F\u56FE\u7247\u6D4F\u89C8\u5668\n\u96F6\u5916\u90E8\u4F9D\u8D56",
                L"\u5173\u4E8E MinView", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;

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

    case WM_TIMER:
        if (wp == 1 && m_grid_mode) {
            m_scroll_active = false;
            m_window.invalidate();
        }
        return 0;

    case WM_DPICHANGED: {
        float dpi = static_cast<float>(LOWORD(wp));
        m_renderer.set_dpi(dpi, dpi);
        // Resize to suggested rect
        RECT* rc = reinterpret_cast<RECT*>(lp);
        if (rc) {
            SetWindowPos(hwnd, nullptr, rc->left, rc->top,
                rc->right - rc->left, rc->bottom - rc->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        m_window.invalidate();
        return 0;
    }

    case WM_CONTEXTMENU:
        show_context_menu(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSEWHEEL: {
        if (m_grid_mode) {
            float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            m_grid_scroll_y -= static_cast<int>(delta * 60);
            if (m_grid_scroll_y < 0) m_grid_scroll_y = 0;
            m_scroll_active = true;
            m_window.invalidate();
            return 0;
        }
        if (!m_has_image) return 0;
        float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrl) {
            // Ctrl+Wheel = zoom (cursor-centered)
            float factor = (delta > 0) ? 1.15f : 1.0f / 1.15f;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            uint32_t iw, ih; m_renderer.image_size(iw, ih);
            if (iw == 0 || ih == 0) return 0;
            float old_scale = m_renderer.scale();
            float new_scale = std::clamp(old_scale * factor, m_renderer.fit_scale(), 100.0f);
            if (new_scale == old_scale) return 0;
            D2D1_SIZE_U ts = m_renderer.target_size();
            float img_x = (ts.width  - iw * old_scale) / 2.0f;
            float img_y = (ts.height - ih * old_scale) / 2.0f + m_renderer.scroll_y();
            float img_cx = (pt.x - img_x) / old_scale;
            float img_cy = (pt.y - img_y) / old_scale;
            float new_img_x = pt.x - img_cx * new_scale;
            float new_img_y = pt.y - img_cy * new_scale;
            m_renderer.set_scale(new_scale);
            m_renderer.set_offset(
                new_img_x - (ts.width  - iw * new_scale) / 2.0f,
                new_img_y - (ts.height - ih * new_scale) / 2.0f);
        } else {
            uint32_t iw, ih; m_renderer.image_size(iw, ih);
            if (iw == 0 || ih == 0) return 0;
            float cur_scale = m_renderer.scale();
            float fit = m_renderer.fit_scale();
            bool zoomed = (cur_scale > fit * 1.02f);

            if (zoomed) {
                // Zoomed in: scroll vertically, clamped to image bounds
                float scaled_h = ih * cur_scale;
                float win_h = static_cast<float>(m_renderer.target_size().height);
                float center_y = (win_h - scaled_h) / 2.0f + m_renderer.offset_y();
                float sy = m_renderer.scroll_y() - delta * 50.0f;
                // Clamp: image top <= window top, image bottom >= window bottom
                float max_scroll = -center_y;           // image top at window top
                float min_scroll = win_h - scaled_h - center_y; // image bottom at window bottom
                if (sy > max_scroll) sy = max_scroll;
                if (sy < min_scroll) sy = min_scroll;
                m_renderer.set_scroll_y(sy);
            } else {
                // Not zoomed: flip images
                navigate_to(m_current_idx + (delta > 0 ? 1 : -1));
                return 0;
            }
        }
        m_window.invalidate();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        // Toolbar click? (nested scope to avoid cross-case init)
        int ty = GET_Y_LPARAM(lp);
        if (ty < m_toolbar_h) {
            int tx = GET_X_LPARAM(lp);
            float x = 12.0f;
            float f_dpi = static_cast<float>(GetDpiForWindow(hwnd));
            float fsize = 13.0f * f_dpi / 96.0f;
            m_toolbar_active = -1;
            for (int i = 0; i < static_cast<int>(m_toolbar_items.size()); ++i) {
                float iw = m_renderer.measure_text(m_toolbar_items[i], fsize) + 24.0f;
                if (tx >= static_cast<int>(x) && tx < static_cast<int>(x + iw)) {
                    m_toolbar_active = i;
                    POINT pt = {static_cast<int>(x), m_toolbar_h};
                    ClientToScreen(hwnd, &pt);
                    show_toolbar_menu(hwnd, i, pt.x, pt.y);
                    m_window.invalidate();
                    return 0;
                }
                x += iw;
            }
            return 0;
        }
    }
        if (m_grid_mode) {
            bool sd = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool cd = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            grid_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), sd, cd);
            return 0;
        }
        if (!m_has_image) return -1;
        m_drag_start_x = GET_X_LPARAM(lp);
        m_drag_start_y = GET_Y_LPARAM(lp);
        m_drag_pending = true;
        SetCapture(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        // Toolbar hover tracking
        {
            int ty = GET_Y_LPARAM(lp);
            int prev = m_toolbar_active;
            m_toolbar_active = -1;
            if (ty < m_toolbar_h) {
                int tx = GET_X_LPARAM(lp);
                float x = 12.0f;
                float fs = 13.0f * static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
                for (int i = 0; i < static_cast<int>(m_toolbar_items.size()); ++i) {
                    float iw = m_renderer.measure_text(m_toolbar_items[i], fs) + 24.0f;
                    if (tx >= static_cast<int>(x) && tx < static_cast<int>(x + iw)) {
                        m_toolbar_active = i; break;
                    }
                    x += iw;
                }
            }
            if (m_toolbar_active != prev) m_window.invalidate();
        }
        if (m_drag_pending) {
            int dx = GET_X_LPARAM(lp) - m_drag_start_x;
            int dy = GET_Y_LPARAM(lp) - m_drag_start_y;
            if (dx*dx + dy*dy >= 16) {
                m_drag_pending = false;
                if (!m_current_path.empty()) {
                    ReleaseCapture();
                    FileDataObject* data = new FileDataObject(m_current_path);
                    SimpleDropSource* src = new SimpleDropSource();
                    DWORD effect;
                    DoDragDrop(data, src, DROPEFFECT_COPY, &effect);
                    data->Release();
                    src->Release();
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }
                return 0;
            }
            return 0;
        }
        return -1;

    case WM_LBUTTONUP:
        if (m_drag_pending) {
            m_drag_pending = false;
            ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        if (m_grid_mode) {
            if (m_grid_sel >= 0 && m_grid_sel < static_cast<int>(m_index.size())) {
                toggle_grid();
                navigate_to(m_grid_sel);
            }
            return 0;
        }
        if (m_has_image) { toggle_grid(); return 0; }
        return 0;

    case WM_KEYDOWN: {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;

        if (ctrl) {
            switch (wp) {
            case '0': case VK_NUMPAD0: fit_to_window(); m_window.invalidate(); return 0;
            case VK_OEM_PLUS: case VK_ADD:   zoom_at_center(1.25f); return 0;
            case VK_OEM_MINUS: case VK_SUBTRACT: zoom_at_center(1.0f/1.25f); return 0;
            case 'O': {
                OPENFILENAMEW ofn = {};
                wchar_t file[MAX_PATH] = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"\u56FE\u7247\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tiff;*.tif\0\u6240\u6709\u6587\u4EF6\0*.*\0";
                ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) open_image(file);
                return 0;
            }
            case 'C':
                if (m_grid_mode && has_selection()) copy_selected();
                else copy_to_clipboard();
                return 0;
            case 'R': toggle_recursive(); return 0;
            }
            return -1;
        }

        switch (wp) {
        case VK_ESCAPE:
            if (m_temp_preview) { m_temp_preview = false; m_window.invalidate(); return 0; }
            if (m_grid_mode) { toggle_grid(); return 0; }
            if (m_fullscreen) { toggle_fullscreen(hwnd); return 0; }
            DestroyWindow(hwnd); return 0;
        case VK_F11:   toggle_fullscreen(hwnd); return 0;
        case 'G':
            if (!m_index.empty()) toggle_grid();
            return 0;
        case 'A':
            if (m_grid_mode) { toggle_thumb_square(); return 0; }
            return -1;
        case 'L':
            if (m_grid_mode) { m_show_labels = !m_show_labels; m_window.invalidate(); return 0; }
            return -1;
        case 'I':
            toggle_info(); return 0;
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
            if (m_grid_mode) { grid_navigate(-1, shift); return 0; }
            navigate_to(m_current_idx - 1); return 0;
        case VK_RIGHT:
            if (m_grid_mode) { grid_navigate(1, shift); return 0; }
            navigate_to(m_current_idx + 1); return 0;
        case VK_UP:
            if (m_grid_mode) { grid_navigate(-m_grid_cols, shift); return 0; }
            return -1;
        case VK_DOWN:
            if (m_grid_mode) { grid_navigate(m_grid_cols, shift); return 0; }
            return -1;
        case VK_HOME:
            if (m_grid_mode) { m_grid_sel = 0; grid_ensure_visible(); m_window.invalidate(); return 0; }
            navigate_to(0); return 0;
        case VK_END:
            if (m_grid_mode) { m_grid_sel = static_cast<int>(m_index.size()) - 1; grid_ensure_visible(); m_window.invalidate(); return 0; }
            navigate_to(static_cast<int>(m_index.size()) - 1); return 0;
        case VK_SPACE:
            if (m_grid_mode && m_grid_sel >= 0) {
                if (m_temp_preview) {
                    m_temp_preview = false;
                } else {
                    open_image(m_index.path_at(m_grid_sel));
                    m_temp_preview = true;
                }
                m_window.invalidate();
                return 0;
            }
            navigate_to(m_current_idx + 1); return 0;
        case VK_BACK:  navigate_to(m_current_idx - 1); return 0;
        case VK_RETURN:
            if (m_has_image && !m_grid_mode) { toggle_fullscreen(hwnd); return 0; }
            return -1;
        case VK_DELETE:
            if (m_grid_mode && has_selection()) {
                delete_selected(shift);
            } else {
                delete_current_file(shift);
            }
            return 0;
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
        m_using_thumb_preview = false;

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
            save_last_dir(dir);
        }
        m_current_idx = m_index.index_of(path);

        update_title();

        // Preload neighbors in background
        preload_neighbors();

        m_window.invalidate();
    } catch (const std::exception&) {
        SetWindowTextW(m_window.handle(), L"\u56FE\u7247\u52A0\u8F7D\u5931\u8D25");
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
    m_show_info = false;
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
    save_last_dir(dir);

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
        m_preload_queue.insert(m_preload_queue.begin(), path);  // front = high priority
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
        AppendMenuW(menu, MF_STRING, 1, L"\u5728\u8D44\u6E90\u7BA1\u7406\u5668\u4E2D\u6253\u5F00");
        AppendMenuW(menu, MF_STRING, 2, L"\u590D\u5236\u56FE\u7247\tCtrl+C");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        // Sort submenu
        HMENU sort_menu = CreatePopupMenu();
        auto sm = m_index.sort_mode();
        AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Name   ? MF_CHECKED : 0), 10, L"\u6309\u540D\u79F0\tN");
        AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Date   ? MF_CHECKED : 0), 11, L"\u6309\u65E5\u671F\tD");
        AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Size   ? MF_CHECKED : 0), 12, L"\u6309\u5927\u5C0F\tS");
        AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Random ? MF_CHECKED : 0), 13, L"\u968F\u673A\u6253\u4E71\tR");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sort_menu), L"\u6392\u5E8F\u65B9\u5F0F");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"\u5220\u9664\tDel");
        AppendMenuW(menu, MF_STRING, 4, L"\u6C38\u4E45\u5220\u9664\tShift+Del");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 7, L"\u67E5\u770B\u751F\u6210\u4FE1\u606F\tI");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        UINT flags = MF_STRING;
        if (m_recursive) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, 6, L"\u9012\u5F52\u6D4F\u89C8\tCtrl+R");
    } else {
        AppendMenuW(menu, MF_STRING, 5, L"\u6253\u5F00\u6587\u4EF6...");
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
    case 7: toggle_info(); break;
    case 10: set_sort_mode(SortMode::Name);   break;
    case 11: set_sort_mode(SortMode::Date);   break;
    case 12: set_sort_mode(SortMode::Size);   break;
    case 13: set_sort_mode(SortMode::Random); break;
    }

    DestroyMenu(menu);
}

void App::show_toolbar_menu(HWND hwnd, int idx, int x, int y) {
    HMENU popup = CreatePopupMenu();
    switch (idx) {
    case 0: // 文件
        AppendMenuW(popup, MF_STRING, 1, L"打开文件\tCtrl+O");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, 2, L"删除\tDel");
        AppendMenuW(popup, MF_STRING, 3, L"永久删除\tShift+Del");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, 4, L"在资源管理器中打开");
        break;
    case 1: // 查看
        AppendMenuW(popup, MF_STRING, 10, L"全屏\tF11");
        AppendMenuW(popup, MF_STRING, 11, L"缩略图网格\tG");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, 12, L"排序: 名称\tN");
        AppendMenuW(popup, MF_STRING, 13, L"排序: 日期\tD");
        AppendMenuW(popup, MF_STRING, 14, L"排序: 大小\tS");
        AppendMenuW(popup, MF_STRING, 15, L"排序: 随机\tR");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, 16, L"递归浏览\tCtrl+R");
        AppendMenuW(popup, MF_STRING, 17, L"方形缩略图\tA");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, 30, L"查看生成信息\tI");
        break;
    case 2: // 编辑
        AppendMenuW(popup, MF_STRING, 20, L"复制\tCtrl+C");
        AppendMenuW(popup, MF_STRING, 21, L"复制图片");
        break;
    case 3: // 帮助
        AppendMenuW(popup, MF_STRING, 31, L"关于 MinView");
        break;
    }
    int cmd = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_NONOTIFY,
        x, y, 0, hwnd, nullptr);
    DestroyMenu(popup);

    // Handle commands
    switch (cmd) {
    case 1: case 10: case 11: case 12: case 13: case 14: case 15:
    case 16: case 17: case 20: case 21: case 30: case 31:
        SendMessageW(hwnd, WM_COMMAND, cmd, 0); break;
    }
}

void App::open_in_explorer() {
    if (m_current_path.empty()) return;
    std::wstring path = m_current_path;
    // Convert forward slashes to backslashes
    for (auto& c : path) if (c == L'/') c = L'\\';
    std::wstring args = L"/select,\"" + path + L"\"";
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
        fit_to_window();
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
    float new_scale = std::clamp(old_scale * factor, m_renderer.fit_scale(), 100.0f);
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

// Save a WIC bitmap as JPEG to disk (for thumbnail cache)
static void save_wic_as_jpeg(IWICBitmapSource* src, const std::wstring& path) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return;

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return;
    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr)) return;
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return;
    frame->Initialize(nullptr);

    uint32_t w, h;
    src->GetSize(&w, &h);
    frame->SetSize(w, h);
    frame->WriteSource(src, nullptr);
    frame->Commit();
    encoder->Commit();
}

// Get a WIC bitmap from the Windows shell thumbnail cache (fast path)
static ComPtr<IWICBitmapSource> get_shell_thumb(const std::wstring& path, uint32_t max_size) {
    ComPtr<IShellItemImageFactory> factory;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return nullptr;

    SIZE sz = {static_cast<LONG>(max_size), static_cast<LONG>(max_size)};
    HBITMAP hbmp = nullptr;
    hr = factory->GetImage(sz, SIIGBF_RESIZETOFIT, &hbmp);
    if (FAILED(hr) || !hbmp) return nullptr;

    // Convert HBITMAP to WIC bitmap via IWICImagingFactory
    ComPtr<IWICImagingFactory> wic_factory;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory));
    if (FAILED(hr)) { DeleteObject(hbmp); return nullptr; }

    ComPtr<IWICBitmap> wic_bmp;
    hr = wic_factory->CreateBitmapFromHBITMAP(hbmp, nullptr,
        WICBitmapUsePremultipliedAlpha, &wic_bmp);
    DeleteObject(hbmp);
    if (FAILED(hr)) return nullptr;

    // Convert to PBGRA
    ComPtr<IWICFormatConverter> converter;
    hr = wic_factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return nullptr;
    hr = converter->Initialize(wic_bmp.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) return nullptr;

    ComPtr<IWICBitmapSource> result;
    converter.As(&result);
    return result;
}

static void thumb_loader_worker(
    std::atomic<bool>& running,
    std::mutex& mtx,
    std::condition_variable& cv,
    std::vector<int>& queue,
    std::vector<App::ThumbEntry>& thumbs,
    ImageIndex& index,
    int thumb_size)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
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
                auto path = index.path_at(idx);

                // Check disk cache first
                std::hash<std::wstring> hasher;
                wchar_t key[32];
                swprintf_s(key, L"%016llx", hasher(path));
                std::wstring cache_file = get_config_dir() + L"\\thumbs\\" + key + L".jpg";

                ComPtr<IWICBitmapSource> wic;
                WIN32_FILE_ATTRIBUTE_DATA src_attr, cache_attr;
                bool cache_hit = false;
                if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &src_attr) &&
                    GetFileAttributesExW(cache_file.c_str(), GetFileExInfoStandard, &cache_attr)) {
                    ULONGLONG src_time = (static_cast<ULONGLONG>(src_attr.ftLastWriteTime.dwHighDateTime) << 32)
                        | src_attr.ftLastWriteTime.dwLowDateTime;
                    ULONGLONG cache_time = (static_cast<ULONGLONG>(cache_attr.ftLastWriteTime.dwHighDateTime) << 32)
                        | cache_attr.ftLastWriteTime.dwLowDateTime;
                    if (cache_time >= src_time) {
                        try { wic = decoder.decode_scaled(cache_file, thumb_size); cache_hit = true; }
                        catch (...) {}
                    }
                }

                if (!cache_hit) {
                    wic = get_shell_thumb(path, thumb_size);
                    if (!wic) {
                        wic = decoder.decode_scaled(path, thumb_size);
                    }
                    // Save to disk cache
                    if (wic) {
                        CreateDirectoryW((get_config_dir() + L"\\thumbs").c_str(), nullptr);
                        save_wic_as_jpeg(wic.Get(), cache_file);
                    }
                }

                std::lock_guard lock(mtx);
                thumbs[idx].wic = wic;
                thumbs[idx].loaded = true;
                // Store original dimensions via probe
                auto info = decoder.probe(index.path_at(idx));
                if (info) { thumbs[idx].orig_w = info->width; thumbs[idx].orig_h = info->height; }
            } catch (...) {
                std::lock_guard lock(mtx);
                thumbs[idx].loaded = true;
            }
        }
    } catch (...) {
        // Decoder creation failed — thread exits cleanly
    }
    CoUninitialize();
}

void App::start_thumb_loader() {
    m_thumb_running = true;
    m_thumb_threads.clear();
    int num_threads = 2;
    for (int i = 0; i < num_threads; ++i) {
        try {
            m_thumb_threads.emplace_back(thumb_loader_worker,
                std::ref(m_thumb_running),
                std::ref(m_thumb_mutex),
                std::ref(m_thumb_cv),
                std::ref(m_thumb_queue),
                std::ref(m_thumbs),
                std::ref(m_index),
                m_thumb_size);
        } catch (...) {
            m_thumb_running = false;
            break;
        }
    }
}

void App::stop_thumb_loader() {
    m_thumb_running = false;
    m_thumb_cv.notify_all();
    for (auto& t : m_thumb_threads) {
        if (t.joinable()) t.join();
    }
    m_thumb_threads.clear();
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
        int n = static_cast<int>(m_index.size());
        bool re_entry = (m_thumbs.size() == static_cast<size_t>(n));
        if (!re_entry) {
            m_thumbs.clear();
            m_thumbs.resize(n);
            m_thumb_d2d.clear();
        }

        // Smart scroll restoration
        if (m_current_idx == m_grid_saved_idx) {
            // User didn't navigate: restore original scroll
            m_grid_scroll_y = m_grid_scroll_saved;
            m_grid_sel = m_grid_saved_idx;
        } else {
            // User navigated: center on current image
            m_grid_sel = m_current_idx;
            m_grid_scroll_y = 0;  // grid_ensure_visible will set correct position
        }
        m_selected.clear();
        m_selected.resize(n, false);
        if (m_grid_sel < n) m_selected[m_grid_sel] = true;
        m_sel_anchor = m_grid_sel;
        if (!re_entry) start_thumb_loader();

        // Request first visible page of thumbnails
        int gw = static_cast<int>(m_renderer.target_size().width) - m_panel_width;
        int cols = std::max(1, (gw + m_thumb_gap) / (m_thumb_size + m_thumb_gap));
        m_grid_cols = cols;
        int thumb_w = (gw - (cols - 1) * m_thumb_gap) / cols;
        int cell = thumb_w + m_thumb_gap;
        int total_rows = (n + cols - 1) / cols;
        m_grid_total_rows = total_rows;
        int rows = (static_cast<int>(m_renderer.target_size().height) - m_toolbar_h) / cell;

        for (int i = 0; i < std::min(n, cols * (rows + 2)); ++i)
            request_thumb(i);

        grid_ensure_visible();
        SetWindowTextW(m_window.handle(),
            (L"\u7F29\u7565\u56FE\u7F51\u683C [" + std::to_wstring(m_index.size()) + L" \u5F20]").c_str());

        // 100ms timer for lazy thumbnail loading (fires even when unfocused)
        m_grid_timer = SetTimer(m_window.handle(), 1, 100, nullptr);
    } else {
        // Exit grid — save state but keep thumb cache
        m_grid_scroll_saved = m_grid_scroll_y;
        m_grid_saved_idx = m_grid_sel;
        if (m_grid_timer) { KillTimer(m_window.handle(), m_grid_timer); m_grid_timer = 0; }
        // Don't stop thumb loader or clear cache — reuse on re-entry
        update_title();
    }
    m_window.invalidate();
}

void App::grid_click(int x, int y, bool shift, bool ctrl) {
    int cols = m_grid_cols, gap = m_thumb_gap;
    int gw = static_cast<int>(m_renderer.target_size().width) - m_panel_width;
    int usable_w = gw - (cols - 1) * gap;
    int total = static_cast<int>(m_index.size());

    // Find row
    int ty = y - m_toolbar_h + m_grid_scroll_y;
    int row = -1, row_y = 0;
    for (int r = 0; r < static_cast<int>(m_row_heights.size()); ++r) {
        if (ty < row_y + m_row_heights[r]) { row = r; break; }
        row_y += m_row_heights[r];
    }
    if (row < 0) return;

    // Recompute row layout to find clicked column
    int start = row * cols;
    int end = std::min(start + cols, total);
    float H = 120.0f;
    double tw = 0;
    for (int i = start; i < end; ++i) {
        uint32_t iw = 1, ih = 1;
        if (i < static_cast<int>(m_thumbs.size()) && m_thumbs[i].wic) m_thumbs[i].wic->GetSize(&iw, &ih);
        if (iw == 0) iw = 1; if (ih == 0) ih = 1;
        tw += (double)H * iw / ih;
    }
    float scale = (tw > 0) ? static_cast<float>(usable_w / tw) : 1.0f;
    int row_h = std::max(40, static_cast<int>(H * scale));
    float cx = 0;
    for (int i = start; i < end; ++i) {
        uint32_t iw = 1, ih = 1;
        if (i < static_cast<int>(m_thumbs.size()) && m_thumbs[i].wic) m_thumbs[i].wic->GetSize(&iw, &ih);
        if (iw == 0) iw = 1; if (ih == 0) ih = 1;
        float iw2 = static_cast<float>(row_h) * iw / ih;
        if (x >= static_cast<int>(cx) && x < static_cast<int>(cx + iw2)) {
            int idx = i;
            if (idx < 0 || idx >= total) return;
            if (shift && m_sel_anchor >= 0) select_range(m_sel_anchor, idx);
            else if (ctrl) { if (idx < static_cast<int>(m_selected.size())) m_selected[idx] = !m_selected[idx]; m_sel_anchor = idx; }
            else { clear_selection(); if (idx < static_cast<int>(m_selected.size())) m_selected[idx] = true; m_sel_anchor = idx; }
            m_grid_sel = idx;
            m_window.invalidate();
            return;
        }
        cx += iw2 + gap;
    }
}

void App::grid_navigate(int dir, bool shift) {
    int total = static_cast<int>(m_index.size());
    if (total == 0) return;
    int next = m_grid_sel + dir;
    if (dir == -1 && m_grid_sel <= 0) return;
    if (dir == 1 && m_grid_sel >= total - 1) return;
    if (dir == -1 && (m_grid_sel % m_grid_cols) == 0) return;
    if (dir == 1 && ((m_grid_sel + 1) % m_grid_cols) == 0) return;
    if (dir == -m_grid_cols && m_grid_sel < m_grid_cols) return;
    if (dir == m_grid_cols && m_grid_sel + m_grid_cols >= total) return;
    if (next < 0) next = 0;
    if (next >= total) next = total - 1;
    m_grid_sel = next;

    if (shift && m_sel_anchor >= 0) {
        select_range(m_sel_anchor, m_grid_sel);
    } else if (!shift) {
        clear_selection();
        if (m_grid_sel < static_cast<int>(m_selected.size()))
            m_selected[m_grid_sel] = true;
        m_sel_anchor = m_grid_sel;
    }

    grid_ensure_visible();
    m_window.invalidate();
}

void App::grid_ensure_visible() {
    if (m_grid_cols == 0) return;
    int row = m_grid_sel / m_grid_cols;
    int gw = static_cast<int>(m_renderer.target_size().width) - m_panel_width;
    int thumb_w = (gw - (m_grid_cols - 1) * m_thumb_gap) / m_grid_cols;
    int cell_h = thumb_w + m_thumb_gap;
    int visible_rows = (static_cast<int>(m_renderer.target_size().height) - m_toolbar_h) / cell_h;

    int top_y = row * cell_h;
    int bot_y = top_y + cell_h;
    int view_top = m_grid_scroll_y;
    int view_bot = m_grid_scroll_y + visible_rows * cell_h;

    if (top_y < view_top) m_grid_scroll_y = top_y - m_thumb_pad;
    else if (bot_y > view_bot) m_grid_scroll_y = bot_y - visible_rows * cell_h + m_thumb_pad;
    if (m_grid_scroll_y < 0) m_grid_scroll_y = 0;
}

void App::toggle_info() {
    if (!m_has_image) return;

    std::wstring target = m_grid_mode && m_grid_sel >= 0
        ? m_index.path_at(m_grid_sel) : m_current_path;

    if (!m_show_info) {
        m_info_meta = extract_metadata(target);
        if (!m_info_meta.valid) return;
    }
    m_show_info = !m_show_info;
    m_window.invalidate();
}

void App::toggle_thumb_square() {
    m_thumb_square = !m_thumb_square;
    m_thumb_d2d.clear();  // force redraw with new aspect
    m_window.invalidate();
}

// ── Multi-select helpers ─────────────────────────────────

bool App::has_selection() const {
    for (auto s : m_selected) if (s) return true;
    return false;
}

void App::clear_selection() {
    std::fill(m_selected.begin(), m_selected.end(), false);
    m_sel_anchor = -1;
}

void App::select_range(int start, int end) {
    clear_selection();
    if (start > end) std::swap(start, end);
    for (int i = start; i <= end && i < static_cast<int>(m_selected.size()); ++i)
        m_selected[i] = true;
}

void App::delete_selected(bool permanent) {
    std::vector<int> to_delete;
    for (int i = 0; i < static_cast<int>(m_selected.size()); ++i)
        if (m_selected[i]) to_delete.push_back(i);
    if (to_delete.empty()) return;

    std::wstring from;
    for (auto rit = to_delete.rbegin(); rit != to_delete.rend(); ++rit) {
        from += m_index.path_at(*rit);
        from.push_back(L'\0');
    }
    from.push_back(L'\0');

    SHFILEOPSTRUCTW fos = {};
    fos.wFunc  = FO_DELETE;
    fos.pFrom  = from.c_str();
    fos.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (!permanent) fos.fFlags |= FOF_ALLOWUNDO;
    if (SHFileOperationW(&fos) != 0) return;

    std::sort(to_delete.begin(), to_delete.end(), std::greater<int>());
    for (int idx : to_delete) m_index.remove(idx);
    clear_selection();

    if (m_index.empty()) {
        m_has_image = false; m_current_path.clear(); m_current_idx = -1;
        m_grid_mode = false; stop_thumb_loader();
        m_thumbs.clear(); m_thumb_d2d.clear();
        SetWindowTextW(m_window.handle(), L"MinView");
        m_window.invalidate(); return;
    }
    m_grid_sel = std::min(m_grid_sel, static_cast<int>(m_index.size()) - 1);
    if (m_current_idx >= static_cast<int>(m_index.size()))
        m_current_idx = static_cast<int>(m_index.size()) - 1;
}

void App::copy_selected() {
    std::vector<std::wstring> paths;
    for (int i = 0; i < static_cast<int>(m_selected.size()); ++i)
        if (m_selected[i]) paths.push_back(m_index.path_at(i));
    if (paths.empty()) return;

    int total_bytes = sizeof(DROPFILES);
    for (auto& p : paths) total_bytes += static_cast<int>((p.size() + 1) * sizeof(wchar_t));
    total_bytes += static_cast<int>(sizeof(wchar_t));

    if (!OpenClipboard(m_window.handle())) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, total_bytes);
    if (!hMem) { CloseClipboard(); return; }
    auto* df = static_cast<DROPFILES*>(GlobalLock(hMem));
    df->pFiles = sizeof(DROPFILES);
    df->fWide  = TRUE;
    auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
    for (auto& p : paths) {
        wcscpy_s(dst, p.size() + 1, p.c_str());
        dst += p.size() + 1;
    }
    *dst = L'\0';
    GlobalUnlock(hMem);
    SetClipboardData(CF_HDROP, hMem);
    CloseClipboard();
}

void App::grid_render() {
    if (!m_renderer.begin_frame()) return;
    m_renderer.clear();

    int total = static_cast<int>(m_index.size());
    int grid_area_w = static_cast<int>(m_renderer.target_size().width) - m_panel_width;
    int cols = std::max(1, (grid_area_w + m_thumb_gap) / (m_thumb_size + m_thumb_gap));
    m_grid_cols = cols;
    int gap = m_thumb_gap;
    int usable_w = grid_area_w - (cols - 1) * gap;

    // --- First pass: justified row heights ---
    struct RowInfo { int start_idx, end_idx, row_h, row_y; std::vector<float> img_x, img_w; };
    std::vector<RowInfo> rows;
    int cur_y = 0, idx = 0;

    while (idx < total) {
        RowInfo ri;
        ri.start_idx = idx;
        ri.end_idx = std::min(idx + cols, total);
        ri.row_y = cur_y;

        // Gather aspect ratios
        float H = 120.0f; // initial guess — will be scaled
        double total_w_at_H = 0;
        for (int i = ri.start_idx; i < ri.end_idx; ++i) {
            uint32_t iw = 1, ih = 1;
            if (i < static_cast<int>(m_thumbs.size()) && m_thumbs[i].wic)
                m_thumbs[i].wic->GetSize(&iw, &ih);
            if (iw == 0) iw = 1; if (ih == 0) ih = 1;
            total_w_at_H += (double)H * iw / ih;
        }
        // Scale so total width fits usable_w
        float scale = (total_w_at_H > 0) ? static_cast<float>(usable_w / total_w_at_H) : 1.0f;
        ri.row_h = static_cast<int>(H * scale);
        if (ri.row_h < 40) ri.row_h = 40;

        // Compute per-image widths and x-positions
        float x = 0;
        for (int i = ri.start_idx; i < ri.end_idx; ++i) {
            uint32_t iw = 1, ih = 1;
            if (i < static_cast<int>(m_thumbs.size()) && m_thumbs[i].wic)
                m_thumbs[i].wic->GetSize(&iw, &ih);
            if (iw == 0) iw = 1; if (ih == 0) ih = 1;
            float img_w = static_cast<float>(ri.row_h) * iw / ih;
            ri.img_x.push_back(x);
            ri.img_w.push_back(img_w);
            x += img_w + gap;
        }
        rows.push_back(ri);
        int label_extra = m_show_labels ? static_cast<int>(42 * static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f) : 0;
        cur_y += ri.row_h + gap + label_extra;
        idx = ri.end_idx;
    }
    m_grid_total_rows = static_cast<int>(rows.size());
    m_row_heights.clear();
    int label_extra2 = m_show_labels ? static_cast<int>(42 * static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f) : 0;
    for (auto& ri : rows) m_row_heights.push_back(ri.row_h + gap + label_extra2);
    int visible_h = static_cast<int>(m_renderer.target_size().height) - m_toolbar_h;

    // --- Scroll calc ---
    int top_px = m_grid_scroll_y;
    int top_row = 0, bot_row = static_cast<int>(rows.size()) - 1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (rows[i].row_y + rows[i].row_h > top_px) { top_row = i; break; }
    }
    for (int i = top_row; i < static_cast<int>(rows.size()); ++i) {
        if (rows[i].row_y > top_px + visible_h) { bot_row = i; break; }
    }

    // --- Request visible thumbs ---
    if (!m_scroll_active) {
        for (int r = top_row; r <= bot_row && r < static_cast<int>(rows.size()); ++r)
            for (int i = rows[r].start_idx; i < rows[r].end_idx; ++i)
                request_thumb(i);
    }

    // --- Batch WIC grab ---
    std::vector<std::pair<int, ComPtr<IWICBitmapSource>>> ready;
    {
        std::lock_guard lock(m_thumb_mutex);
        for (int r = top_row; r <= bot_row && r < static_cast<int>(rows.size()); ++r)
            for (int i = rows[r].start_idx; i < rows[r].end_idx; ++i)
                if (!m_thumb_d2d.count(i) && i < static_cast<int>(m_thumbs.size()) && m_thumbs[i].loaded && m_thumbs[i].wic)
                    ready.push_back({i, m_thumbs[i].wic});
    }
    int d2d_count = 0;
    for (auto& [i, wic] : ready) {
        if (d2d_count >= 2) break;
        ComPtr<ID2D1Bitmap1> d2d_bmp;
        if (SUCCEEDED(m_renderer.create_bitmap_from_wic(wic.Get(), &d2d_bmp)) && d2d_bmp) {
            m_thumb_d2d[i] = d2d_bmp; ++d2d_count;
        }
    }

    // --- Second pass: render ---
    for (int r = top_row; r <= bot_row && r < static_cast<int>(rows.size()); ++r) {
        auto& ri = rows[r];
        float row_y = static_cast<float>(m_toolbar_h + ri.row_y - m_grid_scroll_y);
        for (int j = 0; j < ri.end_idx - ri.start_idx; ++j) {
            int idx2 = ri.start_idx + j;
            if (idx2 >= total) break;
            float x = ri.img_x[j];
            float w = ri.img_w[j];
            bool sel = (idx2 == m_grid_sel) || (idx2 < static_cast<int>(m_selected.size()) && m_selected[idx2]);
            auto dit = m_thumb_d2d.find(idx2);
            if (dit != m_thumb_d2d.end() && dit->second) {
                m_renderer.draw_grid_thumbnail(x, row_y, w, static_cast<float>(ri.row_h), dit->second.Get(), m_thumb_square);
            }
            if (sel) {
                D2D1_RECT_F sel_rc = {x - 2, row_y - 2, x + w + 2, row_y + ri.row_h + 2};
                m_renderer.draw_selection_border(sel_rc);
            }
            // Labels
            if (m_show_labels) {
                auto& spath = m_index.path_at(idx2);
                size_t pos2 = spath.find_last_of(L"\\/");
                std::wstring fname = (pos2 != std::wstring::npos) ? spath.substr(pos2 + 1) : spath;
                float dpi_s = static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
                float ly = row_y + ri.row_h + 4.0f * dpi_s;
                m_renderer.draw_label(x, ly, w, fname, 12.0f);

                if (m_thumbs[idx2].orig_w > 0)
                    m_renderer.draw_label(x, ly + 18.0f * dpi_s, w,
                        std::to_wstring(m_thumbs[idx2].orig_w) + L"\u00D7" + std::to_wstring(m_thumbs[idx2].orig_h), 10.0f);
            }
        }
    }

    // Toolbar + panel + scrollbar
    float px = static_cast<float>(m_renderer.target_size().width) - m_panel_width;
    float tw = static_cast<float>(m_renderer.target_size().width);
    float view_h = static_cast<float>(m_renderer.target_size().height);

    // Scrollbar
    int total_h = 0;
    for (auto& ri : rows) total_h += ri.row_h + gap;
    m_renderer.draw_scrollbar(px - 14.0f, static_cast<float>(m_toolbar_h), 8.0f, view_h - m_toolbar_h,
        static_cast<float>(total_h), view_h, static_cast<float>(m_grid_scroll_y));

    // Toolbar always on top
    m_renderer.draw_toolbar(tw, m_toolbar_items, m_toolbar_active);

    // Side info panel
    {
        float pw = static_cast<float>(m_panel_width);
        float ph = view_h;
        std::vector<std::pair<std::wstring, std::wstring>> pinfo, pgen;
        ID2D1Bitmap1* preview_bmp = nullptr;
        uint32_t pvw = 0, pvh = 0;

        if (m_grid_sel >= 0 && m_grid_sel < total) {
            auto& selpath = m_index.path_at(m_grid_sel);
            size_t pos = selpath.find_last_of(L"\\/");
            std::wstring name = (pos != std::wstring::npos) ? selpath.substr(pos + 1) : selpath;
            pinfo.push_back({L"\u6587\u4EF6\u540D", name});

            auto probe = m_decoder.probe(selpath);
            if (probe) {
                pvw = probe->width; pvh = probe->height;
                pinfo.push_back({L"\u5206\u8FA8\u7387", std::to_wstring(pvw) + L"\u00D7" + std::to_wstring(pvh)});
            }

            auto dit2 = m_thumb_d2d.find(m_grid_sel);
            if (dit2 != m_thumb_d2d.end()) preview_bmp = dit2->second.Get();

            WIN32_FILE_ATTRIBUTE_DATA attr;
            if (GetFileAttributesExW(selpath.c_str(), GetFileExInfoStandard, &attr)) {
                ULONGLONG fsize = (static_cast<ULONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
                if (fsize < 1024) pinfo.push_back({L"\u5927\u5C0F", std::to_wstring(fsize) + L" B"});
                else if (fsize < 1024*1024) pinfo.push_back({L"\u5927\u5C0F", std::to_wstring(fsize/1024) + L" KB"});
                else { wchar_t buf[32]; swprintf_s(buf, L"%.1f MB", fsize/(1024.0*1024.0)); pinfo.push_back({L"\u5927\u5C0F", buf}); }
            }

            ImageMeta meta = extract_metadata(selpath);
            if (meta.valid) {
                if (!meta.model.empty()) pgen.push_back({L"\u6A21\u578B", meta.model});
                if (meta.seed >= 0) pgen.push_back({L"Seed", std::to_wstring(meta.seed)});
                if (meta.steps > 0) pgen.push_back({L"\u6B65\u6570", std::to_wstring(meta.steps)});
                if (meta.cfg > 0) { wchar_t buf[16]; swprintf_s(buf, L"%.1f", meta.cfg); pgen.push_back({L"CFG", buf}); }
                if (!meta.sampler.empty()) pgen.push_back({L"\u91C7\u6837\u5668", meta.sampler});
            }
        } else {
            pinfo.push_back({L"\u6587\u4EF6\u6570", std::to_wstring(total) + L" \u5F20"});
        }
        m_renderer.draw_side_panel(px, static_cast<float>(m_toolbar_h), pw, ph - m_toolbar_h, preview_bmp, pvw, pvh, pinfo, pgen);
    }

    m_renderer.end_frame();
}

void App::render_frame() {
    if (m_temp_preview) {
        if (!m_renderer.begin_frame()) return;
        m_renderer.clear();
        if (m_has_image) {
            if (m_using_thumb_preview) {
                auto full = get_preloaded(m_current_path);
                if (full) { m_renderer.upload_image(full.Get()); m_using_thumb_preview = false; }
            }
            m_renderer.draw_image();
            m_renderer.draw_overlay();
        }
        m_renderer.end_frame();
        return;
    }
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
                ? L"\u52A0\u8F7D\u4E2D..." : L"\u62D6\u5165\u56FE\u7247\u6216\u53F3\u952E\u6253\u5F00\u6587\u4EF6";
            DrawTextW(hdc, msg, -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(m_window.handle(), &ps);
        }
        return;
    }
    m_renderer.clear();
    // Draw custom toolbar
    float tw = static_cast<float>(m_renderer.target_size().width);
    m_renderer.draw_toolbar(tw, m_toolbar_items, m_toolbar_active);
    if (m_has_image) {
        m_renderer.draw_image();
        m_renderer.draw_overlay();
        if (m_show_info) {
            std::vector<std::pair<std::wstring, std::wstring>> items;
            if (!m_info_meta.positive_prompt.empty())
                items.push_back({L"\u63D0\u793A\u8BCD", m_info_meta.positive_prompt});
            if (!m_info_meta.negative_prompt.empty())
                items.push_back({L"\u53CD\u5411\u8BCD", m_info_meta.negative_prompt});
            if (!m_info_meta.model.empty())
                items.push_back({L"\u6A21\u578B", m_info_meta.model});
            if (!m_info_meta.vae.empty())
                items.push_back({L"VAE", m_info_meta.vae});
            if (m_info_meta.width > 0 && m_info_meta.height > 0)
                items.push_back({L"\u5206\u8FA8\u7387", std::to_wstring(m_info_meta.width) + L"\u00D7" + std::to_wstring(m_info_meta.height)});
            if (m_info_meta.seed >= 0)
                items.push_back({L"Seed", std::to_wstring(m_info_meta.seed)});
            if (m_info_meta.steps > 0)
                items.push_back({L"\u6B65\u6570", std::to_wstring(m_info_meta.steps)});
            if (m_info_meta.cfg > 0)
                items.push_back({L"CFG", std::to_wstring(m_info_meta.cfg)});
            if (!m_info_meta.sampler.empty())
                items.push_back({L"\u91C7\u6837\u5668", m_info_meta.sampler});
            if (!m_info_meta.scheduler.empty())
                items.push_back({L"\u8C03\u5EA6\u5668", m_info_meta.scheduler});
            m_renderer.draw_info_card(items);
        }
    } else {
        m_renderer.draw_hint(L"\u62D6\u5165\u56FE\u7247\u6216\u53F3\u952E\u6253\u5F00\u6587\u4EF6");
    }
    m_renderer.end_frame();
}

} // namespace mv

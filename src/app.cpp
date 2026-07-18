#include "app.h"
#include <stdexcept>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>

namespace mv {

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
App::~App() { stop_preloader(); }

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
        if (!m_has_image) return -1;
        m_dragging = true;
        m_drag_start_x = GET_X_LPARAM(lp);
        m_drag_start_y = GET_Y_LPARAM(lp);
        m_drag_offset_x = m_renderer.offset_x();
        m_drag_offset_y = m_renderer.offset_y();
        SetCapture(hwnd);
        SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        return 0;

    case WM_MOUSEMOVE:
        if (!m_dragging) return -1;
        m_renderer.set_offset(
            m_drag_offset_x + GET_X_LPARAM(lp) - m_drag_start_x,
            m_drag_offset_y + GET_Y_LPARAM(lp) - m_drag_start_y);
        m_window.invalidate();
        return 0;

    case WM_LBUTTONUP:
        if (!m_dragging) return -1;
        m_dragging = false;
        ReleaseCapture();
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return 0;

    case WM_LBUTTONDBLCLK:
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
            }
            return -1;
        }

        switch (wp) {
        case VK_ESCAPE:
            if (m_fullscreen) { toggle_fullscreen(hwnd); return 0; }
            DestroyWindow(hwnd); return 0;
        case VK_F11:   toggle_fullscreen(hwnd); return 0;
        case VK_LEFT:  navigate_to(m_current_idx - 1); return 0;
        case VK_RIGHT: navigate_to(m_current_idx + 1); return 0;
        case VK_HOME:  navigate_to(0); return 0;
        case VK_END:   navigate_to(static_cast<int>(m_index.size()) - 1); return 0;
        case VK_SPACE: navigate_to(m_current_idx + 1); return 0;
        case VK_BACK:  navigate_to(m_current_idx - 1); return 0;
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
    auto t0 = std::chrono::high_resolution_clock::now();
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
            m_index.scan(dir, false);
        }
        m_current_idx = m_index.index_of(path);

        auto t2 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count();

        uint32_t iw, ih; m_renderer.image_size(iw, ih);
        size_t pos = path.find_last_of(L"\\/");
        std::wstring fn = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;

        std::wstring title = std::to_wstring(iw) + L"x" + std::to_wstring(ih) +
            L" " + std::to_wstring(ms) + L"ms" +
            L" [" + std::to_wstring(m_current_idx + 1) + L"/" +
            std::to_wstring(m_index.size()) + L"] " + fn;
        SetWindowTextW(m_window.handle(), title.c_str());

        // Preload neighbors in background
        preload_neighbors();

        m_window.invalidate();
    } catch (const std::exception&) {
        SetWindowTextW(m_window.handle(), L"Error loading image");
    }
}

void App::update_title() {
    uint32_t iw, ih; m_renderer.image_size(iw, ih);
    size_t pos = m_current_path.find_last_of(L"\\/");
    std::wstring fn = (pos != std::wstring::npos)
        ? m_current_path.substr(pos + 1) : m_current_path;

    std::wstring title = std::to_wstring(iw) + L"x" + std::to_wstring(ih) +
        L" [" + std::to_wstring(m_current_idx + 1) + L"/" +
        std::to_wstring(m_index.size()) + L"] " + fn;
    SetWindowTextW(m_window.handle(), title.c_str());
}

void App::navigate_to(int idx) {
    if (m_index.empty()) return;
    if (idx < 0 || idx >= static_cast<int>(m_index.size())) return;
    const auto& path = m_index.path_at(idx);
    if (path.empty()) return;
    open_image(path);
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
        AppendMenuW(menu, MF_STRING, 3, L"Delete\tDel");
        AppendMenuW(menu, MF_STRING, 4, L"Delete Permanently\tShift+Del");
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

void App::render_frame() {
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
    } else {
        // No image: show centered hint text
        // (background already cleared to dark gray)
    }
    m_renderer.end_frame();
}

} // namespace mv

#include "app.h"
#include "app_state.h"
#include "album_sampler.h"
#include "design_tokens.h"
#include "file_operation.h"
#include "renderer_state.h"
#include "selection_remap.h"
#include "title_bar_model.h"
#include <stdexcept>
#include <Windows.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <functional>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ole2.h>
#include <ocidl.h>
#include <Psapi.h>
#include <limits>
#include <fstream>
#include <iterator>

extern void save_last_dir(const std::wstring& dir);
extern void save_sort_mode(int mode);
extern int  load_sort_mode();
extern std::wstring get_config_dir();

namespace mv {

constexpr UINT WM_THUMB_READY = WM_APP + 1;
constexpr UINT WM_METADATA_READY = WM_APP + 2;
constexpr UINT WM_RENDER_RETRY = WM_APP + 3;
constexpr UINT WM_COMIC_READY = WM_APP + 4;
constexpr UINT WM_NAV_SCAN_READY = WM_APP + 5;
constexpr UINT WM_NAV_TREE_READY = WM_APP + 6;
constexpr UINT WM_IMAGE_READY = WM_APP + 7;  // async big-image decode done
constexpr UINT WM_DIR_CHANGED = WM_APP + 8;    // watched directory changed
constexpr UINT WM_FOLDER_ICON_READY = WM_APP + 9; // folder-icon tile decoded

// Heap result from the folder-icon worker; the UI thread converts the WIC
// source into a D2D bitmap and deletes this struct.
struct FolderIconResult {
    std::wstring folder;
    int tile = 0;
    Microsoft::WRL::ComPtr<IWICBitmapSource> wic;
    bool failed = false;
};

static std::uint64_t wic_source_bytes(IWICBitmapSource* src);
constexpr UINT_PTR kComicTimerId = 5;
constexpr UINT_PTR kRenderRetryTimerId = 9;  // device-loss recreate throttle
constexpr UINT_PTR kImageDebounceTimerId = 6;
constexpr UINT_PTR kAsyncWatchdogTimerId = 7;  // async decode watchdog (1s tick)
constexpr UINT_PTR kFilmstripHideTimerId = 8;  // filmstrip auto-hide (avoids id-5 collision with kComicTimerId)

std::size_t current_private_bytes() {
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            static_cast<DWORD>(sizeof(counters)))) {
        return kComicSoftCacheBytes;
    }
    return static_cast<std::size_t>(counters.PrivateUsage);
}

// ── OLE Drag source helpers ──────────────────────────────────

// Simple IDataObject that holds one or more file paths as CF_HDROP.
class FileDataObject : public IDataObject {
public:
    explicit FileDataObject(const std::vector<std::wstring>& paths) : m_ref(1) {
        SIZE_T path_bytes = sizeof(wchar_t);
        for (const auto& path : paths) path_bytes += (path.size() + 1) * sizeof(wchar_t);
        m_data_size = sizeof(DROPFILES) + path_bytes;
        m_data = GlobalAlloc(GMEM_MOVEABLE, m_data_size);
        if (m_data) {
            auto* df = static_cast<DROPFILES*>(GlobalLock(m_data));
            if (!df) {
                GlobalFree(m_data);
                m_data = nullptr;
                m_data_size = 0;
                return;
            }
            ZeroMemory(df, sizeof(*df));
            df->pFiles = sizeof(DROPFILES);
            df->fWide  = TRUE;
            auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
            for (const auto& path : paths) {
                wmemcpy(dst, path.c_str(), path.size() + 1);
                dst += path.size() + 1;
            }
            *dst = L'\0';
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
        if (!m_data || m_data_size == 0) return E_OUTOFMEMORY;
        ZeroMemory(sm, sizeof(*sm));
        sm->tymed = TYMED_HGLOBAL;
        sm->hGlobal = GlobalAlloc(GMEM_MOVEABLE, m_data_size);
        if (!sm->hGlobal) return E_OUTOFMEMORY;
        void* src = GlobalLock(m_data);
        void* dst = GlobalLock(sm->hGlobal);
        if (!src || !dst) {
            if (dst) GlobalUnlock(sm->hGlobal);
            if (src) GlobalUnlock(m_data);
            GlobalFree(sm->hGlobal);
            ZeroMemory(sm, sizeof(*sm));
            return STG_E_READFAULT;
        }
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
    IDM_FULLSCREEN   = 1011,
    IDM_RECURSIVE    = 1012,
    IDM_THUMB_SQUARE = 1013,
    IDM_INFO         = 1014,
    IDM_LABELS       = 1015,
    IDM_COMIC        = 1016,
    IDM_COMIC_SEAMLESS = 1017,
    IDM_COMIC_AUTOSCROLL = 1018,
    IDM_SORT_NAME    = 1020,
    IDM_SORT_DATE    = 1021,
    IDM_SORT_SIZE    = 1022,
    IDM_SORT_RANDOM  = 1023,
    IDM_COPY_IMAGE   = 1030,
    IDM_COPY_PATH    = 1034,
    IDM_CREATE_COPY  = 1035,
    IDM_EXPLORER     = 1033,
    IDM_ABOUT        = 1040,
    IDM_COMIC_SPEED_05 = 1041,
    IDM_COMIC_SPEED_10 = 1042,
    IDM_COMIC_SPEED_15 = 1043,
    IDM_COMIC_SPEED_20 = 1044,
    IDM_NAV_PANEL     = 1050,
};

HMENU build_menu_bar() {
    HMENU bar = CreateMenu();

    HMENU file_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, IDM_OPEN_FILE,   L"\u6253\u5F00\u6587\u4EF6...	Ctrl+O");
    AppendMenuW(file_menu, MF_STRING, IDM_OPEN_FOLDER, L"\u6253\u5F00\u6587\u4EF6\u5939...");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file_menu, MF_STRING, IDM_EXIT,        L"\u9000\u51FA	Alt+F4");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"\u6587\u4EF6(&F)");

    HMENU view_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_STRING, IDM_COMIC, L"\u6F2B\u753B\u6A21\u5F0F\tM");
    AppendMenuW(view_menu, MF_STRING, IDM_COMIC_SEAMLESS, L"\u65E0\u7F1D\u9875\u8DDD");
    AppendMenuW(view_menu, MF_STRING, IDM_COMIC_AUTOSCROLL,
        L"\u81EA\u52A8\u6EDA\u52A8\tP");
    HMENU comic_speed_menu = CreatePopupMenu();
    AppendMenuW(comic_speed_menu, MF_STRING, IDM_COMIC_SPEED_05, L"0.5x");
    AppendMenuW(comic_speed_menu, MF_STRING, IDM_COMIC_SPEED_10, L"1.0x");
    AppendMenuW(comic_speed_menu, MF_STRING, IDM_COMIC_SPEED_15, L"1.5x");
    AppendMenuW(comic_speed_menu, MF_STRING, IDM_COMIC_SPEED_20, L"2.0x");
    AppendMenuW(view_menu, MF_POPUP,
        reinterpret_cast<UINT_PTR>(comic_speed_menu),
        L"\u81EA\u52A8\u6EDA\u52A8\u901F\u5EA6");
    AppendMenuW(view_menu, MF_STRING, IDM_FULLSCREEN,  L"\u5168\u5C4F	F11");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);

    HMENU sort_menu = CreatePopupMenu();
    AppendMenuW(sort_menu, MF_STRING, IDM_SORT_NAME,   L"\u6309\u540D\u79F0	N");
    AppendMenuW(sort_menu, MF_STRING, IDM_SORT_DATE,   L"\u6309\u65E5\u671F	D");
    AppendMenuW(sort_menu, MF_STRING, IDM_SORT_SIZE,   L"\u6309\u5927\u5C0F	S");
    AppendMenuW(sort_menu, MF_STRING, IDM_SORT_RANDOM, L"\u968F\u673A\u6253\u4E71	R");
    AppendMenuW(view_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sort_menu), L"\u6392\u5E8F\u65B9\u5F0F");

    AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view_menu, MF_STRING, IDM_RECURSIVE,    L"\u9012\u5F52\u6D4F\u89C8	Ctrl+R");
    AppendMenuW(view_menu, MF_STRING, IDM_THUMB_SQUARE, L"\u65B9\u5F62\u7F29\u7565\u56FE	A");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view_menu, MF_STRING, IDM_INFO,         L"\u4FE1\u606F\u9762\u677F	I");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), L"\u67E5\u770B(&V)");

    HMENU edit_menu = CreatePopupMenu();
    AppendMenuW(edit_menu, MF_STRING, IDM_COPY_IMAGE,  L"\u590D\u5236	Ctrl+C");
    AppendMenuW(edit_menu, MF_STRING, IDM_COPY_PATH,   L"\u590D\u5236\u6587\u4EF6\u8DEF\u5F84");
    AppendMenuW(edit_menu, MF_STRING, IDM_CREATE_COPY, L"\u521B\u5EFA\u526F\u672C");
    AppendMenuW(edit_menu, MF_STRING, IDM_DELETE,      L"\u5220\u9664	Del");
    AppendMenuW(edit_menu, MF_STRING, IDM_DELETE_PERM, L"\u6C38\u4E45\u5220\u9664	Shift+Del");
    AppendMenuW(edit_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit_menu, MF_STRING, IDM_EXPLORER,    L"\u5728\u8D44\u6E90\u7BA1\u7406\u5668\u4E2D\u6253\u5F00");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit_menu), L"\u7F16\u8F91(&E)");

    HMENU help_menu = CreatePopupMenu();
    AppendMenuW(help_menu, MF_STRING, IDM_ABOUT, L"\u5173\u4E8E MinView");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), L"\u5E2E\u52A9(&H)");

    return bar;
}
// ── Owner-draw menu support ──────────────────────────────────

struct OwnerItemData {
    std::wstring text;
    std::wstring shortcut;
    bool disabled  = false;
    bool checked   = false;
};

static void AddOwnerSeparator(HMENU menu) {
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
}

static OwnerItemData* AddOwnerItem(HMENU menu, UINT id, const std::wstring& label, bool disabled = false, bool checked = false) {
    auto* d = new OwnerItemData;
    size_t tab = label.find(L'\t');
    if (tab != std::wstring::npos) {
        d->text = label.substr(0, tab);
        d->shortcut = label.substr(tab + 1);
    } else {
        d->text = label;
    }
    d->disabled = disabled;
    d->checked  = checked;
    MENUITEMINFOW mii = { sizeof(mii) };
    mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STATE;
    mii.fType = MFT_OWNERDRAW;
    mii.fState = disabled ? MFS_DISABLED : MFS_ENABLED;
    mii.wID   = id;
    mii.dwItemData = reinterpret_cast<ULONG_PTR>(d);
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mii);
    return d;
}

static void BuildOwnerMenu(HMENU parent, HMENU sub, const std::wstring& label) {
    auto* d = new OwnerItemData;
    d->text = label;
    MENUITEMINFOW mii = { sizeof(mii) };
    mii.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_DATA | MIIM_SUBMENU;
    mii.fType = MFT_OWNERDRAW;
    mii.hSubMenu = sub;
    mii.dwTypeData = const_cast<LPWSTR>(label.c_str());
    mii.dwItemData = reinterpret_cast<ULONG_PTR>(d);
    InsertMenuItemW(parent, GetMenuItemCount(parent), TRUE, &mii);
}

static void metadata_worker(
    std::atomic<bool>& running,
    std::mutex& mutex,
    std::condition_variable& cv,
    std::wstring& request_path,
    bool& request_pending,
    std::wstring& result_path,
    ImageMeta& result,
    bool& result_ready,
    HWND notify_window)
{
    while (running) {
        std::wstring path;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return !running || request_pending; });
            if (!running) break;
            path = std::move(request_path);
            request_path.clear();
            request_pending = false;
        }

        ImageMeta metadata;
        try {
            metadata = extract_metadata(path);
        } catch (...) {
            metadata = {};
        }

        {
            std::lock_guard lock(mutex);
            if (!running) break;
            result_path = std::move(path);
            result = std::move(metadata);
            result_ready = true;
        }
        PostMessageW(notify_window, WM_METADATA_READY, 0, 0);
    }
}

// ── Async collection scan worker (Issue #5 P2) ──────────────
// Scans into a private ImageIndex off the UI thread; the generation is
// checked at apply time (App::apply_nav_scan_result) so stale results
// from superseded switches are dropped.

static void nav_scan_worker(
    std::atomic<bool>& running,
    std::mutex& mutex,
    std::condition_variable& cv,
    bool& queued,
    NavScanJob& job,
    bool& result_ready,
    NavScanResult& result,
    HWND notify_window)
{
    while (running) {
        NavScanJob current;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return !running || queued; });
            if (!running) break;
            current = job;
            queued = false;
        }

        NavScanResult scanned;
        scanned.path = current.path;
        scanned.recursive = current.recursive;
        scanned.generation = current.generation;
        scanned.refresh = current.refresh;
        scanned.watch_roots = current.watch_roots;
        scanned.album_name = current.album_name;
        if (!current.paths.empty()) {
            scanned.scan_result = scanned.index.load_paths(current.paths);
        } else if (!current.roots.empty()) {
            scanned.scan_result = scanned.index.scan_many(current.roots);
        } else {
            scanned.scan_result =
                scanned.index.scan(current.path, current.recursive);
        }
        if (scanned.scan_result >= 0)
            scanned.index.sort_by(current.sort);

        {
            std::lock_guard lock(mutex);
            if (!running) break;
            result = std::move(scanned);
            result_ready = true;
        }
        PostMessageW(notify_window, WM_NAV_SCAN_READY, 0, 0);
    }
}

// ── Async directory-tree enumeration worker (Issue #5 P2) ───
// Lists subdirectories + counts direct image files in one pass. The
// NavTreeModel generation check drops results superseded by newer
// requests.

static void nav_tree_worker(
    std::atomic<bool>& running,
    std::mutex& mutex,
    std::condition_variable& cv,
    bool& queued,
    NavTreeJob& job,
    bool& outcome_ready,
    NavTreeOutcome& outcome,
    HWND notify_window)
{
    namespace fs = std::filesystem;
    while (running) {
        NavTreeJob current;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return !running || queued; });
            if (!running) break;
            current = job;
            queued = false;
        }

        NavTreeOutcome result;
        result.node_id = current.node_id;
        result.generation = current.generation;
        std::error_code error;
        if (fs::is_directory(current.path, error) && !error) {
            std::vector<NavChildInfo> children;
            int count = 0;
            fs::directory_iterator it(current.path,
                fs::directory_options::skip_permission_denied, error);
            for (; !error && it != fs::directory_iterator();
                 it.increment(error)) {
                std::error_code entry_error;
                if (it->is_directory(entry_error)) {
                    NavChildInfo child;
                    child.path = it->path().wstring();
                    child.name = it->path().filename().wstring();
                    children.push_back(std::move(child));
                } else if (!entry_error && it->is_regular_file(entry_error)) {
                    std::wstring ext =
                        it->path().extension().wstring();
                    for (auto& ch : ext)
                        ch = static_cast<wchar_t>(std::towlower(ch));
                    if (ImageIndex::is_supported_image_extension(ext))
                        ++count;
                }
            }
            if (error) {
                result.ok = false;
                result.error = L"\u65E0\u6CD5\u8BFB\u53D6\u8BE5\u76EE\u5F55";
            } else {
                std::sort(children.begin(), children.end(),
                    [](const NavChildInfo& left, const NavChildInfo& right) {
                        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
                    });
                result.ok = true;
                result.children = std::move(children);
                result.image_count = count;
            }
        } else {
            result.ok = false;
            result.error = L"\u76EE\u5F55\u4E0D\u5B58\u5728\u6216\u65E0\u6CD5\u8BBF\u95EE";
        }

        {
            std::lock_guard lock(mutex);
            if (!running) break;
            outcome = std::move(result);
            outcome_ready = true;
        }
        PostMessageW(notify_window, WM_NAV_TREE_READY, 0, 0);
    }
}

static void FreeOwnerItemData(HMENU menu) {
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        HMENU sub = GetSubMenu(menu, i);
        if (sub) FreeOwnerItemData(sub);

        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_DATA;
        if (GetMenuItemInfoW(menu, i, TRUE, &mii) && mii.dwItemData != 0) {
            delete reinterpret_cast<OwnerItemData*>(mii.dwItemData);
        }
    }
}

// Subclass proc for popup menu windows: fill background dark
static LRESULT CALLBACK MenuSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wp);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = reinterpret_cast<HBRUSH>(GetPropW(hwnd, L"MV_BG"));
        if (bg) FillRect(hdc, &rc, bg);
        return 1;
    }
    if (msg == WM_NCPAINT) {
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, L"MV_OLD");
        RemovePropW(hwnd, L"MV_BG");
    }
    WNDPROC oldProc = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"MV_OLD"));
    return oldProc ? CallWindowProcW(oldProc, hwnd, msg, wp, lp)
                   : DefWindowProcW(hwnd, msg, wp, lp);
}

// Recursively apply dark background to menu and submenus
static void ApplyMenuTheme(HMENU menu, HBRUSH br) {
    MENUINFO mi = { sizeof(mi) };
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = br;
    SetMenuInfo(menu, &mi);
    int cnt = GetMenuItemCount(menu);
    for (int i = 0; i < cnt; ++i) {
        HMENU sub = GetSubMenu(menu, i);
        if (sub) ApplyMenuTheme(sub, br);
    }
}

// ── App lifecycle ────────────────────────────────────────────

App::App()
    : m_delete_composition(make_windows_delete_composition(*this)),
      m_thumb_engine(ThumbEngine::Port{}) {}
App::~App() {
    m_comic_loader.stop();
    stop_metadata_loader();
    stop_async_pool();
}

int App::run(const std::wstring& initial_path) {
    // Scale window size by the primary monitor DPI. GetDpiForSystem() can
    // report 96 while the actual display runs at 168/192 (system DPI vs
    // monitor DPI divergence), which creates an oversized window whose
    // bottom edge (filmstrip, status area) lands outside the screen.
    const HWND desktop = GetDesktopWindow();
    const UINT monitor_dpi = GetDpiForWindow(desktop);
    const float dpi = static_cast<float>(
        monitor_dpi > 0 ? monitor_dpi : GetDpiForSystem());
    int ww = static_cast<int>(layout::kDefaultWindowWidthDip * dpi / 96.0f);
    int wh = static_cast<int>(layout::kDefaultWindowHeightDip * dpi / 96.0f);
    // Clamp to the screen so the whole window (incl. bottom filmstrip)
    // stays on screen regardless of display configuration.
    const int avail_w = GetSystemMetrics(SM_CXSCREEN);
    const int avail_h = GetSystemMetrics(SM_CYSCREEN);
    if (avail_w > 0) ww = std::min(ww, avail_w);
    if (avail_h > 0) wh = std::min(wh, avail_h);
    if (!m_window.create(L"MinView", ww, wh))
        throw std::runtime_error("Failed to create window");

    // Capture the HWND by value: a worker detached by stop() may outlive
    // App, so the notify callback must never dereference `this`.
    m_thumb_engine.set_notify([hwnd = m_window.handle()]() {
        PostMessageW(hwnd, WM_THUMB_READY, 0, 0);
    });

    const float window_dpi = static_cast<float>(GetDpiForWindow(m_window.handle()));

    m_window.set_message_callback(
        [this](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            return handle_message(hwnd, msg, wp, lp);
        });

    // Set DPI before renderer init so fonts scale correctly
    m_renderer.set_dpi(window_dpi, window_dpi);

    if (!m_renderer.init(m_window.handle()))
        throw std::runtime_error("Failed to init Direct2D renderer");
    m_renderer_generation = m_renderer.device_generation();

    apply_dpi_layout(window_dpi);
    update_content_viewport(false);

    // No native menu bar — custom toolbar drawn via D2D
    SetMenu(m_window.handle(), nullptr);

    start_async_pool();
    m_comic_loader.start(m_window.handle(), WM_COMIC_READY);
    start_metadata_loader();
    start_nav_workers();
    load_album_store();

    if (!initial_path.empty()) {
        DWORD attr = GetFileAttributesW(initial_path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            open_directory(initial_path);
        } else {
            open_image(initial_path);
        }
    }

    int ret = m_window.run();
    reset_comic_controls(ComicAppCancelTrigger::ExitMode);
    m_comic_loader.stop();
    stop_metadata_loader();
    stop_nav_workers();
    stop_async_pool();
    stop_folder_icon_worker();
    return ret;
}

// Single DPI-scaling entry for all layout members. Nominal DIP values come
// from mv::layout; the scaled results are the unique runtime layout source.
void App::apply_dpi_layout(float dpi) {
    const float scale = layout::dpi_scale(dpi);
    m_thumb_cell  = static_cast<int>(layout::kThumbCellDip * scale);  // display cell → columns (~5/row)
    m_thumb_size  = static_cast<int>(layout::kThumbSizeDip * scale);  // decode res (2x supersampling)
    m_thumb_gap_h = static_cast<int>(layout::kThumbGapHDip * scale);
    m_thumb_gap_v = static_cast<int>(layout::kThumbGapVDip * scale);
    m_thumb_pad   = static_cast<int>(layout::kThumbPadDip * scale);   // uniform padding
    m_panel_width = static_cast<int>(layout::kPanelWidthDip * scale);
    m_toolbar_h   = static_cast<int>(m_title_h * scale);
    m_nav_visible_width = static_cast<int>(layout::kNavPanelWidthDip * scale);
    m_nav_breadcrumb_h = static_cast<int>(
        layout::kNavBreadcrumbBarHeightDip * scale);
    m_grid_top = m_toolbar_h + m_nav_breadcrumb_h;
}

void App::begin_grid_scroll(HWND hwnd) {
    m_grid_scroll_pause.begin(
        [hwnd](std::uintptr_t timer) {
            KillTimer(hwnd, static_cast<UINT_PTR>(timer));
        },
        [hwnd] {
            return static_cast<std::uintptr_t>(
                SetTimer(hwnd, 1, dt::kDurationScrollPauseMs, nullptr));
        });
}

void App::finish_grid_scroll() {
    HWND hwnd = m_window.handle();
    m_grid_scroll_pause.finish([hwnd](std::uintptr_t timer) {
        KillTimer(hwnd, static_cast<UINT_PTR>(timer));
    });
}

// ── Message handler ──────────────────────────────────────────

LRESULT App::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_NCCALCSIZE:
        if (wp == TRUE) return 0;  // no inset for caption — custom title bar
        break;

    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float dpi_s = m_renderer.is_valid()
            ? static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f : 1.0f;
        int th = static_cast<int>(m_title_h * dpi_s);
        if (toolbar_visible() && pt.y >= 0 && pt.y < th) {
            const float tw = static_cast<float>(m_renderer.target_size().width);
            const TitleBarLayout title{0.0f, tw, static_cast<float>(th), dpi_s};
            // Window buttons → HTCLIENT (handled by WM_LBUTTONDOWN)
            if (title_bar_window_button_at(title,
                    static_cast<float>(pt.x)) >= 0) {
                return HTCLIENT;
            }
            // Menu items → HTCLIENT (handled by WM_LBUTTONDOWN)
            std::vector<float> menu_widths;
            menu_widths.reserve(m_toolbar_items.size());
            const float fsize = title_bar_menu_font_size(title);
            for (const auto& item : m_toolbar_items)
                menu_widths.push_back(m_renderer.measure_text(item, fsize));
            if (title_bar_menu_item_at(title,
                    static_cast<float>(pt.x), menu_widths) >= 0) {
                return HTCLIENT;
            }
            return HTCAPTION;  // rest → drag
        }
        break;
    }

    case WM_CLOSE:
        reset_comic_controls(ComicAppCancelTrigger::ExitMode);
        return -1;

    case WM_COMMAND:
        if (m_delete_composition->handle_command(
                DeleteCommandEntry::WindowCommand, LOWORD(wp)))
            return 0;
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
                if (SHGetPathFromIDListW(pidl, dir)) open_directory(dir);
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        case IDM_EXIT:
            reset_comic_controls(ComicAppCancelTrigger::ExitMode);
            DestroyWindow(hwnd);
            return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_COMIC: toggle_comic_reader(); return 0;
        case IDM_COMIC_SEAMLESS:
            if (m_comic_reader.enabled()) {
                m_comic_reader.set_seamless(!m_comic_reader.seamless());
                request_comic_pages();
                m_window.invalidate();
            }
            return 0;
        case IDM_COMIC_AUTOSCROLL:
            (void)dispatch_comic_command(ComicAppCommand::ToggleCruise);
            return 0;
        case IDM_COMIC_SPEED_05:
            (void)dispatch_comic_command(ComicAppCommand::SetSpeed05); return 0;
        case IDM_COMIC_SPEED_10:
            (void)dispatch_comic_command(ComicAppCommand::SetSpeed10); return 0;
        case IDM_COMIC_SPEED_15:
            (void)dispatch_comic_command(ComicAppCommand::SetSpeed15); return 0;
        case IDM_COMIC_SPEED_20:
            (void)dispatch_comic_command(ComicAppCommand::SetSpeed20); return 0;
        case IDM_RECURSIVE:
            if (can_toggle_recursive(
                    m_grid_mode, m_has_image, m_index.directory()))
                toggle_recursive();
            return 0;
        case IDM_THUMB_SQUARE: if (m_grid_mode) toggle_thumb_square(); return 0;
        case IDM_INFO:         toggle_info(); return 0;
        case IDM_NAV_PANEL:    toggle_nav_panel(); return 0;
        case IDM_LABELS:       toggle_grid_labels(); return 0;
        case IDM_SORT_NAME:   if (m_grid_mode) set_sort_mode(SortMode::Name);   return 0;
        case IDM_SORT_DATE:   if (m_grid_mode) set_sort_mode(SortMode::Date);   return 0;
        case IDM_SORT_SIZE:   if (m_grid_mode) set_sort_mode(SortMode::Size);   return 0;
        case IDM_SORT_RANDOM: if (m_grid_mode) set_sort_mode(SortMode::Random); return 0;
        case IDM_COPY_IMAGE:  copy_image_data(); return 0;
        case IDM_COPY_PATH:   copy_file_paths(); return 0;
        case IDM_CREATE_COPY: create_file_copies(); return 0;
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
        if (w > 0 && h > 0) {
            if (!m_renderer.resize(w, h)) m_window.invalidate();
            m_grid_layout_dirty = true;
            update_content_viewport(false);
        }
        return 0;
    }
    case WM_PAINT: {
        // A nested paint (PrintWindow capture, DWM re-composition) can be
        // dispatched while the outer render is mid-frame. Rendering twice
        // corrupts the D2D clip stack and kills the device; ignore nested
        // paints — the outer frame completes and presents normally.
        if (m_render_busy) return 0;
        m_render_busy = true;
        render_frame();
        m_render_busy = false;
        ValidateRect(hwnd, nullptr);
        // Re-arm the transition: while the filmstrip animation runs, keep
        // invalidating so the next animation frame is painted. FULL-window
        // invalidation is required — filmstrip_rect() returns PHYSICAL
        // coordinates (renderer target size) while InvalidateRect takes
        // window-client coordinates (DPI-virtualized, e.g. 1414 vs 2474),
        // so a strip-region rect lands outside the client area and the
        // animation would freeze after one frame ("flash" appearance).
        if (m_filmstrip.animating()) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 0;

    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mis->CtlType != ODT_MENU) break;
        if (mis->itemData == 0) {  // separator
            mis->itemWidth  = 20;
            mis->itemHeight = static_cast<UINT>(dt::kSpaceSmDip * static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
            return TRUE;
        }
        auto* d = reinterpret_cast<OwnerItemData*>(mis->itemData);
        if (!d) break;
        float dpi = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
        float text_w = m_renderer.measure_text(d->text, dt::kFontSizeXsDip * dpi);
        float shortcut_w = d->shortcut.empty() ? 0 : m_renderer.measure_text(d->shortcut, dt::kFontSizeXsDip * dpi);
        float icon_w = dt::kSpaceLgDip * dpi;
        float pad_l = dt::kSpaceXsDip * dpi;
        float pad_icon = dt::kSpaceSmDip * dpi;
        float pad_shortcut = dt::kSpaceLgDip * dpi;
        mis->itemWidth  = static_cast<UINT>(icon_w + pad_icon + text_w + pad_shortcut + shortcut_w + pad_l * 2);
        mis->itemHeight = static_cast<UINT>(dt::kSize28Dip * dpi);
        return TRUE;
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis->CtlType != ODT_MENU) break;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        float dpi = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;

        // Separator (only itemData == 0, not by itemID)
        if (dis->itemData == 0) {
            COLORREF bg = dt::kColorMenuBgGdi;
            HBRUSH br = CreateSolidBrush(bg);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            int mid_y = (rc.top + rc.bottom) / 2;
            COLORREF sep_c = dt::kColorMenuSeparatorGdi;
            HPEN pen = CreatePen(PS_SOLID, 1, sep_c);
            HGDIOBJ old_pen = SelectObject(hdc, pen);
            MoveToEx(hdc, rc.left + static_cast<int>(dt::kSize28Dip * dpi), mid_y, nullptr);
            LineTo(hdc, rc.right - 4, mid_y);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
            return TRUE;
        }

        auto* d = reinterpret_cast<OwnerItemData*>(dis->itemData);
        if (!d) break;
        bool selected = (dis->itemState & ODS_SELECTED) != 0;
        bool disabled = d->disabled || (dis->itemState & ODS_GRAYED);

        // Background
        COLORREF bg = selected ? dt::kColorMenuSelectedBgGdi
                               : dt::kColorMenuBgGdi;
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(hdc, &rc, br);
        DeleteObject(br);

        // Icon/checkmark area
        float icon_w = dt::kSpaceLgDip * dpi;
        float pad_l = dt::kSpaceXsDip * dpi;
        RECT icon_rc = { static_cast<int>(rc.left + pad_l), rc.top,
                         static_cast<int>(rc.left + pad_l + icon_w), rc.bottom };
        if (d->checked) {
            SetTextColor(hdc, disabled ? dt::kColorMenuTextDisabledGdi
                                       : dt::kColorMenuCheckEnabledGdi);
            HFONT f = CreateFontW(-MulDiv(static_cast<int>(dt::kFontSizeMdDip), GetDeviceCaps(hdc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, dt::kFontFamilySymbols);
            HGDIOBJ old_font = SelectObject(hdc, f);
            SetBkMode(hdc, TRANSPARENT);
            DrawTextW(hdc, L"\x2713", 1, &icon_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, old_font);
            DeleteObject(f);
        }

        // Text
        float icon_right = pad_l + icon_w + dt::kSpaceSmDip * dpi;
        RECT text_rc = { static_cast<int>(rc.left + icon_right), rc.top,
                         rc.right - 4, rc.bottom };
        SetTextColor(hdc, disabled ? dt::kColorMenuTextDisabledGdi
                                   : dt::kColorMenuTextGdi);
        HFONT f2 = CreateFontW(-MulDiv(static_cast<int>(dt::kFontSizeXsDip), GetDeviceCaps(hdc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, dt::kFontFamilyUi);
        HGDIOBJ old_font = SelectObject(hdc, f2);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, d->text.c_str(), -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Shortcut
        if (!d->shortcut.empty()) {
            SetTextColor(hdc, disabled ? dt::kColorMenuShortcutDisabledGdi
                                       : dt::kColorMenuShortcutGdi);
            DrawTextW(hdc, d->shortcut.c_str(), -1, &text_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(hdc, old_font);
        DeleteObject(f2);
        return TRUE;
    }

    case WM_TIMER:
        if (wp == kRenderRetryTimerId) {
            KillTimer(hwnd, kRenderRetryTimerId);
            m_render_retry_timer = 0;
            m_window.invalidate();
            return 0;
        }
        if (wp == kComicTimerId && m_comic_timer) {
            handle_comic_timer(hwnd);
        }
        if (wp == kImageDebounceTimerId) {
            KillTimer(hwnd, kImageDebounceTimerId);
            if (!m_debounce_path.empty()) {
                std::wstring pending = m_debounce_path;
                m_debounce_path.clear();
                open_image(pending);  // full decode after paging stops
            }
        }
        if (wp == 1 && m_grid_mode) {
            finish_grid_scroll();
            m_window.invalidate();
        }
        if (wp == 2) {
            m_panel_sel = -1;
            KillTimer(hwnd, 2);
            m_sel_timer = 0;
            m_window.invalidate();
        }
        if (wp == 3) {
            m_panel_copied.clear();
            KillTimer(hwnd, 3);
            m_toast_timer = 0;
            m_window.invalidate();
        }
        if (wp == 4 && m_anim_timer) {
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            float elapsed = static_cast<float>(now.QuadPart - m_anim_start) / freq.QuadPart;
            m_anim_t = elapsed / dt::kDurationTransitionSec;  // 250ms
            if (m_anim_t >= 1.0f) {
                m_anim_t = 1.0f;
                m_animating = false;
                m_anim_thumb.Reset();
                m_anim_reversed = false;
                m_anim_grid_snapshot.Reset();
                KillTimer(hwnd, 4);
                m_anim_timer = 0;
            }
            m_window.invalidate();
        }
        if (wp == kFilmstripHideTimerId) {
            if (m_filmstrip_timer) {
                KillTimer(hwnd, kFilmstripHideTimerId);
                m_filmstrip_timer = 0;
            }
            if (m_filmstrip_revealed || m_filmstrip_reveal_animating) {
                hide_filmstrip_animated();
                m_window.invalidate();
            }
        }
        if (wp == kAsyncWatchdogTimerId) {
            check_async_timeout();
            if (async_slots_active()) {
                SetTimer(hwnd, kAsyncWatchdogTimerId, 1000, nullptr);
            } else {
                KillTimer(hwnd, kAsyncWatchdogTimerId);
            }
        }
        return 0;

    case WM_THUMB_READY:
        if (m_grid_mode || filmstrip_visible()) m_window.invalidate();
        // A placeholder waiting for its thumbnail gets it now.
        if (!m_grid_mode && m_placeholder_idx >= 0) {
            auto pit = m_thumb_d2d.find(m_placeholder_idx);
            if (pit != m_thumb_d2d.end())
                m_renderer.set_placeholder(pit->second.Get());
        }
        return 0;

    case WM_METADATA_READY:
        apply_metadata_result();
        return 0;

    case WM_COMIC_READY:
        apply_comic_results();
        return 0;

    case WM_NAV_SCAN_READY:
        apply_nav_scan_result();
        return 0;

    case WM_NAV_TREE_READY:
        apply_nav_tree_result();
        return 0;

    case WM_DIR_CHANGED:
        // A watched directory changed: rescan the current collection in
        // place so new/deleted images appear without manual refresh.
        request_collection_refresh();
        return 0;

    case WM_FOLDER_ICON_READY:
        handle_folder_icon_ready(lp);
        return 0;

    case WM_IMAGE_READY: {
        // Async big-image decode finished on a worker. Finish on the UI
        // thread: materialize + upload + commit. Stale generations (user
        // paged again) are dropped.
        const ULONGLONG gen = static_cast<ULONGLONG>(wp);
        ComPtr<IWICBitmapSource> decoded;
        std::wstring prev_path;
        int prev_idx = -1;
        {
            std::lock_guard lock(m_async->mutex);
            if (gen != m_async->gen) return 0;  // stale page flip — drop
            prev_path = m_async->current_prev_path;
            prev_idx = m_async->current_prev_idx;
            if (!m_async->wic) {                // decode failed — keep previous
                m_async_busy = false;
                m_current_path = prev_path;     // roll identity back with the bitmap
                m_current_idx = prev_idx;
                return 0;
            }
            decoded = m_async->wic;
            m_async->wic.Reset();
            m_async_busy = false;
        }
        try {
            // Defer the GPU upload while the filmstrip transition is still
            // running — the heavy decode already happened on a worker.
            if (m_filmstrip.animating()) {
                m_pending_image = decoded;
                m_pending_path = m_current_path;
                return 0;
            }
            if (!m_renderer.upload_image(decoded.Get())) {
                // Upload failed (typically device loss): keep the previous
                // bitmap and restore its identity.
                m_current_path = prev_path;
                m_current_idx = prev_idx;
                return 0;
            }
            m_current_wic = decoded;
            m_has_image = true;
            m_renderer.clear_placeholder();
            m_placeholder_idx = -1;
            update_content_viewport(false);
            fit_to_window();
            preload_neighbors();
            m_window.invalidate();
        } catch (...) {
            // Keep the previous image on decode/materialize failure.
        }
        return 0;
    }

    case WM_RENDER_RETRY:
        // Device loss recovery: recreate only after a short delay so the
        // old flip-model swapchain fully detaches from the DWM (an
        // immediate CreateSwapChainForHwnd fails with E_ACCESSDENIED
        // forever and the window stays black).
        if (!m_render_retry_timer) {
            m_render_retry_timer =
                SetTimer(hwnd, kRenderRetryTimerId, dt::kDurationRenderRetryMs, nullptr);
        }
        return 0;

    case WM_DPICHANGED: {
        float dpi = static_cast<float>(LOWORD(wp));
        m_renderer.set_dpi(dpi, dpi);
        apply_dpi_layout(dpi);
        m_grid_layout_dirty = true;
        update_content_viewport(false);
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

    case WM_CONTEXTMENU: {
        int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);  // screen coords
        POINT pt = {cx, cy};
        ScreenToClient(hwnd, &pt);  // convert to client coords for hit-test
        // Album panel (收藏 tab): row or empty-space menu
        if (nav_panel_visible() && pt.x < nav_panel_width()
            && pt.y >= m_toolbar_h
            && m_nav_panel_state.tab() == NavPanelTab::Favorites) {
            const auto& g = m_nav_panel_geometry;
            if (g.w > 0.0f && pt.y >= g.tree_y
                && pt.y < g.tree_y + g.tree_h) {
                if (m_album_store.folder_view != AlbumFolderView::Tree) {
                    const int cell_row = album_icon_hit(pt.x, pt.y);
                    if (cell_row >= 0) {
                        const auto& row = m_album_rows
                            [static_cast<size_t>(cell_row)];
                        m_album_menu_target =
                            {row.album_index, row.folder_index, true};
                        show_album_row_menu(hwnd, cx, cy);
                        return 0;
                    }
                }
                const int idx = album_row_hit(pt.x, pt.y);
                if (idx >= 0) {
                    const auto& row =
                        m_album_rows[static_cast<size_t>(idx)];
                    if (row.kind == AlbumPanelRow::Kind::Album) {
                        m_album_menu_target =
                            {row.album_index, -1, false};
                        show_album_row_menu(hwnd, cx, cy);
                    } else if (row.kind == AlbumPanelRow::Kind::Folder) {
                        m_album_menu_target =
                            {row.album_index, row.folder_index, true};
                        show_album_row_menu(hwnd, cx, cy);
                    }
                } else {
                    m_album_menu_target = {-1, -1, false};
                    show_album_row_menu(hwnd, cx, cy);
                }
                return 0;
            }
        }
        if (m_grid_mode) {
            // Right-click: select item under cursor and show menu
            if (!grid_click(pt.x, pt.y, false, false))
                return 0;  // no menu on empty space
            show_context_menu(hwnd, cx, cy);  // screen coords for popup
        } else {
            show_context_menu(hwnd, cx, cy);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (m_comic_reader.enabled()) {
            cancel_comic_auto_scroll(ComicAppCancelTrigger::MouseWheel);
        }
        float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        POINT wheel_pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &wheel_pt);
        // Left navigation panel wheel → tree scroll
        if (nav_panel_visible() && wheel_pt.x < nav_panel_width()
            && wheel_pt.y >= m_toolbar_h) {
            nav_tree_scroll(delta);
            return 0;
        }
        // Wheel over the main area releases any navigation-panel focus.
        if (m_nav_panel_state.focused()) {
            m_nav_panel_state.release_focus();
            m_window.invalidate();
        }
        int panel_w = visible_panel_width();
        int panel_x = static_cast<int>(m_renderer.target_size().width) - panel_w;
        if (panel_w > 0 && wheel_pt.x >= panel_x) {
            m_panel_scroll_y -= delta * 40.0f;
            if (m_panel_scroll_y < 0) m_panel_scroll_y = 0;
            float panel_top = m_renderer.content_top();
            float max_scroll = std::max(0.0f,
                m_panel_total_h - (m_renderer.target_size().height - panel_top));
            if (m_panel_scroll_y > max_scroll) m_panel_scroll_y = max_scroll;
            m_window.invalidate();
            return 0;
        }
        if (m_grid_mode) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

            if (ctrl) {
                // Ctrl+Wheel = zoom thumbnail size
                m_thumb_zoom = std::clamp(m_thumb_zoom + delta * 0.1f, 0.4f, 3.0f);
                m_grid_layout_dirty = true;
                m_window.invalidate();
                return 0;
            }

            m_grid_scroll_y -= static_cast<int>(delta * 60);
            if (m_grid_scroll_y < 0) m_grid_scroll_y = 0;
            int max_scroll = std::max(0, m_grid_total_h - (static_cast<int>(m_renderer.target_size().height) - m_grid_top));
            if (m_grid_scroll_y > max_scroll) m_grid_scroll_y = max_scroll;
            begin_grid_scroll(hwnd);
            m_window.invalidate();
            return 0;
        }
        if (!m_has_image) return 0;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (m_comic_reader.enabled()) {
            if (ctrl) {
                adjust_comic_width(delta * 0.10f);
            } else {
                const float dpi_scale =
                    static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
                m_comic_reader.scroll_by(-delta * 80.0f * dpi_scale);
                sync_comic_current();
                request_comic_pages();
                m_window.invalidate();
            }
            return 0;
        }

        // Shift+wheel: horizontal filmstrip scroll while hovering the strip;
        // no-op elsewhere in large-image mode (NAVIGATION_DESIGN 2.1.3).
        // The plain wheel below keeps its existing semantics untouched.
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
            if (filmstrip_visible()) {
                const D2D1_RECT_F fs = filmstrip_rect();
                if (wheel_pt.x >= fs.left && wheel_pt.x < fs.right
                    && wheel_pt.y >= fs.top && wheel_pt.y < fs.bottom) {
                    const int before = m_filmstrip.current();
                    // Wheel direction: scrolling down (delta<0) advances to
                    // the next image (to the right), like the plain wheel.
                    m_filmstrip.scroll_by(-delta);
                    // Wheel browsing steps the current item and keeps it
                    // centered; sync the big view with the new selection.
                    if (m_filmstrip.current() != before
                        && m_filmstrip.current() != m_current_idx) {
                        navigate_to(m_filmstrip.current());
                    }
                    m_window.invalidate();
                }
            }
            return 0;
        }

        if (ctrl) {
            // Ctrl+Wheel = zoom (cursor-centered)
            float factor = (delta > 0) ? 1.15f : 1.0f / 1.15f;
            uint32_t iw, ih; m_renderer.image_size(iw, ih);
            if (iw == 0 || ih == 0) return 0;
            float old_scale = m_renderer.scale();
            float new_scale = std::clamp(old_scale * factor, m_renderer.fit_scale(), 100.0f);
            if (new_scale == old_scale) return 0;
            D2D1_SIZE_U ts = m_renderer.target_size();
            float view_w = m_renderer.content_width();
            float view_top = m_renderer.content_top();
            float view_h = static_cast<float>(ts.height) - view_top;
            float img_x = (view_w - iw * old_scale) / 2.0f + m_renderer.offset_x();
            float img_y = view_top + (view_h - ih * old_scale) / 2.0f
                + m_renderer.offset_y() + m_renderer.scroll_y();
            float img_cx = (wheel_pt.x - img_x) / old_scale;
            float img_cy = (wheel_pt.y - img_y) / old_scale;
            float new_img_x = wheel_pt.x - img_cx * new_scale;
            float new_img_y = wheel_pt.y - img_cy * new_scale;
            m_renderer.set_scale(new_scale);
            m_renderer.set_offset(
                new_img_x - (view_w - iw * new_scale) / 2.0f,
                new_img_y - view_top - (view_h - ih * new_scale) / 2.0f
                    - m_renderer.scroll_y());
        } else {
            uint32_t iw, ih; m_renderer.image_size(iw, ih);
            if (iw == 0 || ih == 0) return 0;
            float cur_scale = m_renderer.scale();
            float fit = m_renderer.fit_scale();
            bool zoomed = is_image_zoomed(cur_scale, fit);

            if (zoomed) {
                // Zoomed in: scroll vertically, clamped to image bounds
                float scaled_h = ih * cur_scale;
                float win_h = static_cast<float>(m_renderer.target_size().height);
                float view_top = m_renderer.content_top();
                float center_y = view_top + (win_h - view_top - scaled_h) / 2.0f
                    + m_renderer.offset_y();
                float sy = m_renderer.scroll_y() - delta * 50.0f;
                // Clamp to the image viewport below the toolbar.
                float max_scroll = view_top - center_y;
                float min_scroll = win_h - scaled_h - center_y; // image bottom at window bottom
                if (sy > max_scroll) sy = max_scroll;
                if (sy < min_scroll) sy = min_scroll;
                m_renderer.set_scroll_y(sy);
            } else {
                // Not zoomed: flip images. Scroll down (delta<0) advances
                // to the next image; scroll up goes to the previous one.
                navigate_to(m_current_idx + (delta < 0 ? 1 : -1));
                return 0;
            }
        }
        m_window.invalidate();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (m_comic_reader.middle_autoscroll_active()) {
            cancel_comic_auto_scroll(ComicAppCancelTrigger::LeftButton);
            return 0;
        }
        if (m_comic_reader.cruise_active()) {
            cancel_comic_auto_scroll(ComicAppCancelTrigger::ManualInput);
        }
        // Page-width slider: drag adjusts the page width directly.
        if (m_comic_reader.enabled()) {
            const ComicControlsLayout controls =
                build_comic_controls_layout(comic_controls_snapshot());
            if (hit_test_comic_width_slider(controls.width_slider,
                    static_cast<float>(GET_X_LPARAM(lp)),
                    static_cast<float>(GET_Y_LPARAM(lp)))) {
                m_comic_width_dragging = true;
                apply_comic_width_slider_x(controls,
                    static_cast<float>(GET_X_LPARAM(lp)));
                SetCapture(hwnd);
                return 0;
            }
        }
        // Title bar: buttons → menu items → drag
        {
            int ty2 = GET_Y_LPARAM(lp);
            float ts = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
            int th = static_cast<int>(m_title_h * ts);
            if (toolbar_visible() && ty2 < th) {
                int tx2 = GET_X_LPARAM(lp);
                float tw2 = static_cast<float>(m_renderer.target_size().width);
                const TitleBarLayout title{0.0f, tw2, static_cast<float>(th), ts};

                // Window buttons
                const int btn = title_bar_window_button_at(title,
                    static_cast<float>(tx2));
                if (btn >= 0) {
                    m_title_btn_press = btn;
                    m_window.invalidate();
                    return 0;
                }

                // Menu items (after "MinView" title)
                std::vector<float> menu_widths;
                menu_widths.reserve(m_toolbar_items.size());
                const float fsize = title_bar_menu_font_size(title);
                for (const auto& item : m_toolbar_items)
                    menu_widths.push_back(m_renderer.measure_text(item, fsize));
                const std::vector<TitleBarMenuBound> menu_bounds =
                    title_bar_menu_bounds(title, menu_widths);
                m_toolbar_active = -1;
                for (int i = 0; i < static_cast<int>(menu_bounds.size()); ++i) {
                    if (tx2 >= static_cast<int>(menu_bounds[static_cast<size_t>(i)].left)
                        && tx2 < static_cast<int>(menu_bounds[static_cast<size_t>(i)].right)) {
                        m_toolbar_active = i;
                        POINT pt = {
                            static_cast<int>(menu_bounds[static_cast<size_t>(i)].left),
                            th};
                        ClientToScreen(hwnd, &pt);
                        show_toolbar_menu(hwnd, i, pt.x, pt.y);
                        m_window.invalidate();
                        return 0;
                    }
                }
                return 0;  // title bar drag area
            }
        }
        // Below title bar: panel, grid clicks
        int ty = GET_Y_LPARAM(lp);
        if (toolbar_visible() && ty < m_toolbar_h) {
            return 0;  // title bar area already handled above
        }
        // Left navigation panel + grid breadcrumb clicks (Issue #5 P2)
        if (nav_panel_visible() && GET_X_LPARAM(lp) < nav_panel_width()) {
            if (nav_panel_hit_test(GET_X_LPARAM(lp), ty)) return 0;
        }
        // Clicking outside the navigation panel returns focus to the main
        // area — otherwise a stray panel hover/click leaves the panel
        // focused and main-area shortcuts (Space/N/D/S/R/A/L) stop working.
        if (m_nav_panel_state.focused()) {
            m_nav_panel_state.release_focus();
            m_window.invalidate();
        }
        if (m_grid_mode
            && grid_breadcrumb_hit_test(GET_X_LPARAM(lp), ty)) {
            return 0;
        }
        if (m_comic_reader.enabled()) {
            const ComicControlsLayout controls =
                build_comic_controls_layout(comic_controls_snapshot());
            const ComicScrollbarHit hit = hit_test_comic_scrollbar(
                controls.scrollbar,
                static_cast<float>(GET_X_LPARAM(lp)),
                static_cast<float>(ty));
            if (hit != ComicScrollbarHit::None) {
                cancel_comic_auto_scroll(ComicAppCancelTrigger::Scrollbar);
                m_comic_scrollbar_hover = true;
                if (hit == ComicScrollbarHit::Thumb) {
                    m_comic_scrollbar_grab_offset_y =
                        static_cast<float>(ty) - controls.scrollbar.thumb.top;
                    m_comic_scrollbar_dragging = true;
                    SetCapture(hwnd);
                } else {
                    const ComicScrollDirection direction =
                        hit == ComicScrollbarHit::PageBackward
                        ? ComicScrollDirection::Backward
                        : ComicScrollDirection::Forward;
                    m_comic_reader.scrollbar_page_step(direction);
                    sync_comic_current();
                    request_comic_pages();
                }
                m_window.invalidate();
                return 0;
            }
        }
        // Filmstrip: click jumps to the item; any click inside the strip is
        // consumed (never starts an OLE drag, spec 2.1.3 / 2.3).
        if (filmstrip_visible()) {
            int fs_hit = filmstrip_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (fs_hit >= -1) {
                if (fs_hit >= 0 && fs_hit != m_current_idx) navigate_to(fs_hit);
                return 0;
            }
        }
        // Panel value click → copy + brief highlight + toast
        if (m_panel_expanded && !m_panel_clickable.empty()) {
            int tx = GET_X_LPARAM(lp);
            for (int i = 0; i < static_cast<int>(m_panel_clickable.size()); ++i) {
                auto& pc = m_panel_clickable[i];
                int cty = GET_Y_LPARAM(lp);
                if (tx >= static_cast<int>(pc.rect.left) && tx <= static_cast<int>(pc.rect.right)
                    && cty >= static_cast<int>(pc.rect.top) && cty <= static_cast<int>(pc.rect.bottom))
                {
                    m_panel_sel = i;
                    if (m_sel_timer) KillTimer(hwnd, m_sel_timer);
                    m_sel_timer = SetTimer(hwnd, 2, dt::kDurationSelectionHighlightMs, nullptr);
                    // Copy to clipboard
                    if (OpenClipboard(hwnd)) {
                        EmptyClipboard();
                        size_t sz = (pc.text.size() + 1) * sizeof(wchar_t);
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sz);
                        if (hMem) {
                            wchar_t* p = static_cast<wchar_t*>(GlobalLock(hMem));
                            if (p) { wmemcpy(p, pc.text.c_str(), pc.text.size() + 1); GlobalUnlock(hMem); }
                            SetClipboardData(CF_UNICODETEXT, hMem);
                        }
                        CloseClipboard();
                    }
                    // Toast: "已复制" + label
                    m_panel_copied = L"\u5df2\u590d\u5236" + pc.label;  // 已复制
                    if (m_toast_timer) KillTimer(hwnd, m_toast_timer);
                    m_toast_timer = SetTimer(hwnd, 3, dt::kDurationToastMs, nullptr);
                    m_window.invalidate();
                    return 0;
                }
            }
        }
        // Scrollbar click in grid mode?
        if (m_grid_mode) {
            int sb_zone2 = static_cast<int>(layout::kScrollbarZoneDip * static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
            int sb_x = static_cast<int>(m_renderer.target_size().width) - visible_panel_width() - sb_zone2;
            int sx = GET_X_LPARAM(lp);
            if (sx >= sb_x && sx < sb_x + sb_zone2 && ty >= m_grid_top &&
                ty < static_cast<int>(m_renderer.target_size().height)) {
                handle_scrollbar_click(hwnd, sx, ty);
                return 0;
            }
        }
    }
        if (m_grid_mode) {
            bool sd = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool cd = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            int clicked = grid_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (clicked < 0) return 0;
            bool clicked_selected = clicked < static_cast<int>(m_selected.size())
                && m_selected[static_cast<size_t>(clicked)];
            if (should_preserve_selection_for_drag(clicked_selected, sd, cd)) {
                m_grid_sel = clicked;
                m_drag_deferred_select = clicked;
                m_window.invalidate();
            } else {
                select_item(clicked, sd, cd);
                m_drag_deferred_select = -1;
            }
        } else {
            m_drag_deferred_select = -1;
        }
        m_drag_paths = selected_paths();
        if (m_drag_paths.empty()) return -1;
        m_drag_start_x = GET_X_LPARAM(lp);
        m_drag_start_y = GET_Y_LPARAM(lp);
        m_drag_pending = true;
        SetCapture(hwnd);
        return 0;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK: {
        if (!m_comic_reader.enabled()) return -1;
        const float pointer_x = static_cast<float>(GET_X_LPARAM(lp));
        const float pointer_y = static_cast<float>(GET_Y_LPARAM(lp));
        ComicControlsRenderInput prospective = comic_controls_snapshot();
        prospective.middle_autoscroll_active = true;
        prospective.autoscroll_anchor_x = pointer_x;
        prospective.autoscroll_anchor_y = pointer_y;
        prospective.autoscroll_pointer_x = pointer_x;
        prospective.autoscroll_pointer_y = pointer_y;
        const bool anchor_visible =
            build_comic_controls_layout(prospective).autoscroll.visible;
        (void)start_comic_middle(
            pointer_x, pointer_y, pointer_x, pointer_y, anchor_visible);
        return 0;
    }

    case WM_NCMOUSEMOVE:
        // Non-client mouse move (title bar area): clear menu hover
        if (m_toolbar_active >= 0) {
            m_toolbar_active = -1;
            m_window.invalidate();
        }
        return 0;

    case WM_MOUSEMOVE:
        if (m_fullscreen && !m_grid_mode) {
            int mouse_y = GET_Y_LPARAM(lp);
            bool reveal = m_toolbar_revealed;
            if (!reveal && mouse_y <= 2) reveal = true;
            else if (reveal && mouse_y > m_toolbar_h + 8) reveal = false;
            if (reveal != m_toolbar_revealed) {
                m_toolbar_revealed = reveal;
                update_content_viewport(false);
                m_window.invalidate();
            }
        }
        // Filmstrip: any mouse movement in large-image mode (windowed AND
        // fullscreen) raises the strip from the bottom (animated); 3s of
        // stillness auto-hides it. Ignore motion while the grid->image
        // transition is running — the post-transition auto-rise owns that
        // window, so the reveal animation is never consumed invisibly.
        if (!m_animating && filmstrip_showable()) {
            if (!m_filmstrip_revealed && !m_filmstrip_reveal_animating) {
                reveal_filmstrip();
                m_window.invalidate();
            } else if (m_filmstrip_revealed && !m_filmstrip_reveal_animating) {
                cancel_filmstrip_hide();
                schedule_filmstrip_hide();
            }
        }
        if (m_comic_reader.middle_autoscroll_active()) {
            m_comic_autoscroll_pointer_x =
                static_cast<float>(GET_X_LPARAM(lp));
            m_comic_autoscroll_pointer_y =
                static_cast<float>(GET_Y_LPARAM(lp));
            m_window.invalidate();
            return 0;
        }
        if (m_comic_width_dragging) {
            const ComicControlsLayout controls =
                build_comic_controls_layout(comic_controls_snapshot());
            apply_comic_width_slider_x(controls,
                static_cast<float>(GET_X_LPARAM(lp)));
            return 0;
        }
        if (m_comic_scrollbar_dragging) {
            const ComicControlsLayout controls =
                build_comic_controls_layout(comic_controls_snapshot());
            const ComicScrollbarDragResult mapped = map_comic_scrollbar_drag(
                controls.scrollbar,
                static_cast<float>(GET_Y_LPARAM(lp)),
                m_comic_scrollbar_grab_offset_y);
            if (mapped.valid) {
                m_comic_reader.set_scroll_from_scrollbar(mapped.scroll_y);
                sync_comic_current();
                request_comic_pages();
                m_window.invalidate();
            }
            return 0;
        }
        // Scrollbar drag
        if (m_scrollbar_dragging) {
            int dy = GET_Y_LPARAM(lp) - m_scrollbar_drag_y;
            int view_h = static_cast<int>(m_renderer.target_size().height);
            int sb_h = view_h - m_grid_top;
            float total = static_cast<float>(m_grid_total_h);
            float view  = static_cast<float>(view_h);
            if (total > view) {
                float move_ratio = static_cast<float>(dy) / static_cast<float>(sb_h);
                float new_pos = static_cast<float>(m_scrollbar_drag_pos) + move_ratio * total;
                m_grid_scroll_y = static_cast<int>(std::clamp(new_pos, 0.0f, total - view));
                m_window.invalidate();
            }
            return 0;
        }
        // Scrollbar hover tracking (for cursor + highlight)
        if (m_grid_mode) {
            int ty2 = GET_Y_LPARAM(lp);
            int sb_zone2 = static_cast<int>(layout::kScrollbarZoneDip * static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
            int sb_x2 = static_cast<int>(m_renderer.target_size().width) - visible_panel_width() - sb_zone2;
            int tx2 = GET_X_LPARAM(lp);
            bool in_sb = (tx2 >= sb_x2 && tx2 < sb_x2 + sb_zone2 && ty2 >= m_grid_top &&
                          ty2 < static_cast<int>(m_renderer.target_size().height));
            if (in_sb != m_scrollbar_hover) {
                m_scrollbar_hover = in_sb;
                m_window.invalidate();
            }
        } else if (m_comic_reader.enabled()) {
            const ComicControlsLayout controls =
                build_comic_controls_layout(comic_controls_snapshot());
            const bool hovered = hit_test_comic_scrollbar(
                controls.scrollbar,
                static_cast<float>(GET_X_LPARAM(lp)),
                static_cast<float>(GET_Y_LPARAM(lp)))
                != ComicScrollbarHit::None;
            if (hovered != m_comic_scrollbar_hover) {
                m_comic_scrollbar_hover = hovered;
                m_window.invalidate();
            }
        }
        // Nav panel hover tracking (breadcrumb segments + tree rows)
        nav_panel_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        // Menu hover tracking (in title bar)
        {
            int ty = GET_Y_LPARAM(lp);
            int prev = m_toolbar_active;
            m_toolbar_active = -1;
            if (toolbar_visible() && ty >= 0 && ty < m_toolbar_h) {
                float dpi_m = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
                int tx = GET_X_LPARAM(lp);
                const TitleBarLayout title{
                    0.0f,
                    static_cast<float>(m_renderer.target_size().width),
                    static_cast<float>(m_toolbar_h),
                    dpi_m};
                std::vector<float> menu_widths;
                menu_widths.reserve(m_toolbar_items.size());
                const float fs = title_bar_menu_font_size(title);
                for (const auto& item : m_toolbar_items)
                    menu_widths.push_back(m_renderer.measure_text(item, fs));
                const std::vector<TitleBarMenuBound> menu_bounds =
                    title_bar_menu_bounds(title, menu_widths);
                for (int i = 0; i < static_cast<int>(menu_bounds.size()); ++i) {
                    const auto& bound = menu_bounds[static_cast<size_t>(i)];
                    if (tx >= static_cast<int>(bound.left)
                        && tx < static_cast<int>(bound.right)) {
                        m_toolbar_active = i;
                        break;
                    }
                }
            }
            if (m_toolbar_active != prev) m_window.invalidate();
        }
        if (m_drag_pending) {
            int dx = GET_X_LPARAM(lp) - m_drag_start_x;
            int dy = GET_Y_LPARAM(lp) - m_drag_start_y;
            if (dx*dx + dy*dy >= 16) {
                m_drag_pending = false;
                if (!m_drag_paths.empty()) {
                    ReleaseCapture();
                    FileDataObject* data = new FileDataObject(m_drag_paths);
                    SimpleDropSource* src = new SimpleDropSource();
                    DWORD effect;
                    DoDragDrop(data, src, DROPEFFECT_COPY, &effect);
                    data->Release();
                    src->Release();
                    m_drag_paths.clear();
                    m_drag_deferred_select = -1;
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }
                return 0;
            }
            return 0;
        }
        // Title button hover
        {
            int ty3 = GET_Y_LPARAM(lp);
            float ts2 = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
            int th2 = static_cast<int>(m_title_h * ts2);
            int prev_btn = m_title_btn_hover;
            m_title_btn_hover = -1;
            if (toolbar_visible() && ty3 >= 0 && ty3 < th2) {
                int tx3 = GET_X_LPARAM(lp);
                float tw3 = static_cast<float>(m_renderer.target_size().width);
                const TitleBarLayout title{
                    0.0f, tw3, static_cast<float>(th2), ts2};
                m_title_btn_hover = title_bar_window_button_at(
                    title, static_cast<float>(tx3));
            }
            if (m_title_btn_hover != prev_btn) m_window.invalidate();
        }
        return -1;

    case WM_LBUTTONUP:
        if (m_title_btn_press >= 0) {
            int id = m_title_btn_press;
            m_title_btn_press = -1;
            m_window.invalidate();
            if (id == 2)      PostMessage(hwnd, WM_CLOSE, 0, 0);
            else if (id == 1) {
                WINDOWPLACEMENT wp2 = {sizeof(WINDOWPLACEMENT)};
                GetWindowPlacement(hwnd, &wp2);
                ShowWindow(hwnd, wp2.showCmd == SW_MAXIMIZE ? SW_RESTORE : SW_MAXIMIZE);
            }
            else if (id == 0) ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }
        if (m_comic_width_dragging) {
            m_comic_width_dragging = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            m_window.invalidate();
            return 0;
        }
        if (m_comic_scrollbar_dragging) {
            finish_comic_scrollbar_drag();
            m_window.invalidate();
            return 0;
        }
        if (m_scrollbar_dragging) {
            m_scrollbar_dragging = false;
            ReleaseCapture();
            return 0;
        }
        if (m_drag_pending) {
            m_drag_pending = false;
            m_drag_paths.clear();
            ReleaseCapture();
            if (m_drag_deferred_select >= 0)
                select_item(m_drag_deferred_select, false, false);
        }
        m_drag_deferred_select = -1;
        return 0;

    case WM_LBUTTONDBLCLK:
        if (m_animating) {
            interrupt_transition(mv::TransitionTrigger::DoubleClick, 0);
            return 0;
        }
        // Double-click on the filmstrip: no-op (single click already jumps).
        if (filmstrip_visible()
            && filmstrip_hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) >= -1) {
            return 0;
        }
        if (m_grid_mode) {
            GridEntryRouteState route_state;
            route_state.grid_mode = true;
            route_state.animating = m_animating;
            route_state.selected_index = m_grid_sel;
            route_state.hit_index = grid_hit_test(
                GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            route_state.item_count = static_cast<int>(m_index.size());
            const auto request = route_grid_entry(
                GridEntryTrigger::DoubleClick, route_state);
            if (request) enter_grid_image(hwnd, *request);
            return 0;
        }
        if (m_comic_reader.enabled() && !leave_comic_reader(true)) return 0;
        if (route_grid_exit(GridExitTrigger::DoubleClick,
                GridExitRouteState{m_animating, m_from_grid, m_has_image})) {
            m_from_grid = false;
            start_transition(hwnd, false);
            begin_animation(hwnd);
            toggle_grid();
            return 0;
        }
        return 0;

    case WM_IME_STARTCOMPOSITION:
        m_ime_composing = true;
        return -1;

    case WM_IME_ENDCOMPOSITION:
        m_ime_composing = false;
        return -1;

    case WM_KILLFOCUS:
        m_ime_composing = false;
        cancel_comic_auto_scroll(ComicAppCancelTrigger::FocusLost);
        finish_comic_scrollbar_drag();
        return -1;

    case WM_ACTIVATEAPP:
        if (!wp) {
            cancel_comic_auto_scroll(ComicAppCancelTrigger::FocusLost);
            finish_comic_scrollbar_drag();
        }
        return -1;

    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        if (m_comic_reader.middle_autoscroll_active()) {
            cancel_comic_auto_scroll(ComicAppCancelTrigger::FocusLost);
        }
        finish_comic_scrollbar_drag();
        return -1;

    case WM_KEYDOWN: {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;

        if (m_comic_reader.middle_autoscroll_active() && wp != 'P') {
            const ComicAppCancelTrigger reason = wp == VK_ESCAPE
                ? ComicAppCancelTrigger::Escape
                : ComicAppCancelTrigger::KeyboardPage;
            cancel_comic_auto_scroll(reason);
        }

        DeleteKeyGuards delete_guards;
        delete_guards.shift_down = shift;
        delete_guards.control_down = ctrl;
        delete_guards.main_window_focused =
                GetForegroundWindow() == hwnd && GetFocus() == hwnd;
        delete_guards.ime_composing = m_ime_composing;
        if (m_delete_composition->handle_key(
                static_cast<UINT>(wp), lp, delete_guards))
            return 0;

        // ── Left navigation panel keys (Issue #5 P2) ──
        if (!m_ime_composing && !ctrl && !shift && wp == 'B') {
            toggle_nav_panel();
            return 0;
        }
        if (!m_ime_composing && !ctrl && !shift && wp == VK_TAB) {
            cycle_nav_focus();
            return 0;
        }
        // The directory tree owns arrow/return navigation; the album tab
        // has none, so its clicks release the panel focus and main-area
        // shortcuts (F/N/D/S/R/A/L/Space) keep working there.
        if (m_nav_panel_state.focused()
            && m_nav_panel_state.tab() == NavPanelTab::Directories
            && handle_nav_panel_key(hwnd, wp, ctrl, shift)) {
            return 0;
        }

        if (ctrl) {
            if (m_comic_reader.enabled()) {
                switch (wp) {
                case '0': case VK_NUMPAD0:
                    cancel_comic_auto_scroll(
                        ComicAppCancelTrigger::KeyboardPage);
                    m_comic_reader.reset_width();
                    clear_comic_cache();
                    request_comic_pages();
                    m_window.invalidate();
                    return 0;
                case VK_OEM_PLUS: case VK_ADD:
                    adjust_comic_width(0.10f); return 0;
                case VK_OEM_MINUS: case VK_SUBTRACT:
                    adjust_comic_width(-0.10f); return 0;
                }
            }
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
                copy_image_data();
                return 0;
            case 'R':
                if (can_toggle_recursive(
                        m_grid_mode, m_has_image, m_index.directory()))
                    toggle_recursive();
                return 0;
            }
            return -1;
        }

        switch (wp) {
        case '0': case VK_NUMPAD0:
            if (m_comic_reader.enabled()) {
                cancel_comic_auto_scroll(
                    ComicAppCancelTrigger::KeyboardPage);
                m_comic_reader.reset_width();
                clear_comic_cache();
                request_comic_pages();
                m_window.invalidate();
                return 0;
            }
            return -1;
        case VK_OEM_PLUS: case VK_ADD:
            if (m_comic_reader.enabled()) { adjust_comic_width(0.10f); return 0; }
            return -1;
        case VK_OEM_MINUS: case VK_SUBTRACT:
            if (m_comic_reader.enabled()) { adjust_comic_width(-0.10f); return 0; }
            return -1;
        case VK_ESCAPE:
            if (m_animating) {
                interrupt_transition(mv::TransitionTrigger::Escape, 0);
                return 0;
            }
            if (m_comic_reader.enabled() && !leave_comic_reader(true)) return 0;
            if (route_grid_exit(GridExitTrigger::Escape,
                    GridExitRouteState{m_animating, m_from_grid, m_has_image})) {
                m_from_grid = false;
                start_transition(hwnd, false);
                begin_animation(hwnd);
                toggle_grid();
                return 0;
            }
            if (m_fullscreen) { toggle_fullscreen(hwnd); return 0; }
            return 0;
        case VK_F11:   toggle_fullscreen(hwnd); return 0;
        case VK_SPACE: {
            if (m_animating) {
                interrupt_transition(mv::TransitionTrigger::Space, 0);
                return 0;
            }
            if (m_comic_reader.enabled() && !leave_comic_reader(true)) return 0;
            if (route_grid_exit(GridExitTrigger::Space,
                    GridExitRouteState{m_animating, m_from_grid, m_has_image})) {
                m_from_grid = false;
                start_transition(hwnd, false);
                begin_animation(hwnd);
                toggle_grid();
                return 0;
            }
            GridEntryRouteState route_state;
            route_state.grid_mode = m_grid_mode;
            route_state.animating = m_animating;
            route_state.selected_index = m_grid_sel;
            route_state.item_count = static_cast<int>(m_index.size());
            const auto request = route_grid_entry(
                GridEntryTrigger::Space, route_state);
            if (request) {
                enter_grid_image(hwnd, *request);
                return 0;
            }
            return 0;  // in normal image mode, do nothing
        }
        case VK_BACK:
            if (m_comic_reader.enabled()) {
                cancel_comic_auto_scroll(
                    ComicAppCancelTrigger::KeyboardPage);
            }
            navigate_to(m_current_idx - 1);
            return 0;
        case VK_RETURN:
            if (m_has_image || m_grid_mode) { toggle_fullscreen(hwnd); return 0; }
            return -1;
        case 'A':
            if (m_grid_mode) { toggle_thumb_square(); return 0; }
            return -1;
        case 'L':
            return toggle_grid_labels() ? 0 : -1;
        case 'F':
            if (!m_ime_composing) { toggle_favourite_current(); return 0; }
            return -1;
        case 'I':
            toggle_info(); return 0;
        case 'M':
            toggle_comic_reader(); return 0;
        case 'P':
            return dispatch_comic_command(ComicAppCommand::ToggleCruise) ? 0 : -1;
        case VK_OEM_4:
            return dispatch_comic_command(ComicAppCommand::DecreaseSpeed) ? 0 : -1;
        case VK_OEM_6:
            return dispatch_comic_command(ComicAppCommand::IncreaseSpeed) ? 0 : -1;
        case 'N':
            if (!ctrl && m_grid_mode) { set_sort_mode(SortMode::Name); return 0; }
            return -1;
        case 'D':
            if (!ctrl && m_grid_mode) { set_sort_mode(SortMode::Date); return 0; }
            return -1;
        case 'S':
            if (!ctrl && m_grid_mode) { set_sort_mode(SortMode::Size); return 0; }
            return -1;
        case 'R':
            if (!ctrl && m_grid_mode) { set_sort_mode(SortMode::Random); return 0; }
            return -1;
        case VK_LEFT:
            if (m_animating) {
                interrupt_transition(mv::TransitionTrigger::ArrowLeft, -1);
                return 0;
            }
            if (m_grid_mode) { grid_navigate(-1, shift); return 0; }
            if (m_comic_reader.enabled()) {
                cancel_comic_auto_scroll(
                    ComicAppCancelTrigger::KeyboardPage);
            }
            navigate_to(m_current_idx - 1); return 0;
        case VK_RIGHT:
            if (m_animating) {
                interrupt_transition(mv::TransitionTrigger::ArrowRight, 1);
                return 0;
            }
            if (m_grid_mode) { grid_navigate(1, shift); return 0; }
            if (m_comic_reader.enabled()) {
                cancel_comic_auto_scroll(
                    ComicAppCancelTrigger::KeyboardPage);
            }
            navigate_to(m_current_idx + 1); return 0;
        case VK_UP:
            if (m_grid_mode) { grid_navigate(-m_grid_cols, shift); return 0; }
            return -1;
        case VK_DOWN:
            if (m_grid_mode) { grid_navigate(m_grid_cols, shift); return 0; }
            return -1;
        case VK_HOME:
            if (m_grid_mode) { select_item(0, shift, false); grid_ensure_visible(); return 0; }
            if (m_comic_reader.enabled()) {
                cancel_comic_auto_scroll(
                    ComicAppCancelTrigger::KeyboardPage);
                m_comic_reader.home(); sync_comic_current();
                request_comic_pages(); m_window.invalidate(); return 0;
            }
            navigate_to(0); return 0;
        case VK_END:
            if (m_grid_mode) { select_item(static_cast<int>(m_index.size()) - 1, shift, false); grid_ensure_visible(); return 0; }
            if (m_comic_reader.enabled()) {
                cancel_comic_auto_scroll(
                    ComicAppCancelTrigger::KeyboardPage);
                m_comic_reader.end(); sync_comic_current();
                request_comic_pages(); m_window.invalidate(); return 0;
            }
            navigate_to(static_cast<int>(m_index.size()) - 1); return 0;
        case VK_PRIOR:
            if (m_comic_reader.enabled()) {
                cancel_comic_auto_scroll(
                    ComicAppCancelTrigger::KeyboardPage);
                m_comic_reader.page_up(); sync_comic_current();
                request_comic_pages(); m_window.invalidate(); return 0;
            }
            return -1;
        case VK_NEXT:
            if (m_comic_reader.enabled()) {
                cancel_comic_auto_scroll(
                    ComicAppCancelTrigger::KeyboardPage);
                m_comic_reader.page_down(); sync_comic_current();
                request_comic_pages(); m_window.invalidate(); return 0;
            }
            return -1;
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

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT
            && m_comic_reader.middle_autoscroll_active()) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        if (LOWORD(lp) == HTCLIENT && m_comic_reader.enabled()
            && m_comic_scrollbar_hover) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        if (LOWORD(lp) == HTCLIENT && m_grid_mode && m_scrollbar_hover) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        if (LOWORD(lp) == HTCLIENT
            && (m_nav_row_hover >= 0 || m_nav_breadcrumb_hover_panel >= 0
                || m_nav_breadcrumb_hover_grid >= 0)) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;  // fall through to DefWindowProc for other areas
    }
    return -1;
}

// ── Image loading ────────────────────────────────────────────

void App::open_directory(const std::wstring& path) {
    if (path.empty()) return;
    m_renderer.clear_placeholder();
    m_placeholder_idx = -1;
    if (m_comic_reader.enabled()) leave_comic_reader(false);
    if (thumb_loader_running()) stop_thumb_loader();
    finish_grid_scroll();

    // Remember the current collection's sort/recursive state, then cancel
    // any in-flight async nav switch (this entry is synchronous).
    if (!m_index.directory().empty()) {
        m_collection_memory.remember(m_index.directory(),
            {m_index.sort_mode(), m_recursive});
    }
    m_nav_switch.invalidate();
    const CollectionMemory nav_memory =
        m_collection_memory.memory_for(path);
    m_recursive = nav_memory.recursive;

    m_grid_mode = false;
    m_from_grid = false;
    m_has_image = false;
    m_current_path.clear();
    m_current_wic.Reset();
    m_current_idx = -1;
    m_grid_sel = -1;
    m_grid_saved_idx = -1;
    m_grid_scroll_y = 0;
    m_grid_scroll_saved = 0;
    m_selected.clear();
    m_sel_anchor = -1;
    m_thumb_engine.pool()->thumbs.clear();
    m_thumb_d2d.clear();
    m_thumb_d2d_use.clear();
    m_panel_path.clear();
    m_grid_layout_dirty = true;

    bool recursive_empty_root = false;
    if (m_recursive) {
        ImageIndex direct_index;
        int direct_result = direct_index.scan(path, false);
        recursive_empty_root = direct_result == 0;
    }

    int result = m_index.scan(path, m_recursive);
    if (result < 0) {
        update_content_viewport(true);
        m_window.invalidate();
        return;
    }
    m_index.sort_by(nav_memory.sort);
    save_last_dir(path);
    // Restore the last-used sort mode (persisted via sortmode.txt).
    const int saved_sort = load_sort_mode();
    if (saved_sort > 0) {
        m_index.sort_by(static_cast<SortMode>(saved_sort));
    }
    // Preload real dimensions in the background so the initial skeleton grid
    // is laid out at (or near) the true aspect ratios instead of 1:1, which
    // removes the drift when the first thumbnails stream in.
    start_dim_preload();
    if (!m_index.empty()) {
        if (!recursive_empty_root) {
            m_current_idx = 0;
            m_current_path = m_index.path_at(0);
            m_has_image = true;
        }
        toggle_grid();
    } else {
        update_content_viewport(true);
        update_title();
        m_window.invalidate();
    }
    static_cast<void>(complete_directory_open(result, m_open_error));
    sync_nav_collection();
    m_watch_roots = {{path, m_recursive}};
    start_dir_watch();
}

bool App::open_image(const std::wstring& path) {
    if (m_comic_reader.enabled()) leave_comic_reader(false);
    namespace fs = std::filesystem;
    SetLastError(ERROR_SUCCESS);
    DWORD attributes = GetFileAttributesW(path.c_str());
    const DWORD attribute_error = attributes == INVALID_FILE_ATTRIBUTES
        ? GetLastError() : ERROR_SUCCESS;
    const auto route = classify_open_input(
        attributes != INVALID_FILE_ATTRIBUTES,
        attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
        attribute_error, fs::path(path).extension().wstring());
    if (route == OpenInputRoute::OpenDirectory) {
        open_directory(path);
        return true;
    }
    if (route != OpenInputRoute::DecodeImage) {
        show_open_error(route);
        return false;
    }
    try {
        // A recursive grid can contain files from many child directories.  Keep
        // that root index intact when opening one of its existing entries.
        int indexed_position = m_index.index_of(path);

        // Rapid-paging debounce: while the user flips pages faster than the
        // synchronous decode keeps up (<200ms cadence), update the index only
        // (instant) and defer the full decode until paging stops (250ms quiet
        // period). This keeps wheel/keyboard paging responsive.
        const ULONGLONG now_tick = GetTickCount64();
        const bool rapid_paging = m_last_open_tick != 0
            && (now_tick - m_last_open_tick) < 200;
        m_last_open_tick = now_tick;
        if (rapid_paging && m_has_image && !m_grid_mode) {
            m_debounce_path = path;
            m_debounce_idx = indexed_position;
            if (indexed_position >= 0) m_current_idx = indexed_position;
            m_current_path = path;
            // Drop any in-flight decode for an image we just scrolled past:
            // it must not flash on screen while the debounce timer waits.
            {
                std::lock_guard lock(m_async->mutex);
                ++m_async->gen;
                m_async_busy = false;
            }
            m_pending_image.Reset();
            m_pending_path.clear();
            show_placeholder_thumb(indexed_position);
            SetTimer(m_window.handle(), kImageDebounceTimerId, dt::kDurationImageDebounceMs, nullptr);
            m_window.invalidate();
            return true;
        }
        if (!m_debounce_path.empty()) {
            m_debounce_path.clear();
            KillTimer(m_window.handle(), kImageDebounceTimerId);
        }

        // Async decode path: when the target isn't already in the preload
        // cache, hand it to the decode pool so the UI thread never blocks on
        // a 300-500ms decode — filmstrip animation and paging stay smooth
        // while the bitmap lands via WM_IMAGE_READY. The newest page flip
        // replaces any queued current job (stale decodes are dropped by
        // generation) and jumps the queue ahead of neighbor preloads.
        auto cached_preload = get_preloaded(path);
        if (!cached_preload && m_has_image && !m_grid_mode) {
            m_current_path = path;
            if (indexed_position >= 0) m_current_idx = indexed_position;
            {
                std::lock_guard lock(m_async->mutex);
                if (!m_async->queue.empty() && m_async->queue.front().current)
                    m_async->queue.pop_front();
                ++m_async->gen;
                m_async->queue.push_front(AsyncJob{path, m_async->gen, true,
                    m_current_path, m_current_idx});
                m_async_busy = true;
                m_async_started_ms = GetTickCount64();
            }
            for (int i = 0; i < static_cast<int>(m_async->slots.size()); ++i) {
                if (!m_async->slots[i].thread.joinable())
                    start_async_worker(i);
            }
            m_async->cv.notify_all();
            SetTimer(m_window.handle(), kAsyncWatchdogTimerId, 1000, nullptr);
            show_placeholder_thumb(indexed_position);
            m_window.invalidate();
            return true;
        }


        ComPtr<IWICBitmapSource> bitmap;
        const auto resolved_route = resolve_open_input_route(route, [&]() {
            return run_image_load_stages(
                [this, &path, &cached_preload]() {
                    // Reuse the cache entry consumed above instead of
                    // re-decoding: it is already fully materialized by a
                    // worker, so a warm flip costs one GPU upload.
                    auto decoded = std::move(cached_preload);
                    if (!decoded) decoded = m_decoder.decode(path);
                    return decoded;
                },
                [this](const ComPtr<IWICBitmapSource>& decoded) {
                    // Cache hits arrive already materialized (workers did the
                    // full decode); only freshly-decoded sources need it.
                    ComPtr<IWICBitmap> check;
                    if (SUCCEEDED(decoded.As(&check)) && check)
                        return decoded;
                    return m_decoder.materialize(decoded.Get());
                },
                [this, &bitmap](const ComPtr<IWICBitmapSource>& materialized) {
                    bitmap = materialized;
                    return m_renderer.upload_image(bitmap.Get());
                });
        });
        if (resolved_route != OpenInputRoute::DecodeImage) {
            show_open_error(resolved_route);
            return false;
        }

        // Exit grid mode if active (file dialog, drag-drop, IPC)
        if (m_grid_mode) {
            m_grid_scroll_saved = m_grid_scroll_y;
            m_grid_saved_idx = m_grid_sel;
            m_grid_mode = false;
            m_from_grid = true;  // Esc/Space will return to grid
            finish_grid_scroll();
            stop_thumb_loader();
        }
        update_content_viewport(false);
        fit_to_window();
        m_current_wic = bitmap;
        m_renderer.clear_placeholder();
        m_placeholder_idx = -1;
        m_pending_image.Reset();
        m_pending_path.clear();

        fs::path p(path);
        std::wstring dir = p.parent_path().wstring();
        if (dir.empty()) dir = L".";

        if (indexed_position < 0) {
            if (thumb_loader_running()) stop_thumb_loader();
            m_nav_switch.invalidate();  // cancel any in-flight nav switch
            m_index.scan(dir, m_recursive);
            save_last_dir(dir);
            indexed_position = m_index.index_of(path);
        }
        sync_nav_collection();
        commit_current_image_identity(path, indexed_position,
            m_current_path, m_current_idx, m_has_image);
        reset_filmstrip_reveal();  // large-image mode starts with the strip hidden
        m_from_grid = m_current_idx >= 0;
        m_open_error.clear();

        update_title();

        // Preload neighbors in background
        preload_neighbors();

        m_window.invalidate();
        return true;
    } catch (const std::exception&) {
        show_open_error(OpenInputRoute::ReadOrDecodeFailed);
        return false;
    }
}

void App::check_async_timeout() {
    // A slot whose decode() never returned (stuck on a pathological file)
    // is abandoned: its slot generation is bumped so the zombie's late
    // result is dropped, and a fresh worker respawns the slot. Self-heals
    // ~10s instead of hanging forever. Runs on the UI thread.
    std::vector<int> stuck;
    {
        std::lock_guard lock(m_async->mutex);
        const ULONGLONG now = GetTickCount64();
        for (int i = 0; i < static_cast<int>(m_async->slots.size()); ++i) {
            AsyncSlot& s = m_async->slots[i];
            if (!s.busy || now - s.started_ms < kAsyncTimeoutMs) continue;
            ++s.slot_gen;
            if (s.current) m_async_busy = false;
            s.busy = false;
            s.path.clear();
            s.current = false;
            stuck.push_back(i);
        }
    }
    for (int i : stuck) {
        AsyncSlot& s = m_async->slots[i];
        if (s.thread.joinable()) s.thread.detach();
        start_async_worker(i);
    }
}

void App::show_open_error(OpenInputRoute route) {
    m_open_error = open_input_error_message(route);
    SetForegroundWindow(m_window.handle());
    SetFocus(m_window.handle());
    m_window.invalidate();
}

void App::update_title() {
    SetWindowTextW(m_window.handle(), L"MinView");
}

void App::navigate_to(int idx) {
    if (m_index.empty()) return;
    if (idx < 0 || idx >= static_cast<int>(m_index.size())) return;
    if (m_comic_reader.enabled()) {
        m_comic_reader.scroll_to_page(idx);
        sync_comic_current();
        request_comic_pages();
        m_window.invalidate();
        return;
    }
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
    m_collection_memory.remember(dir, {m_index.sort_mode(), m_recursive});

    std::wstring selected_path;
    if (m_grid_mode && m_grid_sel >= 0 && m_grid_sel < static_cast<int>(m_index.size()))
        selected_path = m_index.path_at(m_grid_sel);
    std::vector<std::wstring> selected_before;
    if (m_grid_mode) selected_before = selected_paths();

    const bool was_grid = m_grid_mode;
    if (was_grid) stop_thumb_loader();

    int scan_result = m_index.scan(dir, m_recursive);
    save_last_dir(dir);
    m_watch_roots = {{dir, m_recursive}};
    start_dir_watch();
    const RecursiveScanAction scan_action = classify_recursive_scan_action(
        was_grid, m_has_image, scan_result);

    // Re-locate current image in new index
    if (!m_current_path.empty()) {
        m_current_idx = m_index.index_of(m_current_path);
    }

    // Reset grid thumbnails for new file list
    if (scan_action == RecursiveScanAction::RefreshGrid) {
        m_thumb_engine.pool()->thumbs.clear();
        m_thumb_engine.pool()->thumbs.resize(m_index.size());
        m_thumb_d2d.clear();
        m_thumb_d2d_use.clear();
        m_grid_layout_dirty = true;
        m_last_cached_sel = -1;
        const SelectionRemap selection =
            plan_selection_remap(m_index, selected_before, selected_path);
        m_grid_sel = selection.grid_sel;
        m_selected.assign(m_index.size(), false);
        for (int index : selection.selected)
            m_selected[static_cast<size_t>(index)] = true;
        m_sel_anchor = selection.anchor;
        start_thumb_loader();
        if (m_grid_sel >= 0) grid_ensure_visible();
    } else if (scan_action == RecursiveScanAction::EnterUnselectedGrid) {
        // An empty root can become browsable only after recursive scanning.
        // Enter the grid without manufacturing a default selection.
        m_current_idx = -1;
        m_current_path.clear();
        m_grid_saved_idx = -1;
        m_grid_sel = -1;
        m_has_image = false;
        toggle_grid();
    } else if (scan_action == RecursiveScanAction::ShowEmptyRoot) {
        m_from_grid = false;
        m_has_image = false;
        m_current_path.clear();
        m_current_wic.Reset();
        m_current_idx = -1;
        m_grid_sel = -1;
        m_selected.clear();
        m_sel_anchor = -1;
        if (thumb_loader_running()) stop_thumb_loader();
        m_thumb_engine.pool()->thumbs.clear();
        m_thumb_d2d.clear();
        m_thumb_d2d_use.clear();
        m_panel_path.clear();
        m_grid_scroll_y = 0;
        toggle_grid();
    }

    update_title();
    m_window.invalidate();
    sync_nav_collection();
}

// ── Sort mode ────────────────────────────────────────────────

void App::set_sort_mode(SortMode mode) {
    if (m_index.empty()) return;
    m_collection_memory.remember(m_index.directory(),
        {mode, m_recursive});
    std::wstring current = m_current_path;
    std::wstring selected_path;
    if (m_grid_mode && m_grid_sel >= 0 && m_grid_sel < static_cast<int>(m_index.size()))
        selected_path = m_index.path_at(m_grid_sel);
    std::vector<std::wstring> selected_before;
    if (m_grid_mode) selected_before = selected_paths();

    if (m_grid_mode) stop_thumb_loader();
    m_index.sort_by(mode);

    // Re-locate current image
    if (!current.empty()) {
        m_current_idx = m_index.index_of(current);
    }

    // Reset grid if in grid mode (thumbnails need reload)
    if (m_grid_mode) {
        m_thumb_engine.pool()->thumbs.clear();
        m_thumb_engine.pool()->thumbs.resize(m_index.size());
        m_thumb_d2d.clear();
        m_thumb_d2d_use.clear();
        m_grid_layout_dirty = true;
        m_last_cached_sel = -1;
        const SelectionRemap selection =
            plan_selection_remap(m_index, selected_before, selected_path);
        m_grid_sel = selection.grid_sel;
        m_selected.assign(m_index.size(), false);
        for (int index : selection.selected)
            m_selected[static_cast<size_t>(index)] = true;
        m_sel_anchor = selection.anchor;
        start_thumb_loader();
        if (m_grid_sel >= 0) grid_ensure_visible();
    }

    save_sort_mode(static_cast<int>(mode));
    update_title();
    m_window.invalidate();
}

// ── Preloader ────────────────────────────────────────────────

void App::start_async_pool() {
    m_async = std::make_shared<AsyncPoolState>();
    m_async->slots.resize(kAsyncWorkers);
    for (int i = 0; i < static_cast<int>(m_async->slots.size()); ++i)
        start_async_worker(i);
}

void App::stop_async_pool() {
    if (!m_async) return;
    // Prompt shutdown even when a worker is frozen in decode(): detach all
    // slots instead of joining. Idle workers exit on the stop flag; frozen
    // ones keep the shared pool state alive and are reaped at process exit.
    {
        std::lock_guard lock(m_async->mutex);
        m_async->stop = true;
    }
    m_async->cv.notify_all();
    for (auto& slot : m_async->slots) {
        if (slot.thread.joinable())
            slot.thread.detach();
    }
}

void App::start_async_worker(int slot_index) {
    AsyncSlot& slot = m_async->slots[slot_index];
    if (slot.thread.joinable()) return;  // already running
    const ULONGLONG slot_gen = slot.slot_gen;
    // The worker keeps its own shared_ptr to the pool state and the HWND by
    // value — it never dereferences App, so it stays safe even after App is
    // destroyed (frozen decode at shutdown).
    const std::shared_ptr<AsyncPoolState> pool = m_async;
    const HWND hwnd = m_window.handle();
    try {
        slot.thread = std::thread([pool, hwnd, slot_index, slot_gen]() {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try {
                Decoder decoder;  // each worker owns its WIC factory
                while (true) {
                    AsyncJob job;
                    {
                        std::unique_lock lock(pool->mutex);
                        pool->cv.wait(lock, [&] {
                            return pool->stop || !pool->queue.empty();
                        });
                        if (pool->stop) break;
                        AsyncSlot& s = pool->slots[slot_index];
                        if (s.slot_gen != slot_gen) break;  // replaced — exit
                        if (pool->queue.empty()) continue;
                        job = std::move(pool->queue.front());
                        pool->queue.pop_front();
                        // A current job superseded while queued is stale.
                        if (job.current && job.gen != pool->gen) continue;
                        s.busy = true;
                        s.path = job.path;
                        s.started_ms = GetTickCount64();
                        s.current = job.current;
                    }
                    ComPtr<IWICBitmapSource> decoded;
                    try {
                        decoded = decoder.decode(job.path);
                    } catch (...) {
                        decoded.Reset();
                    }
                    // Full decode (materialize) happens here on the worker:
                    // the UI thread then only uploads the finished bitmap,
                    // keeping flips ~10-50ms instead of 300-500ms.
                    ComPtr<IWICBitmapSource> mat;
                    if (decoded) {
                        if (job.current) {
                            std::lock_guard quick(pool->mutex);
                            if (job.gen != pool->gen) continue;  // superseded
                        }
                        try {
                            mat = decoder.materialize(decoded.Get());
                        } catch (...) {
                            mat.Reset();
                        }
                    }
                    {
                        std::lock_guard lock(pool->mutex);
                        AsyncSlot& s = pool->slots[slot_index];
                        if (s.slot_gen != slot_gen) continue;  // zombie: drop
                        s.busy = false;
                        s.path.clear();
                        s.current = false;
                        if (job.current) {
                            if (!pool->stop && job.gen == pool->gen) {
                                pool->wic = mat;
                                pool->current_prev_path = job.prev_path;
                                pool->current_prev_idx = job.prev_idx;
                            }
                        } else if (mat) {
                            std::lock_guard plock(pool->preload_mutex);
                            const std::uint64_t item_bytes =
                                wic_source_bytes(mat.Get());
                            while (!pool->preload_order.empty()
                                && (pool->preload_cache.size()
                                        >= AsyncPoolState::kPreloadCacheMaxItems
                                    || pool->preload_bytes + item_bytes
                                        > AsyncPoolState::kPreloadCacheBytes)) {
                                std::wstring old = pool->preload_order.front();
                                pool->preload_order.pop_front();
                                auto it = pool->preload_cache.find(old);
                                if (it == pool->preload_cache.end()) continue;
                                pool->preload_bytes -=
                                    wic_source_bytes(it->second.Get());
                                pool->preload_cache.erase(it);
                            }
                            pool->preload_cache[job.path] = mat;
                            pool->preload_order.push_back(job.path);
                            pool->preload_bytes += item_bytes;
                        }
                    }
                    if (job.current)
                        PostMessageW(hwnd, WM_IMAGE_READY,
                            static_cast<WPARAM>(job.gen), 0);
                }
            } catch (...) {
                // Decoder creation failed — slot exits; a later request or
                // watchdog pass respawns it.
            }
            CoUninitialize();
        });
    } catch (const std::exception&) {
        // Thread spawn failed — pool degrades, paging still works.
    }
}

bool App::async_slots_active() {
    if (!m_async) return false;
    std::lock_guard lock(m_async->mutex);
    if (m_async_busy) return true;
    for (const auto& s : m_async->slots)
        if (s.busy) return true;
    return false;
}

void App::request_preload(const std::wstring& path) {
    if (path.empty() || !m_async) return;
    {
        std::lock_guard plock(m_async->preload_mutex);
        if (m_async->preload_cache.count(path)) return;  // already cached
    }
    {
        std::lock_guard lock(m_async->mutex);
        // Skip if already queued or being decoded
        for (const auto& j : m_async->queue)
            if (!j.current && j.path == path) return;
        for (const auto& s : m_async->slots)
            if (s.busy && !s.current && s.path == path) return;
        m_async->queue.push_back(AsyncJob{path, 0, false});
    }
    m_async->cv.notify_one();
    // Keep the watchdog tick alive while only preloads are in flight, so a
    // stuck preload decode still gets abandoned.
    SetTimer(m_window.handle(), kAsyncWatchdogTimerId, 1000, nullptr);
}

void App::show_placeholder_thumb(int idx) {
    // Progressive-loading placeholder (Issue #5-P1 C): while the full decode
    // of a page-flip target is in flight, show its filmstrip thumbnail
    // upscaled — instant visual feedback, replaced by the real image on
    // WM_IMAGE_READY. If the thumbnail isn't ready yet, keep the previous
    // image; WM_THUMB_READY retries the lookup.
    if (idx < 0 || m_grid_mode || m_comic_reader.enabled()) return;
    m_placeholder_idx = idx;
    auto it = m_thumb_d2d.find(idx);
    if (it != m_thumb_d2d.end())
        m_renderer.set_placeholder(it->second.Get());
}

static std::uint64_t wic_source_bytes(IWICBitmapSource* src) {
    if (!src) return 0;
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h))) return 0;
    return static_cast<std::uint64_t>(w) * h * 4;  // 32bpp PBGRA
}

Microsoft::WRL::ComPtr<IWICBitmapSource> App::get_preloaded(const std::wstring& path) {
    if (!m_async) return nullptr;
    std::lock_guard lock(m_async->preload_mutex);
    auto it = m_async->preload_cache.find(path);
    if (it != m_async->preload_cache.end()) {
        auto result = it->second;
        m_async->preload_bytes -= wic_source_bytes(it->second.Get());
        m_async->preload_cache.erase(it);  // consumed
        return result;
    }
    return nullptr;
}

void App::preload_neighbors() {
    // Preload 3 ahead, 3 behind (Issue #5-P1 B). Request order matters for
    // the LRU: negatives first, positives far-to-near — the +1/+2/+3 page-
    // forward targets materialize last, so they are the last to be evicted.
    int total = static_cast<int>(m_index.size());
    for (int offset : {-3, -2, -1, 3, 2, 1}) {
        int idx = m_current_idx + offset;
        if (idx >= 0 && idx < total)
            request_preload(m_index.path_at(idx));
    }
    // Refresh LRU priority every flip: entries that survived from earlier
    // positions would otherwise look "old" and get evicted right before
    // they become the next flip target. Reordering by distance keeps the
    // forward neighbors (+1/+2/+3) protected until the very end.
    if (!m_async) return;
    std::lock_guard plock(m_async->preload_mutex);
    for (int offset : {-3, -2, -1, 3, 2, 1}) {
        int idx = m_current_idx + offset;
        if (idx < 0 || idx >= total) continue;
        const std::wstring& p = m_index.path_at(idx);
        for (auto it = m_async->preload_order.begin();
             it != m_async->preload_order.end(); ++it) {
            if (*it == p) {
                m_async->preload_order.erase(it);
                m_async->preload_order.push_back(p);
                break;
            }
        }
    }
}

// ── Comic reader ─────────────────────────────────────────────

void App::toggle_comic_reader() {
    if (m_animating || m_grid_mode || !m_has_image || m_current_idx < 0
        || m_current_idx >= static_cast<int>(m_index.size())) return;
    if (m_comic_reader.enabled()) {
        leave_comic_reader(true);
        return;
    }

    rebuild_comic_pages();
    update_comic_viewport();
    m_comic_fallback_index = m_current_idx;
    if (!m_comic_reader.enter(m_current_idx)) return;
    (void)m_comic_reader.take_page_change_event();
    sync_comic_current();
    request_comic_pages();
    m_window.invalidate();
}

bool App::leave_comic_reader(bool load_visible_page) {
    if (!m_comic_reader.enabled()) return true;
    reset_comic_controls(ComicAppCancelTrigger::ExitMode);
    const int index = m_comic_reader.exit_current_index();
    std::wstring path;
    if (index >= 0 && index < static_cast<int>(m_index.size())) {
        path = m_index.path_at(static_cast<std::size_t>(index));
    }
    clear_comic_cache();
    m_comic_fallback_index = -1;
    if (!load_visible_page) return true;
    return !path.empty() && open_image(path);
}

void App::rebuild_comic_pages() {
    reset_comic_controls(m_index.empty()
        ? ComicAppCancelTrigger::EmptyBook
        : ComicAppCancelTrigger::ManualInput);
    clear_comic_cache();
    m_comic_fallback_index = -1;
    std::vector<ComicPageSource> pages;
    pages.reserve(m_index.size());
    std::uint32_t current_width = 0;
    std::uint32_t current_height = 0;
    m_renderer.image_size(current_width, current_height);
    for (int index = 0; index < static_cast<int>(m_index.size()); ++index) {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (m_grid_dims.size() == m_index.size()) {
            const auto& dimensions = m_grid_dims[static_cast<std::size_t>(index)];
            width = dimensions.first;
            height = dimensions.second;
        }
        if (index == m_current_idx && current_width > 0 && current_height > 0) {
            width = current_width;
            height = current_height;
        }
        pages.push_back({
            m_index.path_at(static_cast<std::size_t>(index)),
            width, height, false});
    }
    m_comic_pages.clear();
    m_comic_pages.resize(m_index.size());
    m_comic_reader.set_pages(std::move(pages));
}

void App::clear_comic_cache() {
    ++m_comic_generation;
    if (m_comic_generation == 0) ++m_comic_generation;
    m_comic_loader.replace_requests({});
    for (auto& page : m_comic_pages) page = ComicPageEntry{};
    m_comic_lru.clear();
}

void App::update_comic_viewport() {
    const float old_width = m_comic_reader.page_width();
    const float content_top = m_renderer.content_top();
    const float content_height = std::max(
        1.0f, static_cast<float>(m_renderer.target_size().height) - content_top);
    const float dpi_scale = m_window.handle()
        ? static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f
        : 1.0f;
    m_comic_reader.set_viewport({
        m_renderer.content_width(), content_height, dpi_scale});
    if (m_comic_reader.enabled()
        && std::fabs(old_width - m_comic_reader.page_width()) > 1.0f) {
        clear_comic_cache();
        request_comic_pages();
    }
}

void App::adjust_comic_width(float delta) {
    if (!m_comic_reader.enabled()) return;
    cancel_comic_auto_scroll(ComicAppCancelTrigger::ManualInput);
    m_comic_reader.set_width_factor(m_comic_reader.width_factor() + delta);
    clear_comic_cache();
    sync_comic_current();
    request_comic_pages();
    m_window.invalidate();
}

class AppComicPort {
public:
    explicit AppComicPort(App& app) : m_app(app) {}

    bool enabled() const { return m_app.m_comic_reader.enabled(); }

    ComicAppAutoOwner owner() const {
        switch (m_app.m_comic_reader.auto_scroll_owner()) {
        case ComicAutoScrollOwner::Cruise: return ComicAppAutoOwner::Cruise;
        case ComicAutoScrollOwner::Middle: return ComicAppAutoOwner::Middle;
        case ComicAutoScrollOwner::None: return ComicAppAutoOwner::None;
        }
        return ComicAppAutoOwner::None;
    }

    bool toggle_cruise() { return m_app.m_comic_reader.toggle_cruise(); }
    int speed_index() const { return m_app.m_comic_reader.cruise_speed_index(); }
    bool timer_running() const { return m_app.m_comic_timer != 0; }
    bool transient_visible() const {
        return m_app.m_comic_transient_kind
            != ComicTransientOverlayKind::None;
    }

    void set_speed(int index) {
        m_app.m_comic_reader.set_cruise_speed_index(index);
    }

    bool cruise_paused() const {
        return m_app.m_comic_reader.cruise_paused();
    }

    void show_cruise_status(ComicAppCruiseStatus status) {
        m_app.m_comic_transient_kind = ComicTransientOverlayKind::Status;
        if (status == ComicAppCruiseStatus::Paused) {
            m_app.m_comic_transient_text =
                L"\u81EA\u52A8\u6EDA\u52A8 \u00B7 \u5DF2\u6682\u505C (P \u7EE7\u7EED)";
        } else if (status == ComicAppCruiseStatus::Boundary) {
            m_app.m_comic_transient_text =
                L"\u81EA\u52A8\u6EDA\u52A8 \u00B7 \u5DF2\u5230\u8FB9\u754C";
        } else {
            static constexpr const wchar_t* speeds[] = {
                L"0.5x", L"1.0x", L"1.5x", L"2.0x"};
            m_app.m_comic_transient_text = L"\u81EA\u52A8\u6EDA\u52A8 \u00B7 ";
            m_app.m_comic_transient_text += speeds[static_cast<std::size_t>(
                std::clamp(speed_index(), 0, 3))];
        }
        m_app.m_comic_transient_until_ms =
            GetTickCount64() + kComicAppTransientDurationMs;
    }

    void begin_tick_clock() {
        m_app.m_comic_last_tick_ms = GetTickCount64();
        m_app.m_comic_advance_last.QuadPart = 0;  // fresh QPC window
    }

    bool start_middle(
        float anchor_x, float anchor_y, float pointer_x, float pointer_y) {
        if (!m_app.m_comic_reader.start_middle_autoscroll(anchor_y)) return false;
        m_app.m_comic_autoscroll_anchor_x = anchor_x;
        m_app.m_comic_autoscroll_anchor_y = anchor_y;
        m_app.m_comic_autoscroll_pointer_x = pointer_x;
        m_app.m_comic_autoscroll_pointer_y = pointer_y;
        return true;
    }

    bool acquire_middle_capture() {
        HWND hwnd = m_app.m_window.handle();
        if (!hwnd) return false;
        SetCapture(hwnd);
        return GetCapture() == hwnd;
    }

    void release_middle_capture() {
        HWND hwnd = m_app.m_window.handle();
        if (hwnd && GetCapture() == hwnd) ReleaseCapture();
    }

    void set_middle_cursor(bool active) {
        SetCursor(LoadCursor(nullptr, active ? IDC_SIZEALL : IDC_ARROW));
    }

    void cancel_auto_scroll(ComicAppCancelTrigger trigger) {
        ComicAutoScrollCancelReason reason =
            ComicAutoScrollCancelReason::InvalidInput;
        switch (trigger) {
        case ComicAppCancelTrigger::ManualInput:
            reason = ComicAutoScrollCancelReason::ManualInput; break;
        case ComicAppCancelTrigger::Scrollbar:
            reason = ComicAutoScrollCancelReason::Scrollbar; break;
        case ComicAppCancelTrigger::RepeatedMiddleClick:
            reason = ComicAutoScrollCancelReason::RepeatedMiddleClick; break;
        case ComicAppCancelTrigger::LeftButton:
            reason = ComicAutoScrollCancelReason::LeftButton; break;
        case ComicAppCancelTrigger::Escape:
            reason = ComicAutoScrollCancelReason::Escape; break;
        case ComicAppCancelTrigger::KeyboardPage:
            reason = ComicAutoScrollCancelReason::KeyboardPage; break;
        case ComicAppCancelTrigger::MouseWheel:
            reason = ComicAutoScrollCancelReason::MouseWheel; break;
        case ComicAppCancelTrigger::FocusLost:
            reason = ComicAutoScrollCancelReason::FocusLost; break;
        case ComicAppCancelTrigger::ExitMode:
            reason = ComicAutoScrollCancelReason::ExitMode; break;
        case ComicAppCancelTrigger::EmptyBook:
            reason = ComicAutoScrollCancelReason::EmptyBook; break;
        case ComicAppCancelTrigger::ViewportChanged:
            reason = ComicAutoScrollCancelReason::ViewportChanged; break;
        case ComicAppCancelTrigger::InvalidInput:
            reason = ComicAutoScrollCancelReason::InvalidInput; break;
        }
        m_app.m_comic_reader.cancel_auto_scroll(reason);
    }

    float advance_cruise(float elapsed_seconds) {
        return m_app.m_comic_reader.advance_cruise(elapsed_seconds);
    }

    float advance_middle(float elapsed_seconds) {
        return m_app.m_comic_reader.advance_middle_autoscroll(
            m_app.m_comic_autoscroll_pointer_y, elapsed_seconds);
    }

    void sync_page() { m_app.sync_comic_current(); }
    void request_pages() { m_app.request_comic_pages(); }

    void clear_status_transient() {
        if (m_app.m_comic_transient_kind == ComicTransientOverlayKind::Status) {
            m_app.clear_comic_transient();
        }
    }

    void clear_all_transient() { m_app.clear_comic_transient(); }
    bool start_timer() { return m_app.start_comic_timer(); }
    void stop_timer() { m_app.stop_comic_timer(); }
    void invalidate() { m_app.m_window.invalidate(); }

private:
    App& m_app;
};

ComicControlsRenderInput App::comic_controls_snapshot() const {
    const D2D1_SIZE_U target = m_renderer.target_size();
    const float dpi = m_window.handle()
        ? static_cast<float>(GetDpiForWindow(m_window.handle()))
        : 96.0f;
    const ComicScrollMetrics scroll = m_comic_reader.scroll_metrics();
    const ComicPageStatus page = m_comic_reader.page_status();
    ComicTransientOverlayKind transient = m_comic_transient_kind;
    if (transient != ComicTransientOverlayKind::None
        && GetTickCount64() >= m_comic_transient_until_ms) {
        transient = ComicTransientOverlayKind::None;
    }
    return ComicControlsRenderInput{
        {0.0f, m_renderer.content_top(), m_renderer.content_width(),
            static_cast<float>(target.height)},
        dpi,
        scroll.is_valid ? scroll.total_height : 0.0f,
        scroll.is_valid ? scroll.scroll : 0.0f,
        m_comic_scrollbar_hover,
        m_comic_scrollbar_dragging,
        page.anchored_index,
        page.total_pages,
        m_comic_page_badge,
        transient,
        m_comic_transient_text,
        m_comic_reader.middle_autoscroll_active(),
        m_comic_autoscroll_anchor_x,
        m_comic_autoscroll_anchor_y,
        m_comic_autoscroll_pointer_x,
        m_comic_autoscroll_pointer_y,
        m_comic_reader.width_factor(),
        m_comic_reader.cruise_active(),
        m_comic_reader.cruise_paused()};
}

bool App::dispatch_comic_command(ComicAppCommand command) {
    AppComicPort port(*this);
    return ComicAppController::dispatch_command(port, command);
}

bool App::start_comic_middle(
    float anchor_x, float anchor_y,
    float pointer_x, float pointer_y, bool anchor_visible) {
    AppComicPort port(*this);
    return ComicAppController::start_middle(
        port, anchor_x, anchor_y, pointer_x, pointer_y, anchor_visible);
}

void App::cancel_comic_auto_scroll(ComicAppCancelTrigger trigger) {
    AppComicPort port(*this);
    (void)ComicAppController::cancel(port, trigger);
}

void App::reset_comic_controls(ComicAppCancelTrigger trigger) {
    cancel_comic_auto_scroll(trigger);
    finish_comic_scrollbar_drag();
    m_comic_width_dragging = false;
    m_comic_scrollbar_hover = false;
    clear_comic_transient();
    m_comic_current_filename.clear();
    m_comic_page_badge.clear();
    m_comic_cached_page_index = -1;
    m_comic_cached_total_pages = 0;
    m_comic_autoscroll_anchor_x = 0.0f;
    m_comic_autoscroll_anchor_y = 0.0f;
    m_comic_autoscroll_pointer_x = 0.0f;
    m_comic_autoscroll_pointer_y = 0.0f;
    stop_comic_timer();
}

bool App::start_comic_timer() {
    HWND hwnd = m_window.handle();
    if (!hwnd) return false;
    if (m_comic_timer) return true;
    if (m_comic_last_tick_ms == 0) m_comic_last_tick_ms = GetTickCount64();
    m_comic_timer = SetTimer(
        hwnd, kComicTimerId, kComicAppTimerIntervalMs, nullptr);
    return m_comic_timer != 0;
}

void App::stop_comic_timer() {
    if (m_comic_timer && m_window.handle()) {
        KillTimer(m_window.handle(), kComicTimerId);
    }
    m_comic_timer = 0;
    m_comic_last_tick_ms = 0;
}

void App::handle_comic_timer(HWND) {
    // The scroll advance is render-driven (QPC); this timer only expires
    // transient overlays and wakes the render loop.
    const ULONGLONG now = GetTickCount64();
    m_comic_last_tick_ms = now;
    const bool transient_expired =
        m_comic_transient_kind != ComicTransientOverlayKind::None
        && now >= m_comic_transient_until_ms;
    AppComicPort port(*this);
    if (transient_expired) {
        port.clear_all_transient();
        port.invalidate();
    }
    ComicAppController::stop_timer_if_idle(port);
    if (port.owner() != ComicAppAutoOwner::None) port.invalidate();
}

void App::clear_comic_transient() {
    m_comic_transient_kind = ComicTransientOverlayKind::None;
    m_comic_transient_until_ms = 0;
    m_comic_transient_text.clear();
}

void App::revalidate_comic_middle_anchor() {
    if (!m_comic_reader.middle_autoscroll_active()) return;
    const bool anchor_visible = build_comic_controls_layout(
        comic_controls_snapshot()).autoscroll.visible;
    AppComicPort port(*this);
    (void)ComicAppController::viewport_changed(port, anchor_visible);
}

void App::finish_comic_scrollbar_drag() {
    if (!m_comic_scrollbar_dragging) return;
    m_comic_scrollbar_dragging = false;
    m_comic_scrollbar_grab_offset_y = 0.0f;
    HWND hwnd = m_window.handle();
    if (hwnd && GetCapture() == hwnd) ReleaseCapture();
}

void App::advance_comic_autoscroll_render() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = 0.0f;
    if (m_comic_advance_last.QuadPart != 0
        && now.QuadPart >= m_comic_advance_last.QuadPart) {
        dt = static_cast<float>(
            now.QuadPart - m_comic_advance_last.QuadPart)
            / static_cast<float>(freq.QuadPart);
    }
    m_comic_advance_last = now;
    AppComicPort port(*this);
    if (ComicAppController::advance(port, dt))
        m_window.invalidate();
}

void App::set_comic_width_factor_direct(float factor) {
    if (!m_comic_reader.enabled()) return;
    m_comic_reader.set_width_factor(factor);
    clear_comic_cache();
    sync_comic_current();
    request_comic_pages();
    m_window.invalidate();
}

void App::apply_comic_width_slider_x(
    const ComicControlsLayout& controls, float x) {
    const ComicWidthSliderLayout& slider = controls.width_slider;
    const float track_w = slider.track.right - slider.track.left;
    if (!slider.visible || track_w <= 0.0f) return;
    const float t = std::clamp(
        (x - slider.track.left) / track_w, 0.0f, 1.0f);
    set_comic_width_factor_direct(0.5f + 1.5f * t);
}

void App::sync_comic_current() {
    if (!m_comic_reader.enabled()) return;
    const std::optional<ComicPageChangeEvent> page_change =
        m_comic_reader.take_page_change_event();
    const ComicPageStatus page = m_comic_reader.page_status();
    const int anchor_index = page.anchored_index;
    if (anchor_index < 0
        || anchor_index >= static_cast<int>(m_index.size())
        || page.total_pages != static_cast<int>(m_index.size())) {
        if (m_comic_cached_page_index != -1
            || m_comic_cached_total_pages != 0) {
            m_comic_cached_page_index = -1;
            m_comic_cached_total_pages = 0;
            m_comic_page_badge.clear();
        }
        return;
    }
    const std::wstring& path = m_index.path_at(
        static_cast<std::size_t>(anchor_index));
    const bool identity_changed =
        m_current_idx != anchor_index || m_comic_current_filename.empty();
    if (identity_changed) {
        const std::size_t separator = path.find_last_of(L"\\/");
        m_comic_current_filename.assign(
            path, separator == std::wstring::npos ? 0 : separator + 1);
    }
    if (m_comic_cached_page_index != anchor_index
        || m_comic_cached_total_pages != page.total_pages) {
        m_comic_cached_page_index = anchor_index;
        m_comic_cached_total_pages = page.total_pages;
        m_comic_page_badge = std::to_wstring(anchor_index + 1);
        m_comic_page_badge += L" / ";
        m_comic_page_badge += std::to_wstring(page.total_pages);
    }
    if (identity_changed) {
        m_current_idx = anchor_index;
        m_current_path = path;
        m_has_image = true;
        update_title();
    }
    if (page_change
        && page_change->current_index == anchor_index
        && page_change->total_pages == page.total_pages) {
        m_comic_transient_kind = ComicTransientOverlayKind::PageChange;
        m_comic_transient_text = m_comic_current_filename;
        m_comic_transient_text += L" \u00B7 \u7B2C";
        m_comic_transient_text += std::to_wstring(anchor_index + 1);
        m_comic_transient_text += L"/";
        m_comic_transient_text += std::to_wstring(page.total_pages);
        m_comic_transient_text += L"\u9875";
        m_comic_transient_until_ms =
            GetTickCount64() + kComicAppTransientDurationMs;
        AppComicPort port(*this);
        (void)ComicAppController::transient_changed(port);
    }
}

void App::request_comic_pages() {
    if (!m_comic_reader.enabled() || !m_comic_loader.running()
        || m_comic_pages.size() != m_index.size()) return;
    const ComicPageRange requested = m_comic_reader.request_range();
    const ComicPageRange visible = m_comic_reader.visible_range();
    std::vector<int> ordered;
    ordered.reserve(static_cast<std::size_t>(requested.size()));
    const auto append = [&ordered, requested](int index) {
        if (!requested.contains(index)
            || std::find(ordered.begin(), ordered.end(), index) != ordered.end()) return;
        ordered.push_back(index);
    };
    for (int index = visible.first; index < visible.last; ++index) append(index);
    if (m_comic_reader.scroll_direction() == ComicScrollDirection::Backward) {
        for (int index = visible.first - 1; index >= requested.first; --index) append(index);
        for (int index = visible.last; index < requested.last; ++index) append(index);
    } else {
        for (int index = visible.last; index < requested.last; ++index) append(index);
        for (int index = visible.first - 1; index >= requested.first; --index) append(index);
    }

    std::vector<ComicLoadRequest> loads;
    loads.reserve(ordered.size());
    const std::uint32_t target_width = static_cast<std::uint32_t>(std::clamp(
        std::ceil(static_cast<double>(m_comic_reader.page_width())), 1.0,
        static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
    for (int index : ordered) {
        const ComicPageEntry& page = m_comic_pages[static_cast<std::size_t>(index)];
        if (page.wic || page.failed) continue;
        loads.push_back({
            index,
            m_index.path_at(static_cast<std::size_t>(index)),
            target_width,
            m_comic_generation});
    }
    m_comic_loader.replace_requests(std::move(loads));
    trim_comic_cache();
}

void App::apply_comic_results() {
    for (ComicLoadResult& result : m_comic_loader.take_ready()) {
        if (result.generation != m_comic_generation
            || result.index < 0
            || result.index >= static_cast<int>(m_comic_pages.size())
            || m_index.path_at(static_cast<std::size_t>(result.index)) != result.path) {
            continue;
        }
        ComicPageEntry& page = m_comic_pages[static_cast<std::size_t>(result.index)];
        page = ComicPageEntry{};
        page.source_width = result.source_width > 0
            ? result.source_width : result.decoded_width;
        page.source_height = result.source_height > 0
            ? result.source_height : result.decoded_height;
        page.decoded_width = result.decoded_width;
        page.decoded_height = result.decoded_height;
        page.estimated_cache_bytes = result.estimated_cache_bytes;
        page.failed = result.failed || !result.bitmap;
        page.wic = std::move(result.bitmap);
        m_comic_reader.update_page(
            result.index, page.source_width, page.source_height, page.failed);
        if (page.wic) {
            m_comic_lru.touch(result.index, page.estimated_cache_bytes);
        }
    }
    sync_comic_current();
    trim_comic_cache();
    request_comic_pages();
    m_window.invalidate();
}

void App::trim_comic_cache() {
    if (!m_comic_reader.enabled()) return;
    const std::size_t resident = m_comic_lru.resident_bytes();
    const std::size_t private_bytes = current_private_bytes();
    const std::size_t other_private = private_bytes > resident
        ? private_bytes - resident : 0;
    const auto evicted = m_comic_lru.evict_to_budget(
        other_private, m_comic_reader.visible_range());
    for (int index : evicted) {
        if (index < 0 || index >= static_cast<int>(m_comic_pages.size())) continue;
        m_comic_pages[static_cast<std::size_t>(index)] = ComicPageEntry{};
    }
}

void App::render_comic_reader(float content_top) {
    update_comic_viewport();
    request_comic_pages();
    const ComicPageRange visible = m_comic_reader.visible_range();
    const auto geometries = m_comic_reader.materialize(visible);
    std::vector<ComicPageDrawItem> draw_items;
    draw_items.reserve(geometries.size());
    for (const ComicPageGeometry& geometry : geometries) {
        ComicPageEntry& page = m_comic_pages[static_cast<std::size_t>(geometry.index)];
        if (page.wic && !page.d2d) {
            m_renderer.create_bitmap_from_wic(page.wic.Get(), &page.d2d);
        }
        ID2D1Bitmap1* bitmap = nullptr;
        if (page.d2d) {
            bitmap = page.d2d.Get();
            m_comic_lru.touch(geometry.index, page.estimated_cache_bytes);
        } else if (geometry.index == m_comic_fallback_index
                   && m_renderer.image_bitmap()) {
            bitmap = m_renderer.image_bitmap();
        }
        draw_items.push_back({geometry, bitmap, page.failed});
    }
    const float viewport_height = std::max(
        1.0f, static_cast<float>(m_renderer.target_size().height) - content_top);
    const float dpi = m_window.handle()
        ? static_cast<float>(GetDpiForWindow(m_window.handle()))
        : 96.0f;
    m_renderer.draw_comic_pages(draw_items, {
        0.0f, content_top, m_renderer.content_width(), viewport_height,
        m_comic_reader.scroll(), dpi, m_comic_reader.seamless()});

    sync_comic_current();
    m_renderer.draw_comic_controls(comic_controls_snapshot());
    const int current = m_current_idx;
    ID2D1Bitmap1* preview = nullptr;
    std::uint32_t preview_width = 0;
    std::uint32_t preview_height = 0;
    if (current >= 0 && current < static_cast<int>(m_comic_pages.size())) {
        ComicPageEntry& page = m_comic_pages[static_cast<std::size_t>(current)];
        preview = page.d2d.Get();
        preview_width = page.source_width;
        preview_height = page.source_height;
    }
    if (!preview && current == m_comic_fallback_index) {
        preview = m_renderer.image_bitmap();
        m_renderer.image_size(preview_width, preview_height);
    }
    draw_panel(m_current_path, preview, preview_width, preview_height, content_top);
    trim_comic_cache();
}

// ── Delete ───────────────────────────────────────────────────

HWND App::delete_owner_window() const {
    return m_window.handle();
}

DeleteCompositionState App::capture_delete_state() const {
    DeleteCompositionState state;
    state.index_paths.reserve(m_index.size());
    for (int i = 0; i < static_cast<int>(m_index.size()); ++i)
        state.index_paths.push_back(m_index.path_at(i));
    state.current_path = m_current_path;
    state.current_index = m_current_idx;
    state.has_image = m_has_image;
    state.grid_mode = m_grid_mode;
    state.grid_selection = m_grid_sel;
    state.selected = m_selected;
    state.selection_anchor = m_sel_anchor;
    state.loader_running = thumb_loader_running();
    return state;
}

void App::remove_delete_indices(const std::vector<int>& indices) {
    // Deleted files leave the fixed favourite album too (Issue #5 P3c).
    bool fav_changed = false;
    for (const int idx : indices) {
        if (idx >= 0 && idx < static_cast<int>(m_index.size())
            && mv::remove_favourite(m_album_store,
                m_index.path_at(static_cast<size_t>(idx))))
            fav_changed = true;
    }
    if (fav_changed) save_album_store();
    m_index.remove_many(indices);
    if (m_comic_reader.enabled()) {
        rebuild_comic_pages();
        sync_comic_current();
        request_comic_pages();
    }
}

bool App::primary_is_favourite() const {
    std::wstring path;
    if (m_grid_mode) {
        if (m_grid_sel >= 0 && m_grid_sel < static_cast<int>(m_index.size()))
            path = m_index.path_at(static_cast<size_t>(m_grid_sel));
    } else if (m_has_image) {
        path = m_current_path;
    }
    return !path.empty() && mv::is_favourite(m_album_store, path);
}

void App::toggle_favourite_current() {
    std::wstring path;
    if (m_grid_mode) {
        if (m_grid_sel >= 0 && m_grid_sel < static_cast<int>(m_index.size()))
            path = m_index.path_at(static_cast<size_t>(m_grid_sel));
    } else if (m_has_image) {
        path = m_current_path;
    }
    if (path.empty()) return;
    if (mv::is_favourite(m_album_store, path))
        mv::remove_favourite(m_album_store, path);
    else
        mv::add_favourite(m_album_store, path);
    save_album_store();
    // The favourite album itself must reflect the toggle immediately: the
    // dir watcher only fires on file-system changes, not list edits.
    if (m_fav_selected) request_collection_refresh();
    m_window.invalidate();
}

bool App::open_delete_successor(const std::wstring& path, int index) {
    if (m_comic_reader.enabled()) {
        if (index < 0 || index >= static_cast<int>(m_index.size())
            || m_index.path_at(static_cast<std::size_t>(index)) != path) return false;
        m_comic_reader.scroll_to_page(index);
        sync_comic_current();
        request_comic_pages();
        return true;
    }
    return open_image(path);
}

void App::set_delete_current_identity(
    const std::wstring& path, int index, bool has_image) {
    m_current_path = path;
    m_current_idx = index;
    m_has_image = has_image;
    if (!has_image) {
        reset_comic_controls(ComicAppCancelTrigger::EmptyBook);
    }
}

void App::set_delete_grid_state(
    bool grid_mode, int grid_selection, const std::vector<bool>& selected,
    int selection_anchor) {
    if (m_grid_mode && !grid_mode) finish_grid_scroll();
    m_grid_mode = grid_mode;
    m_grid_sel = grid_selection;
    m_selected = selected;
    m_sel_anchor = selection_anchor;
}

void App::reset_delete_current_bitmap() {
    m_current_wic.Reset();
}

void App::stop_delete_loader() {
    stop_thumb_loader();
}

void App::start_delete_loader() {
    start_thumb_loader();
}

void App::rebuild_delete_thumbnails() {
    if (thumb_loader_running()) stop_thumb_loader();
    m_thumb_engine.pool()->thumbs.clear();
    m_thumb_engine.pool()->thumbs.resize(m_index.size());
    m_thumb_d2d.clear();
    m_thumb_d2d_use.clear();
    m_grid_layout_dirty = true;
}

void App::clear_delete_thumbnails() {
    if (thumb_loader_running()) stop_thumb_loader();
    m_thumb_engine.pool()->thumbs.clear();
    m_thumb_d2d.clear();
    m_thumb_d2d_use.clear();
    m_grid_layout_dirty = true;
}

void App::reset_delete_grid_cache() {
    m_last_cached_sel = -1;
}

void App::ensure_delete_grid_visible() {
    grid_ensure_visible();
}

void App::update_delete_title() {
    update_title();
}

void App::invalidate_delete_view() {
    m_window.invalidate();
}

// ── Context menu ─────────────────────────────────────────────

void App::show_context_menu(HWND hwnd, int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    auto paths = selected_paths();
    if (!paths.empty()) {
        UINT copy_flags = MF_STRING | (paths.size() == 1 ? 0 : MF_GRAYED);
        AppendMenuW(menu, MF_STRING, 1, L"\u5728\u8D44\u6E90\u7BA1\u7406\u5668\u4E2D\u6253\u5F00");
        AppendMenuW(menu, copy_flags, 2, L"\u590D\u5236	Ctrl+C");
        AppendMenuW(menu, MF_STRING, 8, L"\u590D\u5236\u6587\u4EF6\u8DEF\u5F84");
        AppendMenuW(menu, MF_STRING, 9, L"\u521B\u5EFA\u526F\u672C");
        AppendMenuW(menu,
            MF_STRING | (primary_is_favourite() ? MF_CHECKED : 0), 14,
            L"\u6536\u85CF\tF");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        if (m_grid_mode) {
            HMENU sort_menu = CreatePopupMenu();
            auto sm = m_index.sort_mode();
            AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Name   ? MF_CHECKED : 0), 10, L"\u6309\u540D\u79F0	N");
            AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Date   ? MF_CHECKED : 0), 11, L"\u6309\u65E5\u671F	D");
            AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Size   ? MF_CHECKED : 0), 12, L"\u6309\u5927\u5C0F	S");
            AppendMenuW(sort_menu, MF_STRING | (sm == SortMode::Random ? MF_CHECKED : 0), 13, L"\u968F\u673A\u6253\u4E71	R");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sort_menu), L"\u6392\u5E8F\u65B9\u5F0F");
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_DELETE, L"\u5220\u9664	Del");
        AppendMenuW(menu, MF_STRING, IDM_DELETE_PERM, L"\u6C38\u4E45\u5220\u9664	Shift+Del");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 7, L"\u5C55\u5F00/\u6536\u8D77\u4FE1\u606F\u9762\u677F	I");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (m_grid_mode) {
            UINT flags = MF_STRING;
            if (m_recursive) flags |= MF_CHECKED;
            AppendMenuW(menu, flags, 6, L"\u9012\u5F52\u6D4F\u89C8	Ctrl+R");
        }
    } else {
        AppendMenuW(menu, MF_STRING, 5, L"\u6253\u5F00\u6587\u4EF6...");
        AppendMenuW(menu,
            MF_STRING | (primary_is_favourite() ? MF_CHECKED : 0), 14,
            L"\u6536\u85CF\tF");
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

    if (m_delete_composition->handle_command(
            DeleteCommandEntry::ContextMenu, static_cast<UINT>(cmd)))
        return;

    switch (cmd) {
    case 1: open_in_explorer();         break;
    case 2: copy_image_data();          break;
    case 8: copy_file_paths();          break;
    case 9: create_file_copies();       break;
    case 5: {
        // Open File dialog
        OPENFILENAMEW ofn = {};
        wchar_t file[MAX_PATH] = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"\u56FE\u7247\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tiff;*.tif\0\u6240\u6709\u6587\u4EF6\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile  = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            open_image(file);
        }
        break;
    }
    case 6: if (m_grid_mode) toggle_recursive(); break;
    case 7: toggle_info(); break;
    case 10: if (m_grid_mode) set_sort_mode(SortMode::Name);   break;
    case 11: if (m_grid_mode) set_sort_mode(SortMode::Date);   break;
    case 12: if (m_grid_mode) set_sort_mode(SortMode::Size);   break;
    case 13: if (m_grid_mode) set_sort_mode(SortMode::Random); break;
    case 14: toggle_favourite_current(); break;
    }

    DestroyMenu(menu);
}

void App::show_toolbar_menu(HWND hwnd, int idx, int x, int y) {
    // Precompute menu item bounds (after "MinView" title, matching draw_title_bar)
    float dpi_s_tb = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    const TitleBarLayout title{
        0.0f,
        static_cast<float>(m_renderer.target_size().width),
        static_cast<float>(m_toolbar_h),
        dpi_s_tb};
    const float fsize = title_bar_menu_font_size(title);
    std::vector<float> menu_widths;
    menu_widths.reserve(m_toolbar_items.size());
    for (auto& item : m_toolbar_items)
        menu_widths.push_back(m_renderer.measure_text(item, fsize));
    std::vector<TitleBarMenuBound> tb_bounds =
        title_bar_menu_bounds(title, menu_widths);

    HMENU popup = CreatePopupMenu();
    switch (idx) {
    case 0: // 文件
        AddOwnerItem(popup, IDM_OPEN_FILE, L"打开文件...	Ctrl+O");
        AddOwnerItem(popup, IDM_OPEN_FOLDER, L"打开文件夹...");
        AddOwnerSeparator(popup);
        {
            bool has_sel = !primary_path().empty();
            AddOwnerItem(popup, IDM_EXPLORER, L"在资源管理器中打开", has_sel ? false : true);
        }
        break;
    case 1: // 查看
        AddOwnerItem(popup, IDM_COMIC, L"漫画模式\tM",
            m_grid_mode || !m_has_image, m_comic_reader.enabled());
        AddOwnerItem(popup, IDM_COMIC_SEAMLESS, L"无缝页距",
            !m_comic_reader.enabled(), m_comic_reader.seamless());
        AddOwnerItem(popup, IDM_COMIC_AUTOSCROLL,
            L"\u81EA\u52A8\u6EDA\u52A8\tP", !m_comic_reader.enabled(),
            m_comic_reader.cruise_active());
        {
            HMENU speed_menu = CreatePopupMenu();
            const bool disabled = !m_comic_reader.enabled();
            const int speed = m_comic_reader.cruise_speed_index();
            AddOwnerItem(speed_menu, IDM_COMIC_SPEED_05, L"0.5x",
                disabled, speed == 0);
            AddOwnerItem(speed_menu, IDM_COMIC_SPEED_10, L"1.0x",
                disabled, speed == 1);
            AddOwnerItem(speed_menu, IDM_COMIC_SPEED_15, L"1.5x",
                disabled, speed == 2);
            AddOwnerItem(speed_menu, IDM_COMIC_SPEED_20, L"2.0x",
                disabled, speed == 3);
            BuildOwnerMenu(popup, speed_menu,
                L"\u81EA\u52A8\u6EDA\u52A8\u901F\u5EA6");
        }
        AddOwnerSeparator(popup);
        AddOwnerItem(popup, IDM_FULLSCREEN, L"全屏	F11");
        AddOwnerSeparator(popup);
        {
            HMENU sort_menu = CreatePopupMenu();
            SortMode cur = m_index.sort_mode();
            AddOwnerItem(sort_menu, IDM_SORT_NAME,   L"按名称排序	N", !m_grid_mode, cur == SortMode::Name);
            AddOwnerItem(sort_menu, IDM_SORT_DATE,   L"按日期排序	D", !m_grid_mode, cur == SortMode::Date);
            AddOwnerItem(sort_menu, IDM_SORT_SIZE,   L"按大小排序	S", !m_grid_mode, cur == SortMode::Size);
            AddOwnerItem(sort_menu, IDM_SORT_RANDOM, L"随机排序	R", !m_grid_mode, cur == SortMode::Random);
            BuildOwnerMenu(popup, sort_menu, L"排序方式");
        }
        AddOwnerSeparator(popup);
        AddOwnerItem(popup, IDM_RECURSIVE, L"递归浏览子文件夹	Ctrl+R",
            !can_toggle_recursive(m_grid_mode, m_has_image, m_index.directory()),
            m_recursive);
        AddOwnerItem(popup, IDM_THUMB_SQUARE,
            m_thumb_square ? L"原始比例网格	A" : L"方形缩略图	A", !m_grid_mode);
        AddOwnerItem(popup, IDM_LABELS, L"显示文件名标签	L",
            !m_grid_mode, m_show_labels);
        AddOwnerSeparator(popup);
        AddOwnerItem(popup, IDM_INFO, L"展开/收起信息面板	I", false, m_panel_expanded);
        AddOwnerItem(popup, IDM_NAV_PANEL, L"展开/收起导航面板	B", false,
            m_nav_panel_state.expanded());
        break;
    case 2: // 编辑
        {
            size_t selected_count = selected_paths().size();
            AddOwnerItem(popup, IDM_COPY_IMAGE, L"复制	Ctrl+C", selected_count != 1);
            AddOwnerItem(popup, IDM_COPY_PATH, L"复制文件路径", selected_count == 0);
            AddOwnerItem(popup, IDM_CREATE_COPY, L"创建副本", selected_count == 0);
        }
        AddOwnerSeparator(popup);
        AddOwnerItem(popup, IDM_DELETE, L"移动到回收站	Del");
        AddOwnerItem(popup, IDM_DELETE_PERM, L"永久删除	Shift+Del");
        break;
    case 3: // 帮助
        AddOwnerItem(popup, IDM_ABOUT, L"关于 MinView...");
        break;
    }

    // Set up menu hover-switch state
    static HWND  s_menu_hwnd = nullptr;
    static int   s_active_idx = -1;
    static int   s_switch_to = -1;
    static std::vector<TitleBarMenuBound> s_tb_bounds;
    static int   s_toolbar_h = 0;
    s_menu_hwnd   = hwnd;
    s_active_idx  = idx;
    s_switch_to   = -1;
    s_tb_bounds   = tb_bounds;
    s_toolbar_h   = m_toolbar_h;

    // CBT hook: subclass popup menu to enforce dark background
    static HBRUSH s_menu_bg = CreateSolidBrush(dt::kColorMenuBgGdi);
    HHOOK cbt_hook = SetWindowsHookExW(WH_CBT,
        [](int code, WPARAM wp, LPARAM lp) -> LRESULT {
            if (code == HCBT_CREATEWND) {
                HWND hwnd = reinterpret_cast<HWND>(wp);
                wchar_t cls[16];
                if (GetClassNameW(hwnd, cls, 16) && wcscmp(cls, L"#32768") == 0) {
                    // Strip border styles
                    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
                    style &= ~(WS_BORDER | WS_DLGFRAME | WS_THICKFRAME);
                    SetWindowLongW(hwnd, GWL_STYLE, style);
                    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
                    ex &= ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME);
                    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
                    SetWindowTheme(hwnd, L"", L"");
                    // Subclass to fill background dark
                    WNDPROC oldProc = reinterpret_cast<WNDPROC>(
                        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                            reinterpret_cast<LONG_PTR>(MenuSubclassProc)));
                    SetPropW(hwnd, L"MV_OLD", oldProc);
                    SetPropW(hwnd, L"MV_BG", s_menu_bg);
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }, nullptr, GetCurrentThreadId());

    // Message filter hook for hover-switching menus
    HHOOK hook = SetWindowsHookExW(WH_MSGFILTER,
        [](int code, WPARAM wp, LPARAM lp) -> LRESULT {
            if (code == MSGF_MENU) {
                MSG* msg = (MSG*)lp;
                if (msg->message == WM_MOUSEMOVE || msg->message == WM_NCMOUSEMOVE) {
                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(s_menu_hwnd, &pt);
                    if (pt.y >= 0 && pt.y < s_toolbar_h) {
                        for (int i = 0; i < static_cast<int>(s_tb_bounds.size()); ++i) {
                            if (i != s_active_idx &&
                                pt.x >= s_tb_bounds[i].left &&
                                pt.x < s_tb_bounds[i].right) {
                                s_switch_to = i;
                                PostMessageW(s_menu_hwnd, WM_CANCELMODE, 0, 0);
                                break;
                            }
                        }
                    }
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }, nullptr, GetCurrentThreadId());

    // Align popup text with toolbar text (gutter = pad(4) + icon(16) + gap(8) - toolbar_pad(4) = 24 DIPs)
    x -= static_cast<int>(24 * dpi_s_tb);

    // Dark menu background (recursive)
    HBRUSH menu_br = CreateSolidBrush(dt::kColorMenuBgGdi);
    ApplyMenuTheme(popup, menu_br);

    int cmd = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_NONOTIFY,
        x, y, 0, hwnd, nullptr);

    if (hook) UnhookWindowsHookEx(hook);
    if (cbt_hook) UnhookWindowsHookEx(cbt_hook);
    FreeOwnerItemData(popup);
    DestroyMenu(popup);
    DeleteObject(menu_br);

    // Clear menu hover state after popup dismissed
    if (m_toolbar_active >= 0) {
        m_toolbar_active = -1;
        m_window.invalidate();
    }

    // If menu was cancelled to switch to another toolbar item
    if (cmd == 0 && s_switch_to >= 0) {
        int next = s_switch_to;
        s_switch_to = -1;
        // Update toolbar highlight immediately
        m_toolbar_active = next;
        m_window.invalidate();
        // Compute new popup position for the switched item
        POINT pt = {static_cast<int>(s_tb_bounds[next].left), static_cast<int>(m_toolbar_h)};
        ClientToScreen(hwnd, &pt);
        show_toolbar_menu(hwnd, next, pt.x, pt.y);
        return;
    }

    // Handle commands
    if (m_delete_composition->handle_command(
            DeleteCommandEntry::Toolbar, static_cast<UINT>(cmd)))
        return;

    switch (cmd) {
    case IDM_COMIC: case IDM_COMIC_SEAMLESS: case IDM_COMIC_AUTOSCROLL:
    case IDM_COMIC_SPEED_05: case IDM_COMIC_SPEED_10:
    case IDM_COMIC_SPEED_15: case IDM_COMIC_SPEED_20:
    case IDM_OPEN_FILE: case IDM_OPEN_FOLDER: case IDM_FULLSCREEN: case IDM_RECURSIVE:
    case IDM_THUMB_SQUARE: case IDM_INFO: case IDM_LABELS:
    case IDM_SORT_NAME: case IDM_SORT_DATE:
    case IDM_SORT_SIZE: case IDM_SORT_RANDOM:
    case IDM_COPY_IMAGE: case IDM_COPY_PATH: case IDM_CREATE_COPY:
    case IDM_EXPLORER: case IDM_ABOUT:
        SendMessageW(hwnd, WM_COMMAND, cmd, 0); break;
    }
}

void App::open_in_explorer() {
    std::wstring path = primary_path();
    if (path.empty()) return;
    // Convert forward slashes to backslashes
    for (auto& c : path) if (c == L'/') c = L'\\';
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOW);
}

std::vector<std::wstring> App::selected_paths() const {
    std::vector<std::wstring> paths;
    if (m_grid_mode) {
        if (m_selected.size() != m_index.size()) return paths;
        for (int i = 0; i < static_cast<int>(m_selected.size()); ++i) {
            if (m_selected[static_cast<size_t>(i)]) paths.push_back(m_index.path_at(i));
        }
    } else if (!m_current_path.empty()) {
        paths.push_back(m_current_path);
    }
    return paths;
}

std::wstring App::primary_path() const {
    if (m_grid_mode) {
        if (m_grid_sel >= 0 && m_grid_sel < static_cast<int>(m_index.size())
            && m_grid_sel < static_cast<int>(m_selected.size())
            && m_selected[static_cast<size_t>(m_grid_sel)])
            return m_index.path_at(m_grid_sel);
        for (int i = 0; i < static_cast<int>(m_selected.size()); ++i) {
            if (m_selected[static_cast<size_t>(i)]) return m_index.path_at(i);
        }
        return L"";
    }
    return m_current_path;
}

void App::copy_image_data() {
    if (selected_paths().size() != 1) return;
    std::wstring path = primary_path();
    if (path.empty()) return;

    try {
        auto bitmap = m_decoder.decode(path);
        uint32_t width = 0, height = 0;
        bitmap->GetSize(&width, &height);
        size_t stride = static_cast<size_t>(width) * 4;
        size_t pixel_bytes = stride * height;
        if (width == 0 || height == 0 || stride > std::numeric_limits<UINT>::max()
            || pixel_bytes > std::numeric_limits<UINT>::max()
            || pixel_bytes > std::numeric_limits<SIZE_T>::max() - sizeof(BITMAPV5HEADER)) return;

        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + pixel_bytes);
        if (!memory) return;
        auto* header = static_cast<BITMAPV5HEADER*>(GlobalLock(memory));
        if (!header) { GlobalFree(memory); return; }
        ZeroMemory(header, sizeof(*header));
        header->bV5Size = sizeof(*header);
        header->bV5Width = static_cast<LONG>(width);
        header->bV5Height = -static_cast<LONG>(height);
        header->bV5Planes = 1;
        header->bV5BitCount = 32;
        header->bV5Compression = BI_BITFIELDS;
        header->bV5SizeImage = static_cast<DWORD>(pixel_bytes);
        header->bV5RedMask = 0x00FF0000;
        header->bV5GreenMask = 0x0000FF00;
        header->bV5BlueMask = 0x000000FF;
        header->bV5AlphaMask = 0xFF000000;
        header->bV5CSType = LCS_sRGB;
        header->bV5Intent = LCS_GM_IMAGES;

        auto* pixels = reinterpret_cast<BYTE*>(header + 1);
        HRESULT hr = bitmap->CopyPixels(nullptr, static_cast<UINT>(stride),
            static_cast<UINT>(pixel_bytes), pixels);
        GlobalUnlock(memory);
        if (FAILED(hr)) { GlobalFree(memory); return; }

        if (!OpenClipboard(m_window.handle())) { GlobalFree(memory); return; }
        EmptyClipboard();
        if (!SetClipboardData(CF_DIBV5, memory)) GlobalFree(memory);
        CloseClipboard();
    } catch (const std::exception&) {
        MessageBoxW(m_window.handle(), L"\u65E0\u6CD5\u590D\u5236\u5F53\u524D\u56FE\u7247\u6570\u636E\u3002",
            L"MinView", MB_OK | MB_ICONWARNING);
    }
}

void App::copy_file_paths() {
    auto paths = selected_paths();
    if (paths.empty()) return;
    std::wstring text;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) text += L"\r\n";
        text += paths[i];
    }
    SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;
    auto* dst = static_cast<wchar_t*>(GlobalLock(memory));
    if (!dst) { GlobalFree(memory); return; }
    wmemcpy(dst, text.c_str(), text.size() + 1);
    GlobalUnlock(memory);
    if (!OpenClipboard(m_window.handle())) { GlobalFree(memory); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    CloseClipboard();
}

void App::create_file_copies() {
    namespace fs = std::filesystem;
    auto sources = selected_paths();
    if (sources.empty()) return;

    std::vector<std::wstring> created;
    for (const auto& source : sources) {
        fs::path src(source);
        fs::path dir = src.parent_path();
        std::wstring base = src.stem().wstring() + L" - \u526F\u672C";
        fs::path destination = dir / (base + src.extension().wstring());
        std::error_code error;
        for (int suffix = 2; fs::exists(destination, error) && !error; ++suffix) {
            destination = dir / (base + L" (" + std::to_wstring(suffix) + L")" + src.extension().wstring());
        }
        if (error) continue;
        if (CopyFileW(source.c_str(), destination.c_str(), TRUE))
            created.push_back(destination.wstring());
    }

    if (created.empty()) {
        MessageBoxW(m_window.handle(), L"\u521B\u5EFA\u526F\u672C\u5931\u8D25\u3002", L"MinView",
            MB_OK | MB_ICONWARNING);
        return;
    }

    bool loader_was_running = thumb_loader_running();
    if (loader_was_running) stop_thumb_loader();
    std::wstring dir = m_index.directory();
    if (!dir.empty() && m_index.scan(dir, m_recursive) >= 0) {
        m_current_idx = m_current_path.empty() ? -1 : m_index.index_of(m_current_path);
        m_thumb_engine.pool()->thumbs.clear();
        m_thumb_engine.pool()->thumbs.resize(m_index.size());
        m_thumb_d2d.clear();
        m_thumb_d2d_use.clear();
        m_grid_layout_dirty = true;
        m_selected.assign(m_index.size(), false);
        m_grid_sel = -1;
        for (int idx : remap_paths_to_indices(m_index, created)) {
            if (idx < 0) continue;
            if (m_grid_sel < 0) m_grid_sel = idx;
            m_selected[static_cast<size_t>(idx)] = true;
        }
        m_sel_anchor = m_grid_sel;
        m_panel_path.clear();
        if (loader_was_running || m_grid_mode) start_thumb_loader();
        if (m_grid_mode) grid_ensure_visible();
    } else if (loader_was_running) {
        start_thumb_loader();
    }
    m_window.invalidate();
}

// ── Fullscreen ───────────────────────────────────────────────

std::optional<GridTransitionRect> App::grid_transition_source_rect(int index) const {
    const int total = static_cast<int>(m_index.size());
    if (index < 0 || index >= total
        || static_cast<size_t>(index) >= m_grid_dims.size()
        || static_cast<size_t>(index) >= m_grid_item_x.size()
        || static_cast<size_t>(index) >= m_grid_item_w.size()) {
        return std::nullopt;
    }

    const auto row = std::find_if(m_grid_rows.begin(), m_grid_rows.end(),
        [index](const GridLayoutRow& candidate) {
            return index >= candidate.start_idx && index < candidate.end_idx;
        });
    if (row == m_grid_rows.end()) return std::nullopt;

    const auto [image_width, image_height] =
        m_grid_dims[static_cast<size_t>(index)];
    GridTransitionGeometry geometry;
    geometry.request_index = index;
    geometry.item_count = total;
    geometry.row_start_index = row->start_idx;
    geometry.row_end_index = row->end_idx;
    geometry.row_y = row->row_y;
    geometry.row_height = row->row_h;
    geometry.item_x = m_grid_item_x[static_cast<size_t>(index)];
    geometry.item_width = m_grid_item_w[static_cast<size_t>(index)];
    geometry.image_width = image_width;
    geometry.image_height = image_height;
    geometry.thumb_padding = m_thumb_pad;
    geometry.toolbar_height = m_grid_top;  // grid content top (toolbar + breadcrumb)
    geometry.nav_offset = nav_panel_width();
    geometry.scroll_y = m_grid_scroll_y;
    return calculate_grid_transition_rect(geometry);
}

bool App::capture_grid_transition_source(int index) {
    m_anim_src = {};
    m_anim_dst = {};
    if (!m_grid_mode || index < 0
        || index >= static_cast<int>(m_index.size())) {
        return false;
    }

    const float dpi_scale =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    const int scrollbar_zone = static_cast<int>(layout::kScrollbarZoneDip * dpi_scale);
    const int grid_area_width = std::max(1,
        static_cast<int>(m_renderer.target_size().width)
            - nav_panel_width() - visible_panel_width()
            - scrollbar_zone - m_thumb_pad);
    const uint64_t dimension_generation =
        m_thumb_engine.dimension_generation();
    const GridRebuildReason rebuild_reason = classify_grid_rebuild_reason(
        m_grid_layout_dirty, m_grid_layout_width != grid_area_width,
        m_grid_dims.size() != m_index.size(),
        dimension_generation != m_grid_layout_generation);
    if (rebuild_reason != GridRebuildReason::None)
        rebuild_grid_layout(grid_area_width, rebuild_reason);

    const auto rect = grid_transition_source_rect(index);
    if (!rect) return false;
    m_anim_src = {rect->left, rect->top, rect->right, rect->bottom};
    return true;
}

void App::start_transition(HWND /*hwnd*/, bool forward, int request_index) {
    if (m_animating) return;
    m_anim_forward = forward;
    m_anim_thumb.Reset();
    m_anim_reversed = false;
    m_anim_grid_snapshot.Reset();
    m_anim_iw = 0.0f;
    m_anim_ih = 0.0f;
    m_last_cached_sel = -1;  // force m_anim_src recalculation on next grid_render

    bool source_captured = !forward;
    if (forward) {
        (void)run_best_effort_transition_capture(
            [this, request_index, &source_captured]() {
                source_captured = capture_grid_transition_source(request_index);
            });
        // Entry: snapshot the grid frame so the big-image background can
        // cover it progressively (0 -> 100% opacity) instead of the grid
        // vanishing at t=0.
        m_anim_grid_snapshot = capture_window_frame();
        if (!m_anim_grid_snapshot) source_captured = false;
    } else {
        m_anim_grid_snapshot.Reset();
    }

    int thumb_idx = forward
        ? request_index
        : ((m_current_idx >= 0) ? m_current_idx : m_grid_saved_idx);
    if (!forward) {
        // Apple-style exit: the FULL image zooms back into its cell (the
        // low-res thumbnail previously popped the quality at the start).
        m_anim_thumb = m_renderer.transition_image();
    }
    if (!m_anim_thumb) {
        (void)run_best_effort_transition_capture([this, thumb_idx]() {
            auto it = m_thumb_d2d.find(thumb_idx);
            if (it != m_thumb_d2d.end()) {
                m_anim_thumb = it->second;
            } else if (thumb_idx >= 0 && thumb_idx < static_cast<int>(m_index.size())) {
                auto wic = m_decoder.decode_scaled(m_index.path_at(thumb_idx), m_thumb_size);
                if (wic) {
                    ComPtr<ID2D1Bitmap1> d2d;
                    if (SUCCEEDED(m_renderer.create_bitmap_from_wic(wic.Get(), &d2d)) && d2d)
                        m_anim_thumb = d2d;
                }
            }
        });
    }
    if (!source_captured) m_anim_thumb.Reset();
    if (forward && m_anim_thumb) {
        // Bridge the decode gap: the zooming thumbnail doubles as the
        // placeholder, so the transition never ends on an empty frame —
        // the real upload replaces it seamlessly later.
        m_renderer.set_placeholder(m_anim_thumb.Get());
        m_placeholder_idx = -1;
    }

    // Pre-store target image size from thumbnail metadata (avoids stale image_size())
    if (forward && thumb_idx >= 0 && thumb_idx < static_cast<int>(m_thumb_engine.thumb_count())) {
        std::lock_guard lock(m_thumb_engine.pool()->mutex);
        m_anim_iw = static_cast<float>(m_thumb_engine.pool()->thumbs[thumb_idx].orig_w);
        m_anim_ih = static_cast<float>(m_thumb_engine.pool()->thumbs[thumb_idx].orig_h);
        if (m_anim_iw < 1) m_anim_iw = 1;
        if (m_anim_ih < 1) m_anim_ih = 1;
    } else if (!forward) {
        uint32_t iw, ih; m_renderer.image_size(iw, ih);
        m_anim_iw = static_cast<float>(iw);
        m_anim_ih = static_cast<float>(ih);
    }

    if (!forward) {
        if (m_anim_src.right > m_anim_src.left && m_anim_src.bottom > m_anim_src.top)
            m_anim_dst = m_anim_src;
    }
}

void App::begin_animation(HWND hwnd) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    m_anim_start = now.QuadPart;
    m_animating = true;
    m_anim_t = 0.0f;
    if (m_anim_timer) KillTimer(hwnd, m_anim_timer);
    m_anim_timer = SetTimer(hwnd, 4, dt::kAnimationFrameMs, nullptr);
}

bool App::enter_grid_image(HWND hwnd, const GridEntryRequest& request) {
    return run_grid_entry(
        request,
        GridEntryTransactionState{
            m_grid_mode, m_from_grid, m_animating,
            m_grid_sel, m_selected, m_sel_anchor},
        [this, hwnd, &request](int index) {
            if (request.trigger == GridEntryTrigger::DoubleClick)
                select_item(index, false, false);
            else
                m_grid_sel = index;
            start_transition(hwnd, true, index);
        },
        [this](int index) {
            return open_image(m_index.path_at(static_cast<size_t>(index)));
        },
        [this, hwnd]() {
            begin_animation(hwnd);
            m_window.invalidate();
        });
}

void App::toggle_fullscreen(HWND hwnd) {
    m_fullscreen = !m_fullscreen;
    m_toolbar_revealed = false;
    reset_filmstrip_reveal();

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
        update_content_viewport(true);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE,   m_saved_style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, m_saved_exstyle);
        SetWindowPlacement(hwnd, &m_saved_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        update_content_viewport(true);
    }

    if (m_grid_mode && m_grid_sel >= 0) {
        grid_ensure_visible();
    }
}

// ── Zoom ─────────────────────────────────────────────────────

void App::fit_to_window() {
    uint32_t iw, ih; m_renderer.image_size(iw, ih);
    if (iw == 0 || ih == 0) return;
    RECT rc; GetClientRect(m_window.handle(), &rc);
    float cw = m_renderer.content_width();
    float ch = static_cast<float>(rc.bottom - rc.top) - m_renderer.content_top();
    if (cw <= 0 || ch <= 0) return;
    m_renderer.set_scale(std::min(cw / iw, ch / ih));
    m_renderer.set_offset(0, 0);
    m_renderer.set_scroll_y(0);
    // Pre-build the fit-scale cache now so the transition and the first
    // frames blit a small bitmap instead of re-sampling the original.
    m_renderer.ensure_image_scaled();
}

void App::zoom_at_center(float factor) {
    uint32_t iw, ih; m_renderer.image_size(iw, ih);
    if (iw == 0 || ih == 0) return;
    float old_scale = m_renderer.scale();
    float new_scale = std::clamp(old_scale * factor, m_renderer.fit_scale(), 100.0f);
    if (new_scale == old_scale) return;

    D2D1_SIZE_U ts = m_renderer.target_size();
    float view_w = m_renderer.content_width();
    float view_top = m_renderer.content_top();
    float view_h = static_cast<float>(ts.height) - view_top;
    float cx = view_w / 2.0f, cy = view_top + view_h / 2.0f;
    float img_x = (view_w - iw * old_scale) / 2.0f;
    float img_y = view_top + (view_h - ih * old_scale) / 2.0f;
    float img_cx = (cx - img_x) / old_scale;
    float img_cy = (cy - img_y) / old_scale;

    m_renderer.set_scale(new_scale);
    m_renderer.set_offset(
        (cx - img_cx * new_scale) - (view_w - iw * new_scale) / 2.0f,
        (cy - img_cy * new_scale) - view_top - (view_h - ih * new_scale) / 2.0f);
    m_window.invalidate();
}

// ── Grid mode ────────────────────────────────────────────────

void App::start_dim_preload() {
    // Snapshot the paths on the UI thread: the worker must never touch
    // m_index, which sort/scan/collection-swap mutate on the UI thread.
    // The engine owns the dimension thread and joins the previous probe
    // before starting the next one.
    std::vector<std::wstring> paths;
    paths.reserve(m_index.size());
    for (size_t i = 0; i < m_index.size(); ++i)
        paths.push_back(m_index.path_at(i));
    m_thumb_engine.start_dim_preload(std::move(paths));
}

bool App::thumb_loader_running() const noexcept {
    return m_thumb_engine.running();
}

void App::start_thumb_loader() {
    if (thumb_loader_running()) return;
    // Snapshot the index paths once on the UI thread. Workers get their own
    // copy BY VALUE and never dereference m_index, which sort/scan/swap
    // mutate on the UI thread — this removes the data race without joining
    // the decode workers on every mutation.
    std::vector<std::wstring> paths;
    paths.reserve(m_index.size());
    for (size_t i = 0; i < m_index.size(); ++i)
        paths.push_back(m_index.path_at(i));
    m_thumb_engine.start(std::move(paths), m_thumb_size, get_config_dir());
}

void App::stop_thumb_loader() {
    m_thumb_engine.stop();
}

void App::request_thumb(int idx) {
    m_thumb_engine.request(idx);
}

void App::trim_thumb_cache(int visible_start, int visible_end) {
    constexpr size_t max_d2d_entries = 256;
    while (m_thumb_d2d.size() > max_d2d_entries) {
        auto victim = m_thumb_d2d.end();
        uint64_t oldest_use = std::numeric_limits<uint64_t>::max();
        for (auto it = m_thumb_d2d.begin(); it != m_thumb_d2d.end(); ++it) {
            if (it->first >= visible_start && it->first < visible_end) continue;
            uint64_t last_use = 0;
            auto use = m_thumb_d2d_use.find(it->first);
            if (use != m_thumb_d2d_use.end()) last_use = use->second;
            if (last_use < oldest_use) {
                oldest_use = last_use;
                victim = it;
            }
        }
        if (victim == m_thumb_d2d.end()) break;
        int index = victim->first;
        m_thumb_d2d.erase(victim);
        m_thumb_d2d_use.erase(index);
        // The D2D cache (and its 256-entry LRU policy) stays in App because
        // it is GPU upload state; the engine owns the WIC side of the trim.
        m_thumb_engine.trim(index, index + 1);
    }
}

// Drives the grid<->big-image transition from the render loop (QPC
// elapsed) instead of relying on WM_TIMER(4) delivery: under message-queue
// pressure (e.g. thumb-loader posts during the big->grid switch) the
// low-priority timer messages starve and the animation froze forever.
// The timer stays as a redundant safety net.
void App::advance_transition_animation() {
    if (!m_animating) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float elapsed =
        static_cast<float>(now.QuadPart - m_anim_start) / freq.QuadPart;
    m_anim_t = elapsed / dt::kDurationTransitionSec;  // 250ms
    if (m_anim_t >= 1.0f) {
        m_anim_t = 1.0f;
        m_animating = false;
        m_anim_thumb.Reset();
        m_anim_reversed = false;
        m_anim_grid_snapshot.Reset();
        if (m_anim_timer) {
            KillTimer(m_window.handle(), 4);
            m_anim_timer = 0;
        }
        // 进入大图模式: 转场完成后胶片条自动从底部升起(曲线/时长同转场);
        // 3s 静止后按反向动画收起。转场期间不消费这次升起, 避免"闪现/卡住"。
        if (m_anim_forward && !m_grid_mode && filmstrip_showable()) {
            reveal_filmstrip();
            m_window.invalidate();
        }
    } else {
        // Keep frames flowing until the transition finishes.
        m_window.invalidate();
    }
}

void App::finish_transition_now() {
    if (!m_animating) return;
    m_anim_t = 1.0f;
    advance_transition_animation();
}

ComPtr<ID2D1Bitmap1> App::capture_window_frame() {
    const uint32_t w = m_renderer.target_size().width;
    const uint32_t h = m_renderer.target_size().height;
    if (w == 0 || h == 0) return nullptr;
    HDC win_dc = GetDC(m_window.handle());
    HDC mem_dc = CreateCompatibleDC(win_dc);
    HBITMAP hbmp = CreateCompatibleBitmap(
        win_dc, static_cast<int>(w), static_cast<int>(h));
    HGDIOBJ old = SelectObject(mem_dc, hbmp);
    const BOOL ok = PrintWindow(m_window.handle(), mem_dc, 2);
    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
    ReleaseDC(m_window.handle(), win_dc);
    if (!ok) {
        DeleteObject(hbmp);
        return nullptr;
    }
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory) {
        DeleteObject(hbmp);
        return nullptr;
    }
    ComPtr<IWICBitmap> wic;
    const HRESULT hr = factory->CreateBitmapFromHBITMAP(
        hbmp, nullptr, WICBitmapIgnoreAlpha, &wic);
    DeleteObject(hbmp);
    if (FAILED(hr) || !wic) return nullptr;
    ComPtr<ID2D1Bitmap1> d2d;
    if (FAILED(m_renderer.create_bitmap_from_wic(wic.Get(), &d2d)))
        return nullptr;
    return d2d;
}

void App::reverse_transition() {
    if (!m_animating) return;
    const float t = m_anim_t;
    // The reversal re-runs the CURRENT composition backward: the same
    // three transforms (background opacity, translation, scale) rewind in
    // place — a whole-window snapshot must never enter the animation.
    m_anim_reversed = !m_anim_reversed;
    toggle_grid();  // commit the run's new target mode
    m_from_grid = (m_anim_forward != m_anim_reversed);
    m_anim_t = 1.0f - t;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    m_anim_start = now.QuadPart - static_cast<LONGLONG>(
        m_anim_t * dt::kDurationTransitionSec * static_cast<double>(freq.QuadPart));
    m_window.invalidate();
}

// Draws the transition overlay for the current composition. FLIP model
// (per the recording's affine fit): a top-layer clone of the thumbnail
// scales uniformly and translates its center from the cell to the fitted
// image rect over 250ms, CSS ease-in-out; the content background
// crossfades with the same curve. The image layer stays fully opaque and
// the exit is the exact time-mirror of the entry.
void App::draw_transition_overlay() {
    if (!m_animating) return;
    const float p = m_anim_reversed ? (1.0f - m_anim_t) : m_anim_t;
    const uint32_t target_width = m_renderer.target_size().width;
    const uint32_t target_height = m_renderer.target_size().height;
    uint32_t image_w = static_cast<uint32_t>(m_anim_iw);
    uint32_t image_h = static_cast<uint32_t>(m_anim_ih);
    if (image_w == 0 || image_h == 0)
        m_renderer.image_size(image_w, image_h);
    if (image_w > 0 && image_h > 0) {
        // The fitted rect the image view settles on, expressed with the
        // image-mode viewport semantics (update_content_viewport with
        // m_grid_mode == false): the nav panel follows its image-mode
        // visibility and the toolbar its image-mode presence. This makes
        // the geometry identical in both render paths, so a reversal or
        // the entry/exit boundary never shifts the rect.
        const float top = (!m_fullscreen || m_toolbar_revealed)
            ? static_cast<float>(m_toolbar_h) : 0.0f;
        const float left = m_nav_panel_state.visible(false)
            ? static_cast<float>(m_nav_visible_width) : 0.0f;
        const float right = static_cast<float>(visible_panel_width());
        const float view_width = static_cast<float>(target_width)
            - left - right;
        const float view_height = static_cast<float>(target_height) - top;
        const float scale = std::min(view_width / image_w, view_height / image_h);
        const float width = image_w * scale;
        const float height = image_h * scale;
        const float x = left + (view_width - width) * 0.5f;
        const float y = top + (view_height - height) * 0.5f;
        if (m_anim_forward) {
            // Entry: the thumb flies from its cell to the fitted image rect.
            m_anim_dst = {x, y, x + width, y + height};
        } else {
            // Exit: the image flies from the fitted rect back to the
            // CURRENT image's cell. The grid has already re-centered on
            // the current image (toggle_grid -> grid_ensure_visible runs
            // before the first exit frame), so after cross-page filmstrip
            // navigation this is the re-centered cell — not the stale cell
            // the entry came from. Read it every frame so the zoom always
            // lands exactly where the settled grid will show the thumb.
            m_anim_src = {x, y, x + width, y + height};
            if (const auto cell = grid_transition_source_rect(m_current_idx))
                m_anim_dst = {cell->left, cell->top, cell->right, cell->bottom};
        }
    }
    if (m_anim_forward && m_anim_grid_snapshot)
        m_renderer.draw_fullscreen_bitmap(m_anim_grid_snapshot.Get());
    m_renderer.draw_fade_overlay(p, m_anim_forward);
    if (m_anim_forward) {
        ID2D1Bitmap1* reveal = m_renderer.transition_image();
        if (!reveal) reveal = m_renderer.placeholder_bitmap();
        if (reveal)
            m_renderer.draw_anim_image(reveal, m_anim_src, m_anim_dst, p);
    }
    if (m_anim_thumb) {
        if (m_anim_forward) {
            m_renderer.draw_anim_thumb(m_anim_thumb.Get(), m_anim_src, m_anim_dst, p);
        } else {
            // Exit: the image stays FULLY OPAQUE and shrinks from the
            // fitted rect into the cell with symmetric ease-in-out, so the
            // complete shrink stays visible (no image-opacity fade).
            m_renderer.draw_anim_thumb_faded(m_anim_thumb.Get(),
                m_anim_src, m_anim_dst, p, 1.0f, true);
        }
    }
}

void App::interrupt_transition(mv::TransitionTrigger trigger, int nav_dir) {
    if (!m_animating) return;
    // The effective target: a reversed run heads back to its source.
    const mv::TransitionDirection dir =
        (m_anim_forward != m_anim_reversed)
            ? mv::TransitionDirection::ToImage
            : mv::TransitionDirection::ToGrid;
    switch (mv::plan_transition_interrupt({dir, trigger})) {
    case mv::TransitionInterruptAction::None:
        return;
    case mv::TransitionInterruptAction::Reverse:
        reverse_transition();
        return;
    case mv::TransitionInterruptAction::FastForward:
        finish_transition_now();
        return;
    case mv::TransitionInterruptAction::FastForwardAndNavigate:
        finish_transition_now();
        if (m_grid_mode) {
            grid_navigate(nav_dir, false);
        } else if (m_has_image) {
            navigate_to(m_current_idx + nav_dir);
        }
        return;
    }
}

void App::toggle_grid() {
    m_grid_mode = !m_grid_mode;
    m_grid_layout_dirty = true;
    update_content_viewport(!m_grid_mode);

    if (m_grid_mode) {
        int n = static_cast<int>(m_index.size());
        bool re_entry = (m_thumb_engine.thumb_count() == static_cast<size_t>(n));
        if (!re_entry) {
            if (thumb_loader_running()) stop_thumb_loader();
            m_thumb_engine.pool()->thumbs.clear();
            m_thumb_engine.pool()->thumbs.resize(n);
            m_thumb_d2d.clear();
            m_thumb_d2d_use.clear();
            m_grid_layout_dirty = true;
        }
        start_thumb_loader();
        float dpi_scale = static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
        int scrollbar_zone = static_cast<int>(layout::kScrollbarZoneDip * dpi_scale);
        int grid_width = std::max(1, static_cast<int>(m_renderer.target_size().width)
            - nav_panel_width() - visible_panel_width() - scrollbar_zone - m_thumb_pad);
        rebuild_grid_layout(grid_width, GridRebuildReason::Structural);
        // Smart scroll restoration
        if (m_current_idx == m_grid_saved_idx) {
            // User didn't navigate: restore original scroll
            m_grid_scroll_y = m_grid_scroll_saved;
            m_grid_sel = m_grid_saved_idx;
            clamp_grid_scroll();
        } else {
            // User navigated: center on current image
            m_grid_sel = m_current_idx;
            m_grid_scroll_y = 0;
            grid_ensure_visible();
        }
        m_selected.clear();
        m_selected.resize(n, false);
        if (m_grid_sel >= 0 && m_grid_sel < n) m_selected[m_grid_sel] = true;
        m_sel_anchor = m_grid_sel;

        // Request first visible page of thumbnails
        int sb_zone3 = static_cast<int>(layout::kScrollbarZoneDip * static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f);
        int gw = static_cast<int>(m_renderer.target_size().width)
            - nav_panel_width() - visible_panel_width() - sb_zone3;
        int cols = std::max(1, (gw + m_thumb_gap_h) / (m_thumb_cell + m_thumb_gap_h));
        m_grid_cols = cols;
        int thumb_w = (gw - (cols - 1) * m_thumb_gap_h) / cols;
        int cell = thumb_w + m_thumb_gap_h;
        int total_rows = (n + cols - 1) / cols;
        m_grid_total_rows = total_rows;
        int rows = (static_cast<int>(m_renderer.target_size().height) - m_grid_top) / cell;

        for (int i = 0; i < std::min(n, cols * (rows + 2)); ++i)
            request_thumb(i);

        update_title();

    } else {
        // Exit grid — save state but keep thumb cache
        m_grid_scroll_saved = m_grid_scroll_y;
        m_grid_saved_idx = m_grid_sel;
        finish_grid_scroll();
        // Don't stop thumb loader or clear cache — reuse on re-entry
        reset_filmstrip_reveal();  // strip stays hidden until mouse moves
        update_title();
    }
    m_window.invalidate();
}

int App::grid_hit_test(int x, int y) const {
    int total = static_cast<int>(m_index.size());
    if (m_grid_cols <= 0 || total == 0 || m_grid_rows.empty()) return -1;

    int content_y = y - m_grid_top + m_grid_scroll_y;
    auto row_it = std::lower_bound(m_grid_rows.begin(), m_grid_rows.end(), content_y,
        [](const GridLayoutRow& row, int value) {
            return row.row_y + row.row_h + row.label_extra < value;
        });
    if (row_it == m_grid_rows.end() || content_y < row_it->row_y
        || content_y > row_it->row_y + row_it->row_h + row_it->label_extra) {
        return -1;
    }

    float content_x = static_cast<float>(x - nav_panel_width() - m_thumb_pad);
    for (int index = row_it->start_idx; index < row_it->end_idx; ++index) {
        float left = m_grid_item_x[static_cast<size_t>(index)];
        float right = left + m_grid_item_w[static_cast<size_t>(index)];
        if (content_x >= left && content_x < right) return index;
    }
    return -1;
}
bool App::grid_click(int x, int y, bool shift, bool ctrl) {
    int index = grid_hit_test(x, y);
    if (index < 0) return false;
    select_item(index, shift, ctrl);
    return true;
}
void App::select_item(int idx, bool shift, bool ctrl) {
    int total = static_cast<int>(m_index.size());
    if (!apply_grid_item_selection(
            idx, total, shift, ctrl, m_selected, m_grid_sel, m_sel_anchor))
        return;
    m_window.invalidate();
}

void App::handle_scrollbar_click(HWND hwnd, int /*mx*/, int my) {
    int view_h = static_cast<int>(m_renderer.target_size().height);
    int sb_h = view_h - m_grid_top;
    int sb_y = m_grid_top;

    // Compute thumb position (same as draw_scrollbar)
    float total = static_cast<float>(m_grid_total_h);
    float view  = static_cast<float>(view_h);
    if (total <= view) return;

    float ratio = view / total;
    float thumb_h = std::max(28.0f, sb_h * ratio);
    float range = total - view;
    float pct = (range > 0) ? std::min(1.0f, m_grid_scroll_y / range) : 0.0f;
    float thumb_y = sb_y + (sb_h - thumb_h) * pct;

    int my_f = my;

    // Check if click is on the thumb → start drag
    if (my_f >= static_cast<int>(thumb_y) && my_f < static_cast<int>(thumb_y + thumb_h)) {
        m_scrollbar_dragging = true;
        m_scrollbar_drag_y = my_f;
        m_scrollbar_drag_pos = m_grid_scroll_y;
        SetCapture(hwnd);
        return;
    }

    // Above thumb → page up
    if (my_f < static_cast<int>(thumb_y)) {
        m_grid_scroll_y = std::max(0, m_grid_scroll_y - static_cast<int>(view_h * 0.9f));
    } else {
        // Below thumb → page down
        m_grid_scroll_y = std::min(static_cast<int>(total - view),
            m_grid_scroll_y + static_cast<int>(view_h * 0.9f));
    }
    m_window.invalidate();
}

void App::grid_navigate(int dir, bool shift) {
    int total = static_cast<int>(m_index.size());
    if (total == 0) return;
    if (m_grid_sel < 0) {
        select_item(0, shift, false);
        grid_ensure_visible();
        return;
    }
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
    if (m_grid_sel < 0 || m_grid_cols <= 0) return;
    float dpi_scale = static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    int scrollbar_zone = static_cast<int>(layout::kScrollbarZoneDip * dpi_scale);
    int grid_width = std::max(1, static_cast<int>(m_renderer.target_size().width)
        - nav_panel_width() - visible_panel_width() - scrollbar_zone - m_thumb_pad);
    uint64_t generation = m_thumb_engine.dimension_generation();
    if (m_grid_layout_dirty || m_grid_layout_width != grid_width
        || m_grid_dims.size() != m_index.size()
        || m_grid_layout_generation != generation) {
        rebuild_grid_layout(grid_width, GridRebuildReason::Structural);
    }

    int row_index = m_grid_sel / m_grid_cols;
    if (row_index < 0 || row_index >= static_cast<int>(m_grid_rows.size())) return;
    int visible_height = static_cast<int>(m_renderer.target_size().height) - m_grid_top;
    const auto& row = m_grid_rows[static_cast<size_t>(row_index)];
    m_grid_scroll_y = row.row_y + row.row_h / 2 - visible_height / 2;
    int max_scroll = std::max(0, m_grid_total_h - visible_height);
    m_grid_scroll_y = std::clamp(m_grid_scroll_y, 0, max_scroll);
}

void App::clamp_grid_scroll() {
    int visible_height = static_cast<int>(m_renderer.target_size().height) - m_grid_top;
    m_grid_scroll_y = clamp_grid_scroll_position(
        m_grid_scroll_y, m_grid_total_h, visible_height);
}
// ── Filmstrip (large-image bottom strip, Issue #5 P1) ────────────────

bool App::filmstrip_showable() const {
    if (m_grid_mode || m_comic_reader.enabled() || !m_has_image) return false;
    if (m_index.empty() || m_current_idx < 0) return false;
    return true;
}

bool App::filmstrip_visible() const {
    if (m_animating) return false;
    if (!filmstrip_showable()) return false;
    // Large-image mode: hidden by default; visible while revealed or while
    // the rise/hide animation is running.
    return m_filmstrip_revealed || m_filmstrip_reveal_animating;
}

float App::filmstrip_reveal_progress() const {
    // Eased vertical progress: 0 = fully below the window edge, 1 = docked.
    const float raw = std::clamp(m_filmstrip_reveal_raw, 0.0f, 1.0f);
    return m_filmstrip_reveal_forward
        ? transition_ease_exit(raw)
        : 1.0f - transition_ease_exit(raw);
}

void App::update_filmstrip_reveal() {
    if (!m_filmstrip_reveal_animating) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float elapsed = static_cast<float>(
        now.QuadPart - m_filmstrip_reveal_start.QuadPart) / freq.QuadPart;
    m_filmstrip_reveal_raw = std::clamp(
        elapsed / dt::kDurationFilmstripRevealSec, 0.0f, 1.0f);
    if (m_filmstrip_reveal_raw >= 1.0f) {
        m_filmstrip_reveal_animating = false;
        m_filmstrip_revealed = m_filmstrip_reveal_forward;
        // A completed rise arms the 3s stillness auto-hide.
        if (m_filmstrip_revealed) schedule_filmstrip_hide();
    }
    m_window.invalidate();
}

void App::reveal_filmstrip() {
    if (m_animating || !filmstrip_showable()) return;
    // Instant pre-sync: the strip must already be centered on the current
    // item when it rises — never play a horizontal centering handoff on top
    // of the vertical rise animation.
    const int total = static_cast<int>(m_index.size());
    if (m_filmstrip.item_count() != total) m_filmstrip.set_items(total);
    const float dpi_s =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    const float strip_w = static_cast<float>(m_renderer.target_size().width)
        - static_cast<float>(visible_panel_width());
    m_filmstrip.set_viewport(strip_w, dpi_s);
    m_filmstrip.sync_current_now(m_current_idx);
    cancel_filmstrip_hide();
    m_filmstrip_reveal_forward = true;
    // Flip the raw clock so the current visual position continues smoothly.
    m_filmstrip_reveal_raw = m_filmstrip_reveal_animating
        ? 1.0f - m_filmstrip_reveal_raw
        : 0.0f;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    m_filmstrip_reveal_start.QuadPart = now.QuadPart - static_cast<LONGLONG>(
        m_filmstrip_reveal_raw * dt::kDurationFilmstripRevealSec * freq.QuadPart);
    m_filmstrip_reveal_animating = true;
}

void App::hide_filmstrip_animated() {
    cancel_filmstrip_hide();
    m_filmstrip_reveal_forward = false;
    m_filmstrip_reveal_raw = m_filmstrip_reveal_animating
        ? 1.0f - m_filmstrip_reveal_raw
        : 1.0f;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    m_filmstrip_reveal_start.QuadPart = now.QuadPart - static_cast<LONGLONG>(
        m_filmstrip_reveal_raw * dt::kDurationFilmstripRevealSec * freq.QuadPart);
    m_filmstrip_reveal_animating = true;
}

void App::reset_filmstrip_reveal() {
    cancel_filmstrip_hide();
    m_filmstrip_revealed = false;
    m_filmstrip_reveal_animating = false;
    m_filmstrip_reveal_raw = 0.0f;
}

D2D1_RECT_F App::filmstrip_rect() const {
    const float dpi_s =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    const float strip_h = layout::kFilmstripHeightDip * dpi_s;
    const float slide_down = strip_h * (1.0f - filmstrip_reveal_progress());
    const D2D1_SIZE_U ts = m_renderer.target_size();
    const float width = static_cast<float>(ts.width)
        - static_cast<float>(visible_panel_width());
    return {0.0f, static_cast<float>(ts.height) - strip_h + slide_down,
        width, static_cast<float>(ts.height) + slide_down};
}

// Returns -2 outside the strip, -1 inside the strip but between items,
// or the hit item index.
int App::filmstrip_hit_test(int x, int y) const {
    const D2D1_RECT_F rect = filmstrip_rect();
    if (x < static_cast<int>(rect.left) || x >= static_cast<int>(rect.right)
        || y < static_cast<int>(rect.top) || y >= static_cast<int>(rect.bottom)) {
        return -2;
    }
    return m_filmstrip.hit_test(static_cast<float>(x) - rect.left);
}

void App::schedule_filmstrip_hide() {
    if (m_filmstrip_timer) return;
    m_filmstrip_timer = SetTimer(m_window.handle(), kFilmstripHideTimerId,
        static_cast<UINT>(layout::kFilmstripHideDelaySeconds * 1000.0f),
        nullptr);
}

void App::cancel_filmstrip_hide() {
    if (m_filmstrip_timer) {
        KillTimer(m_window.handle(), kFilmstripHideTimerId);
        m_filmstrip_timer = 0;
    }
}

void App::render_filmstrip() {
    if (!m_renderer.is_valid()) return;
    const D2D1_RECT_F strip = filmstrip_rect();
    const float strip_w = strip.right - strip.left;
    if (strip_w < 40.0f || strip.bottom - strip.top <= 0.0f) return;
    const float dpi_scale =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    const int total = static_cast<int>(m_index.size());

    // The thumb array is owned by the grid/filmstrip shared cache; rebuild it
    // when the index changed under us (new directory, deletion, sort).
    if (static_cast<int>(m_thumb_engine.thumb_count()) != total) {
        stop_thumb_loader();
        m_thumb_engine.pool()->thumbs.clear();
        m_thumb_engine.pool()->thumbs.resize(total);
        m_thumb_d2d.clear();
        m_thumb_d2d_use.clear();
        m_filmstrip_dimension_generation = 0;  // force aspect re-sync
    }

    // Sync model with the current index / current item.
    m_filmstrip.set_viewport(strip_w, dpi_scale);
    if (m_filmstrip.item_count() != total) m_filmstrip.set_items(total);
    if (m_filmstrip.current() != m_current_idx) {
        m_filmstrip.set_current(m_current_idx);
    }

    // Sync aspect ratios when the background probe produced new dimensions.
    const uint64_t dim_gen = m_thumb_engine.dimension_generation();
    if (dim_gen != m_filmstrip_dimension_generation) {
        std::lock_guard lock(m_thumb_engine.pool()->mutex);
        for (int i = 0; i < total && i < static_cast<int>(m_thumb_engine.pool()->thumbs.size()); ++i) {
            if (m_thumb_engine.pool()->thumbs[i].orig_w > 0 && m_thumb_engine.pool()->thumbs[i].orig_h > 0) {
                m_filmstrip.set_item_aspect(i,
                    static_cast<float>(m_thumb_engine.pool()->thumbs[i].orig_w)
                        / static_cast<float>(m_thumb_engine.pool()->thumbs[i].orig_h));
            }
        }
        m_filmstrip_dimension_generation = dim_gen;
    }

    const auto [first_visible, last_visible] = m_filmstrip.visible_range();

    // Prefetch window: request/retain thumbnails beyond the visible viewport
    // so that wheel scrolling does not flash empty/black frames before the
    // decoder catches up (issue: "跨页之后前面的胶片会变黑").
    const int visible_count = std::max(1, last_visible - first_visible);
    const int prefetch = visible_count * 2;
    const int request_first = std::max(0, first_visible - prefetch);
    const int request_last = std::min(total, last_visible + prefetch);

    // Request thumbnails for the prefetch window; keep the loader alive.
    // The queue is REPLACED every frame with the current window's requests
    // (nearest-to-current first), and the worker snapshots it in batches,
    // abandoning a batch when a newer generation arrives. This keeps the
    // centered item loading first under fast scrolling without starving
    // the worker (a plain per-frame clear would discard everything it is
    // about to process).
    if (m_thumb_engine.request_window(request_first, request_last, m_current_idx)
        && !thumb_loader_running()) {
        start_thumb_loader();
    }

    // Upload newly-decoded WIC bitmaps (bounded per frame, grid pattern).
    std::vector<std::pair<int, ComPtr<IWICBitmapSource>>> ready;
    {
        std::lock_guard lock(m_thumb_engine.pool()->mutex);
        for (int i = first_visible; i < last_visible; ++i) {
            if (i < 0 || i >= static_cast<int>(m_thumb_engine.pool()->thumbs.size())) continue;
            if (m_thumb_engine.pool()->thumbs[i].loaded && !m_thumb_d2d.count(i) && m_thumb_engine.pool()->thumbs[i].wic) {
                ready.push_back({i, m_thumb_engine.pool()->thumbs[i].wic});
            }
        }
    }
    std::size_t uploaded = 0;
    for (auto& [index, wic] : ready) {
        if (uploaded >= 8) break;
        ++uploaded;
        ComPtr<ID2D1Bitmap1> bitmap;
        if (SUCCEEDED(m_renderer.create_bitmap_from_wic(wic.Get(), &bitmap))
            && bitmap) {
            m_thumb_d2d[index] = bitmap;
            m_thumb_d2d_use[index] = ++m_thumb_use_clock;
            {
                std::lock_guard lock(m_thumb_engine.pool()->mutex);
                if (index >= 0 && index < static_cast<int>(m_thumb_engine.pool()->thumbs.size())) {
                    m_thumb_engine.pool()->thumbs[index].wic.Reset();
                }
            }
        }
    }
    if (ready.size() > uploaded) {
        PostMessageW(m_window.handle(), WM_THUMB_READY, 0, 0);
    }

    // Collect draw items (visible window only).
    std::vector<FilmstripRenderItem> items;
    {
        std::lock_guard lock(m_thumb_engine.pool()->mutex);
        for (int i = first_visible; i < last_visible; ++i) {
            if (i < 0 || i >= static_cast<int>(m_thumb_engine.pool()->thumbs.size())) continue;
            const FilmstripItemRect rect = m_filmstrip.item_rect(i);
            FilmstripRenderItem item;
            item.index = i;
            item.left = rect.left;
            item.top = rect.top;
            item.width = rect.width;
            item.height = rect.height;
            item.current = (i == m_filmstrip.current());
            item.zoom = rect.zoom;
            item.placeholder_color = m_thumb_engine.pool()->thumbs[i].dominant_color;
            const auto it = m_thumb_d2d.find(i);
            if (it != m_thumb_d2d.end() && it->second) {
                item.bitmap = it->second.Get();
                m_thumb_d2d_use[i] = ++m_thumb_use_clock;
            }
            items.push_back(item);
        }
    }

    trim_thumb_cache(request_first, request_last);

    m_renderer.draw_filmstrip(strip.left, strip.top, strip_w,
        strip.bottom - strip.top, items,
        m_filmstrip.left_overflow(), m_filmstrip.right_overflow(),
        m_filmstrip.anim_t());
}

bool App::toolbar_visible() const {
    return !m_fullscreen || m_grid_mode || m_toolbar_revealed;
}

int App::visible_panel_width() const {
    return m_panel_expanded ? m_panel_width : 0;
}

void App::update_content_viewport(bool refit) {
    float top = toolbar_visible() ? static_cast<float>(m_toolbar_h) : 0.0f;
    float right = m_grid_mode ? 0.0f : static_cast<float>(visible_panel_width());
    float left = m_grid_mode ? 0.0f : static_cast<float>(nav_panel_width());
    m_renderer.set_content_viewport(top, right, left);
    if (m_comic_reader.enabled()) {
        if (!m_comic_scrollbar_dragging) m_comic_scrollbar_hover = false;
        update_comic_viewport();
        revalidate_comic_middle_anchor();
    } else if (refit) {
        fit_to_window();
    }
}

void App::update_panel_data(const std::wstring& path) {
    if (path == m_panel_path
        && (!path.empty() || (m_panel_info.empty() && m_panel_gen.empty()))) return;
    m_panel_path = path;
    m_panel_info.clear();
    m_panel_gen.clear();
    m_panel_scroll_y = 0.0f;
    m_panel_sel = -1;
    m_panel_copied.clear();
    if (path.empty()) return;

    size_t pos = path.find_last_of(L"\\/");
    std::wstring name = (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
    m_panel_info.push_back({L"\u6587\u4EF6\u540D", name});

    auto probe = m_decoder.probe(path);
    if (probe) {
        m_panel_info.push_back({L"\u5206\u8FA8\u7387",
            std::to_wstring(probe->width) + L" \u00D7 " + std::to_wstring(probe->height)});
    }

    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
        ULONGLONG size = (static_cast<ULONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
        if (size < 1024) {
            m_panel_info.push_back({L"\u5927\u5C0F", std::to_wstring(size) + L" B"});
        } else if (size < 1024 * 1024) {
            m_panel_info.push_back({L"\u5927\u5C0F", std::to_wstring(size / 1024) + L" KB"});
        } else {
            wchar_t buf[32];
            swprintf_s(buf, L"%.1f MB", size / (1024.0 * 1024.0));
            m_panel_info.push_back({L"\u5927\u5C0F", buf});
        }
    }

    if (request_metadata(path))
        m_panel_gen.push_back({L"", L"正在读取生成信息..."});
    else
        m_panel_gen.push_back({L"", L"无可用生成信息"});
}

void App::start_metadata_loader() {
    if (m_metadata_running) return;
    m_metadata_running = true;
    try {
        m_metadata_thread = std::thread(metadata_worker,
            std::ref(m_metadata_running),
            std::ref(m_metadata_mutex),
            std::ref(m_metadata_cv),
            std::ref(m_metadata_request_path),
            std::ref(m_metadata_request_pending),
            std::ref(m_metadata_result_path),
            std::ref(m_metadata_result),
            std::ref(m_metadata_result_ready),
            m_window.handle());
    } catch (...) {
        m_metadata_running = false;
    }
}

void App::stop_metadata_loader() {
    m_metadata_running = false;
    m_metadata_cv.notify_all();
    if (m_metadata_thread.joinable()) m_metadata_thread.join();
}

bool App::request_metadata(const std::wstring& path) {
    if (!m_metadata_running || path.empty()) return false;
    {
        std::lock_guard lock(m_metadata_mutex);
        m_metadata_request_path = path;
        m_metadata_request_pending = true;
    }
    m_metadata_cv.notify_one();
    return true;
}

void App::apply_metadata_result() {
    std::wstring path;
    ImageMeta metadata;
    {
        std::lock_guard lock(m_metadata_mutex);
        if (!m_metadata_result_ready) return;
        path = std::move(m_metadata_result_path);
        metadata = std::move(m_metadata_result);
        m_metadata_result_ready = false;
    }
    if (path != m_panel_path) return;
    apply_metadata(metadata);
    m_window.invalidate();
}

void App::apply_metadata(const ImageMeta& meta) {
    m_panel_gen.clear();
    if (meta.valid) {
        if (!meta.model.empty()) m_panel_gen.push_back({L"\u6A21\u578B", meta.model});
        if (!meta.vae.empty()) m_panel_gen.push_back({L"VAE", meta.vae});
        if (meta.seed >= 0) m_panel_gen.push_back({L"Seed", std::to_wstring(meta.seed)});
        if (meta.steps > 0) m_panel_gen.push_back({L"\u6B65\u6570", std::to_wstring(meta.steps)});
        if (meta.cfg > 0) {
            wchar_t buf[16];
            swprintf_s(buf, L"%.1f", meta.cfg);
            m_panel_gen.push_back({L"CFG", buf});
        }
        if (!meta.sampler.empty()) m_panel_gen.push_back({L"\u91C7\u6837\u5668", meta.sampler});
        if (!meta.scheduler.empty()) m_panel_gen.push_back({L"\u8C03\u5EA6\u5668", meta.scheduler});
        if (!meta.positive_prompt.empty())
            m_panel_gen.push_back({L"\u6B63\u5411\u63D0\u793A\u8BCD", meta.positive_prompt});
        if (!meta.negative_prompt.empty())
            m_panel_gen.push_back({L"\u53CD\u5411\u63D0\u793A\u8BCD", meta.negative_prompt});
        if (!meta.lora.empty()) m_panel_gen.push_back({L"LoRA", meta.lora});
    }
    if (m_panel_gen.empty())
        m_panel_gen.push_back({L"", L"\u65E0\u53EF\u7528\u751F\u6210\u4FE1\u606F"});
}

void App::draw_panel(const std::wstring& path, ID2D1Bitmap1* preview,
                     uint32_t preview_w, uint32_t preview_h, float top,
                     int fallback_count) {
    m_panel_clickable.clear();
    if (!m_panel_expanded) {
        m_panel_total_h = 0.0f;
        return;
    }

    update_panel_data(path);
    std::vector<std::pair<std::wstring, std::wstring>> fallback_info;
    const auto* info = &m_panel_info;
    if (path.empty() && fallback_count >= 0) {
        fallback_info.push_back({L"\u6587\u4EF6\u6570", std::to_wstring(fallback_count) + L" \u5F20"});
        info = &fallback_info;
    }

    float target_w = static_cast<float>(m_renderer.target_size().width);
    float target_h = static_cast<float>(m_renderer.target_size().height);
    float panel_w = static_cast<float>(visible_panel_width());
    float available_h = std::max(0.0f, target_h - top);
    m_panel_total_h = m_renderer.draw_side_panel(target_w - panel_w, top,
        panel_w, available_h, preview, preview_w, preview_h, *info, m_panel_gen,
        &m_panel_clickable, m_panel_sel,
        m_panel_copied.empty() ? nullptr : &m_panel_copied, m_panel_scroll_y);
    float max_scroll = std::max(0.0f, m_panel_total_h - available_h);
    m_panel_scroll_y = std::min(m_panel_scroll_y, max_scroll);
}

void App::toggle_info() {
    m_panel_expanded = !m_panel_expanded;
    m_grid_layout_dirty = true;
    if (!m_panel_expanded) m_panel_clickable.clear();
    update_content_viewport(!m_grid_mode);
    m_window.invalidate();
}

void App::toggle_thumb_square() {
    m_thumb_square = !m_thumb_square;
    m_grid_layout_dirty = true;
    m_window.invalidate();
}

bool App::toggle_grid_labels() {
    if (!apply_grid_label_toggle(
            m_grid_mode, m_show_labels, m_grid_layout_dirty)) {
        return false;
    }
    m_window.invalidate();
    return true;
}

// ── Multi-select helpers ─────────────────────────────────

bool App::has_selection() const {
    if (m_selected.size() != m_index.size()) return false;
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

void App::rebuild_grid_layout(int grid_area_width, GridRebuildReason reason) {
    int total = static_cast<int>(m_index.size());
    // Anchor the first visible row before dimension-driven rebuilds so that
    // skeleton -> real-thumbnail aspect changes do not push the viewport
    // (fast scroll stops, then rows above grow and content drifts down).
    int anchor_item = -1;
    int anchor_offset = 0;
    if (reason == GridRebuildReason::BackgroundDimensions) {
        for (const auto& row : m_grid_rows) {
            if (row.row_y + row.row_h + row.label_extra > m_grid_scroll_y) {
                anchor_item = row.start_idx;
                anchor_offset = m_grid_scroll_y - row.row_y;
                break;
            }
        }
    }
    m_grid_dims.assign(static_cast<size_t>(total), {0, 0});
    uint64_t applied_dimension_generation = 0;
    {
        std::lock_guard lock(m_thumb_engine.pool()->mutex);
        int count = std::min(total, static_cast<int>(m_thumb_engine.pool()->thumbs.size()));
        for (int i = 0; i < count; ++i)
            m_grid_dims[static_cast<size_t>(i)] = {m_thumb_engine.pool()->thumbs[i].orig_w, m_thumb_engine.pool()->thumbs[i].orig_h};
        applied_dimension_generation =
            m_thumb_engine.pool()->dimension_generation.load(std::memory_order_relaxed);
    }

    float dpi_scale = static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    int effective_cell = std::max(1, static_cast<int>(m_thumb_cell * m_thumb_zoom));
    GridLayoutInput input;
    input.item_count = total;
    input.area_width = grid_area_width;
    input.cell = effective_cell;
    input.gap_h = m_thumb_gap_h;
    input.gap_v = m_thumb_gap_v;
    input.pad = m_thumb_pad;
    input.square = m_thumb_square;
    input.show_labels = m_show_labels;
    input.dpi_scale = dpi_scale;
    input.dims = m_grid_dims;
    GridLayout planned = plan_grid_layout(input);

    m_grid_cols = planned.cols;
    m_grid_rows = std::move(planned.rows);
    m_grid_item_x = std::move(planned.item_x);
    m_grid_item_w = std::move(planned.item_w);

    m_grid_total_rows = static_cast<int>(m_grid_rows.size());
    m_grid_total_h = planned.total_height;
    const int visible_height = static_cast<int>(m_renderer.target_size().height) - m_grid_top;
    const int selected_row = m_grid_sel >= 0 && m_grid_cols > 0
        ? m_grid_sel / m_grid_cols : -1;
    const bool has_selected_row =
        selected_row >= 0 && selected_row < static_cast<int>(m_grid_rows.size());
    // Dimension-driven rebuild without a selection: keep the anchored row
    // pinned so the viewport does not drift while thumbnails stream in.
    if (reason == GridRebuildReason::BackgroundDimensions && !has_selected_row
        && anchor_item >= 0 && anchor_item < total && m_grid_cols > 0) {
        const int anchor_row = anchor_item / m_grid_cols;
        if (anchor_row >= 0
            && anchor_row < static_cast<int>(m_grid_rows.size())) {
            m_grid_scroll_y = m_grid_rows[static_cast<size_t>(anchor_row)].row_y
                + anchor_offset;
        }
    }
    if (has_selected_row) {
        const auto& row = m_grid_rows[static_cast<size_t>(selected_row)];
        m_grid_scroll_y = reconcile_grid_scroll_after_rebuild(
            reason, m_grid_scroll_y, true, row.row_y,
            row.row_y + row.row_h + row.label_extra,
            m_grid_total_h, visible_height);
    } else {
        m_grid_scroll_y = reconcile_grid_scroll_after_rebuild(
            reason, m_grid_scroll_y, false, 0, 0,
            m_grid_total_h, visible_height);
    }
    m_row_heights.clear();
    m_row_heights.reserve(m_grid_rows.size());
    for (const auto& row : m_grid_rows)
        m_row_heights.push_back(row.row_h + m_thumb_gap_v + row.label_extra);
    m_grid_layout_width = grid_area_width;
    m_grid_layout_generation = applied_dimension_generation;
    m_grid_layout_dirty = false;
}

void App::grid_render() {
    if (!m_renderer.begin_frame()) return;
    if (!synchronize_renderer_generation()) {
        m_renderer.end_frame();
        PostMessageW(m_window.handle(), WM_RENDER_RETRY, 0, 0);
        return;
    }
    m_renderer.clear();

    int total = static_cast<int>(m_index.size());
    float dpi_scale = static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    int scrollbar_zone = static_cast<int>(layout::kScrollbarZoneDip * dpi_scale);
    int grid_area_width = std::max(1, static_cast<int>(m_renderer.target_size().width)
        - nav_panel_width() - visible_panel_width() - scrollbar_zone - m_thumb_pad);
    uint64_t dimension_generation = m_thumb_engine.dimension_generation();
    const GridRebuildReason rebuild_reason = classify_grid_rebuild_reason(
        m_grid_layout_dirty, m_grid_layout_width != grid_area_width,
        m_grid_dims.size() != static_cast<size_t>(total),
        dimension_generation != m_grid_layout_generation);
    if (rebuild_reason != GridRebuildReason::None)
        rebuild_grid_layout(grid_area_width, rebuild_reason);

    auto& rows = m_grid_rows;
    int visible_height = static_cast<int>(m_renderer.target_size().height) - m_grid_top;
    int top_pixel = m_grid_scroll_y;
    auto top_it = std::lower_bound(rows.begin(), rows.end(), top_pixel,
        [](const GridLayoutRow& row, int value) {
            return row.row_y + row.row_h + row.label_extra < value;
        });
    auto bottom_it = std::upper_bound(top_it, rows.end(), top_pixel + visible_height,
        [](int value, const GridLayoutRow& row) { return value < row.row_y; });
    int top_row = static_cast<int>(top_it - rows.begin());
    int bottom_row = bottom_it == rows.begin() ? -1
        : static_cast<int>(bottom_it - rows.begin()) - 1;

    const bool loader_running =
        thumb_loader_running();
    for (int r = top_row; r <= bottom_row; ++r) {
        const auto& row = rows[static_cast<size_t>(r)];
        m_grid_scroll_pause.request_visible(
            loader_running, row.start_idx, row.end_idx,
            [this](int index) { request_thumb(index); });
    }

    std::vector<std::pair<int, ComPtr<IWICBitmapSource>>> ready;
    std::unordered_map<int, D2D1_COLOR_F> placeholder_colors;
    {
        std::lock_guard lock(m_thumb_engine.pool()->mutex);
        for (int r = top_row; r <= bottom_row; ++r) {
            const auto& row = rows[static_cast<size_t>(r)];
            for (int i = row.start_idx; i < row.end_idx; ++i) {
                if (i >= static_cast<int>(m_thumb_engine.pool()->thumbs.size())) continue;
                placeholder_colors.emplace(i, m_thumb_engine.pool()->thumbs[i].dominant_color);
                if (m_thumb_engine.pool()->thumbs[i].loaded && !m_thumb_d2d.count(i) && m_thumb_engine.pool()->thumbs[i].wic)
                    ready.push_back({i, m_thumb_engine.pool()->thumbs[i].wic});
            }
        }
    }
    int upload_count = 0;
    size_t processed_count = 0;
    for (auto& [index, wic] : ready) {
        if (processed_count >= 4) break;
        ++processed_count;
        ComPtr<ID2D1Bitmap1> bitmap;
        if (SUCCEEDED(m_renderer.create_bitmap_from_wic(wic.Get(), &bitmap)) && bitmap) {
            m_thumb_d2d[index] = bitmap;
            m_thumb_d2d_use[index] = ++m_thumb_use_clock;
            {
                std::lock_guard lock(m_thumb_engine.pool()->mutex);
                if (index >= 0 && index < static_cast<int>(m_thumb_engine.pool()->thumbs.size()))
                    m_thumb_engine.pool()->thumbs[index].wic.Reset();
            }
            ++upload_count;
        }
    }
    if (ready.size() > processed_count)
        PostMessageW(m_window.handle(), WM_THUMB_READY, 0, 0);
    int visible_start = top_row <= bottom_row
        ? rows[static_cast<size_t>(top_row)].start_idx : 0;
    int visible_end = top_row <= bottom_row
        ? rows[static_cast<size_t>(bottom_row)].end_idx : 0;
    trim_thumb_cache(visible_start, visible_end);

    if (m_animating || (m_grid_sel >= 0 && m_grid_sel != m_last_cached_sel)) {
        m_last_cached_sel = m_grid_sel;
        const auto rect = grid_transition_source_rect(m_grid_sel);
        m_anim_src = rect
            ? D2D1_RECT_F{rect->left, rect->top, rect->right, rect->bottom}
            : D2D1_RECT_F{};
    }

    // Grid breadcrumb strip (top of the grid content area) + left nav panel
    render_grid_breadcrumb();

    float target_width = static_cast<float>(m_renderer.target_size().width);
    float target_height = static_cast<float>(m_renderer.target_size().height);
    m_renderer.push_clip_below(static_cast<float>(m_grid_top));
    for (int r = top_row; r <= bottom_row; ++r) {
        const auto& row = rows[static_cast<size_t>(r)];
        float row_y = static_cast<float>(m_grid_top + row.row_y - m_grid_scroll_y);
        for (int index = row.start_idx; index < row.end_idx; ++index) {
            float x = static_cast<float>(nav_panel_width())
                + m_grid_item_x[static_cast<size_t>(index)] + m_thumb_pad;
            float width = m_grid_item_w[static_cast<size_t>(index)];
            auto bitmap = m_thumb_d2d.find(index);
            if (bitmap != m_thumb_d2d.end() && bitmap->second) {
                m_thumb_d2d_use[index] = ++m_thumb_use_clock;
                m_renderer.draw_grid_thumbnail(x, row_y, width,
                    static_cast<float>(row.row_h), bitmap->second.Get(), m_thumb_square);
            } else {
                auto color = placeholder_colors.find(index);
                D2D1_COLOR_F fill = color == placeholder_colors.end()
                    ? dt::d2d(dt::kColorPlaceholder) : color->second;
                m_renderer.draw_grid_placeholder(x, row_y, width,
                    static_cast<float>(row.row_h), fill);
            }

            bool selected = grid_item_has_selection_border(
                index, m_grid_sel, m_selected);
            if (selected) {
                D2D1_RECT_F rect = {x - 2, row_y - 2, x + width + 2,
                    row_y + row.row_h + 2};
                m_renderer.draw_selection_border(rect);
            }
            if (mv::is_favourite(
                    m_album_store, m_index.path_at(index))) {
                const float badge = dt::kSpace20Dip * dpi_scale;
                m_renderer.draw_favourite_badge(
                    x + width - badge - dt::kSpace3Dip * dpi_scale,
                    row_y + dt::kSpace3Dip * dpi_scale, badge);
            }
            if (m_show_labels) {
                const auto& path = m_index.path_at(index);
                size_t separator = path.find_last_of(L"\\/");
                std::wstring name = separator == std::wstring::npos
                    ? path : path.substr(separator + 1);
                float label_y = row_y + row.row_h + dt::kSpaceXsDip * dpi_scale;
                m_renderer.draw_label(x, label_y, width, name, dt::kFontSizeXlDip);
                float name_height = m_renderer.label_height(name, width, dt::kFontSizeXlDip);
                auto [image_w, image_h] = m_grid_dims[static_cast<size_t>(index)];
                if (image_w > 0 && image_h > 0) {
                    m_renderer.draw_label(x, label_y + name_height + dt::kSpace3Dip * dpi_scale,
                        width, std::to_wstring(image_w) + L" \u00D7 " + std::to_wstring(image_h),
                        dt::kFontSizeMdDip, 0.5f, 0.5f, 0.55f);
                }
            }
        }
    }

    float scrollbar_x = target_width - visible_panel_width() - scrollbar_zone;
    float scrollbar_width = scrollbar_zone * 0.6f;
    float scrollbar_left = scrollbar_x + (scrollbar_zone - scrollbar_width) * 0.5f;
    m_renderer.draw_scrollbar(scrollbar_left, static_cast<float>(m_grid_top),
        scrollbar_width, target_height - m_grid_top, static_cast<float>(m_grid_total_h),
        target_height, static_cast<float>(m_grid_scroll_y),
        m_scrollbar_dragging || m_scrollbar_hover);

    std::wstring panel_path;
    ID2D1Bitmap1* preview = nullptr;
    uint32_t preview_w = 0, preview_h = 0;
    if (m_grid_sel >= 0 && m_grid_sel < total) {
        panel_path = m_index.path_at(m_grid_sel);
        auto bitmap = m_thumb_d2d.find(m_grid_sel);
        if (bitmap != m_thumb_d2d.end()) preview = bitmap->second.Get();
        preview_w = m_grid_dims[static_cast<size_t>(m_grid_sel)].first;
        preview_h = m_grid_dims[static_cast<size_t>(m_grid_sel)].second;
    }
    draw_panel(panel_path, preview, preview_w, preview_h,
        static_cast<float>(m_grid_top), total);
    m_renderer.pop_clip();

    // Left navigation panel (over content, right side of the title bar)
    if (nav_panel_visible()) {
        render_nav_panel(0.0f, static_cast<float>(m_toolbar_h),
            static_cast<float>(m_nav_visible_width),
            target_height - m_toolbar_h);
    }

    if (m_animating)
        draw_transition_overlay();
    m_renderer.draw_status_message(m_open_error);
    m_renderer.draw_title_bar(target_width, m_title_btn_hover, m_title_btn_press,
        m_toolbar_items, m_toolbar_active);
    if (!m_renderer.end_frame())
        PostMessageW(m_window.handle(), WM_RENDER_RETRY, 0, 0);
}

void App::render_frame() {
    advance_transition_animation();
    update_filmstrip_reveal();
    check_async_timeout();
    if (m_comic_reader.auto_scroll_owner()
        != mv::ComicAutoScrollOwner::None) {
        advance_comic_autoscroll_render();
    }
    if (m_grid_mode) {
        grid_render();
        return;
    }
    // Refresh the pre-scaled image cache before the draw session starts
    // (its rebuild runs its own BeginDraw/EndDraw on the offscreen
    // target, so it must not happen inside the main session).
    m_renderer.ensure_image_scaled();
    if (!m_renderer.begin_frame()) {
        // GDI fallback — paint dark background with status text
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_window.handle(), &ps);
        if (hdc) {
            RECT rc = ps.rcPaint;
            HBRUSH bg = CreateSolidBrush(dt::kColorWindowBgGdi);  // #1a1a1a
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, dt::kColorFallbackTextGdi);
            const wchar_t* msg = !m_open_error.empty() ? m_open_error.c_str()
                : (m_has_image ? L"\u52A0\u8F7D\u4E2D..."
                : (m_index.directory().empty() ? L"\u62D6\u5165\u56FE\u7247\u6216\u53F3\u952E\u6253\u5F00\u6587\u4EF6"
                    : L"\u5F53\u524D\u6587\u4EF6\u5939\u6682\u65E0\u53D7\u652F\u6301\u7684\u56FE\u7247\uFF0C\u53EF\u5F00\u542F\u9012\u5F52\u6D4F\u89C8"));
            DrawTextW(hdc, msg, -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(m_window.handle(), &ps);
        }
        return;
    }
    if (!synchronize_renderer_generation()) {
        m_renderer.end_frame();
        PostMessageW(m_window.handle(), WM_RENDER_RETRY, 0, 0);
        return;
    }

    // Advance filmstrip scroll/zoom transition using REAL elapsed time so
    // dropped frames slow the rendering but never stretch the animation.
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float anim_dt = 1.0f / 60.0f;
    if (m_last_anim_tick.QuadPart != 0) {
        anim_dt = static_cast<float>(now.QuadPart - m_last_anim_tick.QuadPart)
            / static_cast<float>(freq.QuadPart);
        anim_dt = std::clamp(anim_dt, 0.0f, 0.05f);
    }
    m_last_anim_tick = now;
    if (m_filmstrip.advance_animation(anim_dt)) {
        m_window.invalidate();
    }
    // An image that arrived DURING the transition is uploaded now that the
    // animation is over — never between animation frames (the GPU upload
    // would stall the transition).
    if (!m_filmstrip.animating() && m_pending_image) {
        ComPtr<IWICBitmapSource> decoded = m_pending_image;
        m_pending_image.Reset();
        const bool still_current = (m_pending_path == m_current_path);
        m_pending_path.clear();
        // A pending image whose page was already flipped past would flash
        // a stale frame — only show it when it is still the current target.
        if (still_current) {
            try {
                if (m_renderer.upload_image(decoded.Get())) {
                    m_current_wic = decoded;
                    m_has_image = true;
                    m_renderer.clear_placeholder();
                    m_placeholder_idx = -1;
                    update_content_viewport(false);
                    fit_to_window();
                    preload_neighbors();
                    m_window.invalidate();
                }
            } catch (...) {
                // Keep the previous image on failure.
            }
        }
    }
    m_renderer.clear();
    float tw = static_cast<float>(m_renderer.target_size().width);
    float content_top = m_renderer.content_top();
    m_renderer.push_clip_below(content_top);
    if (m_comic_reader.enabled()) {
        render_comic_reader(content_top);
    } else if (m_has_image) {
        // During the entry transition the overlay (grid snapshot + veil +
        // zooming image) covers the whole content area, so the static image
        // and panel layers are skipped to keep every animation frame inside
        // the display-refresh budget. Logic/geometry unchanged.
        if (!m_animating) {
            m_renderer.draw_image();
            const float filmstrip_h = filmstrip_visible()
                ? layout::kFilmstripHeightDip
                    * (static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f)
                    * filmstrip_reveal_progress()
                : 0.0f;
            m_renderer.draw_overlay(filmstrip_h);
            uint32_t image_w = 0, image_h = 0;
            m_renderer.image_size(image_w, image_h);
            draw_panel(m_current_path, m_renderer.image_bitmap(), image_w, image_h, content_top);
        }
    } else {
        m_renderer.draw_hint(m_index.directory().empty()
            ? L"\u62D6\u5165\u56FE\u7247\u6216\u53F3\u952E\u6253\u5F00\u6587\u4EF6"
            : L"\u5F53\u524D\u6587\u4EF6\u5939\u6682\u65E0\u53D7\u652F\u6301\u7684\u56FE\u7247\uFF0C\u53EF\u5F00\u542F\u9012\u5F52\u6D4F\u89C8");
        draw_panel(L"", nullptr, 0, 0, content_top);
    }
    m_renderer.pop_clip();
    // Left navigation panel (over content, right side of the title bar)
    if (nav_panel_visible()) {
        render_nav_panel(0.0f, content_top,
            static_cast<float>(m_nav_visible_width),
            static_cast<float>(m_renderer.target_size().height) - content_top);
    }
    if (m_animating)
        draw_transition_overlay();
    if (filmstrip_visible()) render_filmstrip();
    m_renderer.draw_status_message(m_open_error);
    if (toolbar_visible()) {
        m_renderer.draw_title_bar(tw, m_title_btn_hover, m_title_btn_press,
            m_toolbar_items, m_toolbar_active);
    }
    if (!m_renderer.end_frame())
        PostMessageW(m_window.handle(), WM_RENDER_RETRY, 0, 0);
}

bool App::synchronize_renderer_generation() {
    const uint64_t current_generation = m_renderer.device_generation();
    if (!renderer_generation_changed(m_renderer_generation, current_generation)) return true;

    m_thumb_d2d.clear();
    m_thumb_d2d_use.clear();
    m_anim_thumb.Reset();
    for (auto& page : m_comic_pages) page.d2d.Reset();
    {
        std::lock_guard lock(m_thumb_engine.pool()->mutex);
        for (auto& thumb : m_thumb_engine.pool()->thumbs) {
            if (!thumb.wic) thumb.loaded = false;
        }
    }

    if (m_has_image && m_current_wic
        && !m_renderer.upload_image(m_current_wic.Get(), false)) return false;

    m_renderer_generation = current_generation;
    return true;
}

// ── Left navigation panel + breadcrumbs (Issue #5 P2) ────────

bool App::nav_panel_visible() const {
    return m_nav_panel_state.visible(m_grid_mode);
}

int App::nav_panel_width() const {
    return nav_panel_visible() ? m_nav_visible_width : 0;
}

void App::toggle_nav_panel() {
    m_nav_panel_state.toggle();
    if (!m_nav_panel_state.expanded()) {
        m_nav_row_hover = -1;
        m_nav_breadcrumb_hover_panel = -1;
        m_nav_breadcrumb_hover_grid = -1;
        m_nav_tree_focus_id = 0;
    }
    m_grid_layout_dirty = true;
    update_content_viewport(!m_grid_mode);
    m_window.invalidate();
}

void App::cycle_nav_focus() {
    const bool now_focused = m_nav_panel_state.cycle_focus();
    if (now_focused && m_nav_tree_focus_id == 0)
        m_nav_tree_focus_id = m_nav_highlight_id;
    m_window.invalidate();
}

bool App::handle_nav_panel_key(HWND hwnd, WPARAM wp, bool ctrl, bool shift) {
    (void)hwnd;
    (void)shift;
    if (ctrl) return false;  // Ctrl shortcuts keep working (Ctrl+R/C/O)
    switch (wp) {
    case VK_ESCAPE:
        m_nav_panel_state.release_focus();
        m_window.invalidate();
        return true;
    case VK_UP:
        if (m_nav_tree_focus_id == 0)
            m_nav_tree_focus_id = m_nav_highlight_id;
        m_nav_tree_focus_id = m_nav_tree.focus_prev(m_nav_tree_focus_id);
        nav_ensure_focus_visible();
        m_window.invalidate();
        return true;
    case VK_DOWN:
        if (m_nav_tree_focus_id == 0)
            m_nav_tree_focus_id = m_nav_highlight_id;
        m_nav_tree_focus_id = m_nav_tree.focus_next(m_nav_tree_focus_id);
        nav_ensure_focus_visible();
        m_window.invalidate();
        return true;
    case VK_RIGHT:
        if (m_nav_tree_focus_id != 0)
            request_nav_tree_expand(m_nav_tree_focus_id);
        m_window.invalidate();
        return true;
    case VK_LEFT:
        if (m_nav_tree_focus_id != 0)
            m_nav_tree.collapse(m_nav_tree_focus_id);
        m_window.invalidate();
        return true;
    case VK_RETURN: {
        const NavTreeNode* node = m_nav_tree.node(m_nav_tree_focus_id);
        if (node) switch_collection(node->path, false);
        return true;
    }
    default:
        // Panel focus consumes main shortcuts (N/D/S/R/A/L/Space …)
        return true;
    }
}

void App::start_nav_workers() {
    m_nav_scan_running = true;
    try {
        m_nav_scan_thread = std::thread(nav_scan_worker,
            std::ref(m_nav_scan_running), std::ref(m_nav_scan_mutex),
            std::ref(m_nav_scan_cv), std::ref(m_nav_scan_queued),
            std::ref(m_nav_scan_job), std::ref(m_nav_scan_ready),
            std::ref(m_nav_scan_result), m_window.handle());
    } catch (...) {
        m_nav_scan_running = false;
    }
    m_nav_tree_running = true;
    try {
        m_nav_tree_thread = std::thread(nav_tree_worker,
            std::ref(m_nav_tree_running), std::ref(m_nav_tree_mutex),
            std::ref(m_nav_tree_cv), std::ref(m_nav_tree_queued),
            std::ref(m_nav_tree_job), std::ref(m_nav_tree_outcome_ready),
            std::ref(m_nav_tree_outcome), m_window.handle());
    } catch (...) {
        m_nav_tree_running = false;
    }
}

void App::stop_nav_workers() {
    m_nav_scan_running = false;
    m_nav_scan_cv.notify_all();
    if (m_nav_scan_thread.joinable()) m_nav_scan_thread.join();
    m_nav_tree_running = false;
    m_nav_tree_cv.notify_all();
    if (m_nav_tree_thread.joinable()) m_nav_tree_thread.join();
}

void App::start_dir_watch() {
    if (m_watch_roots.empty()) return;
    std::vector<WatchRoot> roots;
    roots.reserve(m_watch_roots.size());
    for (const auto& root : m_watch_roots)
        roots.push_back(WatchRoot{root.path, root.recursive});
    m_dir_watcher.watch(m_window.handle(), WM_DIR_CHANGED, std::move(roots));
}

void App::stop_dir_watch() {
    m_dir_watcher.stop();
}

void App::load_album_store() {
    if (m_album_loaded) return;
    m_album_loaded = true;
    std::ifstream file(get_config_dir() + L"\\albums.json", std::ios::binary);
    if (!file) return;  // 首次运行:空集合
    const std::string text((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    const auto parsed = mv::parse_album_store(text);
    if (parsed) {
        m_album_store = *parsed;
    } else {
        m_album_store = AlbumStore{};
        m_open_error = L"\u6536\u85CF\u6570\u636E\u5DF2\u91CD\u7F6E(\u6587\u4EF6\u635F\u574F)\u3002";
        m_window.invalidate();
    }
}

void App::save_album_store() {
    const std::string json = mv::serialize_album_store(m_album_store);
    std::ofstream file(get_config_dir() + L"\\albums.json",
        std::ios::binary | std::ios::trunc);
    if (file)
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
}

std::vector<ScanRoot> App::album_scan_roots() const {
    std::vector<ScanRoot> roots;
    if (m_album_sel < 0
        || m_album_sel >= static_cast<int>(m_album_store.albums.size()))
        return roots;
    const auto& album = m_album_store.albums[static_cast<size_t>(m_album_sel)];
    for (const auto& folder : album.folders)
        roots.push_back(ScanRoot{folder.path, folder.recursive});
    return roots;
}

std::vector<ScanRoot> App::album_watch_roots() const {
    return album_scan_roots();
}

void App::open_favourites_collection() {
    const std::uint64_t generation = m_nav_switch.request();
    NavScanJob job;
    job.generation = generation;
    job.sort = m_collection_memory.memory_for(L"favourites").sort;
    job.paths = m_album_store.favourites;
    job.album_name = L"\u6536\u85CF";
    // Watch the unique parent directories of the favourite images.
    for (const auto& path : m_album_store.favourites) {
        const std::wstring parent =
            std::filesystem::path(path).parent_path().wstring();
        bool found = false;
        for (const auto& root : job.watch_roots)
            if (_wcsicmp(root.path.c_str(), parent.c_str()) == 0) found = true;
        if (!found) job.watch_roots.push_back(ScanRoot{parent, false});
    }
    m_fav_selected = true;
    m_album_sel = -1;
    {
        std::lock_guard lock(m_nav_scan_mutex);
        m_nav_scan_job = std::move(job);
        m_nav_scan_queued = true;
    }
    m_nav_scan_cv.notify_one();
    m_window.invalidate();
}

void App::open_album_collection(int index) {
    if (index < 0
        || index >= static_cast<int>(m_album_store.albums.size())) return;
    const auto& album = m_album_store.albums[static_cast<size_t>(index)];
    const std::uint64_t generation = m_nav_switch.request();
    NavScanJob job;
    job.generation = generation;
    job.sort = m_collection_memory.memory_for(L"album:" + album.name).sort;
    job.album_name = album.name;
    for (const auto& folder : album.folders)
        job.roots.push_back(ScanRoot{folder.path, folder.recursive});
    job.watch_roots = job.roots;
    m_fav_selected = false;
    m_album_sel = index;
    {
        std::lock_guard lock(m_nav_scan_mutex);
        m_nav_scan_job = std::move(job);
        m_nav_scan_queued = true;
    }
    m_nav_scan_cv.notify_one();
    m_window.invalidate();
}

void App::toggle_album_folder_view() {
    switch (m_album_store.folder_view) {
    case AlbumFolderView::Tree:
        m_album_store.folder_view = AlbumFolderView::Icons2;
        break;
    case AlbumFolderView::Icons2:
        m_album_store.folder_view = AlbumFolderView::Icons3;
        break;
    case AlbumFolderView::Icons3:
        m_album_store.folder_view = AlbumFolderView::Tree;
        break;
    }
    save_album_store();
    m_window.invalidate();
}

void App::create_album() {
    int n = 1;
    std::wstring name;
    do {
        name = L"\u76F8\u518C " + std::to_wstring(n++);
    } while (!mv::add_album(m_album_store, name));
    save_album_store();
    m_window.invalidate();
}

void App::delete_album(int index) {
    if (index < 0
        || index >= static_cast<int>(m_album_store.albums.size())) return;
    const std::wstring name = m_album_store.albums[static_cast<size_t>(index)].name;
    const std::wstring prompt =
        L"\u5220\u9664\u76F8\u518C\u300C" + name
        + L"\u300D\uFF1F\u4E0D\u4F1A\u5220\u9664\u4EFB\u4F55\u672C\u5730\u6587\u4EF6\u3002";
    const int answer = MessageBoxW(m_window.handle(), prompt.c_str(),
        L"MinView", MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES) return;
    const bool was_active = (m_album_sel == index && !m_fav_selected);
    mv::remove_album(m_album_store, static_cast<size_t>(index));
    save_album_store();
    if (was_active) {
        m_album_sel = -1;
        m_active_album_name.clear();
        m_nav_display_segments.clear();
    }
    m_window.invalidate();
}

void App::add_folder_to_album(int index) {
    if (index < 0
        || index >= static_cast<int>(m_album_store.albums.size())) return;
    BROWSEINFOW bi = {};
    bi.hwndOwner = m_window.handle();
    bi.lpszTitle = L"\u9009\u62E9\u8981\u6DFB\u52A0\u7684\u6587\u4EF6\u5939";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t dir[MAX_PATH] = {};
    if (SHGetPathFromIDListW(pidl, dir)) {
        CoTaskMemFree(pidl);
        const int answer = MessageBoxW(m_window.handle(),
            L"\u9012\u5F52\u52A0\u8F7D\u5B50\u6587\u4EF6\u5939\uFF1F",
            L"MinView", MB_YESNO | MB_ICONQUESTION);
        const bool recursive = (answer == IDYES);
        if (mv::add_folder(m_album_store, static_cast<size_t>(index),
                dir, recursive)) {
            save_album_store();
            if (m_album_sel == index && !m_fav_selected)
                open_album_collection(index);
            else
                m_window.invalidate();
        }
    } else {
        CoTaskMemFree(pidl);
    }
}

void App::remove_album_folder(int album, int folder) {
    if (mv::remove_folder(m_album_store, static_cast<size_t>(album),
            static_cast<size_t>(folder))) {
        save_album_store();
        if (m_album_sel == album && !m_fav_selected)
            open_album_collection(album);
        else
            m_window.invalidate();
    }
}

void App::move_album_folder(int album, int from, int to) {
    if (mv::move_folder(m_album_store, static_cast<size_t>(album),
            static_cast<size_t>(from), static_cast<size_t>(to))) {
        save_album_store();
        if (m_album_sel == album && !m_fav_selected)
            open_album_collection(album);
        else
            m_window.invalidate();
    }
}

void App::toggle_album_folder_recursive(int album, int folder) {
    if (album < 0
        || album >= static_cast<int>(m_album_store.albums.size())) return;
    const bool next =
        !m_album_store.albums[static_cast<size_t>(album)]
            .folders[static_cast<size_t>(folder)].recursive;
    if (mv::set_folder_recursive(m_album_store, static_cast<size_t>(album),
            static_cast<size_t>(folder), next)) {
        save_album_store();
        if (m_album_sel == album && !m_fav_selected)
            open_album_collection(album);
        else
            m_window.invalidate();
    }
}

void App::show_album_row_menu(HWND hwnd, int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const auto& t = m_album_menu_target;
    if (t.folder_row) {
        bool recursive = false;
        if (t.album >= 0 && t.folder >= 0
            && t.album < static_cast<int>(m_album_store.albums.size())) {
            const auto& folders =
                m_album_store.albums[static_cast<size_t>(t.album)].folders;
            if (t.folder < static_cast<int>(folders.size()))
                recursive =
                    folders[static_cast<size_t>(t.folder)].recursive;
        }
        AppendMenuW(menu, MF_STRING | (recursive ? MF_CHECKED : 0), 26,
            L"递归加载子文件夹");
        AppendMenuW(menu, MF_STRING, 24, L"上移");
        AppendMenuW(menu, MF_STRING, 25, L"下移");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 23,
            L"从相册移除文件夹");
    } else if (t.album >= 0) {
        AppendMenuW(menu, MF_STRING, 22,
            L"添加文件夹...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 21, L"删除相册");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 20, L"新建相册");
    } else {
        AppendMenuW(menu, MF_STRING, 20, L"新建相册");
    }
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        x, y, 0, hwnd, nullptr);
    switch (cmd) {
    case 20: create_album(); break;
    case 21: delete_album(t.album); break;
    case 22: add_folder_to_album(t.album); break;
    case 23: remove_album_folder(t.album, t.folder); break;
    case 24: move_album_folder(t.album, t.folder, t.folder - 1); break;
    case 25: move_album_folder(t.album, t.folder, t.folder + 1); break;
    case 26: toggle_album_folder_recursive(t.album, t.folder); break;
    default: break;
    }
    DestroyMenu(menu);
}

// ── Folder-icon grid (Issue #5 P3c) ─────────────────────────────

void App::rebuild_folder_samples() {
    if (m_fav_selected || m_album_sel < 0
        || m_album_sel >= static_cast<int>(m_album_store.albums.size()))
        return;
    const auto& album =
        m_album_store.albums[static_cast<size_t>(m_album_sel)];
    std::vector<std::wstring> paths;
    paths.reserve(m_index.size());
    for (size_t i = 0; i < m_index.size(); ++i)
        paths.push_back(m_index.path_at(i));
    std::vector<std::wstring> folders;
    folders.reserve(album.folders.size());
    for (const auto& folder : album.folders)
        folders.push_back(folder.path);

    std::vector<std::vector<std::wstring>> samples =
        sample_paths_per_folder(paths, folders);
    m_folder_samples.clear();
    for (size_t f = 0; f < album.folders.size(); ++f)
        m_folder_samples[album.folders[f].path] = std::move(samples[f]);
    m_folder_icon_cache.clear();
}

void App::start_folder_icon_worker() {
    if (m_folder_icon_thread.joinable()) return;
    m_folder_icon_stop = false;
    m_folder_icon_thread = std::thread([this]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            Decoder decoder;
            for (;;) {
                FolderIconJob job;
                {
                    std::unique_lock lock(m_folder_icon_mutex);
                    m_folder_icon_cv.wait(lock, [this] {
                        return m_folder_icon_stop
                            || !m_folder_icon_queue.empty();
                    });
                    if (m_folder_icon_stop) break;
                    job = std::move(m_folder_icon_queue.front());
                    m_folder_icon_queue.pop_front();
                }
                auto result = std::make_unique<FolderIconResult>();
                result->folder = job.folder;
                result->tile = job.tile;
                try {
                    result->wic = decoder.decode_scaled(job.path, 160);
                    if (!result->wic) result->failed = true;
                } catch (...) {
                    result->failed = true;
                }
                {
                    std::lock_guard lock(m_folder_icon_mutex);
                    m_folder_icon_inflight.erase(
                        job.folder + L"#" + std::to_wstring(job.tile));
                }
                if (m_window.handle())
                    PostMessageW(m_window.handle(), WM_FOLDER_ICON_READY,
                        0, reinterpret_cast<LPARAM>(result.release()));
            }
        } catch (...) {
        }
        CoUninitialize();
    });
}

void App::stop_folder_icon_worker() {
    {
        std::lock_guard lock(m_folder_icon_mutex);
        m_folder_icon_stop = true;
    }
    m_folder_icon_cv.notify_all();
    if (m_folder_icon_thread.joinable()) m_folder_icon_thread.join();
}

void App::enqueue_folder_icons() {
    if (m_album_rows.size() != m_folder_icon_tiles.size()) return;
    bool queued = false;
    {
        std::lock_guard lock(m_folder_icon_mutex);
        for (size_t i = 0; i < m_album_rows.size(); ++i) {
            if (m_folder_icon_queue.size() >= 64) break;
            const auto& row = m_album_rows[i];
            if (row.kind != AlbumPanelRow::Kind::Folder || row.error)
                continue;
            const auto& tiles = m_folder_icon_tiles[i].tiles;
            const auto samples = m_folder_samples.find(row.name);
            if (samples == m_folder_samples.end()) continue;
            const size_t want =
                std::min<size_t>(4, samples->second.size());
            if (want == 0) continue;
            for (size_t t = 0; t < want; ++t) {
                if (t < tiles.size()) continue;
                const std::wstring key =
                    row.name + L"#" + std::to_wstring(t);
                if (m_folder_icon_inflight.count(key)) continue;
                m_folder_icon_inflight.insert(key);
                m_folder_icon_queue.push_back(FolderIconJob{
                    row.name, samples->second[t], static_cast<int>(t)});
                queued = true;
            }
        }
    }
    if (queued) {
        m_folder_icon_cv.notify_all();
        start_folder_icon_worker();
    }
}

void App::handle_folder_icon_ready(LPARAM payload) {
    std::unique_ptr<FolderIconResult> result(
        reinterpret_cast<FolderIconResult*>(payload));
    if (!result) return;
    auto& tiles = m_folder_icon_cache[result->folder];
    const int t = result->tile;
    if (t >= 0 && !result->failed && result->wic) {
        ComPtr<ID2D1Bitmap1> bitmap;
        if (SUCCEEDED(m_renderer.create_bitmap_from_wic(
                result->wic.Get(), &bitmap)) && bitmap) {
            if (static_cast<int>(tiles.size()) <= t)
                tiles.resize(static_cast<size_t>(t) + 1);
            tiles[static_cast<size_t>(t)] = bitmap;
        }
    } else if (t >= 0) {
        // Failed decode: the null slot marks the tile complete so the
        // worker does not retry the same broken file every frame.
        if (static_cast<int>(tiles.size()) <= t)
            tiles.resize(static_cast<size_t>(t) + 1);
    }
    m_window.invalidate();
}

int App::album_icon_hit(int x, int y) {
    const auto& g = m_nav_panel_geometry;
    if (g.w <= 0.0f) return -1;
    const int cols =
        m_album_store.folder_view == AlbumFolderView::Icons3 ? 3 : 2;
    const float dpi_s =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    const auto cells =
        build_folder_icon_layout(m_album_rows, g, cols, dpi_s);
    for (const auto& cell : cells) {
        if (static_cast<float>(x) >= cell.x
            && static_cast<float>(x) < cell.x + cell.w
            && static_cast<float>(y) >= cell.y
            && static_cast<float>(y)
                < cell.y + cell.h + 18.0f * dpi_s)
            return cell.row_index;
    }
    return -1;
}

void App::request_collection_refresh() {
    if (m_watch_roots.empty() && m_index.directory().empty()
        && !m_fav_selected && !(!m_active_album_name.empty())) return;
    const std::uint64_t generation = m_nav_switch.request();
    {
        std::lock_guard lock(m_nav_scan_mutex);
        NavScanJob job;
        job.generation = generation;
        job.sort = m_index.sort_mode();
        job.refresh = true;
        job.album_name = m_active_album_name;
        if (m_fav_selected) {
            job.paths = m_album_store.favourites;
        } else if (!m_active_album_name.empty()) {
            job.roots = album_scan_roots();
        } else {
            job.path = m_index.directory();
            job.recursive = m_recursive;
        }
        job.watch_roots = m_watch_roots;
        m_nav_scan_job = std::move(job);
        m_nav_scan_queued = true;
    }
    m_nav_scan_cv.notify_one();
}

void App::apply_collection_refresh(NavScanResult&& result) {
    const bool was_grid = m_grid_mode;
    if (thumb_loader_running()) stop_thumb_loader();

    // Snapshot the selection by path BEFORE swapping the index.
    std::vector<std::wstring> selected_before;
    std::wstring sel_path;
    if (was_grid) {
        selected_before = selected_paths();
        if (m_grid_sel >= 0
            && m_grid_sel < static_cast<int>(m_index.size()))
            sel_path = m_index.path_at(static_cast<size_t>(m_grid_sel));
    }

    m_index = std::move(result.index);
    if (!result.album_name.empty()) rebuild_folder_samples();

    // Re-locate the current image in the refreshed index.
    if (!m_current_path.empty()) {
        m_current_idx = m_index.index_of(m_current_path);
        if (m_current_idx < 0) {
            m_current_path.clear();
            m_current_wic.Reset();
            m_has_image = false;
        }
    }

    if (was_grid) {
        m_thumb_engine.pool()->thumbs.clear();
        m_thumb_engine.pool()->thumbs.resize(m_index.size());
        m_thumb_d2d.clear();
        m_thumb_d2d_use.clear();
        m_grid_layout_dirty = true;
        m_last_cached_sel = -1;
        const SelectionRemap selection =
            plan_selection_remap(m_index, selected_before, sel_path);
        m_grid_sel = selection.grid_sel;
        m_selected.assign(m_index.size(), false);
        for (int idx : selection.selected)
            m_selected[static_cast<size_t>(idx)] = true;
        m_sel_anchor = selection.anchor;
        start_dim_preload();
        start_thumb_loader();
        clamp_grid_scroll();
        if (m_grid_sel >= 0) grid_ensure_visible();
    } else {
        preload_neighbors();
    }
    sync_nav_collection();
    m_window.invalidate();
}

void App::switch_collection(const std::wstring& path, bool recursive) {
    if (path.empty()) return;
    const std::wstring target_id = normalize_collection_key(path);
    const std::wstring current_id =
        normalize_collection_key(m_index.directory());
    if (!current_id.empty() && current_id == target_id
        && m_recursive == recursive) {
        // Same collection: clicking the current node just returns to the
        // grid view (D-10); in grid mode this is a no-op.
        if (!m_grid_mode) {
            m_from_grid = false;
            toggle_grid();
        }
        return;
    }
    if (m_comic_reader.enabled()) leave_comic_reader(false);
    finish_grid_scroll();

    if (!current_id.empty()) {
        m_collection_memory.remember(m_index.directory(),
            {m_index.sort_mode(), m_recursive});
    }
    const CollectionMemory memory = m_collection_memory.memory_for(path);
    const std::uint64_t generation = m_nav_switch.request();
    {
        std::lock_guard lock(m_nav_scan_mutex);
        m_nav_scan_job = NavScanJob{path, recursive, generation, memory.sort};
        m_nav_scan_queued = true;
    }
    m_nav_scan_cv.notify_one();
    // Old frame stays visible until the scan lands (D-13).
    m_window.invalidate();
}

void App::apply_nav_scan_result() {
    NavScanResult result;
    {
        std::lock_guard lock(m_nav_scan_mutex);
        if (!m_nav_scan_ready) return;
        result = std::move(m_nav_scan_result);
        m_nav_scan_ready = false;
    }
    if (!m_nav_switch.finish(result.generation)) return;  // stale

    if (result.refresh) {
        apply_collection_refresh(std::move(result));
        return;
    }

    const CollectionApplyAction action =
        plan_collection_apply({result.scan_result, m_grid_mode});
    if (action == CollectionApplyAction::ShowOpenError) {
        m_open_error = open_input_error_message(OpenInputRoute::MissingPath);
        m_window.invalidate();
        return;
    }
    if (thumb_loader_running()) stop_thumb_loader();
    m_index = std::move(result.index);
    m_recursive = result.recursive;
    reset_collection_selection(m_current_idx, m_grid_sel, m_grid_saved_idx,
        m_selected, m_sel_anchor);
    m_current_path.clear();
    m_current_wic.Reset();
    m_has_image = false;
    m_from_grid = false;
    m_grid_scroll_y = 0;
    m_grid_scroll_saved = 0;
    m_panel_path.clear();
    m_panel_scroll_y = 0.0f;
    m_open_error.clear();
    if (result.album_name.empty()) save_last_dir(result.path);
    m_thumb_engine.pool()->thumbs.clear();
    m_thumb_engine.pool()->thumbs.resize(m_index.size());
    m_thumb_d2d.clear();
    m_thumb_d2d_use.clear();
    m_grid_layout_dirty = true;
    m_last_cached_sel = -1;
    if (action == CollectionApplyAction::EnterGrid) {
        toggle_grid();  // enters grid with no default selection
    } else {
        // Selection vector must match the fresh index even when the switch
        // lands while already in grid mode: otherwise clicks update only
        // m_grid_sel (not m_selected) and grid delete / the selection
        // context menu stay disabled.
        m_selected.assign(m_index.size(), false);
        start_thumb_loader();
        clamp_grid_scroll();
    }
    sync_nav_collection();
    update_content_viewport(!m_grid_mode);
    m_active_album_name = result.album_name;
    if (!result.album_name.empty()) {
        rebuild_folder_samples();
        // Album/favourite collection: keep the grid state, skip lastdir.
    } else {
        m_album_sel = -1;
        m_fav_selected = false;
    }
    m_watch_roots = result.watch_roots.empty()
        ? std::vector<ScanRoot>{{result.path, result.recursive}}
        : result.watch_roots;
    start_dir_watch();
    update_title();
    m_window.invalidate();
}

void App::request_nav_tree_expand(std::uint64_t node_id) {
    const NavTreeNode* node = m_nav_tree.node(node_id);
    if (!node) return;
    const std::uint64_t generation = m_nav_tree.request_expand(node_id);
    if (generation == 0) return;  // cached expand or already loading
    {
        std::lock_guard lock(m_nav_tree_mutex);
        if (m_nav_tree_queued) {
            // Replace the pending job; drop the superseded one silently.
            const NavTreeJob previous = m_nav_tree_job;
            m_nav_tree_job = NavTreeJob{node_id, node->path, generation};
            m_nav_tree_queued = true;
            (void)m_nav_tree.finish_expand(previous.node_id,
                previous.generation, {}, 0, false, L"", true);
        } else {
            m_nav_tree_job = NavTreeJob{node_id, node->path, generation};
            m_nav_tree_queued = true;
            m_nav_tree_cv.notify_one();
        }
    }
    m_window.invalidate();  // show loading state
}

void App::apply_nav_tree_result() {
    NavTreeOutcome outcome;
    {
        std::lock_guard lock(m_nav_tree_mutex);
        if (!m_nav_tree_outcome_ready) return;
        outcome = std::move(m_nav_tree_outcome);
        m_nav_tree_outcome_ready = false;
    }
    if (!m_nav_tree.finish_expand(outcome.node_id, outcome.generation,
            outcome.children, outcome.image_count, outcome.ok,
            outcome.error)) {
        return;  // stale
    }
    // Continue revealing the active collection path if needed.
    if (!m_index.directory().empty()) {
        const auto plan = m_nav_tree.reveal(m_index.directory());
        m_nav_highlight_id = plan.highlight_id;
        for (auto id : plan.expansions) request_nav_tree_expand(id);
    }
    m_window.invalidate();
}

void App::ensure_nav_root() {
    const std::wstring dir = m_index.directory();
    if (dir.empty()) return;
    // C1: only the current drive root (+ favorites tab, P3 placeholder).
    std::wstring root;
    if (dir.size() >= 2 && dir[1] == L':') {
        root = dir.substr(0, 2) + L"\\";
    } else {
        const std::vector<std::wstring> segments = split_path_segments(dir);
        if (!segments.empty()) root = path_from_segments(segments, 0);
    }
    if (root.empty()) return;
    std::wstring name = root;
    if (name.size() > 2 && name.back() == L'\\') name.pop_back();
    m_nav_tree.add_root(root, name);
}

void App::sync_nav_collection() {
    const std::wstring dir = m_index.directory();
    rebuild_nav_breadcrumbs();  // refresh path + [递归] tail marker
    if (dir.empty()) {
        m_nav_highlight_id = 0;
        m_window.invalidate();
        return;
    }
    if (normalize_collection_key(dir) == m_nav_synced_key) return;
    m_nav_synced_key = normalize_collection_key(dir);
    ensure_nav_root();
    reveal_active_collection();
    m_window.invalidate();
}

void App::reveal_active_collection() {
    const std::wstring dir = m_index.directory();
    if (dir.empty()) {
        m_nav_highlight_id = 0;
        return;
    }
    const auto plan = m_nav_tree.reveal(dir);
    m_nav_highlight_id = plan.highlight_id;
    for (auto id : plan.expansions) request_nav_tree_expand(id);
}

void App::rebuild_nav_breadcrumbs() {
    if (!m_active_album_name.empty()) {
        m_nav_display_segments = {m_active_album_name};
        return;
    }
    const std::wstring dir = m_index.directory();
    m_nav_segments = split_path_segments(dir);
    m_nav_display_segments = m_nav_segments;
    if (m_recursive && !m_nav_display_segments.empty())
        m_nav_display_segments.back() += L" [\u9012\u5F52]";
}

void App::build_album_rows(std::vector<AlbumPanelRow>& rows) {
    rows.clear();
    {
        AlbumPanelRow fav;
        fav.kind = AlbumPanelRow::Kind::Favourites;
        fav.name = L"\u6536\u85CF";
        fav.selected = m_fav_selected;
        fav.image_count = static_cast<int>(m_album_store.favourites.size());
        rows.push_back(std::move(fav));
    }
    for (int i = 0; i < static_cast<int>(m_album_store.albums.size()); ++i) {
        const auto& album = m_album_store.albums[static_cast<size_t>(i)];
        AlbumPanelRow arow;
        arow.kind = AlbumPanelRow::Kind::Album;
        arow.name = album.name;
        arow.album_index = i;
        const bool is_sel = (!m_fav_selected && m_album_sel == i);
        arow.selected = is_sel;
        rows.push_back(std::move(arow));
        if (is_sel) {
            for (int f = 0;
                 f < static_cast<int>(album.folders.size()); ++f) {
                const auto& folder =
                    album.folders[static_cast<size_t>(f)];
                AlbumPanelRow frow;
                frow.kind = AlbumPanelRow::Kind::Folder;
                frow.depth = 1;
                frow.name = folder.path;
                frow.recursive = folder.recursive;
                frow.album_index = i;
                frow.folder_index = f;
                std::error_code ec;
                frow.error =
                    !std::filesystem::is_directory(folder.path, ec) || ec;
                rows.push_back(std::move(frow));
            }
        }
    }
}

void App::render_nav_panel(float x, float y, float w, float h) {
    const float dpi_s =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    m_nav_panel_geometry = build_nav_panel_geometry(x, y, w, h, dpi_s);
    const auto& g = m_nav_panel_geometry;
    const float pad = layout::kNavPadDip * dpi_s;
    const float fs = layout::kNavFontSizeDip * dpi_s;
    const float gap = layout::kNavBreadcrumbGapDip * dpi_s;
    const float ellipsis_w = m_renderer.measure_text(L"\u2026", fs);

    // Panel breadcrumb (single path source: m_nav_display_segments)
    const float bc_w = std::max(1.0f, g.w - pad * 2.0f);
    m_nav_panel_breadcrumb = layout_breadcrumb(
        m_nav_display_segments, bc_w, gap, ellipsis_w,
        [this, fs](const std::wstring& text) {
            return m_renderer.measure_text(text, fs);
        });
    for (auto& item : m_nav_panel_breadcrumb.items)
        item.x += g.x + pad;

    // Tree rows (lazy model projection)
    const float row_h = layout::kNavRowHeightDip * dpi_s;
    m_nav_tree_total = m_nav_tree.content_height(row_h);
    m_nav_tree_scroll = std::clamp(m_nav_tree_scroll, 0.0f,
        std::max(0.0f, m_nav_tree_total - g.tree_h));
    m_nav_rows = m_nav_tree.layout_rows(m_nav_tree_scroll, g.tree_h, row_h);
    for (auto& row : m_nav_rows) {
        if (row.node_id == m_nav_highlight_id) row.highlighted = true;
    }
    if (m_nav_row_hover >= static_cast<int>(m_nav_rows.size()))
        m_nav_row_hover = -1;

    std::wstring stats;
    if (!m_index.directory().empty() || !m_active_album_name.empty()) {
        stats = std::to_wstring(m_index.size()) + L" \u5F20";
        if (m_recursive && m_active_album_name.empty())
            stats += L" [\u9012\u5F52]";
    }
    if (m_nav_panel_state.tab() == NavPanelTab::Favorites)
        build_album_rows(m_album_rows);

    NavPanelRenderInput input;
    input.geometry = g;
    input.breadcrumb = &m_nav_panel_breadcrumb;
    input.segments = &m_nav_display_segments;
    input.breadcrumb_hover = m_nav_breadcrumb_hover_panel;
    input.tab = m_nav_panel_state.tab();
    input.rows = &m_nav_rows;
    input.row_hover = m_nav_row_hover;
    input.highlight_id = m_nav_highlight_id;
    input.highlight_recursive = m_recursive;
    input.tree_scroll = m_nav_tree_scroll;
    input.tree_total = m_nav_tree_total;
    input.stats_text = stats.empty() ? nullptr : &stats;
    input.dpi_scale = dpi_s;
    input.album_rows = &m_album_rows;
    input.album_row_hover = m_album_row_hover;
    input.icons_mode = m_album_store.folder_view == AlbumFolderView::Icons3
        ? 3
        : m_album_store.folder_view == AlbumFolderView::Icons2 ? 2 : 0;
    m_folder_icon_tiles.clear();
    m_folder_icon_tiles.resize(m_album_rows.size());
    for (size_t i = 0; i < m_album_rows.size(); ++i) {
        if (m_album_rows[i].kind != AlbumPanelRow::Kind::Folder) continue;
        const auto it = m_folder_icon_cache.find(m_album_rows[i].name);
        if (it != m_folder_icon_cache.end())
            m_folder_icon_tiles[i].tiles = it->second;
    }
    input.folder_tiles = &m_folder_icon_tiles;
    if (input.icons_mode > 0) enqueue_folder_icons();
    m_renderer.draw_nav_panel(input);
}

void App::render_grid_breadcrumb() {
    if (!m_grid_mode || m_nav_display_segments.empty()) return;
    const float dpi_s =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    const float nav_w = static_cast<float>(nav_panel_width());
    const float right_w = static_cast<float>(visible_panel_width());
    const int sb_zone = static_cast<int>(layout::kScrollbarZoneDip * dpi_s);
    const float pad = layout::kNavPadDip * dpi_s;
    const float strip_x = nav_w;
    const float strip_w = std::max(1.0f,
        static_cast<float>(m_renderer.target_size().width)
            - nav_w - right_w - sb_zone);
    const float gap = layout::kNavBreadcrumbGapDip * dpi_s;
    const float fs = layout::kNavFontSizeDip * dpi_s;
    const float ellipsis_w = m_renderer.measure_text(L"\u2026", fs);

    m_nav_grid_breadcrumb = layout_breadcrumb(
        m_nav_display_segments, strip_w - pad * 2.0f, gap, ellipsis_w,
        [this, fs](const std::wstring& text) {
            return m_renderer.measure_text(text, fs);
        });
    for (auto& item : m_nav_grid_breadcrumb.items)
        item.x += strip_x + pad;

    NavBreadcrumbRenderInput input;
    input.x = strip_x;
    input.y = static_cast<float>(m_toolbar_h);
    input.width = strip_w;
    input.height = static_cast<float>(m_nav_breadcrumb_h);
    input.layout = &m_nav_grid_breadcrumb;
    input.segments = &m_nav_display_segments;
    input.hover_item = m_nav_breadcrumb_hover_grid;
    input.dpi_scale = dpi_s;
    m_renderer.draw_breadcrumb(input);
}

bool App::nav_panel_hit_test(int x, int y) {
    const auto& g = m_nav_panel_geometry;
    if (g.w <= 0.0f) return false;  // no frame rendered yet
    if (x < g.x || x >= g.x + g.w || y < g.y) return false;
    m_nav_panel_state.set_focus(NavFocusTarget::LeftPanel);
    const float dpi_s =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;

    // Breadcrumb strip
    if (y < g.breadcrumb_y + g.breadcrumb_h) {
        const int item = breadcrumb_hit_item(
            m_nav_panel_breadcrumb, static_cast<float>(x));
        if (item >= 0) {
            const int seg = m_nav_panel_breadcrumb
                .items[static_cast<size_t>(item)].segment_index;
            if (seg >= 0) {
                const std::wstring path =
                    path_from_segments(m_nav_segments, seg);
                if (!path.empty()) switch_collection(path, false);
            }
        }
        m_window.invalidate();
        return true;
    }
    // Tab row: 目录 | 收藏
    if (y < g.tabs_y + g.tabs_h) {
        // Album view-mode toggle button (收藏 tab, right edge)
        if (m_nav_panel_state.tab() == NavPanelTab::Favorites
            && x >= g.toggle_x && x < g.toggle_x + g.toggle_w
            && y >= g.toggle_y && y < g.toggle_y + g.toggle_h) {
            toggle_album_folder_view();
            m_nav_panel_state.release_focus();
            return true;
        }
        const float pad = layout::kNavPadDip * dpi_s;
        const float fs = layout::kNavFontSizeDip * dpi_s;
        float tx = g.x + pad;
        const float w_dirs = m_renderer.measure_text(L"\u76EE\u5F55", fs);
        if (x >= tx && x < tx + w_dirs)
            m_nav_panel_state.set_tab(NavPanelTab::Directories);
        tx += w_dirs + dt::kSpaceLgDip * dpi_s;
        const float w_fav = m_renderer.measure_text(L"\u6536\u85CF", fs);
        if (x >= tx && x < tx + w_fav)
            m_nav_panel_state.set_tab(NavPanelTab::Favorites);
        if (m_nav_panel_state.tab() == NavPanelTab::Favorites)
            m_nav_panel_state.release_focus();
        m_window.invalidate();
        return true;
    }
    // Bottom stats row: consume, no action
    if (y >= g.stats_y) return true;
    // Tree viewport
    if (y >= g.tree_y && y < g.tree_y + g.tree_h) {
        if (m_nav_panel_state.tab() == NavPanelTab::Favorites) {
            if (m_album_store.folder_view != AlbumFolderView::Tree) {
                const int cell_row = album_icon_hit(x, y);
                if (cell_row >= 0) {
                    const auto& row =
                        m_album_rows[static_cast<size_t>(cell_row)];
                    switch_collection(row.name, row.recursive);
                    m_nav_panel_state.release_focus();
                    return true;
                }
            }
            const int idx = album_row_hit(x, y);
            if (idx >= 0) {
                const auto& row = m_album_rows[static_cast<size_t>(idx)];
                if (row.kind == AlbumPanelRow::Kind::Favourites)
                    open_favourites_collection();
                else if (row.kind == AlbumPanelRow::Kind::Album)
                    open_album_collection(row.album_index);
                else if (row.kind == AlbumPanelRow::Kind::Folder)
                    switch_collection(row.name, row.recursive);
            }
            m_nav_panel_state.release_focus();
            return true;
        }
        const float indent = layout::kNavIndentDip * dpi_s;
        const float arrow_w = layout::kNavArrowWidthDip * dpi_s;
        const float row_left = g.tree_x + layout::kNavPadDip * dpi_s;
        int row_idx = -1;
        for (int i = 0; i < static_cast<int>(m_nav_rows.size()); ++i) {
            const auto& row = m_nav_rows[static_cast<size_t>(i)];
            const float row_y = row.y + g.tree_y - m_nav_tree_scroll;
            if (y >= row_y && y < row_y + row.height) {
                row_idx = i;
                break;
            }
        }
        if (row_idx >= 0) {
            const auto& row = m_nav_rows[static_cast<size_t>(row_idx)];
            const NavTreeRowZone zone = hit_nav_tree_row(
                row_left, static_cast<float>(x), row.depth, indent, arrow_w);
            if (zone == NavTreeRowZone::Arrow) {
                if (row.loading) {
                    // enumeration in flight — nothing to do
                } else if (row.expanded) {
                    m_nav_tree.collapse(row.node_id);
                } else {
                    request_nav_tree_expand(row.node_id);  // expand or retry
                }
                m_window.invalidate();
                return true;
            }
            if (zone == NavTreeRowZone::Body) {
                switch_collection(row.path, false);
                return true;
            }
        }
        return true;
    }
    return true;
}

int App::album_row_hit(int x, int y) {
    const auto& g = m_nav_panel_geometry;
    if (g.w <= 0.0f) return -1;
    if (x < g.tree_x || x >= g.tree_x + g.tree_w
        || y < g.tree_y || y >= g.tree_y + g.tree_h) return -1;
    const float dpi_s =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    const float row_h = layout::kNavRowHeightDip * dpi_s;
    const float pad = layout::kNavPadDip * dpi_s;
    const int idx = static_cast<int>(
        (static_cast<float>(y) - g.tree_y - pad) / row_h);
    if (idx < 0 || idx >= static_cast<int>(m_album_rows.size()))
        return -1;
    return idx;
}

bool App::grid_breadcrumb_hit_test(int x, int y) {
    if (!m_grid_mode || m_nav_display_segments.empty()) return false;
    if (y < m_toolbar_h || y >= m_grid_top) return false;
    const float dpi_s =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    const float nav_w = static_cast<float>(nav_panel_width());
    const int sb_zone = static_cast<int>(layout::kScrollbarZoneDip * dpi_s);
    const float right_w = static_cast<float>(visible_panel_width());
    const float strip_w = static_cast<float>(m_renderer.target_size().width)
        - nav_w - right_w - sb_zone;
    if (x < nav_w || x >= nav_w + strip_w) return false;
    const int item = breadcrumb_hit_item(
        m_nav_grid_breadcrumb, static_cast<float>(x));
    if (item >= 0) {
        const int seg = m_nav_grid_breadcrumb
            .items[static_cast<size_t>(item)].segment_index;
        if (seg >= 0) {
            const std::wstring path =
                path_from_segments(m_nav_segments, seg);
            if (!path.empty()) switch_collection(path, false);
        }
    }
    return true;
}

void App::nav_panel_mouse_move(int x, int y) {
    const int prev_row = m_nav_row_hover;
    const int prev_panel = m_nav_breadcrumb_hover_panel;
    const int prev_grid = m_nav_breadcrumb_hover_grid;
    const int prev_album = m_album_row_hover;
    m_nav_row_hover = -1;
    m_nav_breadcrumb_hover_panel = -1;
    m_nav_breadcrumb_hover_grid = -1;
    m_album_row_hover = -1;

    if (nav_panel_visible() && x < nav_panel_width() && y >= m_toolbar_h) {
        const auto& g = m_nav_panel_geometry;
        if (g.w > 0.0f) {
            if (y < g.breadcrumb_y + g.breadcrumb_h) {
                m_nav_breadcrumb_hover_panel = breadcrumb_hit_item(
                    m_nav_panel_breadcrumb, static_cast<float>(x));
            } else if (y >= g.tree_y && y < g.tree_y + g.tree_h) {
                if (m_nav_panel_state.tab() == NavPanelTab::Favorites) {
                    int hover = -1;
                    if (m_album_store.folder_view != AlbumFolderView::Tree)
                        hover = album_icon_hit(x, y);
                    if (hover < 0) {
                        const float dpi_s = static_cast<float>(
                            GetDpiForWindow(m_window.handle())) / 96.0f;
                        const float row_h =
                            layout::kNavRowHeightDip * dpi_s;
                        const float pad = layout::kNavPadDip * dpi_s;
                        const int idx = static_cast<int>(
                            (static_cast<float>(y) - g.tree_y - pad)
                                / row_h);
                        hover = (idx >= 0
                            && idx < static_cast<int>(m_album_rows.size()))
                            ? idx : -1;
                    }
                    m_album_row_hover = hover;
                } else {
                    for (int i = 0;
                         i < static_cast<int>(m_nav_rows.size()); ++i) {
                        const auto& row =
                            m_nav_rows[static_cast<size_t>(i)];
                        const float row_y =
                            row.y + g.tree_y - m_nav_tree_scroll;
                        if (y >= row_y && y < row_y + row.height) {
                            m_nav_row_hover = i;
                            break;
                        }
                    }
                }
            }
        }
    }
    if (m_grid_mode && y >= m_toolbar_h && y < m_grid_top) {
        const float dpi_s =
            static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
        const float nav_w = static_cast<float>(nav_panel_width());
        const int sb_zone =
            static_cast<int>(layout::kScrollbarZoneDip * dpi_s);
        const float right_w = static_cast<float>(visible_panel_width());
        const float strip_w = static_cast<float>(
            m_renderer.target_size().width) - nav_w - right_w - sb_zone;
        if (x >= nav_w && x < nav_w + strip_w) {
            m_nav_breadcrumb_hover_grid = breadcrumb_hit_item(
                m_nav_grid_breadcrumb, static_cast<float>(x));
        }
    }
    if (m_nav_row_hover != prev_row
        || m_nav_breadcrumb_hover_panel != prev_panel
        || m_nav_breadcrumb_hover_grid != prev_grid
        || m_album_row_hover != prev_album) {
        m_window.invalidate();
    }
}

void App::nav_tree_scroll(float delta) {
    if (!nav_panel_visible()) return;
    m_nav_tree_scroll -= delta * 60.0f;
    m_nav_tree_scroll = std::clamp(m_nav_tree_scroll, 0.0f,
        std::max(0.0f, m_nav_tree_total - m_nav_panel_geometry.tree_h));
    m_window.invalidate();
}

void App::nav_ensure_focus_visible() {
    if (m_nav_tree_focus_id == 0) return;
    for (const auto& row : m_nav_rows) {
        if (row.node_id == m_nav_tree_focus_id) return;  // already visible
    }
    const float dpi_s =
        static_cast<float>(GetDpiForWindow(m_window.handle())) / 96.0f;
    const float row_h = layout::kNavRowHeightDip * dpi_s;
    const auto all = m_nav_tree.layout_rows(0.0f, 1.0e9f, row_h, 0.0f);
    const float view_h = m_nav_panel_geometry.tree_h;
    for (const auto& row : all) {
        if (row.node_id != m_nav_tree_focus_id) continue;
        if (row.y < m_nav_tree_scroll) {
            m_nav_tree_scroll = row.y;
        } else if (row.y + row_h > m_nav_tree_scroll + view_h) {
            m_nav_tree_scroll = row.y + row_h - view_h;
        }
        break;
    }
    m_nav_tree_scroll = std::max(0.0f, m_nav_tree_scroll);
}

} // namespace mv

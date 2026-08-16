#include "menu_support.h"
#include "file_operation.h"

namespace mv {

// ── Menu command IDs ─────────────────────────────────────────

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

void AddOwnerSeparator(HMENU menu) {
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
}

OwnerItemData* AddOwnerItem(HMENU menu, UINT id, const std::wstring& label, bool disabled, bool checked) {
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

void BuildOwnerMenu(HMENU parent, HMENU sub, const std::wstring& label) {
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

void FreeOwnerItemData(HMENU menu) {
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
LRESULT CALLBACK MenuSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
void ApplyMenuTheme(HMENU menu, HBRUSH br) {
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

} // namespace mv

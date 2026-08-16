#pragma once
#include <Windows.h>
#include <string>

namespace mv {

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

HMENU build_menu_bar();

struct OwnerItemData {
    std::wstring text;
    std::wstring shortcut;
    bool disabled  = false;
    bool checked   = false;
};

void AddOwnerSeparator(HMENU menu);
OwnerItemData* AddOwnerItem(HMENU menu, UINT id, const std::wstring& label, bool disabled = false, bool checked = false);
void BuildOwnerMenu(HMENU parent, HMENU sub, const std::wstring& label);
void FreeOwnerItemData(HMENU menu);
LRESULT CALLBACK MenuSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void ApplyMenuTheme(HMENU menu, HBRUSH br);

} // namespace mv

// Standalone test: does SetWindowTextW work at all?
#include <windows.h>

LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"BARE_TEST_WINDOW";
    RegisterClassExW(&wc);

    RECT r = {0, 0, 800, 600};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    HWND h = CreateWindowExW(0, L"BARE_TEST_WINDOW", L"BARE TEST TITLE",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, hi, nullptr);

    // Set text after creation
    BOOL ok = SetWindowTextW(h, L"HELLO WORLD 1234567890");
    wchar_t buf[256];
    GetWindowTextW(h, buf, 256);

    MessageBoxW(nullptr, buf, L"GetWindowTextW result", MB_OK);

    ShowWindow(h, SW_SHOW);
    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}

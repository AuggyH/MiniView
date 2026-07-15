// Test 7: No CRT, WinMain instead of wWinMain, pure Win32
#include <windows.h>

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TEST7";
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(0, L"TEST7", L"",
        WS_OVERLAPPEDWINDOW, 0, 0, 400, 300,
        nullptr, nullptr, hi, nullptr);

    SetWindowTextW(h, L"HELLO_PURE_WIN32_NO_CRT");
    wchar_t buf[256];
    GetWindowTextW(h, buf, 256);
    MessageBoxW(nullptr, buf, L"Pure Win32 no CRT", MB_OK);

    ShowWindow(h, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

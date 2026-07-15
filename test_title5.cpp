// Test 5: SetWindowText AFTER message loop starts
#include <windows.h>

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static int count = 0;
    switch (m) {
    case WM_CREATE:
        // Try setting text in WM_CREATE
        SetWindowTextW(h, L"WM_CREATE_TITLE");
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_TIMER:
        if (++count == 1) {
            SetWindowTextW(h, L"TIMER_SET_TITLE_1234567890");
            wchar_t buf[256];
            GetWindowTextW(h, buf, 256);
            MessageBoxW(h, buf, L"After Timer SetWindowText", MB_OK);
        }
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TEST5";
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(0, L"TEST5", L"INITIAL",
        WS_OVERLAPPEDWINDOW, 0, 0, 400, 300,
        nullptr, nullptr, hi, nullptr);

    // Set timer to fire after window is shown
    SetTimer(h, 1, 100, nullptr);
    ShowWindow(h, SW_SHOW);
    
    // Also try PostMessage from here
    PostMessageW(h, WM_SETTEXT, 0, (LPARAM)L"POSTMESSAGE_TITLE");

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

// Test 2: A-only vs W-only vs raw SendMessage
#include <windows.h>
#include <cstdio>

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    // Write test strings to file for verification
    FILE* f = fopen("D:/Projects/minview-native/title_debug.txt", "w");
    
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TEST2";
    RegisterClassExW(&wc);

    RECT r = {0, 0, 800, 600};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    HWND h = CreateWindowExW(0, L"TEST2", L"INITIAL TITLE",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, hi, nullptr);

    // Test 1: SetWindowTextW
    SetWindowTextW(h, L"ABCDEFGHIJ");
    wchar_t wbuf[256];
    GetWindowTextW(h, wbuf, 256);
    fprintf(f, "After SetWindowTextW(L\"ABCDEFGHIJ\"): [%ls]\n", wbuf);

    // Test 2: SendMessage WM_SETTEXT
    SendMessageW(h, WM_SETTEXT, 0, (LPARAM)L"1234567890");
    GetWindowTextW(h, wbuf, 256);
    fprintf(f, "After SendMessage SETTEXT: [%ls]\n", wbuf);

    // Test 3: SetWindowTextA (ASCII)
    SetWindowTextA(h, "QWERTYUIOP");
    GetWindowTextA(h, (LPSTR)wbuf, 512);
    fprintf(f, "After SetWindowTextA(\"QWERTYUIOP\"): [%s]\n", (char*)wbuf);

    // Test 4: Direct DefWindowProc bypass
    SendMessageW(h, WM_SETTEXT, 0, (LPARAM)L"DIRECT_SET");
    GetWindowTextW(h, wbuf, 256);
    fprintf(f, "After direct WM_SETTEXT: [%ls]\n", wbuf);
    
    fclose(f);

    MessageBoxW(nullptr, wbuf, L"Final GetWindowTextW", MB_OK);
    ShowWindow(h, SW_SHOW);
    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}

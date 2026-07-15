// Test 3: message-only window + real window comparison
#include <windows.h>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    FILE* f = fopen("D:/Projects/minview-native/title_debug3.txt", "w");
    
    // --- Test A: message-only window (no UI) ---
    HWND msgOnly = CreateWindowExW(0, L"STATIC", L"MSGONLY_TEST",
        WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr);
    
    SetWindowTextW(msgOnly, L"HELLO_MESSAGE_ONLY");
    wchar_t buf[256];
    GetWindowTextW(msgOnly, buf, 256);
    fprintf(f, "Message-only window: [%ls]\n", buf);
    DestroyWindow(msgOnly);
    
    // --- Test B: visible window, SetWindowText BEFORE ShowWindow ---
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TEST3";
    RegisterClassExW(&wc);
    
    HWND h = CreateWindowExW(0, L"TEST3", L"",
        WS_OVERLAPPEDWINDOW, 0, 0, 400, 300,
        nullptr, nullptr, hi, nullptr);
    
    // Set text BEFORE showing
    SetWindowTextW(h, L"BEFORE_SHOW");
    wchar_t buf2[256], buf3[256];
    GetWindowTextW(h, buf2, 256);
    fprintf(f, "Before ShowWindow: [%ls]\n", buf2);
    
    // Now show and immediately check again
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    GetWindowTextW(h, buf3, 256);
    fprintf(f, "After ShowWindow: [%ls]\n", buf3);
    
    fclose(f);
    
    MessageBoxW(nullptr, buf3, L"After ShowWindow", MB_OK);
    
    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}

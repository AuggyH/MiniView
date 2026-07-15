// Test 6: Use system class STATIC, and copy exe to temp
#include <windows.h>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    FILE* f = fopen("D:/Projects/minview-native/title_debug6.txt", "w");
    
    // Copy ourselves to temp with different name
    wchar_t self[MAX_PATH], tempExe[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wsprintfW(tempExe, L"%s\\minview_title_test.exe", L"C:\\Users\\Huang\\AppData\\Local\\Temp");
    CopyFileW(self, tempExe, FALSE);
    
    // --- Test: window with STATIC class (system class, no RegisterClass) ---
    HWND h = CreateWindowExW(0, L"STATIC", L"STATIC_CLASS_TITLE",
        WS_OVERLAPPEDWINDOW | SS_CENTER, 0, 0, 400, 300,
        nullptr, nullptr, hi, nullptr);
    
    SetWindowTextW(h, L"HELLO_STATIC_WINDOW");
    wchar_t buf[256];
    GetWindowTextW(h, buf, 256);
    fprintf(f, "STATIC class window: [%ls]\n", buf);
    
    // Also test: what class does MessageBox use?
    HWND msgHwnd = FindWindowW(L"#32770", nullptr); // dialog class
    fprintf(f, "Dialog class found: %p\n", msgHwnd);
    
    fclose(f);
    
    MessageBoxW(nullptr, buf, L"STATIC class result", MB_OK);
    ShowWindow(h, SW_SHOW);
    
    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}

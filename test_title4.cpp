// Test 4: MessageBox title + DialogBox title
#include <windows.h>

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    // Test MessageBox (system-drawn title bar)
    int r = MessageBoxW(nullptr, 
        L"Does the TITLE BAR of this dialog show the full text?",
        L"THIS IS A LONG TITLE FOR TESTING PURPOSES",
        MB_YESNO | MB_ICONQUESTION);
    
    if (r == IDYES) {
        MessageBoxW(nullptr, L"You clicked YES", L"RESULT", MB_OK);
    }
    
    return 0;
}

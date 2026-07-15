// MinView Native — lightweight GPU-accelerated image viewer
// Windows / C++20 / Direct2D + WIC / zero external dependencies

#include "app.h"

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE, PWSTR, int /*nCmdShow*/) {
    // Initialize COM (required for WIC)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    int exit_code = 0;
    try {
        // Parse command line for image path
        std::wstring initial_path;
        int argc;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc > 1) {
            initial_path = argv[1];
            LocalFree(argv);
        }

        mv::App app;
        exit_code = app.run(initial_path);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "MinView Fatal Error",
            MB_OK | MB_ICONERROR);
        exit_code = 1;
    }

    CoUninitialize();
    return exit_code;
}

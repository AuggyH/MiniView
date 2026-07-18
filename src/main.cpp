// MinView Native — lightweight GPU-accelerated image viewer
// Windows / C++20 / Direct2D + WIC / zero external dependencies

#include "app.h"
#include <shellapi.h>

namespace {

constexpr const wchar_t* MUTEX_NAME = L"MinView_SingleInstance_Mutex";
constexpr const wchar_t* WINDOW_CLASS = L"MinViewWindow";
constexpr const wchar_t* PROG_ID = L"MinView.Image";

const wchar_t* IMAGE_EXTS[] = {
    L".png", L".jpg", L".jpeg", L".jpe", L".jfif",
    L".bmp", L".dib", L".gif", L".webp",
    L".tiff", L".tif", L".ico", L".cur",
    L".heic", L".heif", L".avif",
    L".svg", L".psd", L".tga", L".dds",
};

bool forward_to_existing(const std::wstring& path) {
    HWND hwnd = FindWindowW(WINDOW_CLASS, nullptr);
    if (!hwnd || !IsWindow(hwnd)) return false;

    COPYDATASTRUCT cds = {};
    cds.dwData = 1;
    cds.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(path.c_str());

    SendMessageW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
    return true;
}

// Get absolute path to this executable
std::wstring get_exe_path() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::wstring(buf, len) : L"";
}

bool register_file_associations() {
    std::wstring exe = get_exe_path();
    if (exe.empty()) return false;

    // ── ProgID ──
    std::wstring prog_key = L"Software\\Classes\\" + std::wstring(PROG_ID);
    HKEY hk;

    // DefaultIcon
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        (prog_key + L"\\DefaultIcon").c_str(),
        0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        std::wstring icon = exe + L",0";
        RegSetValueExW(hk, nullptr, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(icon.c_str()),
            static_cast<DWORD>((icon.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }

    // shell/open/command
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        (prog_key + L"\\shell\\open\\command").c_str(),
        0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        std::wstring cmd = L"\"" + exe + L"\" \"%1\"";
        RegSetValueExW(hk, nullptr, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(cmd.c_str()),
            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }

    // ── File extensions ──
    for (auto ext : IMAGE_EXTS) {
        std::wstring ext_key = L"Software\\Classes\\" + std::wstring(ext);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, ext_key.c_str(),
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hk, nullptr, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(PROG_ID),
                static_cast<DWORD>((wcslen(PROG_ID) + 1) * sizeof(wchar_t)));
            RegCloseKey(hk);
        }
    }

    // ── Registered Applications (for "Open with" menu) ──
    std::wstring app_key = L"Software\\Classes\\Applications\\MinView.exe\\shell\\open\\command";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, app_key.c_str(),
        0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        std::wstring cmd = L"\"" + exe + L"\" \"%1\"";
        RegSetValueExW(hk, nullptr, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(cmd.c_str()),
            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }

    return true;
}

bool unregister_file_associations() {
    // Remove ProgID
    RegDeleteTreeW(HKEY_CURRENT_USER,
        (L"Software\\Classes\\" + std::wstring(PROG_ID)).c_str());

    // Remove extension associations
    for (auto ext : IMAGE_EXTS) {
        std::wstring ext_key = L"Software\\Classes\\" + std::wstring(ext);
        HKEY hk;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, ext_key.c_str(),
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t val[256] = {};
            DWORD size = sizeof(val);
            DWORD type;
            if (RegQueryValueExW(hk, nullptr, 0, &type,
                reinterpret_cast<BYTE*>(val), &size) == ERROR_SUCCESS) {
                if (type == REG_SZ && wcscmp(val, PROG_ID) == 0) {
                    // Only remove if it points to us
                    RegCloseKey(hk);
                    RegDeleteTreeW(HKEY_CURRENT_USER, ext_key.c_str());
                    continue;
                }
            }
            RegCloseKey(hk);
        }
    }

    // Remove application registration
    RegDeleteTreeW(HKEY_CURRENT_USER,
        L"Software\\Classes\\Applications\\MinView.exe");

    return true;
}

} // anonymous namespace

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE, PWSTR, int /*nCmdShow*/) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;

    int exit_code = 0;
    try {
        // Parse command line
        int argc;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring arg1;
        if (argv && argc > 1) {
            arg1 = argv[1];
            LocalFree(argv);
        }

        // --register / --unregister
        if (arg1 == L"--register") {
            bool ok = register_file_associations();
            CoUninitialize();
            return ok ? 0 : 1;
        }
        if (arg1 == L"--unregister") {
            bool ok = unregister_file_associations();
            CoUninitialize();
            return ok ? 0 : 1;
        }

        // Single instance: if already running, forward path and exit
        HANDLE mutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            if (!arg1.empty()) forward_to_existing(arg1);
            CloseHandle(mutex);
            CoUninitialize();
            return 0;
        }

        mv::App app;
        exit_code = app.run(arg1);

        if (mutex) CloseHandle(mutex);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "MinView Fatal Error",
            MB_OK | MB_ICONERROR);
        exit_code = 1;
    }

    CoUninitialize();
    return exit_code;
}

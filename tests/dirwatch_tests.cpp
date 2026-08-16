#include "dirwatch.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kDirwatchTestWindowClass[] = L"MinViewDirwatchTestWindow";
constexpr UINT kDirwatchNotifyMessage = WM_APP + 7;

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool wait_for_notification(HWND hwnd, ULONGLONG timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.hwnd == hwnd
                && message.message == kDirwatchNotifyMessage) {
                return true;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(20);
    }
    return false;
}

void test_counter_api_and_watch_notification() {
    mv::DirWatcher watcher;
    expect(watcher.dropped_directories() == 0,
        "a fresh watcher must report zero dropped directories");
    expect(watcher.arm_failures() == 0,
        "a fresh watcher must report zero arm failures");
    expect(watcher.last_watch_error() == 0,
        "a fresh watcher must report no last watch error");

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    if (!GetClassInfoW(instance, kDirwatchTestWindowClass, &wc)) {
        wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = kDirwatchTestWindowClass;
        if (!RegisterClassW(&wc)) {
            std::cout << "dirwatch_tests: SKIP (RegisterClassW failed)\n";
            return;
        }
    }

    // Hidden message-only window: no GUI, no pump contention, and
    // PostMessage from the watcher thread is delivered to this thread.
    HWND hwnd = CreateWindowExW(
        0, kDirwatchTestWindowClass, L"minview-dirwatch-test", WS_POPUP,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!hwnd) {
        std::cout << "dirwatch_tests: SKIP (CreateWindowExW failed)\n";
        UnregisterClassW(kDirwatchTestWindowClass, instance);
        return;
    }

    std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / (L"minview-dirwatch-"
            + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        std::cout << "dirwatch_tests: SKIP (temp directory unavailable)\n";
        DestroyWindow(hwnd);
        UnregisterClassW(kDirwatchTestWindowClass, instance);
        return;
    }

    watcher.watch(hwnd, kDirwatchNotifyMessage,
        {mv::WatchRoot{root.native(), false}});

    // Give the watcher worker time to open the directory and arm its first
    // overlapped read. The retry loop below also tolerates a slow CI machine:
    // each attempt creates another file, so a missed first arm is not fatal.
    Sleep(400);

    bool received = false;
    for (int attempt = 0; attempt < 5 && !received; ++attempt) {
        const std::filesystem::path file =
            root / (L"file-" + std::to_wstring(attempt) + L".txt");
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << "x";
        output.close();
        received = wait_for_notification(hwnd, 1500);
    }

    watcher.stop();
    expect(received,
        "a directory change must reach the notify window");

    DestroyWindow(hwnd);
    UnregisterClassW(kDirwatchTestWindowClass, instance);
    std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
    try {
        test_counter_api_and_watch_notification();
        if (failures == 0) {
            std::cout << "dirwatch_tests: PASS\n";
            return 0;
        }
        std::cerr << "dirwatch_tests: FAIL (" << failures << " check(s))\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "dirwatch_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}

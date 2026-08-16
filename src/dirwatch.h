#pragma once
// Directory watcher for real-time collection refresh (Issue #5 P3).
// ReadDirectoryChangesW-based; recursive roots are covered by watching their
// subdirectories (capped). Changes are debounced and reported via
// PostMessage(notify_window, notify_message, 0, 0).

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mv {

struct WatchRoot {
    std::wstring path;
    bool recursive = false;
};

class DirWatcher {
public:
    DirWatcher() = default;
    ~DirWatcher();
    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;

    // (Re)bind the watcher to a collection's roots. Stops any previous
    // watch first; safe to call repeatedly.
    void watch(HWND notify_window, UINT notify_message,
               std::vector<WatchRoot> roots);
    void stop();

    // Observability counters (thread-safe, monotonic since construction).
    // Directories are dropped only for non-ENUM errors; arm() failures are
    // counted separately but the handle is left in place as before.
    std::uint64_t dropped_directories() const noexcept;
    std::uint64_t arm_failures() const noexcept;
    DWORD last_watch_error() const noexcept;

private:
    void worker_main(HWND notify_window, UINT notify_message,
                     std::vector<WatchRoot> roots);

    std::mutex m_mutex;
    std::thread m_thread;
    HANDLE m_stop_event = nullptr;
    std::atomic<std::uint64_t> m_dropped_directories{0};
    std::atomic<std::uint64_t> m_arm_failures{0};
    std::atomic<DWORD> m_last_watch_error{0};
};

} // namespace mv

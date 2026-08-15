#pragma once
// Directory watcher for real-time collection refresh (Issue #5 P3).
// ReadDirectoryChangesW-based; recursive roots are covered by watching their
// subdirectories (capped). Changes are debounced and reported via
// PostMessage(notify_window, notify_message, 0, 0).

#include <Windows.h>
#include <atomic>
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

private:
    void worker_main(HWND notify_window, UINT notify_message,
                     std::vector<WatchRoot> roots);

    std::mutex m_mutex;
    std::thread m_thread;
    HANDLE m_stop_event = nullptr;
};

} // namespace mv

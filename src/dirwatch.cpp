#include "dirwatch.h"

#include <filesystem>

namespace mv {

namespace fs = std::filesystem;

namespace {
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME
    | FILE_NOTIFY_CHANGE_DIR_NAME
    | FILE_NOTIFY_CHANGE_SIZE
    | FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr std::size_t kMaxWatchedDirs = 63;  // ≤63 + stop event = one wait batch
constexpr ULONGLONG kDebounceMs = 400;

struct WatchHandle {
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    std::vector<BYTE> buffer;  // one pending read per directory
};

} // namespace

DirWatcher::~DirWatcher() {
    stop();
}

void DirWatcher::stop() {
    std::lock_guard lock(m_mutex);
    if (m_stop_event) {
        SetEvent(m_stop_event);
        if (m_thread.joinable()) m_thread.join();
        CloseHandle(m_stop_event);
        m_stop_event = nullptr;
    }
}

void DirWatcher::watch(HWND notify_window, UINT notify_message,
                       std::vector<WatchRoot> roots) {
    stop();
    std::lock_guard lock(m_mutex);
    m_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop_event) return;
    m_thread = std::thread([this, notify_window, notify_message,
                            roots = std::move(roots)]() mutable {
        worker_main(notify_window, notify_message, std::move(roots));
    });
}

void DirWatcher::worker_main(HWND notify_window, UINT notify_message,
                             std::vector<WatchRoot> roots) {
    // Collect directories: each flat root itself; recursive roots add all
    // reachable subdirectories up to the cap.
    std::vector<std::wstring> dirs;
    for (const auto& root : roots) {
        dirs.push_back(root.path);
        if (!root.recursive) continue;
        std::error_code error;
        fs::recursive_directory_iterator it(
            root.path, fs::directory_options::skip_permission_denied, error);
        const fs::recursive_directory_iterator end;
        if (error) continue;
        while (it != end && dirs.size() < kMaxWatchedDirs) {
            if (it->is_directory()) dirs.push_back(it->path().wstring());
            it.increment(error);
            if (error) {
                error.clear();
                continue;
            }
        }
    }

    std::vector<WatchHandle> handles;
    handles.reserve(dirs.size());
    for (const auto& dir : dirs) {
        WatchHandle entry;
        entry.buffer.resize(64 * 1024);
        entry.dir = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (entry.dir == INVALID_HANDLE_VALUE) continue;
        entry.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!entry.event) {
            CloseHandle(entry.dir);
            continue;
        }
        entry.overlapped.hEvent = entry.event;
        handles.push_back(std::move(entry));
    }

    auto arm = [](WatchHandle& entry) {
        DWORD bytes = 0;
        return ReadDirectoryChangesW(entry.dir, entry.buffer.data(),
            static_cast<DWORD>(entry.buffer.size()), FALSE, kNotifyFilter,
            &bytes, &entry.overlapped, nullptr);
    };
    for (auto& entry : handles) (void)arm(entry);

    ULONGLONG last_notify = 0;
    bool dirty = false;
    std::vector<HANDLE> wait_handles;
    wait_handles.reserve(64);

    const auto flush = [&]() {
        if (!dirty) return;
        const ULONGLONG now = GetTickCount64();
        if (now - last_notify >= kDebounceMs) {
            last_notify = now;
            dirty = false;
            PostMessageW(notify_window, notify_message, 0, 0);
        }
    };

    while (true) {
        // One flat batch: stop event + every live handle event (≤64 total).
        wait_handles.clear();
        wait_handles.push_back(m_stop_event);
        for (const auto& entry : handles)
            wait_handles.push_back(entry.event);
        const DWORD result = WaitForMultipleObjects(
            static_cast<DWORD>(wait_handles.size()),
            wait_handles.data(), FALSE, 800);
        if (result == WAIT_OBJECT_0) return;  // stop
        if (result > WAIT_OBJECT_0
            && result <= WAIT_OBJECT_0 + wait_handles.size()) {
            const std::size_t idx = result - WAIT_OBJECT_0 - 1;
            if (idx < handles.size()) {
                WatchHandle& entry = handles[idx];
                DWORD bytes = 0;
                if (GetOverlappedResult(entry.dir, &entry.overlapped,
                        &bytes, FALSE)) {
                    if (bytes > 0) dirty = true;
                    (void)arm(entry);
                } else {
                    const DWORD error = GetLastError();
                    if (error == ERROR_NOTIFY_ENUM_DIR) {
                        dirty = true;
                        (void)arm(entry);
                    } else {
                        // Directory gone or handle broken: drop it.
                        CloseHandle(entry.event);
                        CloseHandle(entry.dir);
                        handles.erase(handles.begin()
                            + static_cast<std::ptrdiff_t>(idx));
                    }
                }
            }
        }
        flush();
    }
}

} // namespace mv

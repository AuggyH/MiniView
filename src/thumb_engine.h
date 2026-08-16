#pragma once
#include <wincodec.h>
#include <wrl/client.h>
#include <d2d1.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mv {

using Microsoft::WRL::ComPtr;

// Thumb cache entry (background workers write these in the shared pool).
struct ThumbEntry {
    ComPtr<IWICBitmapSource> wic;
    bool loaded = false;
    uint32_t orig_w = 0, orig_h = 0;
    D2D1_COLOR_F dominant_color = {0.10f, 0.10f, 0.12f, 1.0f};
};

// Heap-shared thumbnail loader state. Each worker holds its own shared_ptr
// copy, so a worker still busy decoding at shutdown (detached) keeps the
// state alive instead of touching freed App members. ThumbEngine::stop()
// abandons a pool only after detaching busy workers; start() never reuses
// an abandoned pool, so stale workers cannot re-enter the new generation.
struct ThumbPoolState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<int> queue;
    std::vector<std::thread> threads;
    std::vector<ThumbEntry> thumbs;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> request_generation{0};
    std::atomic<std::uint64_t> dimension_generation{0};
};

// A thumbnail cache entry is only usable when the JPEG is complete: the
// encoder finalizes the file with the FF D9 end marker. Entries truncated
// by an interrupted write decode to broken sources that poison the render
// pipeline, so they must be rejected and rewritten.
bool thumbnail_cache_file_complete(const std::wstring& path);

// Cache file path for an image path: <cache_dir>\thumbs\<std::hash>.jpg.
std::wstring thumbnail_cache_path(
    const std::wstring& cache_dir, const std::wstring& path);

// Owns the WIC thumbnail pipeline: JPEG disk cache, decode workers,
// request queue and dimension preload. App keeps D2D bitmap storage and
// GPU uploads on the UI thread; it reads the WIC thumbs through pool()
// under pool()->mutex, exactly like the old App-owned m_thumb_pool.
class ThumbEngine {
public:
    struct Port {
        // WM_THUMB_READY-style UI wakeup. Called from worker threads after a
        // thumbnail or dimension update; must be cheap and non-blocking.
        std::function<void()> notify;
        std::wstring cache_dir;  // config dir that owns the "thumbs" folder
        int thumb_size = 256;    // decode resolution (WIC)
    };

    explicit ThumbEngine(Port port);
    ~ThumbEngine();

    ThumbEngine(const ThumbEngine&) = delete;
    ThumbEngine& operator=(const ThumbEngine&) = delete;

    // Replaces the worker wakeup callback. Call before start(); the callback
    // must own its state (e.g. capture an HWND by value), because a worker
    // detached by stop() may call it after App is destroyed.
    void set_notify(std::function<void()> notify);

    // Starts the 4 decode workers with a BY-VALUE snapshot of paths.
    // No-op when the loader is already running (the existing snapshot and
    // worker set stay in effect).
    void start(std::vector<std::wstring> paths, int thumb_size,
        const std::wstring& cache_dir);

    // Requests stop, joins each worker for 150 ms, detaches busy workers
    // and abandons the pool for a fresh one so the UI keeps the thumb cache
    // while stale workers can never re-enter the new generation.
    void stop();

    // Queue one index (FIFO push; cap 64 evicts the back = farthest).
    void request(int idx);

    // Drops WIC sources and clears `loaded` for every index in
    // [visible_start, visible_end). App owns the D2D LRU trim and calls
    // this once per evicted index, so the WIC cache stays in lock-step
    // with the D2D cache without the engine touching D2D bitmaps.
    void trim(int visible_start, int visible_end);

    // Replaces the queue with [first, last), sorted nearest-to-current,
    // and bumps request_generation so workers abandon stale batches.
    // Returns true when at least one request was queued.
    bool request_window(int first, int last, int current);

    // Probes dimensions for the given path snapshot on a background thread.
    // Joins the previous probe first (paths are already a UI-thread
    // snapshot; the worker never touches ImageIndex).
    void start_dim_preload(std::vector<std::wstring> paths);

    // Thread-safe accessors App uses for grid/filmstrip layout.
    size_t thumb_count() const noexcept;
    bool running() const noexcept;
    std::vector<std::pair<uint32_t, uint32_t>> dims() const;
    uint32_t orig_width(int idx) const;
    uint32_t orig_height(int idx) const;
    uint64_t dimension_generation() const noexcept;
    uint64_t request_generation() const noexcept;

    // Current pool. App locks pool()->mutex for batch reads, keeping grid
    // dims reading identical to the old m_thumb_pool. Only the UI thread
    // may call stop()/start() and swap the pool; workers hold their own
    // shared_ptr copies and never use this accessor.
    std::shared_ptr<ThumbPoolState> pool() const noexcept { return m_pool; }

private:
    std::shared_ptr<ThumbPoolState> m_pool;
    std::function<void()> m_notify;
    int m_thumb_size = 256;
    std::wstring m_cache_dir;
    std::vector<std::wstring> m_paths;  // UI-thread path snapshot
    std::thread m_dim_preload;
};

} // namespace mv

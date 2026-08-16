#pragma once

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mv {

struct ComicLoadRequest {
    int index = -1;
    std::wstring path;
    std::uint32_t target_width = 1;
    std::uint64_t generation = 0;
};

struct ComicLoadResult {
    int index = -1;
    std::wstring path;
    std::uint64_t generation = 0;
    Microsoft::WRL::ComPtr<IWICBitmapSource> bitmap;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t decoded_width = 0;
    std::uint32_t decoded_height = 0;
    std::size_t estimated_cache_bytes = 0;
    bool failed = false;
};

class ComicReaderLoader {
public:
    using DecodeFunction =
        std::function<ComicLoadResult(const ComicLoadRequest&)>;

    explicit ComicReaderLoader(DecodeFunction decode = {});
    ~ComicReaderLoader();

    ComicReaderLoader(const ComicReaderLoader&) = delete;
    ComicReaderLoader& operator=(const ComicReaderLoader&) = delete;

    bool start(HWND owner, UINT ready_message);
    void stop();
    void replace_requests(std::vector<ComicLoadRequest> requests);
    std::vector<ComicLoadResult> take_ready();
    static std::size_t estimated_cache_bytes(
        std::uint32_t width, std::uint32_t height) noexcept;
    bool running() const noexcept {
        return m_state && m_state->running.load(std::memory_order_acquire);
    }

private:
    // Heap-shared worker state. stop() requests stop and waits briefly
    // (join_for, implemented in the .cpp via the thread handle); a worker
    // still busy decoding is detached with its own shared_ptr copy of this
    // state, so it finishes safely without touching freed ComicReaderLoader
    // members. start() always creates a fresh state, so a detached worker
    // can never re-enter a newer generation.
    struct SharedState {
        HWND owner = nullptr;
        UINT ready_message = 0;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<ComicLoadRequest> queue;
        std::vector<ComicLoadRequest> requested;
        std::vector<ComicLoadResult> ready;
        ComicLoadRequest inflight;
        bool has_inflight = false;
        std::uint64_t latest_generation = 0;
        DecodeFunction decode;
        std::atomic<bool> running{false};
    };

    static bool same_request(
        const ComicLoadRequest& left, const ComicLoadRequest& right) noexcept;
    static bool requested_locked(
        const std::vector<ComicLoadRequest>& requested,
        const ComicLoadRequest& request);
    static void worker(std::shared_ptr<SharedState> state);

    std::thread m_thread;
    std::shared_ptr<SharedState> m_state;
    DecodeFunction m_decode;
};

} // namespace mv

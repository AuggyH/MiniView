#pragma once

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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
    ComicReaderLoader() = default;
    ~ComicReaderLoader();

    ComicReaderLoader(const ComicReaderLoader&) = delete;
    ComicReaderLoader& operator=(const ComicReaderLoader&) = delete;

    bool start(HWND owner, UINT ready_message);
    void stop();
    void replace_requests(std::vector<ComicLoadRequest> requests);
    std::vector<ComicLoadResult> take_ready();
    bool running() const noexcept {
        return m_running.load(std::memory_order_relaxed);
    }

private:
    static bool same_request(
        const ComicLoadRequest& left, const ComicLoadRequest& right) noexcept;
    void worker();

    HWND m_owner = nullptr;
    UINT m_ready_message = 0;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<ComicLoadRequest> m_queue;
    std::vector<ComicLoadResult> m_ready;
    ComicLoadRequest m_inflight;
    bool m_has_inflight = false;
    std::atomic<bool> m_running{false};
};

} // namespace mv

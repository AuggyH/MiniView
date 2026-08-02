#include "comic_reader_loader.h"

#include "comic_reader_model.h"
#include "decoder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mv {

namespace {

constexpr std::uint64_t kMaxDecodedPageBytes = 64ULL * 1024ULL * 1024ULL;

std::uint32_t decode_max_dimension(
    std::uint32_t target_width, std::uint32_t source_width,
    std::uint32_t source_height) {
    const double width = std::max(1.0, static_cast<double>(target_width));
    double height = width;
    if (source_width > 0 && source_height > 0) {
        height = width * static_cast<double>(source_height)
            / static_cast<double>(source_width);
    }
    const double pixels = width * height;
    const double max_pixels = static_cast<double>(kMaxDecodedPageBytes / 4ULL);
    const double scale = pixels > max_pixels
        ? std::sqrt(max_pixels / pixels) : 1.0;
    const double maximum = std::max(width, height) * scale;
    return static_cast<std::uint32_t>(std::clamp(
        std::ceil(maximum), 1.0,
        static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}

} // namespace

ComicReaderLoader::~ComicReaderLoader() {
    stop();
}

bool ComicReaderLoader::start(HWND owner, UINT ready_message) {
    if (running()) return true;
    m_owner = owner;
    m_ready_message = ready_message;
    m_running.store(true, std::memory_order_relaxed);
    try {
        m_thread = std::thread([this] { worker(); });
        return true;
    } catch (...) {
        m_running.store(false, std::memory_order_relaxed);
        return false;
    }
}

void ComicReaderLoader::stop() {
    m_running.store(false, std::memory_order_relaxed);
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard lock(m_mutex);
    m_queue.clear();
    m_ready.clear();
    m_has_inflight = false;
}

bool ComicReaderLoader::same_request(
    const ComicLoadRequest& left, const ComicLoadRequest& right) noexcept {
    return left.index == right.index
        && left.generation == right.generation
        && left.path == right.path;
}

void ComicReaderLoader::replace_requests(
    std::vector<ComicLoadRequest> requests) {
    if (!running()) return;
    std::lock_guard lock(m_mutex);
    m_queue.clear();
    for (auto& request : requests) {
        if (request.index < 0 || request.path.empty()) continue;
        if (m_has_inflight && same_request(request, m_inflight)) continue;
        const auto duplicate = std::find_if(
            m_queue.begin(), m_queue.end(), [&request](const ComicLoadRequest& queued) {
                return same_request(request, queued);
            });
        if (duplicate == m_queue.end()) m_queue.push_back(std::move(request));
    }
    m_cv.notify_one();
}

std::vector<ComicLoadResult> ComicReaderLoader::take_ready() {
    std::lock_guard lock(m_mutex);
    std::vector<ComicLoadResult> result;
    result.swap(m_ready);
    return result;
}

void ComicReaderLoader::worker() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        Decoder decoder;
        while (running()) {
            ComicLoadRequest request;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] { return !running() || !m_queue.empty(); });
                if (!running()) break;
                request = std::move(m_queue.front());
                m_queue.pop_front();
                m_inflight = request;
                m_has_inflight = true;
            }

            ComicLoadResult result;
            result.index = request.index;
            result.path = request.path;
            result.generation = request.generation;
            try {
                const auto info = decoder.probe(request.path);
                if (info) {
                    result.source_width = info->width;
                    result.source_height = info->height;
                }
                const std::uint32_t maximum = decode_max_dimension(
                    request.target_width,
                    result.source_width, result.source_height);
                result.bitmap = decoder.decode_scaled(request.path, maximum);
                if (!result.bitmap
                    || FAILED(result.bitmap->GetSize(
                        &result.decoded_width, &result.decoded_height))) {
                    result.failed = true;
                    result.bitmap.Reset();
                } else {
                    const std::size_t pixels =
                        static_cast<std::size_t>(result.decoded_width)
                        * static_cast<std::size_t>(result.decoded_height);
                    result.estimated_cache_bytes = pixels * 8ULL;
                }
            } catch (...) {
                result.failed = true;
                result.bitmap.Reset();
            }

            {
                std::lock_guard lock(m_mutex);
                m_has_inflight = false;
                m_ready.push_back(std::move(result));
            }
            if (m_owner && m_ready_message != 0) {
                PostMessageW(m_owner, m_ready_message, 0, 0);
            }
        }
    } catch (...) {
        m_running.store(false, std::memory_order_relaxed);
    }
    if (SUCCEEDED(com_result)) CoUninitialize();
}

} // namespace mv

#include "comic_reader_loader.h"

#include "comic_reader_model.h"
#include "decoder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace mv {

namespace {

constexpr std::uint64_t kMaxDecodedPageBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kCachedBytesPerPixel = 8;

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

ComicReaderLoader::ComicReaderLoader(DecodeFunction decode)
    : m_decode(std::move(decode)) {}

ComicReaderLoader::~ComicReaderLoader() {
    stop();
}

bool ComicReaderLoader::start(HWND owner, UINT ready_message) {
    if (running()) return true;
    if (m_thread.joinable()) m_thread.join();
    {
        std::lock_guard lock(m_mutex);
        m_owner = owner;
        m_ready_message = ready_message;
        m_queue.clear();
        m_requested.clear();
        m_ready.clear();
        m_has_inflight = false;
        m_latest_generation = 0;
    }
    m_running.store(true, std::memory_order_release);
    try {
        m_thread = std::thread([this] { worker(); });
        return true;
    } catch (...) {
        m_running.store(false, std::memory_order_release);
        return false;
    }
}

void ComicReaderLoader::stop() {
    m_running.store(false, std::memory_order_release);
    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
        m_requested.clear();
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard lock(m_mutex);
    m_ready.clear();
    m_has_inflight = false;
    m_owner = nullptr;
    m_ready_message = 0;
}

bool ComicReaderLoader::same_request(
    const ComicLoadRequest& left, const ComicLoadRequest& right) noexcept {
    return left.index == right.index
        && left.generation == right.generation
        && left.target_width == right.target_width
        && left.path == right.path;
}

bool ComicReaderLoader::requested_locked(
    const ComicLoadRequest& request) const {
    return std::any_of(
        m_requested.begin(), m_requested.end(),
        [&request](const ComicLoadRequest& requested) {
            return same_request(request, requested);
        });
}

void ComicReaderLoader::replace_requests(
    std::vector<ComicLoadRequest> requests) {
    if (!running()) return;
    std::lock_guard lock(m_mutex);
    if (!running()) return;

    requests.erase(
        std::remove_if(
            requests.begin(), requests.end(), [](const ComicLoadRequest& request) {
                return request.index < 0 || request.path.empty();
            }),
        requests.end());

    std::uint64_t generation = 0;
    for (const auto& request : requests) {
        generation = std::max(generation, request.generation);
    }
    if (!requests.empty() && generation < m_latest_generation) return;
    if (!requests.empty()) m_latest_generation = generation;

    m_requested.clear();
    for (auto& request : requests) {
        if (request.generation != generation) continue;
        const auto duplicate = std::find_if(
            m_requested.begin(), m_requested.end(),
            [&request](const ComicLoadRequest& requested) {
                return same_request(request, requested);
            });
        if (duplicate == m_requested.end()) {
            m_requested.push_back(std::move(request));
        }
    }

    m_ready.erase(
        std::remove_if(
            m_ready.begin(), m_ready.end(), [this](const ComicLoadResult& result) {
                return !std::any_of(
                    m_requested.begin(), m_requested.end(),
                    [&result](const ComicLoadRequest& request) {
                        return request.index == result.index
                            && request.generation == result.generation
                            && request.path == result.path;
                    });
            }),
        m_ready.end());

    m_queue.clear();
    for (const auto& request : m_requested) {
        if (m_has_inflight && same_request(request, m_inflight)) continue;
        const bool ready = std::any_of(
            m_ready.begin(), m_ready.end(), [&request](const ComicLoadResult& result) {
                return request.index == result.index
                    && request.generation == result.generation
                    && request.path == result.path;
            });
        if (!ready) m_queue.push_back(request);
    }
    m_cv.notify_one();
}

std::vector<ComicLoadResult> ComicReaderLoader::take_ready() {
    std::lock_guard lock(m_mutex);
    std::vector<ComicLoadResult> result;
    result.swap(m_ready);
    return result;
}

std::size_t ComicReaderLoader::estimated_cache_bytes(
    std::uint32_t width, std::uint32_t height) noexcept {
    constexpr std::size_t maximum = std::numeric_limits<std::size_t>::max();
    const std::size_t wide = static_cast<std::size_t>(width);
    const std::size_t high = static_cast<std::size_t>(height);
    if (wide != 0 && high > maximum / wide) return maximum;
    const std::size_t pixels = wide * high;
    if (pixels > maximum / kCachedBytesPerPixel) return maximum;
    return pixels * kCachedBytesPerPixel;
}

void ComicReaderLoader::worker() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        std::unique_ptr<Decoder> decoder;
        if (!m_decode) decoder = std::make_unique<Decoder>();
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
            try {
                if (m_decode) {
                    result = m_decode(request);
                } else {
                    const auto info = decoder->probe(request.path);
                    if (info) {
                        result.source_width = info->width;
                        result.source_height = info->height;
                    }
                    const std::uint32_t maximum = decode_max_dimension(
                        request.target_width,
                        result.source_width, result.source_height);
                    result.bitmap = decoder->decode_scaled(
                        request.path, maximum);
                    if (result.bitmap) {
                        const HRESULT size_result = result.bitmap->GetSize(
                            &result.decoded_width, &result.decoded_height);
                        if (FAILED(size_result)) result.bitmap.Reset();
                    }
                }
                result.index = request.index;
                result.path = request.path;
                result.generation = request.generation;
                result.failed = result.failed || !result.bitmap;
                if (result.failed) {
                    result.bitmap.Reset();
                    result.estimated_cache_bytes = 0;
                } else {
                    result.estimated_cache_bytes = estimated_cache_bytes(
                        result.decoded_width, result.decoded_height);
                }
            } catch (...) {
                result.index = request.index;
                result.path = request.path;
                result.generation = request.generation;
                result.failed = true;
                result.bitmap.Reset();
            }

            bool publish = false;
            {
                std::lock_guard lock(m_mutex);
                m_has_inflight = false;
                if (running() && requested_locked(request)) {
                    m_ready.push_back(std::move(result));
                    publish = true;
                }
            }
            if (publish && m_owner && m_ready_message != 0) {
                PostMessageW(m_owner, m_ready_message, 0, 0);
            }
        }
    } catch (...) {
        m_running.store(false, std::memory_order_release);
    }
    {
        std::lock_guard lock(m_mutex);
        m_has_inflight = false;
    }
    if (SUCCEEDED(com_result)) CoUninitialize();
}

} // namespace mv

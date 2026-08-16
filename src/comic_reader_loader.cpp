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

    // Always start a fresh generation of heap-shared state. A worker that
    // was detached by stop() still owns the previous state and respects its
    // stop flag; it can never re-enter the new generation.
    auto state = std::make_shared<SharedState>();
    state->owner = owner;
    state->ready_message = ready_message;
    state->decode = m_decode;
    state->running.store(true, std::memory_order_release);
    m_state = state;
    try {
        m_thread = std::thread([state] { worker(state); });
        return true;
    } catch (...) {
        state->running.store(false, std::memory_order_release);
        m_state.reset();
        return false;
    }
}

void ComicReaderLoader::stop() {
    auto state = m_state;
    if (state) {
        state->running.store(false, std::memory_order_release);
        {
            std::lock_guard lock(state->mutex);
            state->queue.clear();
            state->requested.clear();
        }
        state->cv.notify_all();
    }

    // Request stop, then wait briefly. MSVC has no std::thread::join_for,
    // so wait on the thread handle; a worker still busy decoding is detached
    // with its own shared_ptr copy of the state and finishes safely without
    // touching freed ComicReaderLoader members.
    if (m_thread.joinable()) {
        const DWORD wait = WaitForSingleObject(m_thread.native_handle(), 150);
        if (wait == WAIT_OBJECT_0) {
            m_thread.join();
        } else {
            m_thread.detach();
            // Drop our state reference: the detached worker keeps it alive,
            // and the next start() gets a fresh state.
            m_state.reset();
        }
    }

    if (state) {
        std::lock_guard lock(state->mutex);
        state->ready.clear();
        state->has_inflight = false;
        state->owner = nullptr;
        state->ready_message = 0;
    }
}

bool ComicReaderLoader::same_request(
    const ComicLoadRequest& left, const ComicLoadRequest& right) noexcept {
    return left.index == right.index
        && left.generation == right.generation
        && left.target_width == right.target_width
        && left.path == right.path;
}

bool ComicReaderLoader::requested_locked(
    const std::vector<ComicLoadRequest>& requested,
    const ComicLoadRequest& request) {
    return std::any_of(
        requested.begin(), requested.end(),
        [&request](const ComicLoadRequest& item) {
            return same_request(request, item);
        });
}

void ComicReaderLoader::replace_requests(
    std::vector<ComicLoadRequest> requests) {
    auto state = m_state;
    if (!state || !state->running.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(state->mutex);
    if (!state->running.load(std::memory_order_relaxed)) return;

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
    if (!requests.empty() && generation < state->latest_generation) return;
    if (!requests.empty()) state->latest_generation = generation;

    state->requested.clear();
    for (auto& request : requests) {
        if (request.generation != generation) continue;
        const auto duplicate = std::find_if(
            state->requested.begin(), state->requested.end(),
            [&request](const ComicLoadRequest& requested) {
                return same_request(request, requested);
            });
        if (duplicate == state->requested.end()) {
            state->requested.push_back(std::move(request));
        }
    }

    state->ready.erase(
        std::remove_if(
            state->ready.begin(), state->ready.end(),
            [&](const ComicLoadResult& result) {
                return !std::any_of(
                    state->requested.begin(), state->requested.end(),
                    [&result](const ComicLoadRequest& request) {
                        return request.index == result.index
                            && request.generation == result.generation
                            && request.path == result.path;
                    });
            }),
        state->ready.end());

    state->queue.clear();
    for (const auto& request : state->requested) {
        if (state->has_inflight
            && same_request(request, state->inflight)) continue;
        const bool ready = std::any_of(
            state->ready.begin(), state->ready.end(),
            [&request](const ComicLoadResult& result) {
                return request.index == result.index
                    && request.generation == result.generation
                    && request.path == result.path;
            });
        if (!ready) state->queue.push_back(request);
    }
    state->cv.notify_one();
}

std::vector<ComicLoadResult> ComicReaderLoader::take_ready() {
    auto state = m_state;
    if (!state) return {};
    std::lock_guard lock(state->mutex);
    std::vector<ComicLoadResult> result;
    result.swap(state->ready);
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

void ComicReaderLoader::worker(std::shared_ptr<SharedState> state) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        std::unique_ptr<Decoder> decoder;
        if (!state->decode) decoder = std::make_unique<Decoder>();
        while (state->running.load(std::memory_order_relaxed)) {
            ComicLoadRequest request;
            {
                std::unique_lock lock(state->mutex);
                state->cv.wait(lock, [&] {
                    return !state->running.load(std::memory_order_relaxed)
                        || !state->queue.empty();
                });
                if (!state->running.load(std::memory_order_relaxed)) break;
                request = std::move(state->queue.front());
                state->queue.pop_front();
                state->inflight = request;
                state->has_inflight = true;
            }

            ComicLoadResult result;
            try {
                if (state->decode) {
                    result = state->decode(request);
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
                std::lock_guard lock(state->mutex);
                state->has_inflight = false;
                if (state->running.load(std::memory_order_relaxed)
                    && requested_locked(state->requested, request)) {
                    state->ready.push_back(std::move(result));
                    publish = true;
                }
            }
            if (publish && state->owner && state->ready_message != 0) {
                PostMessageW(state->owner, state->ready_message, 0, 0);
            }
        }
    } catch (...) {
        state->running.store(false, std::memory_order_release);
    }
    {
        std::lock_guard lock(state->mutex);
        state->has_inflight = false;
    }
    if (SUCCEEDED(com_result)) CoUninitialize();
}

} // namespace mv

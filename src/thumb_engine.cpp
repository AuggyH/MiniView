#include "thumb_engine.h"
#include "decoder.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <vector>

namespace mv {

namespace {

bool join_for(std::thread& thread, DWORD timeout_ms) {
    if (!thread.joinable()) return true;
    const DWORD wait = WaitForSingleObject(thread.native_handle(), timeout_ms);
    if (wait == WAIT_OBJECT_0) {
        thread.join();
        return true;
    }
    return false;
}

// Save a WIC bitmap as JPEG to disk (for thumbnail cache).
void save_wic_as_jpeg(IWICBitmapSource* src, const std::wstring& path) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return;

    // Write to a temp file then rename: an interrupted save must never
    // leave a truncated entry behind (see thumbnail_cache_file_complete).
    const std::wstring temp_path = path + L".tmp";
    DeleteFileW(temp_path.c_str());
    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return;
    hr = stream->InitializeFromFilename(temp_path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr)) return;
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);

    // Set JPEG quality to 90 (default is ~75, too low for thumbnails).
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) return;
    if (props) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v; VariantInit(&v);
        v.vt = VT_R4; v.fltVal = 0.90f;
        props->Write(1, &opt, &v);
    }
    hr = frame->Initialize(props.Get());

    uint32_t w = 0, h = 0;
    src->GetSize(&w, &h);
    frame->SetSize(w, h);
    frame->WriteSource(src, nullptr);
    frame->Commit();
    encoder->Commit();
    // Commit the temp file atomically into the cache slot.
    if (!MoveFileExW(temp_path.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp_path.c_str());
    }
}

void thumb_loader_worker(
    std::shared_ptr<ThumbPoolState> pool,
    std::vector<std::wstring> paths,
    int thumb_size,
    std::wstring cache_dir,
    std::function<void()> notify)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        Decoder decoder;
        while (pool->running.load(std::memory_order_relaxed)) {
            std::vector<int> batch;
            uint64_t batch_gen = 0;
            {
                std::unique_lock lock(pool->mutex);
                pool->cv.wait(lock, [&] {
                    return !pool->running.load(std::memory_order_relaxed)
                        || !pool->queue.empty();
                });
                if (!pool->running.load(std::memory_order_relaxed)) break;
                if (pool->queue.empty()) continue;
                // Snapshot the whole queue (replaced each frame with the
                // current window's requests, nearest-first). Processing a
                // stale batch is abandoned as soon as a newer generation
                // lands, so fast scrolling always serves the newest window.
                batch.swap(pool->queue);
                batch_gen = pool->request_generation.load(
                    std::memory_order_relaxed);
            }
            for (int idx : batch) {
                {
                    std::lock_guard lock(pool->mutex);
                    if (!pool->running.load(std::memory_order_relaxed)) break;
                    if (pool->request_generation.load(std::memory_order_relaxed)
                        != batch_gen) break;  // newer window arrived
                    if (idx < 0 || idx >= static_cast<int>(pool->thumbs.size())) continue;
                    if (pool->thumbs[static_cast<size_t>(idx)].loaded) continue;
                }

                try {
                    // The worker uses only the path snapshot taken on the UI
                    // thread in start(); it never dereferences ImageIndex,
                    // which sort/scan/collection-swap mutate.
                    if (idx < 0 || idx >= static_cast<int>(paths.size())) continue;
                    const std::wstring path = paths[static_cast<size_t>(idx)];

                    // 骨架屏主题色优先: 先解 64px 小图提取主色并立即通知,
                    // 让占位块在本项完整缩略图排到之前就有颜色。
                    try {
                        auto tiny = decoder.decode_scaled(path, 64);
                        if (tiny) {
                            const D2D1_COLOR_F dom =
                                decoder.extract_dominant(tiny.Get());
                            bool color_changed = false;
                            {
                                std::lock_guard lock(pool->mutex);
                                if (idx >= 0
                                    && idx < static_cast<int>(pool->thumbs.size())
                                    && !pool->thumbs[static_cast<size_t>(idx)].loaded) {
                                    const auto old = pool->thumbs[static_cast<size_t>(idx)].dominant_color;
                                    pool->thumbs[static_cast<size_t>(idx)].dominant_color = dom;
                                    color_changed = old.r != dom.r || old.g != dom.g
                                        || old.b != dom.b || old.a != dom.a;
                                }
                            }
                            if (color_changed && notify) notify();
                        }
                    } catch (...) {}

                    // Probe dimensions early (before decode) for accurate
                    // layout. Skip probing when the directory preload
                    // already supplied them.
                    uint32_t orig_w = 0, orig_h = 0;
                    {
                        std::lock_guard lock(pool->mutex);
                        orig_w = pool->thumbs[static_cast<size_t>(idx)].orig_w;
                        orig_h = pool->thumbs[static_cast<size_t>(idx)].orig_h;
                    }
                    if (orig_w == 0) {
                        if (auto info = decoder.probe(path)) {
                            orig_w = info->width;
                            orig_h = info->height;
                        }
                    }

                    // Check disk cache first.
                    const std::wstring cache_file =
                        thumbnail_cache_path(cache_dir, path);

                    ComPtr<IWICBitmapSource> wic;
                    WIN32_FILE_ATTRIBUTE_DATA src_attr = {};
                    WIN32_FILE_ATTRIBUTE_DATA cache_attr = {};
                    bool cache_hit = false;
                    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &src_attr) &&
                        GetFileAttributesExW(cache_file.c_str(), GetFileExInfoStandard, &cache_attr)) {
                        ULONGLONG src_time =
                            (static_cast<ULONGLONG>(src_attr.ftLastWriteTime.dwHighDateTime) << 32)
                            | src_attr.ftLastWriteTime.dwLowDateTime;
                        ULONGLONG cache_time =
                            (static_cast<ULONGLONG>(cache_attr.ftLastWriteTime.dwHighDateTime) << 32)
                            | cache_attr.ftLastWriteTime.dwLowDateTime;
                        if (cache_time >= src_time
                            && thumbnail_cache_file_complete(cache_file)) {
                            try {
                                wic = decoder.decode_scaled(cache_file, thumb_size);
                                uint32_t cw = 0, ch = 0;
                                if (wic) wic->GetSize(&cw, &ch);
                                // A corrupt cache entry can decode to a broken
                                // source that poisons the render pipeline;
                                // refuse it and fall back to the source file.
                                cache_hit = wic && cw > 0 && ch > 0
                                    && cw <= static_cast<uint32_t>(thumb_size) + 2
                                    && ch <= static_cast<uint32_t>(thumb_size) + 2;
                            } catch (...) {
                                cache_hit = false;
                            }
                            if (!cache_hit) {
                                wic.Reset();
                                DeleteFileW(cache_file.c_str());
                            }
                        }
                    }

                    if (!cache_hit) {
                        wic = decoder.decode_scaled(path, thumb_size);
                        // Save to disk cache.
                        if (wic) {
                            CreateDirectoryW((cache_dir + L"\\thumbs").c_str(), nullptr);
                            save_wic_as_jpeg(wic.Get(), cache_file);
                        }
                    }

                    // Extract dominant color for skeleton screen.
                    D2D1_COLOR_F dom = {0.10f, 0.10f, 0.12f, 1.0f};
                    if (wic) {
                        dom = decoder.extract_dominant(wic.Get());
                    }

                    std::lock_guard lock(pool->mutex);
                    if (idx < 0 || idx >= static_cast<int>(pool->thumbs.size())) continue;
                    bool dimensions_changed =
                        pool->thumbs[static_cast<size_t>(idx)].orig_w != orig_w
                        || pool->thumbs[static_cast<size_t>(idx)].orig_h != orig_h;
                    pool->thumbs[static_cast<size_t>(idx)].wic = wic;
                    pool->thumbs[static_cast<size_t>(idx)].loaded = true;
                    pool->thumbs[static_cast<size_t>(idx)].orig_w = orig_w;
                    pool->thumbs[static_cast<size_t>(idx)].orig_h = orig_h;
                    pool->thumbs[static_cast<size_t>(idx)].dominant_color = dom;
                    if (dimensions_changed)
                        pool->dimension_generation.fetch_add(1,
                            std::memory_order_relaxed);
                    if (notify) notify();
                } catch (...) {
                    std::lock_guard lock(pool->mutex);
                    if (idx >= 0 && idx < static_cast<int>(pool->thumbs.size())) {
                        pool->thumbs[static_cast<size_t>(idx)].loaded = true;
                        if (notify) notify();
                    }
                }
            }  // for batch
        }  // while running
    } catch (...) {
        // Decoder creation failed — thread exits cleanly.
    }
    CoUninitialize();
}

} // namespace

bool thumbnail_cache_file_complete(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    const bool has_size =
        GetFileSizeEx(file, &size) && size.QuadPart >= 4;
    bool complete = false;
    if (has_size) {
        LARGE_INTEGER off;
        off.QuadPart = size.QuadPart - 2;
        SetFilePointerEx(file, off, nullptr, FILE_BEGIN);
        unsigned char tail[2] = {};
        DWORD read = 0;
        if (ReadFile(file, tail, 2, &read, nullptr) && read == 2)
            complete = tail[0] == 0xFF && tail[1] == 0xD9;
    }
    CloseHandle(file);
    return complete;
}

std::wstring thumbnail_cache_path(
    const std::wstring& cache_dir, const std::wstring& path) {
    std::hash<std::wstring> hasher;
    wchar_t key[32];
    swprintf_s(key, L"%016llx", hasher(path));
    return cache_dir + L"\\thumbs\\" + key + L".jpg";
}

ThumbEngine::ThumbEngine(Port port)
    : m_pool(std::make_shared<ThumbPoolState>()),
      m_notify(std::move(port.notify)),
      m_thumb_size(port.thumb_size),
      m_cache_dir(std::move(port.cache_dir)) {}

void ThumbEngine::set_notify(std::function<void()> notify) {
    m_notify = std::move(notify);
}

ThumbEngine::~ThumbEngine() {
    if (m_dim_preload.joinable()) m_dim_preload.join();
    stop();
}

void ThumbEngine::start(std::vector<std::wstring> paths, int thumb_size,
    const std::wstring& cache_dir) {
    auto pool = m_pool;
    if (pool->running.load(std::memory_order_relaxed)) return;

    m_paths = std::move(paths);
    m_thumb_size = thumb_size;
    m_cache_dir = cache_dir;

    // Clean up any joinable leftovers from a partially failed previous
    // start (normal stop leaves them joined or detached already).
    bool has_joinable = false;
    for (auto& thread : pool->threads) {
        if (thread.joinable()) {
            has_joinable = true;
            break;
        }
    }
    if (has_joinable) {
        stop();
        pool = m_pool;
    }

    pool->running.store(true, std::memory_order_release);
    const int num_threads = 4;
    for (int i = 0; i < num_threads; ++i) {
        try {
            pool->threads.emplace_back(thumb_loader_worker, pool, m_paths,
                m_thumb_size, m_cache_dir, m_notify);
        } catch (...) {
            pool->running.store(false, std::memory_order_release);
            pool->cv.notify_all();
            break;
        }
    }
    if (pool->threads.empty()) {
        pool->running.store(false, std::memory_order_release);
    }
}

void ThumbEngine::stop() {
    auto pool = m_pool;
    if (!pool) return;

    pool->running.store(false, std::memory_order_release);
    pool->cv.notify_all();

    // Request stop, wait briefly for the workers to finish their current
    // decode, and detach any worker that is still busy. Detached workers
    // hold `pool` (and its heap-shared state) by shared_ptr, so they finish
    // safely without touching freed App members. MSVC has no std::thread
    // join_for, so join_for() above uses the thread handle.
    bool any_detached = false;
    for (auto& thread : pool->threads) {
        if (!thread.joinable()) continue;
        if (any_detached) {
            thread.detach();
            continue;
        }
        if (join_for(thread, 150)) continue;  // joined within the wait
        thread.detach();
        any_detached = true;
    }
    pool->threads.clear();

    if (any_detached) {
        // Abandon the pool: copy the thumbnail cache into a fresh pool so
        // the UI keeps its cache, while stale detached workers can never
        // re-enter the new generation (they still own the old pool).
        auto replacement = std::make_shared<ThumbPoolState>();
        {
            std::lock_guard lock(pool->mutex);
            replacement->thumbs = pool->thumbs;
        }
        m_pool = std::move(replacement);
    } else {
        std::lock_guard lock(pool->mutex);
        pool->queue.clear();
    }
}

void ThumbEngine::request(int idx) {
    const auto pool = m_pool;
    if (!pool) return;
    {
        std::lock_guard lock(pool->mutex);
        if (idx < 0 || idx >= static_cast<int>(pool->thumbs.size())) return;
        if (pool->thumbs[static_cast<size_t>(idx)].loaded) return;
        for (int q : pool->queue) if (q == idx) return;
        if (pool->queue.size() >= 64) {
            // Drop the oldest/farthest pending request (FIFO back): the
            // front holds nearest-to-current items the worker should serve
            // first, so evict from the back instead.
            pool->queue.pop_back();
        }
        pool->queue.push_back(idx);
    }
    pool->cv.notify_one();
}

void ThumbEngine::trim(int visible_start, int visible_end) {
    const auto pool = m_pool;
    if (!pool) return;
    std::lock_guard lock(pool->mutex);
    for (int index = visible_start; index < visible_end; ++index) {
        if (index >= 0 && index < static_cast<int>(pool->thumbs.size())) {
            pool->thumbs[static_cast<size_t>(index)].wic.Reset();
            pool->thumbs[static_cast<size_t>(index)].loaded = false;
        }
    }
}

bool ThumbEngine::request_window(int first, int last, int current) {
    const auto pool = m_pool;
    std::vector<int> to_request;
    {
        std::lock_guard lock(pool->mutex);
        pool->queue.clear();
        for (int i = first; i < last; ++i) {
            if (i >= 0 && i < static_cast<int>(pool->thumbs.size())
                && !pool->thumbs[static_cast<size_t>(i)].loaded) {
                to_request.push_back(i);
            }
        }
    }
    if (to_request.empty()) return false;
    std::sort(to_request.begin(), to_request.end(),
        [current](int a, int b) {
            const int da = std::abs(a - current);
            const int db = std::abs(b - current);
            return da != db ? da < db : a < b;
        });
    {
        std::lock_guard lock(pool->mutex);
        pool->queue = to_request;
        pool->request_generation.fetch_add(1, std::memory_order_relaxed);
    }
    pool->cv.notify_all();  // wake workers for the new window
    return true;
}

void ThumbEngine::start_dim_preload(std::vector<std::wstring> paths) {
    if (m_dim_preload.joinable()) m_dim_preload.join();
    if (paths.empty()) return;
    const size_t total = paths.size();
    const auto pool = m_pool;
    const auto notify = m_notify;
    m_dim_preload = std::thread(
        [pool, notify, total, paths = std::move(paths)]() {
            std::vector<std::pair<uint32_t, uint32_t>> dims(total, {0, 0});
            try {
                Decoder probe_decoder;
                for (size_t i = 0; i < total; ++i) {
                    try {
                        if (auto info = probe_decoder.probe(paths[i])) {
                            dims[i] = {info->width, info->height};
                        }
                    } catch (...) {}
                }
            } catch (...) {}
            bool any_changed = false;
            {
                std::lock_guard lock(pool->mutex);
                const size_t count = dims.size() < pool->thumbs.size()
                    ? dims.size() : pool->thumbs.size();
                for (size_t i = 0; i < count; ++i) {
                    if (dims[i].first == 0) continue;
                    if (pool->thumbs[i].orig_w != dims[i].first
                        || pool->thumbs[i].orig_h != dims[i].second) {
                        pool->thumbs[i].orig_w = dims[i].first;
                        pool->thumbs[i].orig_h = dims[i].second;
                        any_changed = true;
                    }
                }
            }
            if (any_changed) {
                pool->dimension_generation.fetch_add(1,
                    std::memory_order_relaxed);
                if (notify) notify();
            }
        });
}

size_t ThumbEngine::thumb_count() const noexcept {
    return m_pool->thumbs.size();
}

bool ThumbEngine::running() const noexcept {
    return m_pool->running.load(std::memory_order_relaxed);
}

std::vector<std::pair<uint32_t, uint32_t>> ThumbEngine::dims() const {
    std::vector<std::pair<uint32_t, uint32_t>> result;
    std::lock_guard lock(m_pool->mutex);
    result.reserve(m_pool->thumbs.size());
    for (const auto& thumb : m_pool->thumbs)
        result.push_back({thumb.orig_w, thumb.orig_h});
    return result;
}

uint32_t ThumbEngine::orig_width(int idx) const {
    std::lock_guard lock(m_pool->mutex);
    if (idx < 0 || idx >= static_cast<int>(m_pool->thumbs.size())) return 0;
    return m_pool->thumbs[static_cast<size_t>(idx)].orig_w;
}

uint32_t ThumbEngine::orig_height(int idx) const {
    std::lock_guard lock(m_pool->mutex);
    if (idx < 0 || idx >= static_cast<int>(m_pool->thumbs.size())) return 0;
    return m_pool->thumbs[static_cast<size_t>(idx)].orig_h;
}

uint64_t ThumbEngine::dimension_generation() const noexcept {
    return m_pool->dimension_generation.load(std::memory_order_relaxed);
}

uint64_t ThumbEngine::request_generation() const noexcept {
    return m_pool->request_generation.load(std::memory_order_relaxed);
}

} // namespace mv

// Unit tests for the thumbnail engine policy parts (Maintainability Stage 5).
// Headless: queue cap/eviction, JPEG completeness, request-generation
// supersession, and path-snapshot reuse through the real WIC decoder.

#include "thumb_engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <objbase.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class TempDirectory {
public:
    TempDirectory() {
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path()
            / (L"minview-thumb-engine-tests-" + std::to_wstring(suffix));
        fs::create_directories(m_path);
    }
    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(m_path, error);
    }
    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

void write_u32_le(std::ofstream& output, uint32_t value) {
    const std::array<unsigned char, 4> bytes = {
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff),
        static_cast<unsigned char>((value >> 16) & 0xff),
        static_cast<unsigned char>((value >> 24) & 0xff)
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write_u16_le(std::ofstream& output, uint16_t value) {
    const std::array<unsigned char, 2> bytes = {
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff)
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Minimal 24-bit bottom-up BMP that WIC can decode without extension packs.
void write_bmp(const fs::path& path, uint32_t width, uint32_t height) {
    const uint32_t row_bytes = (width * 3 + 3) & ~3u;
    const uint32_t image_bytes = row_bytes * height;
    std::ofstream output(path, std::ios::binary);
    output.put('B');
    output.put('M');
    write_u32_le(output, 54 + image_bytes);
    write_u32_le(output, 0);
    write_u32_le(output, 54);
    write_u32_le(output, 40);
    write_u32_le(output, width);
    write_u32_le(output, height);
    write_u16_le(output, 1);
    write_u16_le(output, 24);
    write_u32_le(output, 0);
    write_u32_le(output, image_bytes);
    write_u32_le(output, 2835);
    write_u32_le(output, 2835);
    write_u32_le(output, 0);
    write_u32_le(output, 0);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            output.put(static_cast<char>(255));  // B
            output.put(static_cast<char>(0));    // G
            output.put(static_cast<char>(0));    // R
        }
        for (uint32_t pad = width * 3; pad < row_bytes; ++pad)
            output.put('\0');
    }
}

struct ScopedCom {
    HRESULT m_result;
    ScopedCom() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() {
        if (SUCCEEDED(m_result)) CoUninitialize();
    }
};

std::vector<int> queue_snapshot(mv::ThumbEngine& engine) {
    std::lock_guard lock(engine.pool()->mutex);
    return engine.pool()->queue;
}

void test_jpeg_completeness() {
    TempDirectory temp;
    const fs::path valid = temp.path() / L"complete.jpg";
    {
        std::ofstream file(valid, std::ios::binary);
        const unsigned char bytes[] = {0x01, 0x02, 0xFF, 0xD9};
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
    expect(mv::thumbnail_cache_file_complete(valid.wstring()),
        "JPEG file ending in FF D9 must be complete");

    const fs::path truncated = temp.path() / L"truncated.jpg";
    {
        std::ofstream file(truncated, std::ios::binary);
        const unsigned char bytes[] = {0x01, 0x02, 0x03, 0x04};
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
    expect(!mv::thumbnail_cache_file_complete(truncated.wstring()),
        "file without FF D9 tail must be incomplete");

    const fs::path tiny = temp.path() / L"tiny.jpg";
    {
        std::ofstream file(tiny, std::ios::binary);
        file.put('A');
    }
    expect(!mv::thumbnail_cache_file_complete(tiny.wstring()),
        "file shorter than 4 bytes must be incomplete");

    expect(!mv::thumbnail_cache_file_complete((temp.path() / L"missing.jpg").wstring()),
        "missing file must be incomplete");
}

void test_queue_cap_and_eviction_order() {
    mv::ThumbEngine engine(mv::ThumbEngine::Port{});
    engine.pool()->thumbs.resize(100);

    for (int i = 0; i < 64; ++i)
        engine.request(i);
    std::vector<int> queue = queue_snapshot(engine);
    expect(queue.size() == 64, "request queue must cap at 64");
    expect(queue.front() == 0, "queue front keeps the first requested index");
    expect(queue.back() == 63, "queue back holds the 64th requested index");

    engine.request(64);  // cap reached: evict the back (63), then push 64
    queue = queue_snapshot(engine);
    expect(queue.size() == 64, "queue must stay at 64 after overflow");
    expect(queue.front() == 0, "overflow must not evict the front");
    expect(queue.back() == 64, "overflow pushes the new index at the back");
    bool has_63 = false;
    for (int index : queue) has_63 = has_63 || index == 63;
    expect(!has_63, "overflow must evict the farthest pending index (63)");

    engine.request(64);  // already queued
    queue = queue_snapshot(engine);
    expect(queue.size() == 64, "duplicate request must not grow the queue");
    int count_64 = 0;
    for (int index : queue) if (index == 64) ++count_64;
    expect(count_64 == 1, "duplicate request must be rejected");
}

void test_request_generation_supersession() {
    mv::ThumbEngine engine(mv::ThumbEngine::Port{});
    engine.pool()->thumbs.resize(20);

    const uint64_t g0 = engine.request_generation();
    const bool first = engine.request_window(0, 10, 5);
    expect(first, "first window must queue requests");
    expect(engine.request_generation() == g0 + 1,
        "first window must bump request generation once");
    {
        std::vector<int> queue = queue_snapshot(engine);
        const std::vector<int> expected = {5, 4, 6, 3, 7, 2, 8, 1, 9, 0};
        expect(queue == expected, "window must be sorted nearest-to-current");
    }

    const bool second = engine.request_window(10, 20, 15);
    expect(second, "second window must queue requests");
    expect(engine.request_generation() == g0 + 2,
        "second window must bump request generation again");
    {
        std::vector<int> queue = queue_snapshot(engine);
        const std::vector<int> expected = {15, 14, 16, 13, 17, 12, 18, 11, 19, 10};
        expect(queue == expected,
            "new window must replace the previous queue (supersession)");
    }

    // Loaded entries are skipped by the window builder.
    {
        std::lock_guard lock(engine.pool()->mutex);
        engine.pool()->thumbs[13].loaded = true;
    }
    const bool third = engine.request_window(10, 20, 15);
    expect(third, "window with remaining unloaded entries must queue");
    {
        std::vector<int> queue = queue_snapshot(engine);
        bool has_13 = false;
        for (int index : queue) has_13 = has_13 || index == 13;
        expect(!has_13, "loaded entries must be skipped by request_window");
    }

    // Mark the rest loaded: nothing to queue and no generation bump.
    {
        std::lock_guard lock(engine.pool()->mutex);
        for (int i = 10; i < 20; ++i)
            engine.pool()->thumbs[static_cast<size_t>(i)].loaded = true;
    }
    const uint64_t before_empty = engine.request_generation();
    const bool fourth = engine.request_window(10, 20, 15);
    expect(!fourth, "window with only loaded entries must report no queueing");
    expect(engine.request_generation() == before_empty,
        "empty window must not bump the request generation");
}

void test_path_snapshot_reuse() {
    ScopedCom com;
    TempDirectory temp;
    const fs::path image = temp.path() / L"snapshot.bmp";
    write_bmp(image, 8, 6);

    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    std::atomic<int> notify_count{0};
    mv::ThumbEngine engine(mv::ThumbEngine::Port{
        [&]() {
            notify_count.fetch_add(1, std::memory_order_relaxed);
            wait_cv.notify_all();
        }});

    engine.pool()->thumbs.resize(1);
    std::vector<std::wstring> paths = {image.wstring()};
    engine.start(paths, 32, temp.path().wstring());
    paths.clear();  // mutate the caller's vector: workers must use the snapshot

    engine.request(0);
    {
        std::unique_lock lock(wait_mutex);
        const bool ready = wait_cv.wait_for(lock, std::chrono::seconds(10),
            [&] { return notify_count.load(std::memory_order_relaxed) > 0; });
        expect(ready, "worker must notify after decoding the snapshot path");
    }
    // 主题色提取的通知可能先于尺寸写入到达, 轮询等待尺寸落地。
    for (int i = 0; i < 200 && engine.orig_width(0) != 8; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(engine.thumb_count() == 1, "engine must keep one thumb entry");
    expect(engine.orig_width(0) == 8,
        "worker must decode the snapshot path (width)");
    expect(engine.orig_height(0) == 6,
        "worker must decode the snapshot path (height)");

    engine.stop();
    expect(!engine.running(), "stop() must clear the running flag");
}

} // namespace

int main() {
    try {
        test_jpeg_completeness();
        test_queue_cap_and_eviction_order();
        test_request_generation_supersession();
        test_path_snapshot_reuse();
    } catch (const std::exception& error) {
        std::cerr << "uncaught exception: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "uncaught non-standard exception\n";
        return 2;
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "thumb_engine_tests: PASS\n";
    return 0;
}

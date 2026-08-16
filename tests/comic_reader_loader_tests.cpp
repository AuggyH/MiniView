#include "comic_reader_loader.h"
#include "comic_reader_model.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

class ScopedCom {
public:
    ScopedCom() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() {
        if (SUCCEEDED(m_result)) CoUninitialize();
    }
    bool initialized() const { return SUCCEEDED(m_result); }

private:
    HRESULT m_result;
};

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

mv::ComicLoadRequest request(
    int index, std::uint64_t generation = 1,
    std::uint32_t target_width = 100) {
    return {index, L"page-" + std::to_wstring(index), target_width, generation};
}

std::vector<mv::ComicLoadResult> wait_for_results(
    mv::ComicReaderLoader& loader, std::size_t expected) {
    std::vector<mv::ComicLoadResult> results;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (results.size() < expected
           && std::chrono::steady_clock::now() < deadline) {
        auto ready = loader.take_ready();
        for (auto& result : ready) results.push_back(std::move(result));
        if (results.size() < expected) std::this_thread::sleep_for(1ms);
    }
    return results;
}

struct DecodeGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<mv::ComicLoadRequest> calls;
    bool first_started = false;
    bool release_first = false;

    mv::ComicLoadResult decode(const mv::ComicLoadRequest& load) {
        std::unique_lock lock(mutex);
        calls.push_back(load);
        if (calls.size() == 1) {
            first_started = true;
            cv.notify_all();
            cv.wait(lock, [this] { return release_first; });
        }
        mv::ComicLoadResult result;
        result.failed = true;
        return result;
    }

    void wait_for_first() {
        std::unique_lock lock(mutex);
        expect(cv.wait_for(lock, 3s, [this] { return first_started; }),
            "the first decode did not start");
    }

    void release() {
        std::lock_guard lock(mutex);
        release_first = true;
        cv.notify_all();
    }

    std::vector<mv::ComicLoadRequest> snapshot() {
        std::lock_guard lock(mutex);
        return calls;
    }
};

void test_direction_change_replaces_pending_order() {
    auto gate = std::make_shared<DecodeGate>();
    mv::ComicReaderLoader loader(
        [gate](const mv::ComicLoadRequest& load) { return gate->decode(load); });
    expect(loader.start(nullptr, 0), "loader must start");
    loader.replace_requests({request(2), request(3), request(4)});
    gate->wait_for_first();

    loader.replace_requests({request(2), request(1), request(0)});
    gate->release();
    const auto results = wait_for_results(loader, 3);
    loader.stop();

    const auto calls = gate->snapshot();
    expect(calls.size() == 3, "direction replacement must discard old queued work");
    expect(calls[0].index == 2 && calls[1].index == 1 && calls[2].index == 0,
        "the latest viewport order must drive directional prefetch");
    expect(results.size() == 3,
        "requests retained by the latest viewport must publish results");
}

void test_generation_discards_stale_result() {
    auto gate = std::make_shared<DecodeGate>();
    mv::ComicReaderLoader loader(
        [gate](const mv::ComicLoadRequest& load) { return gate->decode(load); });
    expect(loader.start(nullptr, 0), "loader must start");
    loader.replace_requests({request(5, 1)});
    gate->wait_for_first();

    loader.replace_requests({request(5, 2)});
    gate->release();
    const auto results = wait_for_results(loader, 1);
    loader.stop();

    const auto calls = gate->snapshot();
    expect(calls.size() == 2
            && calls[0].generation == 1 && calls[1].generation == 2,
        "a new generation must supersede an in-flight generation");
    expect(results.size() == 1 && results[0].generation == 2,
        "the stale generation result must never reach the consumer");
}

void test_fast_scroll_drops_distant_inflight_result() {
    auto gate = std::make_shared<DecodeGate>();
    mv::ComicReaderLoader loader(
        [gate](const mv::ComicLoadRequest& load) { return gate->decode(load); });
    expect(loader.start(nullptr, 0), "loader must start");
    loader.replace_requests({request(0), request(1)});
    gate->wait_for_first();

    loader.replace_requests({request(20), request(21)});
    gate->release();
    const auto results = wait_for_results(loader, 2);
    loader.stop();

    const auto calls = gate->snapshot();
    expect(calls.size() == 3
            && calls[0].index == 0
            && calls[1].index == 20
            && calls[2].index == 21,
        "fast scrolling must replace queued work with the latest viewport");
    expect(results.size() == 2
            && results[0].index == 20 && results[1].index == 21,
        "a distant in-flight decode may finish but its stale result must be dropped");
}

void test_duplicate_requests_are_deduplicated() {
    auto gate = std::make_shared<DecodeGate>();
    mv::ComicReaderLoader loader(
        [gate](const mv::ComicLoadRequest& load) { return gate->decode(load); });
    expect(loader.start(nullptr, 0), "loader must start");
    const auto duplicate = request(7);
    loader.replace_requests({duplicate, duplicate, duplicate});
    gate->wait_for_first();
    loader.replace_requests({duplicate, duplicate});
    gate->release();
    const auto results = wait_for_results(loader, 1);
    loader.stop();

    expect(gate->snapshot().size() == 1,
        "duplicate queued and in-flight requests must decode once");
    expect(results.size() == 1 && results[0].index == 7,
        "a deduplicated request must still publish one result");
}

void test_failure_result_preserves_identity() {
    mv::ComicReaderLoader loader(
        [](const mv::ComicLoadRequest&) -> mv::ComicLoadResult {
            throw std::runtime_error("deterministic decode failure");
        });
    expect(loader.start(nullptr, 0), "loader must start");
    const auto load = request(9, 4);
    loader.replace_requests({load});
    const auto results = wait_for_results(loader, 1);
    loader.stop();

    expect(results.size() == 1, "a decode failure must produce one result");
    expect(results[0].failed && !results[0].bitmap,
        "a decode exception must become a failed card result");
    expect(results[0].index == load.index
            && results[0].path == load.path
            && results[0].generation == load.generation,
        "a failed result must preserve request identity");
    expect(results[0].estimated_cache_bytes == 0,
        "a failed decode must not consume cache budget");
}

class TemporaryBitmap {
public:
    TemporaryBitmap() {
        m_path = std::filesystem::temp_directory_path()
            / (L"minview-comic-loader-"
                + std::to_wstring(GetCurrentProcessId()) + L".bmp");
        write();
    }

    ~TemporaryBitmap() {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }

    const std::wstring& path() const { return m_path.native(); }

private:
    void write() {
        constexpr std::uint32_t width = 8;
        constexpr std::uint32_t height = 4;
        constexpr std::uint32_t row_bytes = width * 3;
        constexpr std::uint32_t pixel_bytes = row_bytes * height;

        BITMAPFILEHEADER file_header{};
        file_header.bfType = 0x4D42;
        file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file_header.bfSize = file_header.bfOffBits + pixel_bytes;
        BITMAPINFOHEADER info_header{};
        info_header.biSize = sizeof(BITMAPINFOHEADER);
        info_header.biWidth = static_cast<LONG>(width);
        info_header.biHeight = static_cast<LONG>(height);
        info_header.biPlanes = 1;
        info_header.biBitCount = 24;
        info_header.biCompression = BI_RGB;
        info_header.biSizeImage = pixel_bytes;

        std::vector<std::uint8_t> pixels(pixel_bytes, 0x7F);
        std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
        expect(output.good(), "temporary WIC input must be created");
        output.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
        output.write(reinterpret_cast<const char*>(&info_header), sizeof(info_header));
        output.write(reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
        expect(output.good(), "temporary WIC input must be complete");
    }

    std::filesystem::path m_path;
};

void test_wic_scaled_decode_and_byte_measurement() {
    TemporaryBitmap bitmap;
    mv::ComicReaderLoader loader;
    expect(loader.start(nullptr, 0), "WIC loader must start");
    loader.replace_requests({{0, bitmap.path(), 4, 1}});
    const auto results = wait_for_results(loader, 1);
    loader.stop();

    expect(results.size() == 1 && !results[0].failed && results[0].bitmap,
        "the production WIC path must decode the fixture");
    expect(results[0].source_width == 8 && results[0].source_height == 4,
        "the loader must retain source geometry");
    expect(results[0].decoded_width == 4 && results[0].decoded_height == 2,
        "decode_scaled must honor the target display width");
    expect(results[0].estimated_cache_bytes == 64,
        "cache bytes must account for 32bpp WIC plus 32bpp D2D residency");
}

void test_byte_budget_uses_lru_not_entry_count() {
    constexpr std::size_t mib = 1024ULL * 1024ULL;
    mv::ComicLruBudget budget(80 * mib, 512 * mib);
    const std::size_t small_bytes =
        mv::ComicReaderLoader::estimated_cache_bytes(1024, 1024);
    const std::size_t large_bytes =
        mv::ComicReaderLoader::estimated_cache_bytes(3072, 2048);
    expect(small_bytes == 8 * mib && large_bytes == 48 * mib,
        "decoded cache measurement must use bytes rather than entry count");

    budget.touch(1, small_bytes);
    budget.touch(2, large_bytes);
    budget.touch(3, large_bytes);
    const auto evicted = budget.evict_to_budget(440 * mib, {3, 4});
    expect(evicted == std::vector<int>({1, 2}),
        "LRU must evict oldest non-visible entries until byte allowance is met");
    expect(budget.contains(3) && budget.resident_bytes() == large_bytes,
        "the visible decoded page must remain resident within the 512 MiB contract");
}

void test_stop_returns_while_decode_blocked_and_discards_results() {
    auto gate = std::make_shared<DecodeGate>();
    auto callback_finished = std::make_shared<std::atomic<bool>>(false);
    mv::ComicReaderLoader loader(
        [gate, callback_finished](const mv::ComicLoadRequest& load) {
            auto result = gate->decode(load);
            callback_finished->store(true, std::memory_order_release);
            return result;
        });
    expect(loader.start(nullptr, 0), "loader must start");
    loader.replace_requests({request(1)});
    gate->wait_for_first();

    std::thread stopper([&loader] { loader.stop(); });
    const DWORD wait = WaitForSingleObject(stopper.native_handle(), 2000);
    expect(wait == WAIT_OBJECT_0,
        "stop must return promptly while the decode is still blocked");
    if (wait != WAIT_OBJECT_0) gate->release();
    stopper.join();

    expect(!loader.running(), "stop must clear the running state");
    expect(loader.take_ready().empty(),
        "stop must discard an in-flight result and release its cache reference");

    gate->release();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!callback_finished->load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expect(callback_finished->load(std::memory_order_acquire),
        "the detached worker must finish after the gate is released");
}

void test_destructor_returns_while_worker_blocked() {
    auto gate = std::make_shared<DecodeGate>();
    auto callback_finished = std::make_shared<std::atomic<bool>>(false);
    auto loader = std::make_unique<mv::ComicReaderLoader>(
        [gate, callback_finished](const mv::ComicLoadRequest& load) {
            auto result = gate->decode(load);
            callback_finished->store(true, std::memory_order_release);
            return result;
        });
    expect(loader->start(nullptr, 0), "loader must start");
    loader->replace_requests({request(1)});
    gate->wait_for_first();

    std::thread destroyer([owned = std::move(loader)]() mutable {
        owned.reset();  // must not block on the frozen decode
    });
    const DWORD wait = WaitForSingleObject(destroyer.native_handle(), 2000);
    expect(wait == WAIT_OBJECT_0,
        "the destructor must return promptly while the decode is still blocked");
    if (wait != WAIT_OBJECT_0) gate->release();
    destroyer.join();

    gate->release();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!callback_finished->load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expect(callback_finished->load(std::memory_order_acquire),
        "the detached worker must finish after the gate is released");
}

} // namespace

int main() {
    try {
        ScopedCom com;
        expect(com.initialized(), "test thread COM apartment must initialize");
        test_direction_change_replaces_pending_order();
        test_generation_discards_stale_result();
        test_fast_scroll_drops_distant_inflight_result();
        test_duplicate_requests_are_deduplicated();
        test_failure_result_preserves_identity();
        test_wic_scaled_decode_and_byte_measurement();
        test_byte_budget_uses_lru_not_entry_count();
        test_stop_returns_while_decode_blocked_and_discards_results();
        test_destructor_returns_while_worker_blocked();
        std::cout << "comic_reader_loader_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "comic_reader_loader_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}

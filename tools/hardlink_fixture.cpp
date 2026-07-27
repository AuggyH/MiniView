#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uintmax_t kMinimumSize = 20ull * 1024ull * 1024ull;

bool is_supported_image(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension == L".png" || extension == L".jpg" ||
           extension == L".jpeg" || extension == L".bmp" ||
           extension == L".gif" || extension == L".webp" ||
           extension == L".tif" || extension == L".tiff";
}

void remove_partial_fixture(const fs::path& target) {
    std::error_code error;
    for (fs::directory_iterator it(target, error), end; it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        fs::remove(it->path(), error);
        error.clear();
    }
    fs::remove(target, error);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::wcerr << L"Usage: hardlink_fixture <source-dir> <target-dir> <count>\n";
        return 2;
    }

    fs::path source = fs::absolute(argv[1]).lexically_normal();
    fs::path target = fs::absolute(argv[2]).lexically_normal();
    wchar_t* count_end = nullptr;
    unsigned long requested = std::wcstoul(argv[3], &count_end, 10);
    if (!count_end || *count_end != L'\0' || requested == 0) {
        std::wcerr << L"Count must be a positive integer.\n";
        return 2;
    }

    std::error_code error;
    if (!fs::is_directory(source, error)) {
        std::wcerr << L"Source directory is unavailable: " << source << L"\n";
        return 3;
    }
    error.clear();
    if (fs::exists(target, error)) {
        std::wcerr << L"Target already exists; refusing to modify it: " << target << L"\n";
        return 4;
    }
    if (source.root_name() != target.root_name()) {
        std::wcerr << L"Hard links require source and target on the same volume.\n";
        return 5;
    }

    std::vector<fs::path> sources;
    fs::recursive_directory_iterator it(
        source, fs::directory_options::skip_permission_denied, error);
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!it->is_regular_file(error) || error || !is_supported_image(it->path())) {
            error.clear();
            continue;
        }
        uintmax_t size = it->file_size(error);
        if (!error && size >= kMinimumSize) sources.push_back(it->path());
        error.clear();
    }
    if (sources.empty()) {
        std::wcerr << L"No supported images of at least 20 MiB were found.\n";
        return 6;
    }
    std::sort(sources.begin(), sources.end(), [](const fs::path& left,
                                                 const fs::path& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    if (!fs::create_directory(target, error) || error) {
        std::wcerr << L"Could not create target directory: " << target << L"\n";
        return 7;
    }

    auto started = std::chrono::steady_clock::now();
    for (unsigned long index = 0; index < requested; ++index) {
        const fs::path& original = sources[index % sources.size()];
        wchar_t filename[64]{};
        _snwprintf_s(filename, _countof(filename), _TRUNCATE,
                     L"qa_%06lu%s", index, original.extension().c_str());
        fs::path link = target / filename;
        if (!CreateHardLinkW(link.c_str(), original.c_str(), nullptr)) {
            DWORD failure = GetLastError();
            remove_partial_fixture(target);
            std::wcerr << L"CreateHardLinkW failed at item " << index
                       << L" with error " << failure << L". Partial fixture removed.\n";
            return 8;
        }
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    std::wcout << L"created=" << requested << L"\n"
               << L"unique_sources=" << sources.size() << L"\n"
               << L"minimum_bytes=" << kMinimumSize << L"\n"
               << L"elapsed_ms=" << elapsed.count() << L"\n"
               << L"target=" << target << L"\n";
    return 0;
}

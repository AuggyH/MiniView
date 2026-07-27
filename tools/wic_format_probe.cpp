#include "decoder.h"

#include <Windows.h>
#include <chrono>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"usage: wic_format_probe <image> [image ...]\n";
        return 2;
    }

    HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        std::wcerr << L"COM initialization failed\n";
        return 2;
    }

    int failures = 0;
    try {
        mv::Decoder decoder;
        for (int i = 1; i < argc; ++i) {
            const auto started = std::chrono::steady_clock::now();
            auto info = decoder.probe(argv[i]);
            bool decoded = false;
            try {
                decoded = decoder.decode_scaled(argv[i], 512) != nullptr;
            } catch (...) {
                decoded = false;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            std::wcout << argv[i]
                       << L"\tprobe=" << (info ? L"yes" : L"no")
                       << L"\tdecode=" << (decoded ? L"yes" : L"no");
            if (info)
                std::wcout << L"\tsize=" << info->width << L"x" << info->height;
            std::wcout << L"\telapsed_ms=" << elapsed << L'\n';
            if (!info || !decoded) ++failures;
        }
    } catch (const std::exception&) {
        CoUninitialize();
        return 2;
    }

    CoUninitialize();
    return failures == 0 ? 0 : 1;
}

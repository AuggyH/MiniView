#include "indexer.h"

#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) {
        std::wcerr << L"usage: indexer_benchmark <directory>\n";
        return 2;
    }

    const auto started = std::chrono::steady_clock::now();
    mv::ImageIndex index;
    const int count = index.scan(argv[1], true);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    PROCESS_MEMORY_COUNTERS_EX memory = {};
    memory.cb = sizeof(memory);
    const BOOL memory_ok = GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));

    std::wcout << L"count=" << count << L"\n";
    std::wcout << L"elapsed_ms=" << elapsed << L"\n";
    if (memory_ok) {
        std::wcout << L"private_bytes=" << memory.PrivateUsage << L"\n";
        std::wcout << L"working_set_bytes=" << memory.WorkingSetSize << L"\n";
    }
    return count < 0 ? 1 : 0;
}

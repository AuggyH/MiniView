#include "metadata.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <Windows.h>

int main(int argc, char* argv[]) {
    if (argc < 2) { std::cout << "Usage: test_meta <path>" << std::endl; return 1; }
    
    int len = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
    std::wstring path(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &path[0], len);
    while (!path.empty() && path.back() == L'\0') path.pop_back();
    
    auto meta = mv::extract_metadata(path);
    
    std::cout << "Valid: " << (meta.valid ? "true" : "false") << std::endl;
    std::wcout << L"Model: " << meta.model << std::endl;
    std::cout << "Seed: " << meta.seed << std::endl;
    std::cout << "Steps: " << meta.steps << std::endl;
    std::cout << "CFG: " << meta.cfg << std::endl;
    std::wcout << L"Sampler: " << meta.sampler << std::endl;
    std::wcout << L"Pos(" << meta.positive_prompt.size() << L"): " << meta.positive_prompt.substr(0, std::min(size_t(80), meta.positive_prompt.size())) << std::endl;
    std::wcout << L"Neg(" << meta.negative_prompt.size() << L"): " << meta.negative_prompt.substr(0, std::min(size_t(80), meta.negative_prompt.size())) << std::endl;
    std::wcout << L"LoRA: " << meta.lora << std::endl;
    return 0;
}

#include "metadata.h"
#include <iostream>
#include <Windows.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: test_meta <png_path>" << std::endl;
        return 1;
    }
    
    // Convert to wstring
    int len = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
    std::wstring path(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &path[0], len);
    
    auto meta = mv::extract_metadata(path);
    
    std::cout << "Valid: " << (meta.valid ? "true" : "false") << std::endl;
    std::wcout << L"Model: " << meta.model << std::endl;
    std::cout << "Seed: " << meta.seed << std::endl;
    std::cout << "Steps: " << meta.steps << std::endl;
    std::cout << "CFG: " << meta.cfg << std::endl;
    std::wcout << L"Sampler: " << meta.sampler << std::endl;
    std::wcout << L"Scheduler: " << meta.scheduler << std::endl;
    std::cout << "Width: " << meta.width << std::endl;
    std::cout << "Height: " << meta.height << std::endl;
    std::wcout << L"Positive: " << meta.positive_prompt << std::endl;
    std::wcout << L"Negative: " << meta.negative_prompt << std::endl;
    std::wcout << L"VAE: " << meta.vae << std::endl;
    
    return 0;
}

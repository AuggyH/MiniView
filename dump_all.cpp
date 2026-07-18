#include "metadata.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <Windows.h>

namespace fs = std::filesystem;

void dump_meta(const std::wstring& path) {
    auto meta = mv::extract_metadata(path);
    std::wcout << L"  Valid=" << (meta.valid ? L"Y" : L"N")
               << L" Model=" << meta.model
               << L" Seed=" << meta.seed
               << L" Steps=" << meta.steps
               << L" CFG=" << meta.cfg
               << L" Sampler=" << meta.sampler << std::endl;
    if (!meta.positive_prompt.empty())
        std::wcout << L"  POS(" << meta.positive_prompt.size() << L"): " 
                   << meta.positive_prompt.substr(0, std::min(size_t(80), meta.positive_prompt.size())) << L"..." << std::endl;
    if (!meta.negative_prompt.empty())
        std::wcout << L"  NEG(" << meta.negative_prompt.size() << L"): " 
                   << meta.negative_prompt.substr(0, std::min(size_t(80), meta.negative_prompt.size())) << L"..." << std::endl;
    if (!meta.vae.empty()) std::wcout << L"  VAE=" << meta.vae << std::endl;
}

int main() {
    std::wstring dir = L"D:\\AIGC\\ComfyUI\\output";
    int count = 0;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().wstring();
        for (auto& c : ext) c = towlower(c);
        if (ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".webp") continue;
        
        std::wcout << entry.path().filename().wstring() << std::endl;
        dump_meta(entry.path());
        std::wcout << std::endl;
        if (++count >= 10) break;
    }
    return 0;
}

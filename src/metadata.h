#pragma once
#include <string>

namespace mv {

struct ImageMeta {
    std::wstring positive_prompt;
    std::wstring negative_prompt;
    std::wstring model;
    std::wstring vae;
    std::wstring sampler;
    std::wstring scheduler;
    long long seed = -1;
    int    steps = 0;
    float  cfg = 0.0f;
    int    width = 0;
    int    height = 0;
    // LoRA info: comma-separated list of "name:strength"
    std::wstring lora;
    bool   valid = false;
};

/// Parse PNG metadata (tEXt chunks) for ComfyUI workflow or SD WebUI parameters.
ImageMeta extract_metadata(const std::wstring& path);

} // namespace mv

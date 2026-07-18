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
    int    seed = -1;
    int    steps = 0;
    float  cfg = 0.0f;
    int    width = 0;
    int    height = 0;
    bool   valid = false;
};

/// Parse PNG metadata (tEXt chunks) for ComfyUI workflow or SD WebUI parameters.
ImageMeta extract_metadata(const std::wstring& path);

} // namespace mv

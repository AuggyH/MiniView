#include "metadata.h"
#include <fstream>
#include <vector>
#include <regex>
#include <algorithm>
#include <unordered_map>
#include <Windows.h>

namespace mv {

namespace {

// ── Helpers ──────────────────────────────────────────────────

std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &result[0], len);
    return result;
}

// ── PNG chunk reader ─────────────────────────────────────────

struct PngChunk {
    uint32_t length;
    char     type[5];  // 4 chars + null
    std::vector<char> data;
};

// Read PNG chunks from file, return all tEXt chunk values keyed by keyword
std::unordered_map<std::string, std::string> read_png_texts(const std::wstring& path) {
    std::unordered_map<std::string, std::string> result;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return result;

    // Check PNG signature
    char sig[8];
    f.read(sig, 8);
    if (f.gcount() != 8 || sig[0] != '\x89' || sig[1] != 'P' ||
        sig[2] != 'N' || sig[3] != 'G') return result;

    while (f.good()) {
        uint32_t len_be;
        f.read(reinterpret_cast<char*>(&len_be), 4);
        if (f.gcount() != 4) break;
        uint32_t len = _byteswap_ulong(len_be);

        char type[5] = {};
        f.read(type, 4);
        if (f.gcount() != 4) break;

        std::vector<char> data(len);
        if (len > 0) {
            f.read(data.data(), len);
            if (f.gcount() != len) break;
        }

        // Skip CRC
        f.seekg(4, std::ios::cur);

        if (strcmp(type, "tEXt") == 0 && len > 0) {
            // Format: keyword\0value
            auto null_pos = std::find(data.begin(), data.end(), '\0');
            if (null_pos != data.end()) {
                std::string keyword(data.begin(), null_pos);
                std::string value(null_pos + 1, data.end());
                result[keyword] = value;
            }
        }
    }
    return result;
}

// ── Simple JSON value extractor (no full parser — targeted extraction) ─

int extract_json_int(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return -1;
    pos += search.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n'))
        pos++;
    try {
        return std::stoi(json.substr(pos));
    } catch (...) { return -1; }
}

float extract_json_float(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0f;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n'))
        pos++;
    try {
        return std::stof(json.substr(pos));
    } catch (...) { return 0.0f; }
}

std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// ── ComfyUI workflow parser ──────────────────────────────────

ImageMeta parse_comfyui(const std::string& json) {
    ImageMeta m;

    // Find seed from KSampler
    m.seed = extract_json_int(json, "seed");
    m.steps = extract_json_int(json, "steps");
    m.cfg = extract_json_float(json, "cfg");
    m.sampler = utf8_to_wstring(extract_json_string(json, "sampler_name"));
    m.scheduler = utf8_to_wstring(extract_json_string(json, "scheduler"));

    // Find model from CheckpointLoaderSimple
    m.model = utf8_to_wstring(extract_json_string(json, "ckpt_name"));
    if (m.model.empty())
        m.model = utf8_to_wstring(extract_json_string(json, "model_name"));

    // Find VAE from VAELoader
    m.vae = utf8_to_wstring(extract_json_string(json, "vae_name"));

    // Find resolution from EmptyLatentImage
    m.width = extract_json_int(json, "width");
    m.height = extract_json_int(json, "height");

    // Extract CLIPTextEncode text values (prompts)
    // Look for "text":"..." patterns and take the longest ones
    std::vector<std::string> texts;
    std::string search = "\"text\":\"";
    size_t pos = 0;
    while ((pos = json.find(search, pos)) != std::string::npos) {
        pos += search.size();
        auto end = json.find('"', pos);
        if (end != std::string::npos && end - pos > 3) {
            std::string text = json.substr(pos, end - pos);
            texts.push_back(text);
        }
        pos = end + 1;
    }

    if (!texts.empty()) {
        // Sort by length: longest is usually the main positive prompt
        std::sort(texts.begin(), texts.end(),
            [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        m.positive_prompt = utf8_to_wstring(texts[0]);
        if (texts.size() > 1 && texts[1].size() > 3)
            m.negative_prompt = utf8_to_wstring(texts[1]);
    }

    // Also look for a node titled "negative" for negative prompt
    // Simple heuristic: find second-longest text
    if (texts.size() > 1 && m.negative_prompt.empty()) {
        // The negative prompt node often has "negative" in its title nearby
        for (size_t i = 1; i < texts.size(); ++i) {
            if (texts[i].size() > 3) {
                m.negative_prompt = utf8_to_wstring(texts[i]);
                break;
            }
        }
    }

    m.valid = (m.seed >= 0 || !m.model.empty() || !m.positive_prompt.empty());
    return m;
}

// ── SD WebUI parser ──────────────────────────────────────────

ImageMeta parse_webui(const std::string& text) {
    ImageMeta m;

    // Split at "Negative prompt:"
    auto neg_pos = text.find("Negative prompt:");
    if (neg_pos != std::string::npos) {
        m.positive_prompt = utf8_to_wstring(text.substr(0, neg_pos));
        // Trim trailing whitespace from positive
        while (!m.positive_prompt.empty() &&
               (m.positive_prompt.back() == L' ' || m.positive_prompt.back() == L'\n'))
            m.positive_prompt.pop_back();

        auto neg_start = neg_pos + 17; // length of "Negative prompt:"
        auto param_pos = text.find("\nSteps:", neg_start);
        if (param_pos == std::string::npos) param_pos = text.size();
        m.negative_prompt = utf8_to_wstring(text.substr(neg_start, param_pos - neg_start));
    } else {
        // No negative prompt separator — whole text is positive
        m.positive_prompt = utf8_to_wstring(text);
    }

    // Extract parameters with regex
    std::regex re;
    std::smatch match;
    std::string s(text);

    re = R"(Steps:\s*(\d+))";
    if (std::regex_search(s, match, re)) m.steps = std::stoi(match[1]);

    re = R"(Sampler:\s*([^,]+))";
    if (std::regex_search(s, match, re)) m.sampler = utf8_to_wstring(match[1]);

    re = R"(CFG scale:\s*([\d.]+))";
    if (std::regex_search(s, match, re)) m.cfg = std::stof(match[1]);

    re = R"(Seed:\s*(\d+))";
    if (std::regex_search(s, match, re)) m.seed = std::stoi(match[1]);

    re = R"(Size:\s*(\d+)x(\d+))";
    if (std::regex_search(s, match, re)) {
        m.width = std::stoi(match[1]);
        m.height = std::stoi(match[2]);
    }

    re = R"(Model:\s*([^,]+))";
    if (std::regex_search(s, match, re)) m.model = utf8_to_wstring(match[1]);

    re = R"(VAE:\s*([^,]+))";
    if (std::regex_search(s, match, re)) m.vae = utf8_to_wstring(match[1]);

    m.valid = true;
    return m;
}

} // anonymous namespace

// ── Public API ───────────────────────────────────────────────

ImageMeta extract_metadata(const std::wstring& path) {
    // Only process PNG files
    auto ext_pos = path.rfind(L'.');
    if (ext_pos == std::wstring::npos) return {};
    std::wstring ext = path.substr(ext_pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
    if (ext != L".png") return {};

    auto texts = read_png_texts(path);

    // Check for ComfyUI workflow (keywords: "prompt" or "workflow")
    auto it = texts.find("prompt");
    if (it == texts.end()) it = texts.find("workflow");
    if (it != texts.end()) {
        return parse_comfyui(it->second);
    }

    // Check for SD WebUI (keyword "parameters")
    it = texts.find("parameters");
    if (it != texts.end()) {
        return parse_webui(it->second);
    }

    // Also check for "Description" (some tools use this)
    return {};
}

} // namespace mv

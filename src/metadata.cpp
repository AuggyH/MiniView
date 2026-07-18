#include "metadata.h"
#include <fstream>
#include <iostream>
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

// ── SD WebUI parser ──────────────────────────────────────────

/// Simple JSON-like extractor: find key in JSON text and return value as string
static std::string json_str_val(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos) + 1;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos < json.size() && json[pos] == '"') {
        size_t end = json.find('"', pos + 1);
        if (end != std::string::npos) return json.substr(pos + 1, end - pos - 1);
    }
    // Numeric — read until , or }
    size_t end = json.find_first_of(",}", pos);
    if (end != std::string::npos) return json.substr(pos, end - pos);
    return "";
}

static long long json_llong(const std::string& json, const std::string& key) {
    auto s = json_str_val(json, key);
    if (s.empty()) return -1;
    try { return std::stoll(s); } catch (...) { return -1; }
}

static int json_int(const std::string& json, const std::string& key) {
    auto s = json_str_val(json, key);
    if (s.empty()) return -1;
    try { return std::stoi(s); } catch (...) { return -1; }
}

static float json_float(const std::string& json, const std::string& key) {
    auto s = json_str_val(json, key);
    if (s.empty()) return 0.0f;
    try { return std::stof(s); } catch (...) { return 0.0f; }
}

/// Find a node by class_type, return the "inputs" sub-object
static std::string find_node(const std::string& json, const std::string& class_type) {
    // Search for "class_type": "ClassName" with optional whitespace after colon
    std::string key = "\"class_type\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return "";

    // Scan all class_type instances
    while (pos != std::string::npos) {
        // Find the value after ":"
        size_t colon = json.find(':', pos + key.size());
        if (colon == std::string::npos) break;
        size_t val_start = colon + 1;
        while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t'))
            val_start++;
        if (val_start >= json.size() || json[val_start] != '"') { pos = json.find(key, val_start); continue; }
        size_t val_end = json.find('"', val_start + 1);
        if (val_end == std::string::npos) { pos = json.find(key, val_end); continue; }
        std::string found_class = json.substr(val_start + 1, val_end - val_start - 1);
        if (found_class != class_type) { pos = json.find(key, val_end); continue; }

        // Found the right class_type node — backtrack to find the node's opening brace
        size_t node_start = pos;
        int depth = 0;
        while (node_start > 0) {
            if (json[node_start] == '}') depth++;
            else if (json[node_start] == '{') {
                if (depth == 0) break;
                depth--;
            }
            node_start--;
        }
        if (node_start == 0) { pos = json.find(key, val_end); continue; }

        // Find the "inputs" section within this node
        size_t inp = json.find("\"inputs\"", node_start);
        if (inp == std::string::npos || inp > val_end + 500) {
            pos = json.find(key, val_end); continue;
        }

        // Extract inputs object (brace-balanced)
        size_t start = json.find('{', inp);
        if (start == std::string::npos) { pos = json.find(key, val_end); continue; }
        depth = 1;
        size_t end = start + 1;
        while (end < json.size() && depth > 0) {
            if (json[end] == '{') depth++;
            else if (json[end] == '}') depth--;
            end++;
        }
        return json.substr(start, end - start);
    }
    return "";
}

ImageMeta parse_comfyui(const std::string& json) {
    ImageMeta m;

    auto ksampler = find_node(json, "KSampler");
    if (!ksampler.empty()) {
        m.seed = json_llong(ksampler, "seed");
        m.steps = json_int(ksampler, "steps");
        m.cfg = json_float(ksampler, "cfg");
        m.sampler = utf8_to_wstring(json_str_val(ksampler, "sampler_name"));
        m.scheduler = utf8_to_wstring(json_str_val(ksampler, "scheduler"));
    }

    auto loader = find_node(json, "CheckpointLoaderSimple");
    if (!loader.empty()) {
        m.model = utf8_to_wstring(json_str_val(loader, "ckpt_name"));
    }

    auto vae = find_node(json, "VAELoader");
    if (!vae.empty()) {
        m.vae = utf8_to_wstring(json_str_val(vae, "vae_name"));
    }

    auto latent = find_node(json, "EmptyLatentImage");
    if (!latent.empty()) {
        m.width = json_int(latent, "width");
        m.height = json_int(latent, "height");
    }

    // Extract prompts from CLIPTextEncode nodes by _meta.title
    {
        std::string search = "\"CLIPTextEncode\"";
        size_t pos = 0;
        while ((pos = json.find(search, pos)) != std::string::npos) {
            pos += search.size();
            // Backtrack to find node start
            size_t ns = pos;
            int d = 0;
            while (ns > 0) {
                if (json[ns] == '}') d++; else if (json[ns] == '{') {
                    if (d == 0) break; d--;
                } ns--;
            }
            // Get _meta.title
            size_t mt = json.find("\"_meta\"", ns);
            if (mt == std::string::npos || mt > pos + 300) continue;
            size_t tt = json.find("\"title\"", mt);
            if (tt == std::string::npos || tt > mt + 100) continue;
            size_t tc = json.find(':', tt + 7);
            if (tc == std::string::npos || tc > tt + 10) continue;
            tc++;
            while (tc < json.size() && (json[tc] == ' ' || json[tc] == '\t')) tc++;
            if (tc >= json.size() || json[tc] != '"') continue;
            size_t te = json.find('"', tc + 1);
            if (te == std::string::npos) continue;
            std::string title = json.substr(tc + 1, te - tc - 1);

            // Get text from inputs
            auto resolve = [&]() -> std::string {
                size_t inp = json.find("\"inputs\"", ns);
                if (inp == std::string::npos || inp > pos + 500) return "";
                size_t ts = json.find("\"text\"", inp);
                if (ts == std::string::npos || ts > inp + 500) return "";
                size_t tvs = json.find(':', ts + 6);
                if (tvs == std::string::npos || tvs > ts + 10) return "";
                tvs++;
                while (tvs < json.size() && (json[tvs]==' ' || json[tvs]=='\t')) tvs++;
                if (tvs >= json.size()) return "";
                // Direct string
                if (json[tvs] == '"') {
                    size_t ve = json.find('"', tvs + 1);
                    if (ve != std::string::npos) return json.substr(tvs + 1, ve - tvs - 1);
                }
                // Link [id, slot]
                if (json[tvs] == '[') {
                    size_t ls = tvs + 1;
                    while (ls < json.size() && (json[ls]==' ' || json[ls]=='\t')) ls++;
                    bool q = (ls < json.size() && json[ls]=='"'); if (q) ls++;
                    size_t le = json.find_first_of(q ? "\"":",]", ls);
                    std::string wid = json.substr(ls, le - ls);
                    std::string wk = "\"" + wid + "\"";
                    // Find the actual node key "\"id\": {", not a reference inside a link
                    size_t wp = 0;
                    while (true) {
                        wp = json.find(wk, wp);
                        if (wp == std::string::npos) return "";
                        // Check if followed by ": {"
                        size_t after = wp + wk.size();
                        while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) after++;
                        if (after < json.size() && json[after] == ':') {
                            after++;
                            while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) after++;
                            if (after < json.size() && json[after] == '{') break; // Found real node
                        }
                        wp++; // Try next occurrence
                    }
                    size_t wb = json.find('{', wp + wk.size());
                    if (wb == std::string::npos || wb > wp + wk.size() + 20) return "";
                    // Search widget inputs & widgets_values
                    for (auto* area : {"\"inputs\"", "\"widgets_values\""}) {
                        size_t wi = json.find(area, wp);
                        if (wi == std::string::npos || wi > wb + 2000) continue;
                        for (auto* fld : {"\"text\"", "\"string\"", "\"value\"", "\"widget_value\"", "\"multiline\""}) {
                            size_t f = json.find(fld, wi);
                            if (f == std::string::npos || f > wi + 2000) continue;
                            size_t fc = json.find(':', f + strlen(fld));
                            if (fc == std::string::npos || fc > f + 20) continue;
                            fc++;
                            while (fc < json.size() && (json[fc]==' ' || json[fc]=='\t')) fc++;
                            if (fc < json.size() && json[fc] == '"') {
                                size_t fe = json.find('"', fc + 1);
                                if (fe != std::string::npos) return json.substr(fc + 1, fe - fc - 1);
                            }
                        }
                        // widgets_values array
                        size_t ws = json.find('[', wi);
                        if (ws != std::string::npos && ws < wi + 20) {
                            ws++;
                            while (ws < json.size() && (json[ws]==' ' || json[ws]=='\t')) ws++;
                            if (ws < json.size() && json[ws] == '"') {
                                size_t we = json.find('"', ws + 1);
                                if (we != std::string::npos) return json.substr(ws + 1, we - ws - 1);
                            }
                        }
                    }
                }
                return "";
            };
            std::string text = resolve();

            if (!text.empty()) {
                if (title.find("\\u6b63\\u9762") != std::string::npos ||
                    title.find("\\u6b63\\u5411") != std::string::npos)
                    m.positive_prompt = utf8_to_wstring(text);
                else if (title.find("\\u8d1f\\u9762") != std::string::npos ||
                         title.find("\\u53cd\\u5411") != std::string::npos)
                    m.negative_prompt = utf8_to_wstring(text);
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

// Forward declaration for JPEG reader
std::string read_jpeg_comment(const std::wstring& path);

ImageMeta extract_metadata(const std::wstring& path) {
    auto ext_pos = path.rfind(L'.');
    if (ext_pos == std::wstring::npos) return {};
    std::wstring ext = path.substr(ext_pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);

    // PNG: read tEXt chunks
    if (ext == L".png") {
        auto texts = read_png_texts(path);
        
        {
            std::ofstream dbg("D:/Projects/minview-native/meta_debug.log", std::ios::app);
            dbg << "=== [PNG] ===" << std::endl;
            for (auto& [k, v] : texts) {
                bool is_json = (!v.empty() && (v.front() == '{' || v.front() == '['));
                dbg << "  tEXt[" << k << "]: " << (is_json ? "JSON" : "TEXT") 
                    << " len=" << v.size() << std::endl;
                if (is_json) {
                    // Show first 300 chars
                    dbg << "    " << v.substr(0, std::min(size_t(300), v.size())) << std::endl;
                } else {
                    dbg << "    " << v.substr(0, std::min(size_t(100), v.size())) << std::endl;
                }
            }
            dbg << std::endl;
        }

        // ComfyUI: "prompt" or "workflow" keywords
        auto it = texts.find("prompt");
        if (it == texts.end()) it = texts.find("workflow");
        if (it != texts.end()) return parse_comfyui(it->second);

        // SD WebUI: "parameters"
        it = texts.find("parameters");
        if (it != texts.end()) return parse_webui(it->second);

        // Some tools use "Description" or "Comment"
        it = texts.find("Description");
        if (it == texts.end()) it = texts.find("Comment");
        if (it != texts.end()) return parse_webui(it->second);
    }

    // JPEG / WebP: check EXIF UserComment for SD-style parameters
    if (ext == L".jpg" || ext == L".jpeg" || ext == L".webp") {
        auto comment = read_jpeg_comment(path);
        if (!comment.empty()) {
            // Try ComfyUI JSON first
            if (comment.find("\"class_type\"") != std::string::npos)
                return parse_comfyui(comment);
            // Otherwise treat as SD WebUI parameters
            return parse_webui(comment);
        }
    }

    return {};
}

// ── JPEG EXIF reader ─────────────────────────────────────────

// Read EXIF UserComment (tag 0x9286) from a JPEG file
std::string read_jpeg_comment(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";

    // Check SOI
    char soi[2]; f.read(soi, 2);
    if (f.gcount() != 2 || (unsigned char)soi[0] != 0xFF || (unsigned char)soi[1] != 0xD8)
        return "";

    while (f.good()) {
        unsigned char marker[2];
        f.read(reinterpret_cast<char*>(marker), 2);
        if (f.gcount() != 2 || marker[0] != 0xFF) break;
        if (marker[1] == 0xD9) break; // EOI

        // APP1 (EXIF)
        if (marker[1] == 0xE1) {
            unsigned char len_be[2];
            f.read(reinterpret_cast<char*>(len_be), 2);
            if (f.gcount() != 2) break;
            uint16_t seg_len = (len_be[0] << 8) | len_be[1];
            if (seg_len < 8) { f.seekg(seg_len - 2, std::ios::cur); continue; }

            std::vector<char> data(seg_len - 2);
            f.read(data.data(), seg_len - 2);
            if (f.gcount() != seg_len - 2) break;

            // Check "Exif\0\0" header
            if (seg_len < 8 || memcmp(data.data(), "Exif\0\0", 6) != 0) continue;

            // TIFF header: byte order + magic
            size_t off = 6;
            bool big_endian = (data[off] == 'M');
            off += 2; // byte order mark
            uint16_t magic = big_endian ? ((data[off]<<8)|data[off+1]) : (data[off]|(data[off+1]<<8));
            if (magic != 0x002A) continue;
            off += 2;

            // First IFD offset
            uint32_t ifd_off = big_endian
                ? ((uint32_t)data[off]<<24)|(data[off+1]<<16)|(data[off+2]<<8)|data[off+3]
                : (data[off]|(data[off+1]<<8)|(data[off+2]<<16)|(data[off+3]<<24));
            off = ifd_off;

            // Read IFD entry count
            uint16_t count = big_endian ? ((data[off]<<8)|data[off+1]) : (data[off]|(data[off+1]<<8));
            off += 2;

            for (uint16_t i = 0; i < count; ++i) {
                uint16_t tag = big_endian ? ((data[off]<<8)|data[off+1]) : (data[off]|(data[off+1]<<8));
                uint32_t val_or_off = big_endian
                    ? ((uint32_t)data[off+8]<<24)|(data[off+9]<<16)|(data[off+10]<<8)|data[off+11]
                    : (data[off+8]|(data[off+9]<<8)|(data[off+10]<<16)|(data[off+11]<<24));

                if (tag == 0x9286) { // UserComment
                    // type 7 = undefined, type 2 = ASCII
                    // First 8 bytes of the pointer area = character code ("UNICODE\0" or "ASCII\0\0\0\0\0")
                    uint32_t count2 = big_endian
                        ? ((uint32_t)data[off+4]<<24)|(data[off+5]<<16)|(data[off+6]<<8)|data[off+7]
                        : (data[off+4]|(data[off+5]<<8)|(data[off+6]<<16)|(data[off+7]<<24));
                    if (count2 > 8 && val_or_off + count2 <= data.size()) {
                        const char* p = &data[val_or_off + 8]; // skip charset prefix
                        return std::string(p, count2 - 8);
                    }
                    break;
                }
                off += 12;
            }
            break;
        } else {
            unsigned char len_be[2];
            f.read(reinterpret_cast<char*>(len_be), 2);
            if (f.gcount() != 2) break;
            uint16_t len = (len_be[0] << 8) | len_be[1];
            if (len < 2) break;
            f.seekg(len - 2, std::ios::cur);
        }
    }
    return "";
}

} // namespace mv

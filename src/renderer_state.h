#pragma once

#include <cstdint>
#include <winerror.h>

namespace mv {

inline bool should_recreate_render_device(HRESULT result) {
    return FAILED(result);
}

inline bool renderer_generation_changed(uint64_t cached, uint64_t current) {
    return cached != current;
}

} // namespace mv

#include "renderer_state.h"

#include <d2d1.h>
#include <dxgi.h>
#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    expect(!mv::should_recreate_render_device(S_OK),
        "successful rendering must keep the current device");
    expect(mv::should_recreate_render_device(D2DERR_RECREATE_TARGET),
        "D2D target loss must recreate the renderer");
    expect(mv::should_recreate_render_device(DXGI_ERROR_DEVICE_REMOVED),
        "DXGI device removal must recreate the renderer");
    expect(mv::should_recreate_render_device(DXGI_ERROR_DEVICE_RESET),
        "DXGI device reset must recreate the renderer");
    expect(mv::should_recreate_render_device(E_FAIL),
        "unclassified failed Present or Resize results must fail closed");

    expect(mv::renderer_generation_changed(1, 2),
        "App caches must be invalidated after renderer recreation");
    expect(!mv::renderer_generation_changed(2, 2),
        "App caches should remain valid within one renderer generation");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "renderer state tests passed\n";
    return 0;
}

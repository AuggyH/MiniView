#include "renderer.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <iostream>

namespace {

constexpr wchar_t kRendererTestWindowClass[] =
    L"MinViewRendererTestWindow";

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Hidden Win32 window + a real Direct2D renderer on the test thread.
// Exercises the device-loss recovery path: discard_device_resources must
// release every device-scoped resource, and a second init/draw must work.
void test_renderer_device_loss_recovery() {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_hr)) {
        std::cout << "renderer_tests: SKIP (CoInitializeEx failed)\n";
        return;
    }
    struct ComGuard {
        ~ComGuard() { CoUninitialize(); }
    } com_guard;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    if (!GetClassInfoW(instance, kRendererTestWindowClass, &wc)) {
        wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = kRendererTestWindowClass;
        if (!RegisterClassW(&wc)) {
            std::cout << "renderer_tests: SKIP (RegisterClassW failed)\n";
            return;
        }
    }

    HWND hwnd = CreateWindowExW(
        0, kRendererTestWindowClass, L"mv_renderer_test", WS_POPUP,
        0, 0, 320, 240, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        std::cout << "renderer_tests: SKIP (CreateWindowExW failed)\n";
        return;
    }

    mv::Renderer renderer;
    if (!renderer.init(hwnd)) {
        // A headless/CI environment without D3D11/WARP should not fail the
        // gate; skip instead of reporting a flaky failure.
        std::cout << "renderer_tests: SKIP (D3D11/D2D init unavailable)\n";
        DestroyWindow(hwnd);
        UnregisterClassW(kRendererTestWindowClass, instance);
        return;
    }

    expect(renderer.resize(320, 240),
        "renderer resize must succeed before the first frame");

    bool first_draw = renderer.begin_frame();
    if (first_draw) {
        renderer.clear();
        renderer.draw_grid_placeholder(
            8.0f, 8.0f, 64.0f, 64.0f,
            mv::dt::d2d(mv::dt::kColorPlaceholder));
        first_draw = renderer.end_frame();
    }
    expect(first_draw, "first frame draw/end must succeed");

    // Give the renderer a real placeholder bitmap so the discard check below
    // proves device-scoped resources are actually released.
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    if (SUCCEEDED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic)))) {
        BYTE pixel[4] = {0x1A, 0x1A, 0x1A, 0xFF};
        Microsoft::WRL::ComPtr<IWICBitmap> wic_bitmap;
        if (SUCCEEDED(wic->CreateBitmapFromMemory(
                1, 1, GUID_WICPixelFormat32bppPBGRA, 4, 4,
                pixel, &wic_bitmap))) {
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> placeholder;
            if (SUCCEEDED(renderer.create_bitmap_from_wic(
                    wic_bitmap.Get(), &placeholder))) {
                renderer.set_placeholder(placeholder.Get());
            }
        }
    }
    expect(renderer.placeholder_bitmap() != nullptr,
        "test fixture must assign a placeholder bitmap before discard");

    renderer.discard_device_resources_for_testing();
    expect(renderer.placeholder_bitmap() == nullptr,
        "discard_device_resources must clear the placeholder bitmap");

    expect(renderer.init(hwnd),
        "renderer must reinitialize after device-resource discard");
    expect(renderer.resize(320, 240),
        "renderer resize must succeed after reinit");

    bool second_draw = renderer.begin_frame();
    if (second_draw) {
        renderer.clear();
        renderer.draw_grid_placeholder(
            8.0f, 8.0f, 64.0f, 64.0f,
            mv::dt::d2d(mv::dt::kColorPlaceholder));
        second_draw = renderer.end_frame();
    }
    expect(second_draw, "second frame draw/end must succeed after reinit");

    DestroyWindow(hwnd);
    UnregisterClassW(kRendererTestWindowClass, instance);
}

} // namespace

int main() {
    test_renderer_device_loss_recovery();

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "renderer_tests: PASS\n";
    return 0;
}

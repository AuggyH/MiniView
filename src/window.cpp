#include "window.h"
#include <stdexcept>
#include <dwmapi.h>

namespace mv {

constexpr const wchar_t* CLASS_NAME = L"MinViewWindow";

Window::~Window() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool Window::create(const std::wstring& title, int width, int height) {
    HINSTANCE hinst = GetModuleHandle(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(26, 26, 26));  // dark background
    wc.lpszClassName = CLASS_NAME;

    RegisterClassExW(&wc);

    RECT rect = {0, 0, width, height};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE,
        WS_EX_APPWINDOW | WS_EX_WINDOWEDGE);

    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_WINDOWEDGE,
        CLASS_NAME, title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hinst, this);

    if (!m_hwnd) return false;

    // Dark mode title bar (Windows 10 1809+)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
        &dark, sizeof(dark));

    // Rounded corners (Windows 11)
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    // Accept file drops
    DragAcceptFiles(m_hwnd, TRUE);

    return true;
}

int Window::run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void Window::invalidate() {
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

int Window::width() const {
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    return rc.right - rc.left;
}

int Window::height() const {
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    return rc.bottom - rc.top;
}

Window* Window::get_this(HWND hwnd) {
    return reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT Window::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        auto* w  = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
        w->m_hwnd = hwnd;
        return TRUE;
    }

    auto* w = get_this(hwnd);

    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        if (w) w->m_hwnd = nullptr;
        return 0;
    }

    if (w && w->m_callback) {
        LRESULT result = w->m_callback(hwnd, msg, wp, lp);
        if (result != -1) return result;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace mv

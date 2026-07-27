#pragma once
#include <windows.h>
#include <string>
#include <functional>

namespace mv {

inline constexpr wchar_t WINDOW_CLASS_NAME[] = L"MVFrameW32";

/// Thin Win32 window wrapper with DPI awareness.
class Window {
public:
    using MessageCallback = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)>;

    Window() = default;
    ~Window();

    /// Create the window. Returns true on success.
    bool create(const std::wstring& title, int width, int height);

    /// Show and run the message loop. Returns when window closes.
    int  run();

    /// Force a redraw.
    void invalidate();

    HWND handle() const { return m_hwnd; }
    int  width()  const;
    int  height() const;

    /// Set a custom message handler (returns false to use default handling).
    void set_message_callback(MessageCallback cb) { m_callback = std::move(cb); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static Window* get_this(HWND hwnd);

    HWND            m_hwnd = nullptr;
    MessageCallback m_callback;
};

} // namespace mv

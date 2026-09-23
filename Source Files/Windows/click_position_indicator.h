#pragma once

#include "Windows/input_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win {

class ClickPositionIndicator {
public:
    ClickPositionIndicator() = default;
    ~ClickPositionIndicator();

    ClickPositionIndicator(const ClickPositionIndicator&) = delete;
    ClickPositionIndicator& operator=(const ClickPositionIndicator&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance) noexcept;
    void ShowAt(ScreenPoint point, UINT dpi) noexcept;
    void Hide() noexcept;
    void Destroy() noexcept;
    [[nodiscard]] bool IsVisible() const noexcept;

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    [[nodiscard]] bool EnsureSurface(int diameter_pixels) noexcept;
    void RenderSurface() noexcept;
    [[nodiscard]] bool PresentAt(int left, int top) noexcept;

    HINSTANCE instance_{};
    HWND window_{};
    HDC memory_dc_{};
    HBITMAP bitmap_{};
    HGDIOBJ previous_bitmap_{};
    void* pixels_{};
    int diameter_pixels_{};
    bool surface_presented_{};
    bool visible_{};
    ScreenPoint last_point_{};
    UINT last_dpi_{};
};

} // namespace vectorclick::win

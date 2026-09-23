#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win::numeric_field {

struct VisualState {
    HWND hot_edit{};
    int hot_part{};
    HWND pressed_edit{};
    int pressed_part{};
};

[[nodiscard]] RECT ArrowBounds(HWND edit) noexcept;
[[nodiscard]] int ArrowPartAt(HWND edit, POINT point) noexcept;
void InvalidateArrow(HWND edit) noexcept;
void DrawChrome(HWND edit, HDC dc, const VisualState& state) noexcept;
void UpdateFormatting(HWND edit) noexcept;

} // namespace vectorclick::win::numeric_field

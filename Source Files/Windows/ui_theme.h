#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Windows/ui_presentation_state.h"

#include <string_view>

namespace vectorclick::win::ui {

constexpr COLORREF MakeColor(const unsigned char red,
                             const unsigned char green,
                             const unsigned char blue) noexcept {
    return static_cast<COLORREF>(red) |
           (static_cast<COLORREF>(green) << 8U) |
           (static_cast<COLORREF>(blue) << 16U);
}

// Fixed VectorClick interface palette. The slightly blue-black window and
// layered navy surfaces follow the Auto Light visual language without copying
// its exact controls or layout.
inline constexpr COLORREF Window = MakeColor(7, 15, 23);
inline constexpr COLORREF WindowAlt = MakeColor(9, 19, 29);
inline constexpr COLORREF Surface = MakeColor(13, 25, 36);
inline constexpr COLORREF SurfaceAlt = MakeColor(10, 21, 31);
inline constexpr COLORREF SurfaceHover = MakeColor(19, 39, 56);
inline constexpr COLORREF SurfacePressed = MakeColor(14, 58, 105);
inline constexpr COLORREF Border = MakeColor(44, 62, 77);
inline constexpr COLORREF BorderSoft = MakeColor(31, 47, 60);
inline constexpr COLORREF Divider = MakeColor(28, 45, 58);
inline constexpr COLORREF Text = MakeColor(235, 239, 244);
inline constexpr COLORREF Muted = MakeColor(148, 160, 176);
inline constexpr COLORREF Icon = MakeColor(160, 174, 190);
inline constexpr COLORREF Accent = MakeColor(19, 111, 230);
inline constexpr COLORREF AccentHover = MakeColor(47, 139, 255);
inline constexpr COLORREF AccentPressed = MakeColor(14, 82, 174);
inline constexpr COLORREF AccentGradientStart = MakeColor(32, 145, 255);
inline constexpr COLORREF AccentGradientEnd = MakeColor(15, 83, 220);
// The Start action uses its own layered blue surface so its richer light /
// deep transition does not change other controls that share the accent palette.
inline constexpr COLORREF StartSurfaceBase = MakeColor(9, 67, 196);
inline constexpr COLORREF StartSurfaceBaseHover = MakeColor(13, 80, 213);
inline constexpr COLORREF StartSurfaceBloom = MakeColor(58, 166, 255);
inline constexpr COLORREF StartSurfaceBloomHover = MakeColor(80, 181, 255);
inline constexpr COLORREF StartSurfaceBridge = MakeColor(29, 121, 236);
inline constexpr COLORREF StartSurfaceBridgeHover = MakeColor(40, 136, 248);
inline constexpr COLORREF Disabled = MakeColor(78, 89, 103);
inline constexpr COLORREF Danger = MakeColor(244, 74, 82);
inline constexpr COLORREF DangerPressed = MakeColor(106, 31, 39);
inline constexpr COLORREF Success = MakeColor(82, 222, 125);
// Recovery actions use a dark green-tinted surface with a brighter green
// outline. This keeps the action visually distinct without turning the entire
// button into a high-luminance block.
inline constexpr COLORREF CleanupSurface = MakeColor(13, 37, 22);
inline constexpr COLORREF CleanupSurfaceHover = MakeColor(18, 52, 29);
inline constexpr COLORREF CleanupSurfacePressed = MakeColor(20, 68, 32);
inline constexpr COLORREF CleanupAction = MakeColor(103, 191, 65);
inline constexpr COLORREF CleanupActionHover = MakeColor(128, 191, 95);
inline constexpr COLORREF Warning = MakeColor(245, 199, 92);
// Status-only colors are deliberately separate from the warning and danger
// colors used by buttons and other controls. Violet marks transitional safety
// work, while white marks a condition that needs the user's attention.
inline constexpr COLORREF StatusTransition = MakeColor(177, 122, 255);
inline constexpr COLORREF StatusAttention = MakeColor(245, 247, 250);
// A deliberately subdued status lamp for settings that are not currently
// runnable. It remains visible against the navy surface without implying an
// error, warning, or active state.
inline constexpr COLORREF StatusInactive = MakeColor(48, 58, 67);

// Shared custom-scrollbar palette and emphasis rules. The Advanced page,
// application-owned combo popups, and other Vector Click-owned scrollbars use
// these exact states so hover / drag feedback stays visually identical instead
// of drifting into per-control copies.
inline constexpr COLORREF ScrollbarTrack = MakeColor(22, 26, 33);
inline constexpr COLORREF ScrollbarIdle = MakeColor(55, 63, 76);
inline constexpr COLORREF ScrollbarHover = MakeColor(22, 112, 219);
inline constexpr COLORREF ScrollbarActive = MakeColor(35, 129, 239);

enum class ScrollbarVisualState {
    Idle,
    Hover,
    Active,
};

[[nodiscard]] constexpr ScrollbarVisualState ScrollbarState(
    const bool hovered,
    const bool active) noexcept {
    return active ? ScrollbarVisualState::Active
                  : hovered ? ScrollbarVisualState::Hover
                            : ScrollbarVisualState::Idle;
}

[[nodiscard]] constexpr COLORREF ScrollbarThumbColor(
    const ScrollbarVisualState state) noexcept {
    switch (state) {
    case ScrollbarVisualState::Active:
        return ScrollbarActive;
    case ScrollbarVisualState::Hover:
        return ScrollbarHover;
    case ScrollbarVisualState::Idle:
    default:
        return ScrollbarIdle;
    }
}

[[nodiscard]] constexpr int ScrollbarThumbLogicalWidth(
    const ScrollbarVisualState state) noexcept {
    return state == ScrollbarVisualState::Idle ? 6 : 7;
}

enum class Glyph {
    None,
    Mouse,
    Crosshair,
    Repeat,
    Keyboard,
    Clock,
    Action,
    Spacing,
    MouseClick,
    KeyPress,
    Gauge,
    NotificationIndicator,
    List,
    WindowTarget,
    Shield,
    Gear,
    Play,
    Stop,
    Emergency,
    Capture,
    Administrator,
    ChevronUp,
    ChevronDown,
    Check,
    Dot,
    Import,
};

[[nodiscard]] int Scale(int value, UINT dpi) noexcept;
void ApplyDarkTitleBar(HWND window) noexcept;
void ApplyDarkControlTheme(HWND control) noexcept;
void Fill(HDC dc, const RECT& rectangle, COLORREF color) noexcept;
void FillOutsideRoundedPanel(HDC dc,
                             const RECT& rectangle,
                             COLORREF color,
                             int radius,
                             int border_width = 1) noexcept;
void DrawRoundedPanel(HDC dc,
                      const RECT& rectangle,
                      COLORREF fill,
                      COLORREF border,
                      int radius,
                      int border_width = 1) noexcept;
void DrawDockedTabPanel(HDC dc,
                        const RECT& rectangle,
                        COLORREF fill,
                        COLORREF border,
                        int top_radius,
                        int bottom_radius,
                        int border_width = 1) noexcept;
void DrawDockedPageShell(HDC dc,
                         const RECT& rectangle,
                         COLORREF fill,
                         COLORREF border,
                         int radius,
                         int dock_left,
                         int dock_right,
                         int border_width = 1) noexcept;
void DrawStartButtonSurface(HDC dc,
                            const RECT& rectangle,
                            COLORREF border,
                            int radius,
                            bool hover,
                            int border_width = 1) noexcept;
void DrawRoundedOutline(HDC dc,
                        const RECT& rectangle,
                        COLORREF color,
                        int radius,
                        int border_width = 1) noexcept;
void DrawTextLine(HDC dc,
                  std::wstring_view text,
                  RECT rectangle,
                  HFONT font,
                  COLORREF color,
                  UINT format) noexcept;
void DrawSmoothRadioMark(HDC dc,
                         const RECT& rectangle,
                         COLORREF fill,
                         COLORREF border,
                         COLORREF indicator,
                         bool checked,
                         int border_width = 1) noexcept;
void DrawStatusIndicator(HDC dc,
                         const RECT& panel,
                         UINT dpi,
                         COLORREF color) noexcept;
void DrawFocusOutline(HDC dc, const RECT& rectangle, COLORREF color, int inset = 3) noexcept;
void DrawGlyph(HDC dc,
               Glyph glyph,
               const RECT& rectangle,
               COLORREF color,
               int stroke_width = 1) noexcept;
void DrawWarningIcon(HDC dc, const RECT& rectangle) noexcept;
void DrawInformationIcon(HDC dc, const RECT& rectangle) noexcept;
[[nodiscard]] COLORREF StatusIndicatorColor(StatusCategory category) noexcept;

} // namespace vectorclick::win::ui

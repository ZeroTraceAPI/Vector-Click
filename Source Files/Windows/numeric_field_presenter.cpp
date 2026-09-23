#include "Windows/numeric_field_presenter.h"

#include "Windows/ui_theme.h"

#include <algorithm>

namespace vectorclick::win::numeric_field {
namespace {

constexpr int ArrowWidthLogical = 24;

} // namespace

RECT ArrowBounds(const HWND edit) noexcept {
    RECT bounds{};
    if (edit == nullptr || GetClientRect(edit, &bounds) == FALSE) {
        return RECT{};
    }
    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(edit));
    const int arrow_width = ui::Scale(ArrowWidthLogical, control_dpi);
    bounds.left = std::max(bounds.left, bounds.right - arrow_width);
    return bounds;
}

int ArrowPartAt(const HWND edit, const POINT point) noexcept {
    const RECT arrows = ArrowBounds(edit);
    if (IsRectEmpty(&arrows) || PtInRect(&arrows, point) == FALSE) {
        return 0;
    }
    const int center_y = arrows.top + ((arrows.bottom - arrows.top) / 2);
    return point.y < center_y ? 1 : 2;
}

void InvalidateArrow(const HWND edit) noexcept {
    if (edit == nullptr || IsWindow(edit) == FALSE) {
        return;
    }
    RECT arrows = ArrowBounds(edit);
    if (!IsRectEmpty(&arrows)) {
        InvalidateRect(edit, &arrows, FALSE);
    }
}

void DrawChrome(const HWND edit,
                const HDC dc,
                const VisualState& state) noexcept {
    if (dc == nullptr || edit == nullptr || IsWindow(edit) == FALSE) {
        return;
    }

    RECT bounds{};
    if (GetClientRect(edit, &bounds) == FALSE || IsRectEmpty(&bounds)) {
        return;
    }

    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(edit));
    const bool enabled = IsWindowEnabled(edit) != FALSE;
    const bool focused = GetFocus() == edit;
    const int corner_radius = ui::Scale(8, control_dpi);
    const int border_width = std::max(1, ui::Scale(1, control_dpi));

    RECT arrow_bounds = ArrowBounds(edit);

    // Preserve the established paint order and HWND ownership exactly.
    // The reusable presenter contains geometry and chrome rendering without
    // adding a host window, reparenting the edit, or changing z-order.
    ui::FillOutsideRoundedPanel(dc,
                                bounds,
                                ui::Surface,
                                corner_radius,
                                border_width);

    RECT inner_clip = bounds;
    InflateRect(&inner_clip, -border_width, -border_width);
    const int inner_radius = std::max(1, corner_radius - border_width);
    const HRGN rounded_clip = CreateRoundRectRgn(
        inner_clip.left,
        inner_clip.top,
        inner_clip.right + 1,
        inner_clip.bottom + 1,
        std::max(2, inner_radius * 2),
        std::max(2, inner_radius * 2));
    const int saved_dc = SaveDC(dc);
    if (rounded_clip != nullptr) {
        ExtSelectClipRgn(dc, rounded_clip, RGN_AND);
        DeleteObject(rounded_clip);
    }

    ui::Fill(dc, arrow_bounds, ui::SurfaceAlt);

    const int center_y = bounds.top + ((bounds.bottom - bounds.top) / 2);
    RECT upper_hover = arrow_bounds;
    upper_hover.left += ui::Scale(2, control_dpi);
    upper_hover.top += ui::Scale(2, control_dpi);
    upper_hover.right -= ui::Scale(2, control_dpi);
    upper_hover.bottom = center_y;
    RECT lower_hover = arrow_bounds;
    lower_hover.left += ui::Scale(2, control_dpi);
    lower_hover.top = center_y;
    lower_hover.right -= ui::Scale(2, control_dpi);
    lower_hover.bottom -= ui::Scale(2, control_dpi);

    if (enabled) {
        const bool pressed_here = state.pressed_edit == edit;
        const bool hot_here = state.hot_edit == edit;
        if (pressed_here && state.pressed_part == 1) {
            ui::Fill(dc, upper_hover, ui::SurfacePressed);
        } else if (hot_here && state.hot_part == 1) {
            ui::Fill(dc, upper_hover, ui::SurfaceHover);
        }
        if (pressed_here && state.pressed_part == 2) {
            ui::Fill(dc, lower_hover, ui::SurfacePressed);
        } else if (hot_here && state.hot_part == 2) {
            ui::Fill(dc, lower_hover, ui::SurfaceHover);
        }
    }

    const HPEN divider_pen = CreatePen(
        PS_SOLID,
        std::max(1, ui::Scale(1, control_dpi)),
        enabled ? ui::BorderSoft : ui::Divider);
    if (divider_pen != nullptr) {
        const HGDIOBJ old_pen = SelectObject(dc, divider_pen);
        MoveToEx(dc,
                 arrow_bounds.left,
                 bounds.top + ui::Scale(4, control_dpi),
                 nullptr);
        LineTo(dc,
               arrow_bounds.left,
               bounds.bottom - ui::Scale(4, control_dpi));
        SelectObject(dc, old_pen);
        DeleteObject(divider_pen);
    }

    // Give the numeric-field chevrons a final small size increase without
    // crowding the button column. The larger drawing bounds preserve the
    // clearer center tip, while the additional inward shift reduces the gap
    // between the upper and lower icons without changing their hit regions.
    const int glyph_width = ui::Scale(12, control_dpi);
    const int glyph_height = ui::Scale(10, control_dpi);
    const int center_x = arrow_bounds.left +
                         ((arrow_bounds.right - arrow_bounds.left) / 2);
    const int arrow_height = arrow_bounds.bottom - arrow_bounds.top;
    const int icon_gap_adjustment = 3;
    const int upper_center_y = arrow_bounds.top + (arrow_height / 4) +
                               icon_gap_adjustment;
    const int lower_center_y = arrow_bounds.top + ((arrow_height * 3) / 4) -
                               icon_gap_adjustment;
    RECT up{
        center_x - (glyph_width / 2),
        upper_center_y - (glyph_height / 2),
        center_x + ((glyph_width + 1) / 2),
        upper_center_y + ((glyph_height + 1) / 2),
    };
    RECT down{
        center_x - (glyph_width / 2),
        lower_center_y - (glyph_height / 2),
        center_x + ((glyph_width + 1) / 2),
        lower_center_y + ((glyph_height + 1) / 2),
    };
    const COLORREF arrow_color = enabled ? ui::Icon : ui::Disabled;
    ui::DrawGlyph(dc,
                  ui::Glyph::ChevronUp,
                  up,
                  arrow_color,
                  std::max(1, ui::Scale(1, control_dpi)));
    ui::DrawGlyph(dc,
                  ui::Glyph::ChevronDown,
                  down,
                  arrow_color,
                  std::max(1, ui::Scale(1, control_dpi)));

    if (saved_dc != 0) {
        RestoreDC(dc, saved_dc);
    }

    ui::DrawRoundedOutline(dc,
                           bounds,
                           enabled && focused ? ui::AccentHover
                                              : enabled ? ui::Border
                                                        : ui::BorderSoft,
                           corner_radius,
                           border_width);
}

void UpdateFormatting(const HWND edit) noexcept {
    if (edit == nullptr || IsWindow(edit) == FALSE) {
        return;
    }

    RECT client{};
    if (GetClientRect(edit, &client) == FALSE || IsRectEmpty(&client)) {
        return;
    }

    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(edit));
    const int horizontal_padding = ui::Scale(9, control_dpi);
    const int arrow_width = ui::Scale(ArrowWidthLogical, control_dpi);

    RECT format = client;
    format.left += horizontal_padding;
    format.right = std::max(format.left + 1,
                            format.right - arrow_width -
                                ui::Scale(5, control_dpi));

    int text_height = 0;
    const HDC dc = GetDC(edit);
    if (dc != nullptr) {
        const HFONT edit_font = reinterpret_cast<HFONT>(
            SendMessageW(edit, WM_GETFONT, 0, 0));
        const HGDIOBJ old_font = edit_font != nullptr
                                      ? SelectObject(dc, edit_font)
                                      : nullptr;
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics) != FALSE) {
            text_height = metrics.tmHeight;
        }
        if (old_font != nullptr) {
            SelectObject(dc, old_font);
        }
        ReleaseDC(edit, dc);
    }

    const int client_height = client.bottom - client.top;
    if (text_height <= 0 || text_height > client_height) {
        text_height = std::max(1,
                               client_height - ui::Scale(4, control_dpi));
    }
    const int upward_adjustment = ui::Scale(1, control_dpi);
    format.top = client.top + std::max(
        0, ((client_height - text_height) / 2) - upward_adjustment);
    format.bottom = std::min(client.bottom,
                             format.top + text_height +
                                 ui::Scale(1, control_dpi));
    SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&format));
}

} // namespace vectorclick::win::numeric_field

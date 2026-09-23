#include "Windows/ui_theme.h"

#include <dwmapi.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace vectorclick::win::ui {
namespace {

class GdiPlusRuntime final {
public:
    GdiPlusRuntime() noexcept {
        Gdiplus::GdiplusStartupInput input;
        ready_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
    }

    ~GdiPlusRuntime() {
        if (ready_) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    GdiPlusRuntime(const GdiPlusRuntime&) = delete;
    GdiPlusRuntime& operator=(const GdiPlusRuntime&) = delete;

    [[nodiscard]] bool Ready() const noexcept { return ready_; }

private:
    ULONG_PTR token_{};
    bool ready_{};
};

GdiPlusRuntime& GraphicsRuntime() noexcept {
    static GdiPlusRuntime runtime;
    return runtime;
}

Gdiplus::Color ToGdiPlusColor(const COLORREF color,
                              const BYTE alpha = 255) noexcept {
    return Gdiplus::Color(alpha,
                          GetRValue(color),
                          GetGValue(color),
                          GetBValue(color));
}

void ConfigureGraphics(Gdiplus::Graphics& graphics) noexcept {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
}

void AddRoundedRectangle(Gdiplus::GraphicsPath& path,
                         const Gdiplus::RectF& rectangle,
                         const Gdiplus::REAL radius) noexcept {
    const Gdiplus::REAL maximum_radius =
        std::max<Gdiplus::REAL>(0.0F,
                                std::min(rectangle.Width, rectangle.Height) / 2.0F);
    const Gdiplus::REAL actual_radius =
        std::clamp(radius, 0.0F, maximum_radius);
    if (actual_radius <= 0.5F) {
        path.AddRectangle(rectangle);
        return;
    }

    const Gdiplus::REAL diameter = actual_radius * 2.0F;
    path.AddArc(rectangle.X,
                rectangle.Y,
                diameter,
                diameter,
                180.0F,
                90.0F);
    path.AddArc(rectangle.GetRight() - diameter,
                rectangle.Y,
                diameter,
                diameter,
                270.0F,
                90.0F);
    path.AddArc(rectangle.GetRight() - diameter,
                rectangle.GetBottom() - diameter,
                diameter,
                diameter,
                0.0F,
                90.0F);
    path.AddArc(rectangle.X,
                rectangle.GetBottom() - diameter,
                diameter,
                diameter,
                90.0F,
                90.0F);
    path.CloseFigure();
}

void AddCompactShieldOutline(Gdiplus::GraphicsPath& path,
                             const Gdiplus::RectF& bounds) noexcept {
    const Gdiplus::REAL left = bounds.X;
    const Gdiplus::REAL top = bounds.Y;
    const Gdiplus::REAL width = bounds.Width;
    const Gdiplus::REAL height = bounds.Height;
    const Gdiplus::REAL cx = left + (width * 0.50F);

    path.StartFigure();
    path.AddBezier(
        cx,
        top + (height * 0.080F),
        left + (width * 0.610F),
        top + (height * 0.120F),
        left + (width * 0.720F),
        top + (height * 0.160F),
        left + (width * 0.840F),
        top + (height * 0.200F));
    path.AddBezier(
        left + (width * 0.840F),
        top + (height * 0.200F),
        left + (width * 0.840F),
        top + (height * 0.460F),
        left + (width * 0.790F),
        top + (height * 0.670F),
        left + (width * 0.660F),
        top + (height * 0.800F));
    path.AddBezier(
        left + (width * 0.660F),
        top + (height * 0.800F),
        left + (width * 0.600F),
        top + (height * 0.860F),
        left + (width * 0.560F),
        top + (height * 0.920F),
        cx,
        top + (height * 0.920F));
    path.AddBezier(
        cx,
        top + (height * 0.920F),
        left + (width * 0.440F),
        top + (height * 0.920F),
        left + (width * 0.400F),
        top + (height * 0.860F),
        left + (width * 0.340F),
        top + (height * 0.800F));
    path.AddBezier(
        left + (width * 0.340F),
        top + (height * 0.800F),
        left + (width * 0.210F),
        top + (height * 0.670F),
        left + (width * 0.160F),
        top + (height * 0.460F),
        left + (width * 0.160F),
        top + (height * 0.200F));
    path.AddBezier(
        left + (width * 0.160F),
        top + (height * 0.200F),
        left + (width * 0.280F),
        top + (height * 0.160F),
        left + (width * 0.390F),
        top + (height * 0.120F),
        cx,
        top + (height * 0.080F));
    path.CloseFigure();
}

void AddDockedTabRectangle(Gdiplus::GraphicsPath& path,
                           const Gdiplus::RectF& rectangle,
                           const Gdiplus::REAL top_radius,
                           const Gdiplus::REAL bottom_radius) noexcept {
    const Gdiplus::REAL maximum_radius =
        std::max<Gdiplus::REAL>(0.0F,
                                std::min(rectangle.Width, rectangle.Height) / 2.0F);
    const Gdiplus::REAL actual_top_radius =
        std::clamp(top_radius, 0.0F, maximum_radius);
    const Gdiplus::REAL actual_bottom_radius =
        std::clamp(bottom_radius, 0.0F, maximum_radius);

    const Gdiplus::REAL left = rectangle.X;
    const Gdiplus::REAL top = rectangle.Y;
    const Gdiplus::REAL right = rectangle.GetRight();
    const Gdiplus::REAL bottom = rectangle.GetBottom();

    path.StartFigure();
    path.AddLine(left, bottom - actual_bottom_radius,
                 left, top + actual_top_radius);
    if (actual_top_radius > 0.5F) {
        const Gdiplus::REAL diameter = actual_top_radius * 2.0F;
        path.AddArc(left, top, diameter, diameter, 180.0F, 90.0F);
        path.AddLine(left + actual_top_radius, top,
                     right - actual_top_radius, top);
        path.AddArc(right - diameter,
                    top,
                    diameter,
                    diameter,
                    270.0F,
                    90.0F);
    } else {
        path.AddLine(left, top, right, top);
    }
    path.AddLine(right, top + actual_top_radius,
                 right, bottom - actual_bottom_radius);
    if (actual_bottom_radius > 0.5F) {
        const Gdiplus::REAL diameter = actual_bottom_radius * 2.0F;
        path.AddArc(right - diameter,
                    bottom - diameter,
                    diameter,
                    diameter,
                    0.0F,
                    90.0F);
        path.AddLine(right - actual_bottom_radius, bottom,
                     left + actual_bottom_radius, bottom);
        path.AddArc(left,
                    bottom - diameter,
                    diameter,
                    diameter,
                    90.0F,
                    90.0F);
    } else {
        path.AddLine(right, bottom, left, bottom);
    }
    path.CloseFigure();
}

template <std::size_t Count>
void AddRoundedPolygon(Gdiplus::GraphicsPath& path,
                       const std::array<Gdiplus::PointF, Count>& points,
                       const Gdiplus::REAL radius) noexcept {
    static_assert(Count >= 3);

    std::array<Gdiplus::PointF, Count> entries{};
    std::array<Gdiplus::PointF, Count> exits{};
    for (std::size_t index = 0; index < Count; ++index) {
        const Gdiplus::PointF& previous = points[(index + Count - 1U) % Count];
        const Gdiplus::PointF& current = points[index];
        const Gdiplus::PointF& next = points[(index + 1U) % Count];

        const Gdiplus::REAL previous_dx = previous.X - current.X;
        const Gdiplus::REAL previous_dy = previous.Y - current.Y;
        const Gdiplus::REAL next_dx = next.X - current.X;
        const Gdiplus::REAL next_dy = next.Y - current.Y;
        const Gdiplus::REAL previous_length =
            std::sqrt((previous_dx * previous_dx) + (previous_dy * previous_dy));
        const Gdiplus::REAL next_length =
            std::sqrt((next_dx * next_dx) + (next_dy * next_dy));

        if (previous_length <= 0.001F || next_length <= 0.001F) {
            entries[index] = current;
            exits[index] = current;
            continue;
        }

        const Gdiplus::REAL corner = std::min(
            radius,
            std::min(previous_length * 0.34F, next_length * 0.34F));
        entries[index] = Gdiplus::PointF(
            current.X + ((previous_dx / previous_length) * corner),
            current.Y + ((previous_dy / previous_length) * corner));
        exits[index] = Gdiplus::PointF(
            current.X + ((next_dx / next_length) * corner),
            current.Y + ((next_dy / next_length) * corner));
    }

    path.StartFigure();
    path.AddLine(exits[Count - 1U], entries[0]);
    for (std::size_t index = 0; index < Count; ++index) {
        const Gdiplus::PointF& current = points[index];
        path.AddBezier(entries[index], current, current, exits[index]);
        const std::size_t next_index = (index + 1U) % Count;
        path.AddLine(exits[index], entries[next_index]);
    }
    path.CloseFigure();
}

Gdiplus::REAL EffectiveRoundedRadius(const Gdiplus::RectF& bounds,
                                      const int radius) noexcept {
    return std::clamp(
        static_cast<Gdiplus::REAL>(std::max(1, radius)),
        0.0F,
        std::min(bounds.Width, bounds.Height) / 2.0F);
}

void FillOpaqueRoundedInterior(const HDC dc,
                               const Gdiplus::RectF& bounds,
                               const Gdiplus::REAL radius,
                               const COLORREF color) noexcept {
    if (dc == nullptr || bounds.Width <= 0.0F || bounds.Height <= 0.0F) {
        return;
    }

    const int left = static_cast<int>(std::ceil(bounds.X));
    const int top = static_cast<int>(std::ceil(bounds.Y));
    const int right = static_cast<int>(std::floor(bounds.GetRight()));
    const int bottom = static_cast<int>(std::floor(bounds.GetBottom()));
    if (right <= left || bottom <= top) {
        return;
    }

    const int corner = std::max(1, static_cast<int>(std::ceil(radius)));
    const HBRUSH brush = CreateSolidBrush(color);
    if (brush == nullptr) {
        return;
    }

    // A rounded rectangle is guaranteed to contain these two intersecting
    // center bands. Fill their large opaque area with GDI and reserve GDI+
    // only for the antialiased perimeter. The edge pass overlaps these bands,
    // so integer rounding cannot leave a seam.
    RECT horizontal{left,
                    std::min(bottom, top + corner),
                    right,
                    std::max(top, bottom - corner)};
    if (horizontal.bottom > horizontal.top) {
        FillRect(dc, &horizontal, brush);
    }

    RECT vertical{std::min(right, left + corner),
                  top,
                  std::max(left, right - corner),
                  bottom};
    if (vertical.right > vertical.left) {
        FillRect(dc, &vertical, brush);
    }

    DeleteObject(brush);
}

void FillRoundedPerimeter(const Gdiplus::GraphicsPath& path,
                          Gdiplus::Graphics& graphics,
                          const Gdiplus::RectF& bounds,
                          const Gdiplus::REAL radius,
                          const COLORREF color) noexcept {
    if (bounds.Width <= 0.0F || bounds.Height <= 0.0F) {
        return;
    }

    // Restrict the high-quality GDI+ solid fill to a perimeter band. Filling
    // the entire opaque interior with GDI+ was the dominant cost for large
    // cards and the docked page shell at high DPI / maximized sizes.
    const Gdiplus::REAL edge = std::min<Gdiplus::REAL>(
        std::max<Gdiplus::REAL>(2.0F, radius + 2.0F),
        std::max(bounds.Width, bounds.Height));
    const Gdiplus::REAL horizontal_edge =
        std::min(bounds.Height, edge);
    const Gdiplus::REAL vertical_edge =
        std::min(bounds.Width, edge);

    Gdiplus::Region perimeter(
        Gdiplus::RectF(bounds.X, bounds.Y, bounds.Width, horizontal_edge));
    perimeter.Union(Gdiplus::RectF(bounds.X,
                                   bounds.GetBottom() - horizontal_edge,
                                   bounds.Width,
                                   horizontal_edge));
    perimeter.Union(Gdiplus::RectF(bounds.X,
                                   bounds.Y,
                                   vertical_edge,
                                   bounds.Height));
    perimeter.Union(Gdiplus::RectF(bounds.GetRight() - vertical_edge,
                                   bounds.Y,
                                   vertical_edge,
                                   bounds.Height));

    const Gdiplus::GraphicsState state = graphics.Save();
    graphics.SetClip(&perimeter, Gdiplus::CombineModeIntersect);
    Gdiplus::SolidBrush brush(ToGdiPlusColor(color));
    graphics.FillPath(&brush, &path);
    graphics.Restore(state);
}

bool DrawSmoothRoundedPanel(const HDC dc,
                            const RECT& rectangle,
                            const COLORREF fill,
                            const COLORREF border,
                            const int radius,
                            const int border_width) noexcept {
    if (!GraphicsRuntime().Ready()) {
        return false;
    }

    const Gdiplus::REAL stroke =
        static_cast<Gdiplus::REAL>(std::max(1, border_width));
    // Keep the anti-aliased stroke fully inside the control's paint rectangle.
    // Drawing exactly on the outer half-pixel can clip the right and bottom
    // edges, especially when a card touches the edge of its page host.
    const Gdiplus::REAL inset = (stroke / 2.0F) + 0.5F;
    const Gdiplus::REAL width =
        static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left) - (inset * 2.0F);
    const Gdiplus::REAL height =
        static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top) - (inset * 2.0F);
    if (width <= 0.0F || height <= 0.0F) {
        return true;
    }

    const Gdiplus::RectF bounds(
        static_cast<Gdiplus::REAL>(rectangle.left) + inset,
        static_cast<Gdiplus::REAL>(rectangle.top) + inset,
        width,
        height);
    const Gdiplus::REAL actual_radius = EffectiveRoundedRadius(bounds, radius);
    Gdiplus::GraphicsPath path;
    AddRoundedRectangle(path, bounds, actual_radius);

    FillOpaqueRoundedInterior(dc, bounds, actual_radius, fill);

    Gdiplus::Graphics graphics(dc);
    ConfigureGraphics(graphics);
    FillRoundedPerimeter(path, graphics, bounds, actual_radius, fill);

    Gdiplus::Pen border_pen(ToGdiPlusColor(border), stroke);
    border_pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&border_pen, &path);
    return true;
}

bool DrawSmoothStartButtonSurface(const HDC dc,
                                  const RECT& rectangle,
                                  const COLORREF border,
                                  const int radius,
                                  const bool hover,
                                  const int border_width) noexcept {
    if (!GraphicsRuntime().Ready()) {
        return false;
    }

    Gdiplus::Graphics graphics(dc);
    ConfigureGraphics(graphics);

    const Gdiplus::REAL stroke =
        static_cast<Gdiplus::REAL>(std::max(1, border_width));
    const Gdiplus::REAL inset = (stroke / 2.0F) + 0.5F;
    const Gdiplus::REAL width =
        static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left) - (inset * 2.0F);
    const Gdiplus::REAL height =
        static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top) - (inset * 2.0F);
    if (width <= 0.0F || height <= 0.0F) {
        return true;
    }

    const Gdiplus::RectF bounds(
        static_cast<Gdiplus::REAL>(rectangle.left) + inset,
        static_cast<Gdiplus::REAL>(rectangle.top) + inset,
        width,
        height);
    Gdiplus::GraphicsPath panel_path;
    AddRoundedRectangle(panel_path,
                        bounds,
                        static_cast<Gdiplus::REAL>(std::max(1, radius)));

    const COLORREF base_color = hover ? StartSurfaceBaseHover : StartSurfaceBase;
    const COLORREF bloom_color = hover ? StartSurfaceBloomHover : StartSurfaceBloom;
    const COLORREF bridge_color = hover ? StartSurfaceBridgeHover : StartSurfaceBridge;

    Gdiplus::SolidBrush base_brush(ToGdiPlusColor(base_color));
    graphics.FillPath(&base_brush, &panel_path);

    const Gdiplus::GraphicsState clip_state = graphics.Save();
    graphics.SetClip(&panel_path, Gdiplus::CombineModeIntersect);

    // A broad light-blue bloom supplies the familiar bright left side. Its
    // reach is intentionally limited so the deep-blue base owns more of the
    // right side without introducing a hard hand-off line.
    {
        Gdiplus::GraphicsPath bloom_path;
        const Gdiplus::RectF bloom_bounds(
            bounds.X - (bounds.Width * 0.29F),
            bounds.Y - (bounds.Height * 0.40F),
            bounds.Width * 1.12F,
            bounds.Height * 1.78F);
        bloom_path.AddEllipse(bloom_bounds);
        Gdiplus::PathGradientBrush bloom_brush(&bloom_path);
        bloom_brush.SetCenterPoint(
            Gdiplus::PointF(bounds.X + (bounds.Width * 0.14F),
                            bounds.Y + (bounds.Height * 0.48F)));
        bloom_brush.SetCenterColor(
            ToGdiPlusColor(bloom_color, hover ? static_cast<BYTE>(216)
                                              : static_cast<BYTE>(204)));
        Gdiplus::Color surround[] = {ToGdiPlusColor(base_color, 0)};
        INT surround_count = 1;
        bloom_brush.SetSurroundColors(surround, &surround_count);
        bloom_brush.SetFocusScales(0.17F, 0.56F);
        graphics.FillPath(&bloom_brush, &bloom_path);
    }

    // The middle-blue bridge is intentionally faint. It smooths the overlap
    // between the light bloom and deep base while preserving a clearly darker
    // right side.
    {
        Gdiplus::GraphicsPath bridge_path;
        const Gdiplus::RectF bridge_bounds(
            bounds.X + (bounds.Width * 0.10F),
            bounds.Y - (bounds.Height * 0.50F),
            bounds.Width * 0.94F,
            bounds.Height * 1.98F);
        bridge_path.AddEllipse(bridge_bounds);
        Gdiplus::PathGradientBrush bridge_brush(&bridge_path);
        bridge_brush.SetCenterPoint(
            Gdiplus::PointF(bounds.X + (bounds.Width * 0.40F),
                            bounds.Y + (bounds.Height * 0.50F)));
        bridge_brush.SetCenterColor(
            ToGdiPlusColor(bridge_color, hover ? static_cast<BYTE>(58)
                                               : static_cast<BYTE>(48)));
        Gdiplus::Color surround[] = {ToGdiPlusColor(base_color, 0)};
        INT surround_count = 1;
        bridge_brush.SetSurroundColors(surround, &surround_count);
        bridge_brush.SetFocusScales(0.11F, 0.60F);
        graphics.FillPath(&bridge_brush, &bridge_path);
    }

    // Avoid a full-width vertical sheen. Even a low-opacity linear pass can
    // create a straight horizontal contour that becomes conspicuous over the
    // darker right side. This large clipped ellipse keeps a restrained upper
    // highlight while making its falloff curved instead of one level line.
    {
        Gdiplus::GraphicsPath highlight_path;
        const Gdiplus::RectF highlight_bounds(
            bounds.X - (bounds.Width * 0.08F),
            bounds.Y - (bounds.Height * 0.92F),
            bounds.Width * 1.16F,
            bounds.Height * 1.28F);
        highlight_path.AddEllipse(highlight_bounds);
        Gdiplus::PathGradientBrush highlight_brush(&highlight_path);
        highlight_brush.SetCenterPoint(
            Gdiplus::PointF(bounds.X + (bounds.Width * 0.42F),
                            bounds.Y - (bounds.Height * 0.12F)));
        highlight_brush.SetCenterColor(
            Gdiplus::Color(hover ? static_cast<BYTE>(24)
                                 : static_cast<BYTE>(18),
                            255,
                            255,
                            255));
        Gdiplus::Color surround[] = {Gdiplus::Color(0, 255, 255, 255)};
        INT surround_count = 1;
        highlight_brush.SetSurroundColors(surround, &surround_count);
        highlight_brush.SetFocusScales(0.20F, 0.36F);
        graphics.FillPath(&highlight_brush, &highlight_path);
    }

    graphics.Restore(clip_state);

    Gdiplus::Pen border_pen(ToGdiPlusColor(border), stroke);
    border_pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&border_pen, &panel_path);
    return true;
}

bool DrawSmoothGlyph(const HDC dc,
                     const Glyph glyph,
                     const RECT& rectangle,
                     const COLORREF color,
                     const int stroke_width) noexcept {
    if (!GraphicsRuntime().Ready()) {
        return false;
    }

    const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(rectangle.left);
    const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(rectangle.top);
    const Gdiplus::REAL width =
        static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left);
    const Gdiplus::REAL height =
        static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top);
    if (width <= 1.0F || height <= 1.0F) {
        return true;
    }

    const Gdiplus::REAL right = left + width;
    const Gdiplus::REAL bottom = top + height;
    const Gdiplus::REAL cx = left + (width / 2.0F);
    const Gdiplus::REAL cy = top + (height / 2.0F);
    const Gdiplus::REAL base_stroke =
        static_cast<Gdiplus::REAL>(std::max(1, stroke_width));
    const Gdiplus::REAL stroke =
        glyph == Glyph::Emergency
            ? base_stroke * 1.52F
            : (glyph == Glyph::Keyboard || glyph == Glyph::MouseClick ||
               glyph == Glyph::KeyPress)
                  ? base_stroke * 1.10F
                  : base_stroke;

    Gdiplus::Graphics graphics(dc);
    ConfigureGraphics(graphics);
    Gdiplus::Pen pen(ToGdiPlusColor(color), stroke);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetDashCap(Gdiplus::DashCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush brush(ToGdiPlusColor(color));

    const auto line = [&graphics, &pen](const Gdiplus::REAL x1,
                                        const Gdiplus::REAL y1,
                                        const Gdiplus::REAL x2,
                                        const Gdiplus::REAL y2) {
        graphics.DrawLine(&pen, x1, y1, x2, y2);
    };

    switch (glyph) {
    case Glyph::Mouse: {
        const Gdiplus::RectF body(left + (width * 0.22F),
                                  top + 0.7F,
                                  width * 0.56F,
                                  height - 1.4F);
        Gdiplus::GraphicsPath path;
        AddRoundedRectangle(path,
                            body,
                            std::min(body.Width * 0.46F, body.Height * 0.30F));
        graphics.DrawPath(&pen, &path);

        const Gdiplus::REAL divider_y = body.Y + (body.Height * 0.37F);
        line(body.X + 0.9F,
             divider_y,
             body.GetRight() - 0.9F,
             divider_y);
        line(cx,
             body.Y + 0.8F,
             cx,
             divider_y - 0.5F);
        break;
    }
    case Glyph::Crosshair:
    case Glyph::Capture: {
        const Gdiplus::REAL radius = std::min(width, height) * 0.29F;
        graphics.DrawEllipse(&pen,
                             cx - radius,
                             cy - radius,
                             radius * 2.0F,
                             radius * 2.0F);
        const Gdiplus::REAL gap = radius + (stroke * 1.4F);
        line(cx, top + 0.5F, cx, cy - gap);
        line(cx, cy + gap, cx, bottom - 0.5F);
        line(left + 0.5F, cy, cx - gap, cy);
        line(cx + gap, cy, right - 0.5F, cy);
        if (glyph == Glyph::Capture) {
            const Gdiplus::REAL dot = std::max(2.0F, stroke * 1.5F);
            graphics.FillEllipse(&brush,
                                 cx - dot,
                                 cy - dot,
                                 dot * 2.0F,
                                 dot * 2.0F);
        }
        break;
    }
    case Glyph::Repeat: {
        // Match the approved concept with two long circular arcs and compact
        // rounded L-shaped heads. The heads use horizontal and vertical wings,
        // rather than tangent-aligned chevrons, so the symbol keeps the same
        // recognizable geometry at both header and cleanup-button sizes.
        const Gdiplus::REAL diameter = std::min(width, height);
        const Gdiplus::REAL repeat_stroke =
            std::max(base_stroke * 1.14F, diameter * 0.070F);
        Gdiplus::Pen repeat_pen(ToGdiPlusColor(color), repeat_stroke);
        repeat_pen.SetStartCap(Gdiplus::LineCapRound);
        repeat_pen.SetEndCap(Gdiplus::LineCapRound);
        repeat_pen.SetDashCap(Gdiplus::DashCapRound);
        repeat_pen.SetLineJoin(Gdiplus::LineJoinRound);

        const Gdiplus::REAL inset = std::max(1.0F, repeat_stroke * 0.60F);
        const Gdiplus::REAL cycle_size =
            std::max(1.0F, diameter - (inset * 2.0F));
        const Gdiplus::REAL radius = cycle_size / 2.0F;
        const Gdiplus::RectF cycle_bounds(cx - radius,
                                          cy - radius,
                                          cycle_size,
                                          cycle_size);

        // The concept's corners sit slightly outside the circular centerline
        // and about 24 degrees above / below the horizontal axis.
        const Gdiplus::REAL tip_x_offset = radius * 0.987F;
        const Gdiplus::REAL tip_y_offset = radius * 0.439F;
        const Gdiplus::REAL head_leg =
            std::max(repeat_stroke * 2.35F, cycle_size * 0.170F);

        Gdiplus::GraphicsPath upper_arc;
        upper_arc.AddArc(cycle_bounds, 180.0F, 149.0F);
        graphics.DrawPath(&repeat_pen, &upper_arc);

        const Gdiplus::PointF upper_tip(cx + tip_x_offset,
                                        cy - tip_y_offset);
        Gdiplus::GraphicsPath upper_head;
        upper_head.StartFigure();
        upper_head.AddLine(Gdiplus::PointF(upper_tip.X - head_leg,
                                           upper_tip.Y),
                           upper_tip);
        upper_head.AddLine(upper_tip,
                           Gdiplus::PointF(upper_tip.X,
                                           upper_tip.Y - head_leg));
        graphics.DrawPath(&repeat_pen, &upper_head);

        Gdiplus::GraphicsPath lower_arc;
        lower_arc.AddArc(cycle_bounds, 0.0F, 149.0F);
        graphics.DrawPath(&repeat_pen, &lower_arc);

        const Gdiplus::PointF lower_tip(cx - tip_x_offset,
                                        cy + tip_y_offset);
        Gdiplus::GraphicsPath lower_head;
        lower_head.StartFigure();
        lower_head.AddLine(Gdiplus::PointF(lower_tip.X + head_leg,
                                           lower_tip.Y),
                           lower_tip);
        lower_head.AddLine(lower_tip,
                           Gdiplus::PointF(lower_tip.X,
                                           lower_tip.Y + head_leg));
        graphics.DrawPath(&repeat_pen, &lower_head);
        break;
    }
    case Glyph::Keyboard: {
        const Gdiplus::RectF body(left + (width * 0.045F),
                                  top + (height * 0.14F),
                                  width * 0.91F,
                                  height * 0.72F);
        Gdiplus::GraphicsPath body_path;
        AddRoundedRectangle(body_path,
                            body,
                            std::max(2.0F, std::min(width, height) * 0.12F));
        graphics.DrawPath(&pen, &body_path);

        const auto draw_key = [&graphics, &brush](const Gdiplus::REAL x,
                                                   const Gdiplus::REAL y,
                                                   const Gdiplus::REAL key_width,
                                                   const Gdiplus::REAL key_height) {
            Gdiplus::GraphicsPath key_path;
            AddRoundedRectangle(key_path,
                                Gdiplus::RectF(x, y, key_width, key_height),
                                std::max(0.7F, key_height * 0.45F));
            graphics.FillPath(&brush, &key_path);
        };

        // Center each row from its measured total width so the key field has
        // equal left and right breathing room at every DPI.
        const Gdiplus::REAL key_width = std::max(1.55F, body.Width * 0.072F);
        const Gdiplus::REAL key_height = std::max(1.35F, body.Height * 0.085F);
        const Gdiplus::REAL top_y = body.Y + (body.Height * 0.24F);
        const Gdiplus::REAL middle_y = body.Y + (body.Height * 0.45F);
        const Gdiplus::REAL top_step = body.Width * 0.125F;
        const Gdiplus::REAL middle_step = body.Width * 0.137F;
        const Gdiplus::REAL top_row_width = key_width + (top_step * 6.0F);
        const Gdiplus::REAL middle_row_width = key_width + (middle_step * 5.0F);
        const Gdiplus::REAL top_start = body.X + ((body.Width - top_row_width) / 2.0F);
        const Gdiplus::REAL middle_start = body.X + ((body.Width - middle_row_width) / 2.0F);

        for (int column = 0; column < 7; ++column) {
            draw_key(top_start + (static_cast<Gdiplus::REAL>(column) * top_step),
                     top_y,
                     key_width,
                     key_height);
        }
        for (int column = 0; column < 6; ++column) {
            draw_key(middle_start + (static_cast<Gdiplus::REAL>(column) * middle_step),
                     middle_y,
                     key_width,
                     key_height);
        }

        const Gdiplus::REAL bottom_y = body.Y + (body.Height * 0.67F);
        const Gdiplus::REAL modifier_width = key_width * 1.25F;
        const Gdiplus::REAL space_width = body.Width * 0.36F;
        const Gdiplus::REAL bottom_gap = body.Width * 0.035F;
        const Gdiplus::REAL bottom_row_width =
            (modifier_width * 2.0F) + space_width + (bottom_gap * 2.0F);
        const Gdiplus::REAL bottom_start =
            body.X + ((body.Width - bottom_row_width) / 2.0F);
        draw_key(bottom_start,
                 bottom_y,
                 modifier_width,
                 key_height);
        draw_key(bottom_start + modifier_width + bottom_gap,
                 bottom_y,
                 space_width,
                 key_height);
        draw_key(bottom_start + modifier_width + bottom_gap + space_width + bottom_gap,
                 bottom_y,
                 modifier_width,
                 key_height);
        break;
    }
    case Glyph::Clock: {
        const Gdiplus::REAL inset = std::max(0.8F, stroke / 2.0F);
        graphics.DrawEllipse(&pen,
                             left + inset,
                             top + inset,
                             width - (inset * 2.0F),
                             height - (inset * 2.0F));
        line(cx, cy, cx, top + (height * 0.27F));
        line(cx, cy, right - (width * 0.24F), cy + (height * 0.16F));
        break;
    }
    case Glyph::Action:
    case Glyph::Spacing: {
        const Gdiplus::REAL inset = std::max(1.4F, stroke);
        graphics.DrawEllipse(&pen,
                             left + inset,
                             top + inset,
                             width - (inset * 2.0F),
                             height - (inset * 2.0F));

        if (glyph == Glyph::Action) {
            // The action-pattern mark is a clean two-ring target. Omitting the
            // center point keeps it visually distinct from Action spacing.
            const Gdiplus::REAL inner_radius =
                std::min(width, height) * 0.205F;
            graphics.DrawEllipse(&pen,
                                 cx - inner_radius,
                                 cy - inner_radius,
                                 inner_radius * 2.0F,
                                 inner_radius * 2.0F);
        } else {
            // The spacing concept uses one strong center point inside the ring.
            // Increase it slightly so the center reads more clearly at small
            // row-label sizes without changing the outer ring.
            const Gdiplus::REAL dot =
                std::max(3.25F, std::min(width, height) * 0.145F);
            graphics.FillEllipse(&brush,
                                 cx - dot,
                                 cy - dot,
                                 dot * 2.0F,
                                 dot * 2.0F);
        }
        break;
    }
    case Glyph::MouseClick: {
        // Cursor-click mark used by the live rate row when mouse input is
        // selected. Keep the cursor itself outlined and balance the activation
        // rays across the upper half of its tip. The geometry is generated at
        // the destination size so it stays smooth at high DPI instead of
        // scaling a raster asset.
        const Gdiplus::PointF tip(left + (width * 0.30F),
                                  top + (height * 0.25F));
        std::array<Gdiplus::PointF, 7> cursor_points{{
            tip,
            Gdiplus::PointF(left + (width * 0.31F),
                            top + (height * 0.80F)),
            Gdiplus::PointF(left + (width * 0.45F),
                            top + (height * 0.65F)),
            Gdiplus::PointF(left + (width * 0.57F),
                            top + (height * 0.87F)),
            Gdiplus::PointF(left + (width * 0.68F),
                            top + (height * 0.81F)),
            Gdiplus::PointF(left + (width * 0.545F),
                            top + (height * 0.59F)),
            Gdiplus::PointF(left + (width * 0.725F),
                            top + (height * 0.555F)),
        }};
        Gdiplus::GraphicsPath cursor_path;
        cursor_path.StartFigure();
        for (std::size_t index = 1; index < cursor_points.size(); ++index) {
            cursor_path.AddLine(cursor_points[index - 1],
                                cursor_points[index]);
        }
        cursor_path.CloseFigure();

        Gdiplus::Pen cursor_pen(ToGdiPlusColor(color), stroke);
        cursor_pen.SetStartCap(Gdiplus::LineCapRound);
        cursor_pen.SetEndCap(Gdiplus::LineCapRound);
        cursor_pen.SetLineJoin(Gdiplus::LineJoinMiter);
        graphics.DrawPath(&cursor_pen, &cursor_path);

        // Four evenly spaced rounded rays form a shallow semicircle around the
        // cursor head. Keeping their inner endpoints clear of the outline
        // prevents the mark from becoming visually crowded at 125% DPI.
        line(left + (width * 0.160F),
             top + (height * 0.212F),
             left + (width * 0.073F),
             top + (height * 0.189F));
        line(left + (width * 0.239F),
             top + (height * 0.119F),
             left + (width * 0.201F),
             top + (height * 0.037F));
        line(left + (width * 0.361F),
             top + (height * 0.119F),
             left + (width * 0.399F),
             top + (height * 0.037F));
        line(left + (width * 0.440F),
             top + (height * 0.212F),
             left + (width * 0.527F),
             top + (height * 0.189F));
        break;
    }
    case Glyph::KeyPress: {
        // Generic square keycap for keyboard mode. Keep the key close to the
        // proportions of a normal single keyboard key rather than a compact
        // text field. The W is intentionally symbolic: it identifies keyboard
        // input without mirroring the selected generated key, so the rate row
        // remains visually stable while the key selector changes.
        const Gdiplus::REAL key_side =
            std::min(width, height) * 0.76F;
        const Gdiplus::RectF keycap(cx - (key_side / 2.0F),
                                    cy - (key_side / 2.0F),
                                    key_side,
                                    key_side);
        Gdiplus::GraphicsPath keycap_path;
        AddRoundedRectangle(keycap_path,
                            keycap,
                            std::max(2.0F, key_side * 0.12F));
        graphics.DrawPath(&pen, &keycap_path);

        // Fit the W to the square rather than stretching it to the full icon
        // slot. This keeps the letter comfortably inset from the keycap edge
        // and gives it the same visual weight as the mouse-click cursor.
        const Gdiplus::REAL letter_half_width = key_side * 0.23F;
        const Gdiplus::REAL letter_top = keycap.Y + (key_side * 0.34F);
        const Gdiplus::REAL letter_bottom = keycap.Y + (key_side * 0.66F);
        const Gdiplus::REAL letter_inner = keycap.Y + (key_side * 0.50F);
        std::array<Gdiplus::PointF, 5> letter_points{{
            Gdiplus::PointF(cx - letter_half_width, letter_top),
            Gdiplus::PointF(cx - (letter_half_width * 0.50F), letter_bottom),
            Gdiplus::PointF(cx, letter_inner),
            Gdiplus::PointF(cx + (letter_half_width * 0.50F), letter_bottom),
            Gdiplus::PointF(cx + letter_half_width, letter_top),
        }};
        Gdiplus::Pen letter_pen(ToGdiPlusColor(color),
                                std::max(1.0F, stroke * 0.92F));
        letter_pen.SetStartCap(Gdiplus::LineCapRound);
        letter_pen.SetEndCap(Gdiplus::LineCapRound);
        letter_pen.SetLineJoin(Gdiplus::LineJoinRound);
        graphics.DrawLines(&letter_pen,
                           letter_points.data(),
                           static_cast<INT>(letter_points.size()));
        break;
    }
    case Glyph::Gauge: {
        // A compact speedometer silhouette inspired by a conventional
        // half-gauge, but kept within Vector Click's existing line-glyph
        // language. Keep the dial gently widened and slightly flatter so the
        // upper arc reads closer to a conventional semicircle without making
        // the scale look compressed. Keep the needle clearly seated inside
        // the rim rather than touching or escaping it.
        const Gdiplus::REAL radius_x = width * 0.382F;
        const Gdiplus::REAL radius_y = height * 0.408F;
        const Gdiplus::REAL gauge_cy = top + (height * 0.70F);
        const Gdiplus::RectF dial_bounds(
            cx - radius_x,
            gauge_cy - radius_y,
            radius_x * 2.0F,
            radius_y * 2.0F);
        Gdiplus::GraphicsPath dial;
        dial.AddArc(dial_bounds, 180.0F, 180.0F);
        graphics.DrawPath(&pen, &dial);

        constexpr std::array<Gdiplus::REAL, 5> tick_angles{
            195.0F, 232.5F, 270.0F, 307.5F, 345.0F};
        for (const Gdiplus::REAL degrees : tick_angles) {
            const Gdiplus::REAL radians =
                degrees * (3.14159265358979323846F / 180.0F);
            const Gdiplus::REAL sx =
                cx + (std::cos(radians) * radius_x * 0.76F);
            const Gdiplus::REAL sy =
                gauge_cy + (std::sin(radians) * radius_y * 0.76F);
            const Gdiplus::REAL ex =
                cx + (std::cos(radians) * radius_x * 0.94F);
            const Gdiplus::REAL ey =
                gauge_cy + (std::sin(radians) * radius_y * 0.94F);
            line(sx, sy, ex, ey);
        }

        const Gdiplus::REAL pivot_y = gauge_cy + (height * 0.01F);
        const Gdiplus::REAL needle_angle =
            326.25F * (3.14159265358979323846F / 180.0F);
        const Gdiplus::REAL needle_scale = 0.72F;
        line(cx,
             pivot_y,
             cx + (std::cos(needle_angle) * radius_x * needle_scale),
             pivot_y + (std::sin(needle_angle) * radius_y * needle_scale));
        const Gdiplus::REAL pivot = std::max(2.0F, stroke * 1.25F);
        graphics.FillEllipse(&brush,
                             cx - pivot,
                             pivot_y - pivot,
                             pivot * 2.0F,
                             pivot * 2.0F);
        break;
    }
    case Glyph::NotificationIndicator: {
        // Conventional notification bell plus a neutral status lamp. Keep the
        // static header lamp neutral; violet is reserved for the live running
        // badge on the application icon.
        const Gdiplus::REAL bell_top = top + (height * 0.22F);
        const Gdiplus::REAL bell_bottom = top + (height * 0.72F);
        const Gdiplus::REAL left_rim = left + (width * 0.20F);
        const Gdiplus::REAL right_rim = right - (width * 0.20F);
        const Gdiplus::REAL upper_left = left + (width * 0.36F);
        const Gdiplus::REAL upper_right = right - (width * 0.36F);

        Gdiplus::GraphicsPath bell;
        bell.StartFigure();
        bell.AddBezier(
            Gdiplus::PointF(cx, bell_top),
            Gdiplus::PointF(upper_left, bell_top),
            Gdiplus::PointF(left + (width * 0.29F), top + (height * 0.36F)),
            Gdiplus::PointF(left + (width * 0.28F), top + (height * 0.50F)));
        bell.AddBezier(
            Gdiplus::PointF(left + (width * 0.28F), top + (height * 0.50F)),
            Gdiplus::PointF(left + (width * 0.27F), top + (height * 0.61F)),
            Gdiplus::PointF(left_rim, bell_bottom - (height * 0.04F)),
            Gdiplus::PointF(left_rim, bell_bottom));
        bell.AddBezier(
            Gdiplus::PointF(left_rim, bell_bottom),
            Gdiplus::PointF(cx - (width * 0.17F), bell_bottom + (height * 0.025F)),
            Gdiplus::PointF(cx + (width * 0.17F), bell_bottom + (height * 0.025F)),
            Gdiplus::PointF(right_rim, bell_bottom));
        bell.AddBezier(
            Gdiplus::PointF(right_rim, bell_bottom),
            Gdiplus::PointF(right - (width * 0.27F), top + (height * 0.61F)),
            Gdiplus::PointF(right - (width * 0.28F), top + (height * 0.50F)),
            Gdiplus::PointF(right - (width * 0.28F), top + (height * 0.50F)));
        bell.AddBezier(
            Gdiplus::PointF(right - (width * 0.28F), top + (height * 0.50F)),
            Gdiplus::PointF(right - (width * 0.29F), top + (height * 0.36F)),
            Gdiplus::PointF(upper_right, bell_top),
            Gdiplus::PointF(cx, bell_top));
        graphics.DrawPath(&pen, &bell);

        // A short hanger and a small rounded clapper make the silhouette read
        // as a bell at small DPI-scaled sizes instead of as a dome or bucket.
        line(cx, top + (height * 0.15F), cx, bell_top);
        const Gdiplus::REAL clapper =
            std::max(3.2F, std::min(width, height) * 0.14F);
        graphics.FillEllipse(
            &brush,
            cx - (clapper * 0.5F),
            bell_bottom + (height * 0.06F),
            clapper,
            clapper);

        const Gdiplus::REAL lamp_radius =
            std::max(1.7F, std::min(width, height) * 0.085F);
        const Gdiplus::REAL lamp_cx = right - (width * 0.15F);
        const Gdiplus::REAL lamp_cy = top + (height * 0.17F);
        graphics.FillEllipse(&brush,
                             lamp_cx - lamp_radius,
                             lamp_cy - lamp_radius,
                             lamp_radius * 2.0F,
                             lamp_radius * 2.0F);
        break;
    }
    case Glyph::List: {
        const Gdiplus::REAL dot = std::max(1.4F, stroke);
        for (int row = 0; row < 3; ++row) {
            const Gdiplus::REAL row_y =
                top + (height * (0.22F + (static_cast<Gdiplus::REAL>(row) * 0.29F)));
            graphics.FillEllipse(&brush,
                                 left + (width * 0.09F) - dot,
                                 row_y - dot,
                                 dot * 2.0F,
                                 dot * 2.0F);
            line(left + (width * 0.24F),
                 row_y,
                 right - (width * 0.05F),
                 row_y);
        }
        break;
    }
    case Glyph::WindowTarget: {
        // Browser-style target icon adapted from the accepted concept. Four
        // compact title-bar pills complete the window detail while the
        // larger destination bounds give the center target comfortable room.
        // A mouse cursor remains omitted because it becomes cluttered at the
        // label and button sizes used by VectorClick.
        const Gdiplus::REAL frame_inset = std::max(1.0F, stroke * 0.62F);
        const Gdiplus::RectF window_bounds(
            left + frame_inset,
            top + (height * 0.07F),
            width - (frame_inset * 2.0F),
            height * 0.82F);
        Gdiplus::GraphicsPath window_path;
        AddRoundedRectangle(window_path,
                            window_bounds,
                            std::max(1.6F, width * 0.075F));
        graphics.DrawPath(&pen, &window_path);

        const Gdiplus::REAL header_y =
            window_bounds.Y + (window_bounds.Height * 0.28F);
        line(window_bounds.X + (stroke * 0.40F),
             header_y,
             window_bounds.GetRight() - (stroke * 0.40F),
             header_y);

        const Gdiplus::REAL pill_height =
            std::max(1.15F, std::min(width, height) * 0.055F);
        const Gdiplus::REAL pill_width = pill_height * 1.55F;
        const Gdiplus::REAL pill_gap = pill_height * 0.70F;
        const Gdiplus::REAL pill_y =
            window_bounds.Y + ((header_y - window_bounds.Y - pill_height) * 0.50F);
        Gdiplus::REAL pill_x = window_bounds.X + (window_bounds.Width * 0.13F);
        for (int index = 0; index < 4; ++index) {
            Gdiplus::GraphicsPath pill_path;
            AddRoundedRectangle(pill_path,
                                Gdiplus::RectF(pill_x,
                                               pill_y,
                                               pill_width,
                                               pill_height),
                                pill_height * 0.50F);
            graphics.FillPath(&brush, &pill_path);
            pill_x += pill_width + pill_gap;
        }

        const Gdiplus::REAL target_cx =
            window_bounds.X + (window_bounds.Width * 0.50F);
        const Gdiplus::REAL target_cy =
            header_y + ((window_bounds.GetBottom() - header_y) * 0.54F);
        const Gdiplus::REAL radius =
            std::min(width, height) * 0.168F;
        graphics.DrawEllipse(&pen,
                             target_cx - radius,
                             target_cy - radius,
                             radius * 2.0F,
                             radius * 2.0F);

        const Gdiplus::REAL outer_arm = radius * 0.58F;
        const Gdiplus::REAL inner_arm = radius * 0.35F;
        line(target_cx,
             target_cy - radius - outer_arm,
             target_cx,
             target_cy - radius + inner_arm);
        line(target_cx,
             target_cy + radius - inner_arm,
             target_cx,
             target_cy + radius + outer_arm);
        line(target_cx - radius - outer_arm,
             target_cy,
             target_cx - radius + inner_arm,
             target_cy);
        line(target_cx + radius - inner_arm,
             target_cy,
             target_cx + radius + outer_arm,
             target_cy);
        break;
    }
    case Glyph::Emergency: {
        // Smooth crown-style shield for Emergency Stop. The outline uses a
        // rounded upper crest, broad shoulders, gently tapering sides, and a
        // soft lower point so it remains recognizable at high DPI without
        // looking like a narrow diamond.
        Gdiplus::GraphicsPath shield_path;
        shield_path.StartFigure();
        shield_path.AddBezier(
            left + (width * 0.480F),
            top + (height * 0.075F),
            left + (width * 0.490F),
            top + (height * 0.062F),
            left + (width * 0.510F),
            top + (height * 0.062F),
            left + (width * 0.520F),
            top + (height * 0.075F));
        shield_path.AddBezier(
            left + (width * 0.520F),
            top + (height * 0.075F),
            left + (width * 0.620F),
            top + (height * 0.140F),
            left + (width * 0.720F),
            top + (height * 0.180F),
            left + (width * 0.840F),
            top + (height * 0.200F));
        shield_path.AddBezier(
            left + (width * 0.840F),
            top + (height * 0.200F),
            left + (width * 0.840F),
            top + (height * 0.480F),
            left + (width * 0.790F),
            top + (height * 0.680F),
            left + (width * 0.670F),
            top + (height * 0.810F));
        shield_path.AddBezier(
            left + (width * 0.670F),
            top + (height * 0.810F),
            left + (width * 0.610F),
            top + (height * 0.875F),
            left + (width * 0.560F),
            top + (height * 0.940F),
            cx,
            top + (height * 0.940F));
        shield_path.AddBezier(
            cx,
            top + (height * 0.940F),
            left + (width * 0.440F),
            top + (height * 0.940F),
            left + (width * 0.390F),
            top + (height * 0.875F),
            left + (width * 0.330F),
            top + (height * 0.810F));
        shield_path.AddBezier(
            left + (width * 0.330F),
            top + (height * 0.810F),
            left + (width * 0.210F),
            top + (height * 0.680F),
            left + (width * 0.160F),
            top + (height * 0.480F),
            left + (width * 0.160F),
            top + (height * 0.200F));
        shield_path.AddBezier(
            left + (width * 0.160F),
            top + (height * 0.200F),
            left + (width * 0.280F),
            top + (height * 0.180F),
            left + (width * 0.380F),
            top + (height * 0.140F),
            left + (width * 0.480F),
            top + (height * 0.075F));
        shield_path.CloseFigure();
        graphics.DrawPath(&pen, &shield_path);

        Gdiplus::Pen mark_pen(ToGdiPlusColor(color), stroke * 1.08F);
        mark_pen.SetStartCap(Gdiplus::LineCapRound);
        mark_pen.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(&mark_pen,
                          cx,
                          top + (height * 0.300F),
                          cx,
                          top + (height * 0.575F));
        const Gdiplus::REAL dot = std::max(1.75F, stroke * 0.90F);
        graphics.FillEllipse(&brush,
                             cx - dot,
                             top + (height * 0.705F) - dot,
                             dot * 2.0F,
                             dot * 2.0F);
        break;
    }
    case Glyph::Shield: {
        // Compact authority shield for background input. Keep this shared
        // outline as the canonical small shield used by related UI glyphs.
        Gdiplus::GraphicsPath shield_path;
        AddCompactShieldOutline(
            shield_path,
            Gdiplus::RectF(left, top, width, height));
        graphics.DrawPath(&pen, &shield_path);
        break;
    }
    case Glyph::Administrator: {
        // Application-window plus shield, adapted from the selected concept.
        // The window identifies VectorClick itself, while the overlapping
        // shield communicates administrator elevation. Window strokes are
        // clipped beneath a slightly expanded shield silhouette so the badge
        // reads as a separate foreground mark instead of tangled geometry.
        const Gdiplus::REAL frame_inset = std::max(1.0F, stroke * 0.55F);
        const Gdiplus::RectF window_bounds(
            left + frame_inset,
            top + (height * 0.09F),
            width * 0.66F,
            height * 0.61F);

        // Reuse the established background-input shield outline rather than
        // maintaining a separate administrator-only silhouette.
        const Gdiplus::RectF shield_bounds(
            left + (width * 0.40F),
            top + (height * 0.35F),
            width * 0.60F,
            height * 0.64F);
        Gdiplus::GraphicsPath shield_path;
        AddCompactShieldOutline(shield_path, shield_bounds);

        const Gdiplus::REAL mask_margin = std::max(1.0F, stroke * 0.85F);
        const Gdiplus::RectF shield_mask_bounds(
            shield_bounds.X - mask_margin,
            shield_bounds.Y - mask_margin,
            shield_bounds.Width + (mask_margin * 2.0F),
            shield_bounds.Height + (mask_margin * 2.0F));
        Gdiplus::GraphicsPath shield_mask_path;
        AddCompactShieldOutline(shield_mask_path, shield_mask_bounds);
        graphics.SetClip(&shield_mask_path, Gdiplus::CombineModeExclude);

        Gdiplus::GraphicsPath window_path;
        AddRoundedRectangle(window_path,
                            window_bounds,
                            std::max(1.5F, width * 0.070F));
        graphics.DrawPath(&pen, &window_path);

        const Gdiplus::REAL header_y =
            window_bounds.Y + (window_bounds.Height * 0.28F);
        line(window_bounds.X + (stroke * 0.40F),
             header_y,
             window_bounds.GetRight() - (stroke * 0.40F),
             header_y);

        const Gdiplus::REAL pill_height =
            std::max(1.00F, std::min(width, height) * 0.043F);
        const Gdiplus::REAL pill_width = pill_height * 1.38F;
        const Gdiplus::REAL pill_gap = pill_height * 0.60F;
        const Gdiplus::REAL pill_y =
            window_bounds.Y + ((header_y - window_bounds.Y - pill_height) * 0.50F);
        Gdiplus::REAL pill_x = window_bounds.X + (window_bounds.Width * 0.11F);
        for (int index = 0; index < 4; ++index) {
            Gdiplus::GraphicsPath pill_path;
            AddRoundedRectangle(pill_path,
                                Gdiplus::RectF(pill_x,
                                               pill_y,
                                               pill_width,
                                               pill_height),
                                pill_height * 0.50F);
            graphics.FillPath(&brush, &pill_path);
            pill_x += pill_width + pill_gap;
        }

        graphics.ResetClip();
        graphics.DrawPath(&pen, &shield_path);
        break;
    }
    case Glyph::Import: {
        // Compact document-and-arrow import mark. The page outline establishes
        // that the action reads a file; the right-facing arrow enters the page
        // rather than suggesting that Vector Click will move the source file.
        const Gdiplus::REAL page_left = left + (width * 0.31F);
        const Gdiplus::REAL page_top = top + (height * 0.07F);
        const Gdiplus::REAL page_right = right - (width * 0.08F);
        const Gdiplus::REAL page_bottom = bottom - (height * 0.07F);
        const Gdiplus::REAL fold = std::min(width, height) * 0.22F;

        Gdiplus::GraphicsPath page_path;
        page_path.StartFigure();
        page_path.AddLine(page_left, page_top, page_right - fold, page_top);
        page_path.AddLine(page_right - fold, page_top, page_right, page_top + fold);
        page_path.AddLine(page_right, page_top + fold, page_right, page_bottom);
        page_path.AddLine(page_right, page_bottom, page_left, page_bottom);
        page_path.AddLine(page_left, page_bottom, page_left, page_top);
        page_path.CloseFigure();
        graphics.DrawPath(&pen, &page_path);
        line(page_right - fold, page_top, page_right - fold, page_top + fold);
        line(page_right - fold, page_top + fold, page_right, page_top + fold);

        const Gdiplus::REAL arrow_y = top + (height * 0.56F);
        const Gdiplus::REAL arrow_start = left + (width * 0.05F);
        const Gdiplus::REAL arrow_tip = left + (width * 0.57F);
        const Gdiplus::REAL arrow_half = height * 0.18F;
        line(arrow_start, arrow_y, arrow_tip, arrow_y);
        line(arrow_tip, arrow_y, arrow_tip - arrow_half, arrow_y - arrow_half);
        line(arrow_tip, arrow_y, arrow_tip - arrow_half, arrow_y + arrow_half);
        break;
    }
    case Glyph::Gear: {
        constexpr Gdiplus::REAL Pi = 3.14159265358979323846F;
        constexpr int PointCount = 24;
        std::array<Gdiplus::PointF, PointCount> points{};
        const Gdiplus::REAL outer_radius = std::min(width, height) * 0.43F;
        const Gdiplus::REAL inner_radius = outer_radius * 0.76F;
        for (int index = 0; index < PointCount; ++index) {
            // Center a tooth on the vertical axis. The 24-point pattern uses
            // two adjacent outer points per tooth, so a half-point rotation
            // (7.5 degrees) makes the top and bottom teeth read symmetrically.
            const Gdiplus::REAL angle =
                (-Pi / 2.0F) + (Pi / static_cast<Gdiplus::REAL>(PointCount)) +
                ((2.0F * Pi * static_cast<Gdiplus::REAL>(index)) /
                 static_cast<Gdiplus::REAL>(PointCount));
            const bool tooth = (index % 3) != 1;
            const Gdiplus::REAL radius = tooth ? outer_radius : inner_radius;
            points[static_cast<std::size_t>(index)] =
                Gdiplus::PointF(cx + (std::cos(angle) * radius),
                                cy + (std::sin(angle) * radius));
        }
        Gdiplus::GraphicsPath gear_path;
        gear_path.AddPolygon(points.data(), PointCount);
        gear_path.CloseFigure();
        graphics.DrawPath(&pen, &gear_path);
        const Gdiplus::REAL hole = outer_radius * 0.34F;
        graphics.DrawEllipse(&pen,
                             cx - hole,
                             cy - hole,
                             hole * 2.0F,
                             hole * 2.0F);
        break;
    }
    case Glyph::Play: {
        std::array<Gdiplus::PointF, 3> triangle{{
            Gdiplus::PointF(left + (width * 0.28F), top + (height * 0.14F)),
            Gdiplus::PointF(right - (width * 0.10F), cy),
            Gdiplus::PointF(left + (width * 0.28F), bottom - (height * 0.14F)),
        }};
        graphics.FillPolygon(&brush,
                             triangle.data(),
                             static_cast<INT>(triangle.size()));
        break;
    }
    case Glyph::Stop: {
        // Slightly enlarge the stop square so it reads more clearly in the
        // action button without changing the button layout or icon slot.
        const Gdiplus::REAL side = std::min(width, height) * 0.58F;
        const Gdiplus::RectF square(cx - (side / 2.0F),
                                    cy - (side / 2.0F),
                                    side,
                                    side);
        Gdiplus::GraphicsPath path;
        AddRoundedRectangle(path, square, std::max(1.0F, side * 0.12F));
        graphics.FillPath(&brush, &path);
        break;
    }
    case Glyph::ChevronUp:
    case Glyph::ChevronDown: {
        const Gdiplus::REAL half = width * 0.25F;
        const Gdiplus::REAL rise = height * 0.17F;
        if (glyph == Glyph::ChevronUp) {
            line(cx - half, cy + rise, cx, cy - rise);
            line(cx, cy - rise, cx + half, cy + rise);
        } else {
            line(cx - half, cy - rise, cx, cy + rise);
            line(cx, cy + rise, cx + half, cy - rise);
        }
        break;
    }
    case Glyph::Check: {
        std::array<Gdiplus::PointF, 3> points{{
            Gdiplus::PointF(left + (width * 0.18F), cy),
            Gdiplus::PointF(left + (width * 0.42F), bottom - (height * 0.20F)),
            Gdiplus::PointF(right - (width * 0.12F), top + (height * 0.19F)),
        }};
        graphics.DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
        break;
    }
    case Glyph::Dot: {
        const Gdiplus::REAL diameter = std::min(width, height) * 0.42F;
        graphics.FillEllipse(&brush,
                             cx - (diameter / 2.0F),
                             cy - (diameter / 2.0F),
                             diameter,
                             diameter);
        break;
    }
    case Glyph::None:
        break;
    }

    return true;
}

bool DrawSmoothRadioMarkInternal(const HDC dc,
                                   const RECT& rectangle,
                                   const COLORREF fill,
                                   const COLORREF border,
                                   const COLORREF indicator,
                                   const bool checked,
                                   const int border_width) noexcept {
    if (!GraphicsRuntime().Ready()) {
        return false;
    }

    const Gdiplus::REAL width =
        static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left);
    const Gdiplus::REAL height =
        static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top);
    if (width <= 1.0F || height <= 1.0F) {
        return true;
    }

    Gdiplus::Graphics graphics(dc);
    ConfigureGraphics(graphics);

    const Gdiplus::REAL stroke =
        static_cast<Gdiplus::REAL>(std::max(1, border_width));
    const Gdiplus::REAL inset = (stroke / 2.0F) + 0.5F;
    const Gdiplus::REAL diameter =
        std::max<Gdiplus::REAL>(1.0F, std::min(width, height) - (inset * 2.0F));
    const Gdiplus::REAL x =
        static_cast<Gdiplus::REAL>(rectangle.left) + ((width - diameter) / 2.0F);
    const Gdiplus::REAL y =
        static_cast<Gdiplus::REAL>(rectangle.top) + ((height - diameter) / 2.0F);

    Gdiplus::SolidBrush fill_brush(ToGdiPlusColor(fill));
    Gdiplus::Pen border_pen(ToGdiPlusColor(border), stroke);
    border_pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.FillEllipse(&fill_brush, x, y, diameter, diameter);
    graphics.DrawEllipse(&border_pen, x, y, diameter, diameter);

    if (checked) {
        // Keep the center dot large enough to read clearly while retaining a
        // visible ring at every supported DPI.
        const Gdiplus::REAL dot_diameter =
            std::max<Gdiplus::REAL>(3.0F, diameter * 0.36F);
        const Gdiplus::REAL center_x = x + (diameter / 2.0F);
        const Gdiplus::REAL center_y = y + (diameter / 2.0F);
        Gdiplus::SolidBrush indicator_brush(ToGdiPlusColor(indicator));
        graphics.FillEllipse(&indicator_brush,
                             center_x - (dot_diameter / 2.0F),
                             center_y - (dot_diameter / 2.0F),
                             dot_diameter,
                             dot_diameter);
    }
    return true;
}

} // namespace

int Scale(const int value, const UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

namespace {

constexpr int StatusIndicatorReferenceDiameterLogical = 9;
constexpr int StatusIndicatorAcceptedDiameterLogical = 13;
constexpr int StatusIndicatorDiameterLogical = 14;
constexpr int StatusIndicatorReferenceLeftLogical = 18;
constexpr int StatusIndicatorMinimumDiameterPixels = 7;

struct StatusIndicatorPlacement {
    RECT integer_bounds{};
    Gdiplus::REAL center_x{};
    Gdiplus::REAL center_y{};
};

[[nodiscard]] StatusIndicatorPlacement CalculateStatusIndicatorPlacement(
    const RECT& panel,
    const UINT dpi) noexcept {
    // Reconstruct the established 13-logical-pixel center, then expand the
    // current 14-logical-pixel indicator around that same center. The explicit
    // center matters when odd / even pixel parity changes at a given DPI, where
    // an integer RECT alone can otherwise
    // move an anti-aliased circle by half a physical pixel.
    const int reference_diameter = std::max(
        Scale(StatusIndicatorReferenceDiameterLogical, dpi),
        StatusIndicatorMinimumDiameterPixels);
    const int accepted_diameter = std::max(
        Scale(StatusIndicatorAcceptedDiameterLogical, dpi),
        StatusIndicatorMinimumDiameterPixels);
    const int diameter = std::max(
        Scale(StatusIndicatorDiameterLogical, dpi),
        StatusIndicatorMinimumDiameterPixels);
    const int reference_left =
        panel.left + Scale(StatusIndicatorReferenceLeftLogical, dpi);
    const int reference_center_twice =
        (reference_left * 2) + reference_diameter;
    const int accepted_left =
        (reference_center_twice - accepted_diameter) / 2;
    const int accepted_top =
        panel.top + ((panel.bottom - panel.top) - accepted_diameter) / 2;
    const int accepted_center_x_twice =
        (accepted_left * 2) + accepted_diameter;
    const int accepted_center_y_twice =
        (accepted_top * 2) + accepted_diameter;
    const int left = (accepted_center_x_twice - diameter) / 2;
    const int top = (accepted_center_y_twice - diameter) / 2;

    return {
        {left, top, left + diameter, top + diameter},
        static_cast<Gdiplus::REAL>(accepted_center_x_twice) / 2.0F,
        static_cast<Gdiplus::REAL>(accepted_center_y_twice) / 2.0F,
    };
}

} // namespace

void DrawSmoothRadioMark(const HDC dc,
                         const RECT& rectangle,
                         const COLORREF fill,
                         const COLORREF border,
                         const COLORREF indicator,
                         const bool checked,
                         const int border_width) noexcept {
    if (DrawSmoothRadioMarkInternal(dc,
                                    rectangle,
                                    fill,
                                    border,
                                    indicator,
                                    checked,
                                    border_width)) {
        return;
    }

    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN pen = CreatePen(PS_SOLID, std::max(1, border_width), border);
    if (brush != nullptr && pen != nullptr) {
        const HGDIOBJ old_brush = SelectObject(dc, brush);
        const HGDIOBJ old_pen = SelectObject(dc, pen);
        Ellipse(dc, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
    }
    if (pen != nullptr) {
        DeleteObject(pen);
    }
    if (brush != nullptr) {
        DeleteObject(brush);
    }

    if (checked) {
        RECT dot = rectangle;
        const int inset = std::max(1, static_cast<int>(rectangle.right - rectangle.left) * 11 / 32);
        InflateRect(&dot, -inset, -inset);
        const HBRUSH dot_brush = CreateSolidBrush(indicator);
        if (dot_brush != nullptr) {
            const HGDIOBJ old_brush = SelectObject(dc, dot_brush);
            const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(dot_brush);
        }
    }
}

void DrawStatusIndicator(const HDC dc,
                         const RECT& panel,
                         const UINT dpi,
                         const COLORREF color) noexcept {
    if (dc == nullptr || panel.right <= panel.left ||
        panel.bottom <= panel.top) {
        return;
    }

    const StatusIndicatorPlacement placement =
        CalculateStatusIndicatorPlacement(panel, dpi);
    const RECT& rectangle = placement.integer_bounds;

    if (GraphicsRuntime().Ready()) {
        Gdiplus::Graphics graphics(dc);
        ConfigureGraphics(graphics);
        const Gdiplus::REAL width =
            static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left);
        const Gdiplus::REAL height =
            static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top);
        const Gdiplus::REAL diameter = std::max<Gdiplus::REAL>(
            1.0F, std::min(width, height) - 1.0F);
        const Gdiplus::REAL x = placement.center_x - (diameter / 2.0F);
        const Gdiplus::REAL y = placement.center_y - (diameter / 2.0F);
        Gdiplus::SolidBrush brush(ToGdiPlusColor(color));
        graphics.FillEllipse(&brush, x, y, diameter, diameter);
        return;
    }

    const HBRUSH brush = CreateSolidBrush(color);
    if (brush == nullptr) {
        return;
    }
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc,
            rectangle.left,
            rectangle.top,
            rectangle.right,
            rectangle.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(brush);
}

void ApplyDarkTitleBar(const HWND window) noexcept {
    if (window == nullptr || IsWindow(window) == FALSE) {
        return;
    }

    const BOOL enabled = TRUE;
    if (DwmSetWindowAttribute(window, 20, &enabled, sizeof(enabled)) != S_OK) {
        (void)DwmSetWindowAttribute(window, 19, &enabled, sizeof(enabled));
    }

    const COLORREF caption = Window;
    const COLORREF caption_text = Text;
    const COLORREF caption_border = BorderSoft;
    (void)DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
    (void)DwmSetWindowAttribute(window, 36, &caption_text, sizeof(caption_text));
    (void)DwmSetWindowAttribute(window, 34, &caption_border, sizeof(caption_border));
}

void ApplyDarkControlTheme(const HWND control) noexcept {
    if (control == nullptr || IsWindow(control) == FALSE) {
        return;
    }

    wchar_t class_name[64]{};
    (void)GetClassNameW(control,
                        class_name,
                        static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));

    if (_wcsicmp(class_name, L"Edit") == 0 ||
        _wcsicmp(class_name, L"ComboBox") == 0) {
        (void)SetWindowTheme(control, L"DarkMode_CFD", nullptr);
    } else {
        (void)SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
    }
}

void Fill(const HDC dc, const RECT& rectangle, const COLORREF color) noexcept {
    if (dc == nullptr) {
        return;
    }
    const HBRUSH brush = CreateSolidBrush(color);
    if (brush != nullptr) {
        FillRect(dc, &rectangle, brush);
        DeleteObject(brush);
    }
}

void FillOutsideRoundedPanel(const HDC dc,
                             const RECT& rectangle,
                             const COLORREF color,
                             const int radius,
                             const int border_width) noexcept {
    if (dc == nullptr || rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return;
    }

    if (GraphicsRuntime().Ready()) {
        Gdiplus::Graphics graphics(dc);
        ConfigureGraphics(graphics);

        const Gdiplus::REAL width =
            static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left);
        const Gdiplus::REAL height =
            static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top);
        const Gdiplus::RectF full_bounds(
            static_cast<Gdiplus::REAL>(rectangle.left),
            static_cast<Gdiplus::REAL>(rectangle.top),
            width,
            height);

        // Leave the rounded field itself untouched and repaint only the four
        // rectangular child-window corners with the card surface behind it.
        // The half-pixel inset aligns this mask with the outer edge of the
        // antialiased outline drawn by DrawRoundedOutline.
        constexpr Gdiplus::REAL inset = 0.5F;
        const Gdiplus::RectF rounded_bounds(
            full_bounds.X + inset,
            full_bounds.Y + inset,
            std::max<Gdiplus::REAL>(0.0F, full_bounds.Width - (inset * 2.0F)),
            std::max<Gdiplus::REAL>(0.0F, full_bounds.Height - (inset * 2.0F)));
        if (rounded_bounds.Width > 0.0F && rounded_bounds.Height > 0.0F) {
            Gdiplus::GraphicsPath outside(Gdiplus::FillModeAlternate);
            outside.AddRectangle(full_bounds);
            const Gdiplus::REAL outer_radius =
                static_cast<Gdiplus::REAL>(std::max(1, radius)) +
                (static_cast<Gdiplus::REAL>(std::max(1, border_width)) / 2.0F);
            AddRoundedRectangle(outside, rounded_bounds, outer_radius);

            // The alternate path contains only the four outside corners, but
            // an unrestricted high-quality FillPath still asks GDI+ to walk
            // the complete panel-sized bounds. Clip the fill to those corner
            // neighborhoods so maximized page-shell repaint cost stays tied
            // to the rounded edge rather than the full client area.
            const Gdiplus::REAL corner_extent = std::min<Gdiplus::REAL>(
                std::max<Gdiplus::REAL>(2.0F, outer_radius + 2.0F),
                std::min(full_bounds.Width, full_bounds.Height));
            Gdiplus::Region corners(Gdiplus::RectF(full_bounds.X,
                                                   full_bounds.Y,
                                                   corner_extent,
                                                   corner_extent));
            corners.Union(Gdiplus::RectF(full_bounds.GetRight() - corner_extent,
                                         full_bounds.Y,
                                         corner_extent,
                                         corner_extent));
            corners.Union(Gdiplus::RectF(full_bounds.X,
                                         full_bounds.GetBottom() - corner_extent,
                                         corner_extent,
                                         corner_extent));
            corners.Union(Gdiplus::RectF(full_bounds.GetRight() - corner_extent,
                                         full_bounds.GetBottom() - corner_extent,
                                         corner_extent,
                                         corner_extent));
            const Gdiplus::GraphicsState state = graphics.Save();
            graphics.SetClip(&corners, Gdiplus::CombineModeIntersect);
            Gdiplus::SolidBrush brush(ToGdiPlusColor(color));
            graphics.FillPath(&brush, &outside);
            graphics.Restore(state);
            return;
        }
    }

    const HRGN full_region = CreateRectRgn(rectangle.left,
                                           rectangle.top,
                                           rectangle.right,
                                           rectangle.bottom);
    const int diameter = std::max(
        2,
        (std::max(1, radius) * 2) + std::max(1, border_width));
    const HRGN rounded_region = CreateRoundRectRgn(rectangle.left,
                                                   rectangle.top,
                                                   rectangle.right + 1,
                                                   rectangle.bottom + 1,
                                                   diameter,
                                                   diameter);
    if (full_region != nullptr && rounded_region != nullptr &&
        CombineRgn(full_region,
                   full_region,
                   rounded_region,
                   RGN_DIFF) != ERROR) {
        const HBRUSH brush = CreateSolidBrush(color);
        if (brush != nullptr) {
            FillRgn(dc, full_region, brush);
            DeleteObject(brush);
        }
    }
    if (rounded_region != nullptr) {
        DeleteObject(rounded_region);
    }
    if (full_region != nullptr) {
        DeleteObject(full_region);
    }
}

void DrawRoundedPanel(const HDC dc,
                      const RECT& rectangle,
                      const COLORREF fill,
                      const COLORREF border,
                      const int radius,
                      const int border_width) noexcept {
    if (dc == nullptr || rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return;
    }

    if (DrawSmoothRoundedPanel(dc,
                               rectangle,
                               fill,
                               border,
                               radius,
                               border_width)) {
        return;
    }

    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN pen = CreatePen(PS_SOLID, std::max(1, border_width), border);
    if (brush == nullptr || pen == nullptr) {
        if (brush != nullptr) {
            DeleteObject(brush);
        }
        if (pen != nullptr) {
            DeleteObject(pen);
        }
        return;
    }

    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc,
              rectangle.left,
              rectangle.top,
              rectangle.right,
              rectangle.bottom,
              std::max(2, radius),
              std::max(2, radius));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawDockedTabPanel(const HDC dc,
                        const RECT& rectangle,
                        const COLORREF fill,
                        const COLORREF border,
                        const int top_radius,
                        const int bottom_radius,
                        const int border_width) noexcept {
    if (dc == nullptr || rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return;
    }

    const int stroke_width = std::max(1, border_width);
    const int safe_top_radius = std::max(1, top_radius);
    const int safe_bottom_radius = std::max(1, bottom_radius);
    if (GraphicsRuntime().Ready()) {
        Gdiplus::Graphics graphics(dc);
        ConfigureGraphics(graphics);

        const Gdiplus::REAL stroke = static_cast<Gdiplus::REAL>(stroke_width);
        const Gdiplus::REAL inset = (stroke / 2.0F) + 0.5F;
        const Gdiplus::REAL width =
            static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left) -
            (inset * 2.0F);
        const Gdiplus::REAL height =
            static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top) -
            (inset * 2.0F);
        if (width > 0.0F && height > 0.0F) {
            const Gdiplus::RectF bounds(
                static_cast<Gdiplus::REAL>(rectangle.left) + inset,
                static_cast<Gdiplus::REAL>(rectangle.top) + inset,
                width,
                height);
            Gdiplus::GraphicsPath path;
            AddDockedTabRectangle(
                path,
                bounds,
                static_cast<Gdiplus::REAL>(safe_top_radius),
                static_cast<Gdiplus::REAL>(safe_bottom_radius));
            Gdiplus::SolidBrush fill_brush(ToGdiPlusColor(fill));
            Gdiplus::Pen border_pen(ToGdiPlusColor(border), stroke);
            border_pen.SetLineJoin(Gdiplus::LineJoinRound);
            graphics.FillPath(&fill_brush, &path);
            graphics.DrawPath(&border_pen, &path);
            return;
        }
    }

    const int top_diameter = std::max(2, safe_top_radius * 2);
    const int bottom_diameter = std::max(2, safe_bottom_radius * 2);
    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN pen = CreatePen(PS_SOLID, stroke_width, border);
    if (brush == nullptr || pen == nullptr) {
        if (pen != nullptr) {
            DeleteObject(pen);
        }
        if (brush != nullptr) {
            DeleteObject(brush);
        }
        return;
    }

    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    BeginPath(dc);
    MoveToEx(dc, rectangle.left, rectangle.bottom - safe_bottom_radius, nullptr);
    LineTo(dc, rectangle.left, rectangle.top + safe_top_radius);
    ArcTo(dc,
          rectangle.left,
          rectangle.top,
          rectangle.left + top_diameter,
          rectangle.top + top_diameter,
          rectangle.left,
          rectangle.top + safe_top_radius,
          rectangle.left + safe_top_radius,
          rectangle.top);
    LineTo(dc, rectangle.right - safe_top_radius, rectangle.top);
    ArcTo(dc,
          rectangle.right - top_diameter,
          rectangle.top,
          rectangle.right,
          rectangle.top + top_diameter,
          rectangle.right - safe_top_radius,
          rectangle.top,
          rectangle.right,
          rectangle.top + safe_top_radius);
    LineTo(dc, rectangle.right, rectangle.bottom - safe_bottom_radius);
    ArcTo(dc,
          rectangle.right - bottom_diameter,
          rectangle.bottom - bottom_diameter,
          rectangle.right,
          rectangle.bottom,
          rectangle.right,
          rectangle.bottom - safe_bottom_radius,
          rectangle.right - safe_bottom_radius,
          rectangle.bottom);
    LineTo(dc, rectangle.left + safe_bottom_radius, rectangle.bottom);
    ArcTo(dc,
          rectangle.left,
          rectangle.bottom - bottom_diameter,
          rectangle.left + bottom_diameter,
          rectangle.bottom,
          rectangle.left + safe_bottom_radius,
          rectangle.bottom,
          rectangle.left,
          rectangle.bottom - safe_bottom_radius);
    CloseFigure(dc);
    EndPath(dc);
    StrokeAndFillPath(dc);

    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawDockedPageShell(const HDC dc,
                         const RECT& rectangle,
                         const COLORREF fill,
                         const COLORREF border,
                         const int radius,
                         const int dock_left,
                         const int dock_right,
                         const int border_width) noexcept {
    if (dc == nullptr || rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return;
    }

    const int stroke_width = std::max(1, border_width);
    const int safe_radius = std::max(2, radius);
    const int minimum_dock_left = rectangle.left + safe_radius;
    const int maximum_dock_right = rectangle.right - safe_radius;
    const int safe_dock_left = std::clamp(dock_left,
                                          minimum_dock_left,
                                          maximum_dock_right);
    const int safe_dock_right = std::clamp(dock_right,
                                           safe_dock_left,
                                           maximum_dock_right);

    if (GraphicsRuntime().Ready()) {
        const Gdiplus::REAL stroke = static_cast<Gdiplus::REAL>(stroke_width);
        const Gdiplus::REAL inset = (stroke / 2.0F) + 0.5F;
        const Gdiplus::RectF bounds(
            static_cast<Gdiplus::REAL>(rectangle.left) + inset,
            static_cast<Gdiplus::REAL>(rectangle.top) + inset,
            static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left) - (inset * 2.0F),
            static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top) - (inset * 2.0F));

        if (bounds.Width > 0.0F && bounds.Height > 0.0F) {
            const Gdiplus::REAL r = EffectiveRoundedRadius(bounds, safe_radius);
            Gdiplus::GraphicsPath fill_path;
            AddRoundedRectangle(fill_path, bounds, r);

            FillOpaqueRoundedInterior(dc, bounds, r, fill);

            Gdiplus::Graphics graphics(dc);
            ConfigureGraphics(graphics);
            FillRoundedPerimeter(fill_path, graphics, bounds, r, fill);

            const Gdiplus::REAL left = bounds.X;
            const Gdiplus::REAL top = bounds.Y;
            const Gdiplus::REAL right = bounds.GetRight();
            const Gdiplus::REAL bottom = bounds.GetBottom();

            // Draw one continuous shell outline whose top edge is open only
            // beneath the actual tab dock. The short left segment and longer
            // right segment make the tabs appear seated into the page body
            // instead of floating over a full-width rectangular strip.
            const Gdiplus::REAL gap_left = std::clamp<Gdiplus::REAL>(
                static_cast<Gdiplus::REAL>(safe_dock_left),
                left + r,
                right - r);
            const Gdiplus::REAL gap_right = std::clamp<Gdiplus::REAL>(
                static_cast<Gdiplus::REAL>(safe_dock_right),
                gap_left,
                right - r);

            Gdiplus::GraphicsPath border_path;
            border_path.StartFigure();
            border_path.AddLine(gap_left, top, left + r, top);
            border_path.AddArc(Gdiplus::RectF(left,
                                             top,
                                             r * 2.0F,
                                             r * 2.0F),
                               270.0F,
                               -90.0F);
            border_path.AddLine(left, top + r, left, bottom - r);
            border_path.AddArc(Gdiplus::RectF(left,
                                             bottom - (r * 2.0F),
                                             r * 2.0F,
                                             r * 2.0F),
                               180.0F,
                               -90.0F);
            border_path.AddLine(left + r, bottom, right - r, bottom);
            border_path.AddArc(Gdiplus::RectF(right - (r * 2.0F),
                                             bottom - (r * 2.0F),
                                             r * 2.0F,
                                             r * 2.0F),
                               90.0F,
                               -90.0F);
            border_path.AddLine(right, bottom - r, right, top + r);
            border_path.AddArc(Gdiplus::RectF(right - (r * 2.0F),
                                             top,
                                             r * 2.0F,
                                             r * 2.0F),
                               0.0F,
                               -90.0F);
            border_path.AddLine(right - r, top, gap_right, top);

            Gdiplus::Pen border_pen(ToGdiPlusColor(border), stroke);
            border_pen.SetStartCap(Gdiplus::LineCapRound);
            border_pen.SetEndCap(Gdiplus::LineCapRound);
            border_pen.SetLineJoin(Gdiplus::LineJoinRound);
            graphics.DrawPath(&border_pen, &border_path);

            // The tab windows repaint the portions beneath their own bodies.
            // Keep the same shell baseline visible through the inter-tab gap
            // so the two docked tabs and page body read as one structure.
            graphics.DrawLine(&border_pen, gap_left, top, gap_right, top);
            return;
        }
    }

    // GDI fallback: preserve the same tab-sized opening and completed upper
    // corner arcs as the GDI+ path above.
    const HBRUSH fill_brush = CreateSolidBrush(fill);
    const HPEN transparent_pen = CreatePen(PS_NULL, 0, fill);
    if (fill_brush != nullptr && transparent_pen != nullptr) {
        const HGDIOBJ old_brush = SelectObject(dc, fill_brush);
        const HGDIOBJ old_pen = SelectObject(dc, transparent_pen);
        RoundRect(dc,
                  rectangle.left,
                  rectangle.top,
                  rectangle.right,
                  rectangle.bottom,
                  safe_radius * 2,
                  safe_radius * 2);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
    }
    if (transparent_pen != nullptr) {
        DeleteObject(transparent_pen);
    }
    if (fill_brush != nullptr) {
        DeleteObject(fill_brush);
    }

    const HPEN border_pen = CreatePen(PS_SOLID, stroke_width, border);
    if (border_pen == nullptr) {
        return;
    }
    const HGDIOBJ old_pen = SelectObject(dc, border_pen);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    MoveToEx(dc, safe_dock_left, rectangle.top, nullptr);
    LineTo(dc, rectangle.left + safe_radius, rectangle.top);
    Arc(dc,
        rectangle.left,
        rectangle.top,
        rectangle.left + (safe_radius * 2),
        rectangle.top + (safe_radius * 2),
        rectangle.left + safe_radius,
        rectangle.top,
        rectangle.left,
        rectangle.top + safe_radius);
    MoveToEx(dc, rectangle.left, rectangle.top + safe_radius, nullptr);
    LineTo(dc, rectangle.left, rectangle.bottom - safe_radius);
    Arc(dc,
        rectangle.left,
        rectangle.bottom - (safe_radius * 2),
        rectangle.left + (safe_radius * 2),
        rectangle.bottom,
        rectangle.left,
        rectangle.bottom - safe_radius,
        rectangle.left + safe_radius,
        rectangle.bottom);
    LineTo(dc, rectangle.right - safe_radius, rectangle.bottom);
    Arc(dc,
        rectangle.right - (safe_radius * 2),
        rectangle.bottom - (safe_radius * 2),
        rectangle.right,
        rectangle.bottom,
        rectangle.right - safe_radius,
        rectangle.bottom,
        rectangle.right,
        rectangle.bottom - safe_radius);
    LineTo(dc, rectangle.right, rectangle.top + safe_radius);
    Arc(dc,
        rectangle.right - (safe_radius * 2),
        rectangle.top,
        rectangle.right,
        rectangle.top + (safe_radius * 2),
        rectangle.right,
        rectangle.top + safe_radius,
        rectangle.right - safe_radius,
        rectangle.top);
    LineTo(dc, safe_dock_right, rectangle.top);
    MoveToEx(dc, safe_dock_left, rectangle.top, nullptr);
    LineTo(dc, safe_dock_right, rectangle.top);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border_pen);
}

void DrawStartButtonSurface(const HDC dc,
                            const RECT& rectangle,
                            const COLORREF border,
                            const int radius,
                            const bool hover,
                            const int border_width) noexcept {
    if (dc == nullptr || rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return;
    }

    if (DrawSmoothStartButtonSurface(dc,
                                     rectangle,
                                     border,
                                     radius,
                                     hover,
                                     border_width)) {
        return;
    }

    DrawRoundedPanel(dc,
                     rectangle,
                     hover ? StartSurfaceBaseHover : StartSurfaceBase,
                     border,
                     radius,
                     border_width);
}

void DrawRoundedOutline(const HDC dc,
                        const RECT& rectangle,
                        const COLORREF color,
                        const int radius,
                        const int border_width) noexcept {
    if (dc == nullptr || rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return;
    }

    if (GraphicsRuntime().Ready()) {
        Gdiplus::Graphics graphics(dc);
        ConfigureGraphics(graphics);
        const Gdiplus::REAL stroke =
            static_cast<Gdiplus::REAL>(std::max(1, border_width));
        const Gdiplus::REAL inset = (stroke / 2.0F) + 0.5F;
        const Gdiplus::RectF bounds(
            static_cast<Gdiplus::REAL>(rectangle.left) + inset,
            static_cast<Gdiplus::REAL>(rectangle.top) + inset,
            static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left) - (inset * 2.0F),
            static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top) - (inset * 2.0F));
        if (bounds.Width > 0.0F && bounds.Height > 0.0F) {
            Gdiplus::GraphicsPath path;
            AddRoundedRectangle(path,
                                bounds,
                                static_cast<Gdiplus::REAL>(std::max(1, radius)));
            Gdiplus::Pen pen(ToGdiPlusColor(color), stroke);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            graphics.DrawPath(&pen, &path);
        }
        return;
    }

    const HPEN pen = CreatePen(PS_SOLID, std::max(1, border_width), color);
    if (pen == nullptr) {
        return;
    }
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc,
              rectangle.left,
              rectangle.top,
              rectangle.right,
              rectangle.bottom,
              std::max(2, radius),
              std::max(2, radius));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void DrawTextLine(const HDC dc,
                  const std::wstring_view text,
                  RECT rectangle,
                  const HFONT font,
                  const COLORREF color,
                  const UINT format) noexcept {
    if (dc == nullptr) {
        return;
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    const HGDIOBJ selected_font =
        font != nullptr ? font : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ previous = SelectObject(dc, selected_font);
    DrawTextW(dc,
              text.data(),
              static_cast<int>(text.size()),
              &rectangle,
              format | DT_NOPREFIX);
    SelectObject(dc, previous);
}

void DrawFocusOutline(const HDC dc,
                      const RECT& rectangle,
                      const COLORREF color,
                      const int inset) noexcept {
    RECT focus = rectangle;
    InflateRect(&focus, -std::max(1, inset), -std::max(1, inset));
    if (focus.right <= focus.left || focus.bottom <= focus.top) {
        return;
    }

    const HPEN pen = CreatePen(PS_DOT, 1, color);
    if (pen == nullptr) {
        return;
    }
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, focus.left, focus.top, focus.right, focus.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void DrawGlyph(const HDC dc,
               const Glyph glyph,
               const RECT& rectangle,
               const COLORREF color,
               const int stroke_width) noexcept {
    if (dc == nullptr || glyph == Glyph::None ||
        rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top) {
        return;
    }

    if (DrawSmoothGlyph(dc, glyph, rectangle, color, stroke_width)) {
        return;
    }

    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    const int cx = rectangle.left + (width / 2);
    const int cy = rectangle.top + (height / 2);
    const HPEN pen = CreatePen(PS_SOLID, std::max(1, stroke_width), color);
    if (pen == nullptr) {
        return;
    }
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc,
            rectangle.left + 1,
            rectangle.top + 1,
            rectangle.right - 1,
            rectangle.bottom - 1);
    MoveToEx(dc, rectangle.left, cy, nullptr);
    LineTo(dc, rectangle.right, cy);
    MoveToEx(dc, cx, rectangle.top, nullptr);
    LineTo(dc, cx, rectangle.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void DrawWarningIcon(const HDC dc, const RECT& rectangle) noexcept {
    if (dc == nullptr || rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return;
    }

    const Gdiplus::REAL width =
        static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left);
    const Gdiplus::REAL height =
        static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top);
    const Gdiplus::REAL side = std::min(width, height);

    if (GraphicsRuntime().Ready()) {
        Gdiplus::Graphics graphics(dc);
        ConfigureGraphics(graphics);

        const Gdiplus::REAL inset = std::max<Gdiplus::REAL>(1.0F, side * 0.035F);
        const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(rectangle.left) +
                                    ((width - side) / 2.0F) + inset;
        const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(rectangle.top) +
                                   ((height - side) / 2.0F) + inset;
        const Gdiplus::REAL usable = side - (inset * 2.0F);
        std::array<Gdiplus::PointF, 3> points{{
            Gdiplus::PointF(left + (usable * 0.50F), top + (usable * 0.07F)),
            Gdiplus::PointF(left + (usable * 0.06F), top + (usable * 0.88F)),
            Gdiplus::PointF(left + (usable * 0.94F), top + (usable * 0.88F)),
        }};

        Gdiplus::GraphicsPath triangle;
        AddRoundedPolygon(triangle, points, std::max<Gdiplus::REAL>(1.0F, side * 0.045F));
        Gdiplus::SolidBrush fill_brush(ToGdiPlusColor(Warning));
        Gdiplus::Pen border_pen(ToGdiPlusColor(Text),
                                std::max<Gdiplus::REAL>(1.2F, side * 0.045F));
        border_pen.SetLineJoin(Gdiplus::LineJoinRound);
        graphics.FillPath(&fill_brush, &triangle);
        graphics.DrawPath(&border_pen, &triangle);

        Gdiplus::SolidBrush mark_brush(ToGdiPlusColor(Window));
        const Gdiplus::REAL mark_width = std::max<Gdiplus::REAL>(2.0F, usable * 0.105F);
        const Gdiplus::REAL mark_height = usable * 0.34F;
        const Gdiplus::REAL mark_x = left + ((usable - mark_width) / 2.0F);
        const Gdiplus::REAL mark_y = top + (usable * 0.30F);
        Gdiplus::GraphicsPath mark;
        AddRoundedRectangle(mark,
                            Gdiplus::RectF(mark_x, mark_y, mark_width, mark_height),
                            mark_width * 0.42F);
        graphics.FillPath(&mark_brush, &mark);

        const Gdiplus::REAL dot = std::max<Gdiplus::REAL>(2.0F, usable * 0.115F);
        graphics.FillEllipse(&mark_brush,
                             left + ((usable - dot) / 2.0F),
                             top + (usable * 0.70F),
                             dot,
                             dot);
        return;
    }

    const int center_x = (rectangle.left + rectangle.right) / 2;
    POINT points[3]{{center_x, rectangle.top + 2},
                    {rectangle.left + 2, rectangle.bottom - 2},
                    {rectangle.right - 2, rectangle.bottom - 2}};
    const HBRUSH fill = CreateSolidBrush(Warning);
    const HPEN border = CreatePen(PS_SOLID, 2, Text);
    if (fill != nullptr && border != nullptr) {
        const HGDIOBJ old_fill = SelectObject(dc, fill);
        const HGDIOBJ old_border = SelectObject(dc, border);
        Polygon(dc, points, 3);
        SelectObject(dc, old_border);
        SelectObject(dc, old_fill);
    }
    if (border != nullptr) DeleteObject(border);
    if (fill != nullptr) DeleteObject(fill);
}

void DrawInformationIcon(const HDC dc, const RECT& rectangle) noexcept {
    if (dc == nullptr || rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return;
    }

    const Gdiplus::REAL width =
        static_cast<Gdiplus::REAL>(rectangle.right - rectangle.left);
    const Gdiplus::REAL height =
        static_cast<Gdiplus::REAL>(rectangle.bottom - rectangle.top);
    const Gdiplus::REAL side = std::min(width, height);

    if (GraphicsRuntime().Ready()) {
        Gdiplus::Graphics graphics(dc);
        ConfigureGraphics(graphics);

        const Gdiplus::REAL inset = std::max<Gdiplus::REAL>(1.0F, side * 0.055F);
        const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(rectangle.left) +
                                    ((width - side) / 2.0F) + inset;
        const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(rectangle.top) +
                                   ((height - side) / 2.0F) + inset;
        const Gdiplus::REAL usable = side - (inset * 2.0F);
        const Gdiplus::RectF circle(left, top, usable, usable);

        Gdiplus::LinearGradientBrush fill_brush(
            circle,
            ToGdiPlusColor(AccentGradientStart),
            ToGdiPlusColor(AccentGradientEnd),
            90.0F,
            FALSE);
        graphics.FillEllipse(&fill_brush, circle);

        const Gdiplus::REAL border_width =
            std::max<Gdiplus::REAL>(1.4F, side * 0.055F);
        Gdiplus::Pen border_pen(ToGdiPlusColor(Text), border_width);
        graphics.DrawEllipse(&border_pen, circle);

        const Gdiplus::REAL inner_inset = std::max<Gdiplus::REAL>(1.0F, side * 0.075F);
        const Gdiplus::RectF inner_ring(
            circle.X + inner_inset,
            circle.Y + inner_inset,
            circle.Width - (inner_inset * 2.0F),
            circle.Height - (inner_inset * 2.0F));
        if (inner_ring.Width > 0.0F && inner_ring.Height > 0.0F) {
            Gdiplus::Pen inner_pen(ToGdiPlusColor(Text, 78),
                                   std::max<Gdiplus::REAL>(0.8F, side * 0.018F));
            graphics.DrawEllipse(&inner_pen, inner_ring);
        }

        Gdiplus::SolidBrush highlight_brush(ToGdiPlusColor(Text, 22));
        graphics.FillEllipse(&highlight_brush,
                             left + (usable * 0.16F),
                             top + (usable * 0.11F),
                             usable * 0.68F,
                             usable * 0.20F);

        const Gdiplus::REAL dot = std::max<Gdiplus::REAL>(3.0F, usable * 0.155F);
        const Gdiplus::REAL stem_width =
            std::max<Gdiplus::REAL>(3.0F, usable * 0.145F);
        const Gdiplus::REAL stem_height = usable * 0.37F;
        const Gdiplus::REAL stem_x = left + ((usable - stem_width) / 2.0F);
        const Gdiplus::REAL stem_y = top + (usable * 0.43F);
        const Gdiplus::REAL shadow_offset =
            std::max<Gdiplus::REAL>(0.7F, side * 0.018F);

        Gdiplus::SolidBrush shadow_brush(ToGdiPlusColor(Window, 96));
        graphics.FillEllipse(&shadow_brush,
                             left + ((usable - dot) / 2.0F) + shadow_offset,
                             top + (usable * 0.205F) + shadow_offset,
                             dot,
                             dot);
        Gdiplus::GraphicsPath shadow_stem;
        AddRoundedRectangle(
            shadow_stem,
            Gdiplus::RectF(stem_x + shadow_offset,
                           stem_y + shadow_offset,
                           stem_width,
                           stem_height),
            stem_width * 0.42F);
        graphics.FillPath(&shadow_brush, &shadow_stem);

        Gdiplus::SolidBrush mark_brush(ToGdiPlusColor(Text));
        graphics.FillEllipse(&mark_brush,
                             left + ((usable - dot) / 2.0F),
                             top + (usable * 0.205F),
                             dot,
                             dot);
        Gdiplus::GraphicsPath stem;
        AddRoundedRectangle(stem,
                            Gdiplus::RectF(stem_x, stem_y, stem_width, stem_height),
                            stem_width * 0.42F);
        graphics.FillPath(&mark_brush, &stem);
        return;
    }

    const int side_pixels = static_cast<int>(side);
    const int left = rectangle.left +
                     ((rectangle.right - rectangle.left - side_pixels) / 2) + 2;
    const int top = rectangle.top +
                    ((rectangle.bottom - rectangle.top - side_pixels) / 2) + 2;
    const int right = left + side_pixels - 4;
    const int bottom = top + side_pixels - 4;
    const HBRUSH fill = CreateSolidBrush(Accent);
    const HPEN border = CreatePen(PS_SOLID, 2, Text);
    if (fill != nullptr && border != nullptr) {
        const HGDIOBJ old_fill = SelectObject(dc, fill);
        const HGDIOBJ old_border = SelectObject(dc, border);
        Ellipse(dc, left, top, right, bottom);
        SelectObject(dc, old_border);
        SelectObject(dc, old_fill);
    }
    if (border != nullptr) DeleteObject(border);
    if (fill != nullptr) DeleteObject(fill);

    const HBRUSH mark = CreateSolidBrush(Text);
    if (mark != nullptr) {
        const HGDIOBJ old_mark = SelectObject(dc, mark);
        const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        const int usable = std::max(1, right - left);
        const int center = left + (usable / 2);
        const int dot_size = std::max(3, usable * 15 / 100);
        Ellipse(dc,
                center - (dot_size / 2),
                top + (usable * 20 / 100),
                center - (dot_size / 2) + dot_size,
                top + (usable * 20 / 100) + dot_size);
        const int stem_width = std::max(3, usable * 14 / 100);
        RoundRect(dc,
                  center - (stem_width / 2),
                  top + (usable * 43 / 100),
                  center - (stem_width / 2) + stem_width,
                  top + (usable * 80 / 100),
                  stem_width,
                  stem_width);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_mark);
        DeleteObject(mark);
    }
}

COLORREF StatusIndicatorColor(const StatusCategory category) noexcept {
    switch (category) {
    case StatusCategory::Ready:
        return Success;
    case StatusCategory::Running:
        return AccentHover;
    case StatusCategory::Transition:
        return StatusTransition;
    case StatusCategory::Attention:
        return StatusAttention;
    case StatusCategory::NotReady:
        return StatusInactive;
    }
    return StatusAttention;
}

} // namespace vectorclick::win::ui

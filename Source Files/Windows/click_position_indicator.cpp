#include "Windows/click_position_indicator.h"

#include "Windows/capture_exclusion.h"
#include "Windows/shared_image_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vectorclick::win {
namespace {

constexpr wchar_t IndicatorClassName[] = L"VectorClickClickPositionIndicator";
constexpr int LogicalDiameter = 34;

int ScaleForDpi(const int value, const UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

std::uint32_t PremultipliedBgra(const std::uint8_t red,
                                const std::uint8_t green,
                                const std::uint8_t blue,
                                const std::uint8_t alpha) noexcept {
    const auto premultiply = [alpha](const std::uint8_t component) {
        return static_cast<std::uint8_t>(
            (static_cast<unsigned>(component) * alpha + 127U) / 255U);
    };
    return static_cast<std::uint32_t>(premultiply(blue)) |
           (static_cast<std::uint32_t>(premultiply(green)) << 8U) |
           (static_cast<std::uint32_t>(premultiply(red)) << 16U) |
           (static_cast<std::uint32_t>(alpha) << 24U);
}

} // namespace

ClickPositionIndicator::~ClickPositionIndicator() {
    Destroy();
}

// The indicator is a click-through layered popup owned independently from the
// main window. Keeping its surface application-owned avoids normal child-window
// repaint behavior and lets capture exclusion follow the current privacy state.
bool ClickPositionIndicator::Create(const HINSTANCE instance) noexcept {
    if (window_ != nullptr && IsWindow(window_) != FALSE) {
        return true;
    }

    instance_ = instance;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = IndicatorClassName;

    if (GetClassInfoExW(instance_, IndicatorClassName, &window_class) == FALSE) {
        window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = IndicatorClassName;
        if (RegisterClassExW(&window_class) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        IndicatorClassName,
        L"",
        WS_POPUP,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ != nullptr && IsCaptureExclusionRequested() &&
        ApplyRequestedCaptureExclusion(window_).result !=
            CaptureExclusionResult::Applied) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    return window_ != nullptr;
}

void ClickPositionIndicator::ShowAt(const ScreenPoint point, UINT dpi) noexcept {
    if ((window_ == nullptr || IsWindow(window_) == FALSE) && !Create(instance_)) {
        return;
    }

    if (dpi == 0) {
        dpi = 96;
    }
    int diameter = std::max(12, ScaleForDpi(LogicalDiameter, dpi));
    if ((diameter & 1) == 0) {
        ++diameter;
    }

    const bool surface_changed = diameter != diameter_pixels_ || bitmap_ == nullptr;
    if (!EnsureSurface(diameter) || pixels_ == nullptr || memory_dc_ == nullptr) {
        return;
    }

    const int left = point.x - diameter / 2;
    const int top = point.y - diameter / 2;
    const bool position_changed = !visible_ || point != last_point_ || dpi != last_dpi_;

    if (!visible_) {
        (void)SetWindowPos(window_,
                           HWND_TOPMOST,
                           left,
                           top,
                           diameter,
                           diameter,
                           SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);
        const CaptureExclusionOutcome indicator_affinity =
            SynchronizeRequestedCaptureExclusion(window_);
        if (IsCaptureExclusionRequested() &&
            indicator_affinity.result != CaptureExclusionResult::Applied) {
            return;
        }
    }

    if (!surface_presented_ || surface_changed) {
        if (!PresentAt(left, top)) {
            return;
        }
    } else if (position_changed) {
        if (SetWindowPos(window_,
                         HWND_TOPMOST,
                         left,
                         top,
                         diameter,
                         diameter,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW) == FALSE) {
            return;
        }
    }

    last_point_ = point;
    last_dpi_ = dpi;
    visible_ = true;
}

void ClickPositionIndicator::Hide() noexcept {
    if (window_ != nullptr && IsWindow(window_) != FALSE && visible_) {
        ShowWindow(window_, SW_HIDE);
    }
    visible_ = false;
}

void ClickPositionIndicator::Destroy() noexcept {
    Hide();

    if (memory_dc_ != nullptr && bitmap_ != nullptr) {
        if (previous_bitmap_ != nullptr) {
            SelectObject(memory_dc_, previous_bitmap_);
        }
        DeleteObject(bitmap_);
    }
    bitmap_ = nullptr;
    previous_bitmap_ = nullptr;
    pixels_ = nullptr;
    diameter_pixels_ = 0;
    surface_presented_ = false;

    if (memory_dc_ != nullptr) {
        DeleteDC(memory_dc_);
        memory_dc_ = nullptr;
    }
    if (window_ != nullptr && IsWindow(window_) != FALSE) {
        DestroyWindow(window_);
    }
    window_ = nullptr;
    visible_ = false;
}

bool ClickPositionIndicator::IsVisible() const noexcept {
    return window_ != nullptr && IsWindow(window_) != FALSE && visible_;
}

LRESULT CALLBACK ClickPositionIndicator::WindowProc(const HWND window,
                                                    const UINT message,
                                                    const WPARAM w_param,
                                                    const LPARAM l_param) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

// Rebuild the premultiplied DIB only when DPI changes the physical diameter.
// Ordinary position updates reuse the same pixels and only republish the layer.
bool ClickPositionIndicator::EnsureSurface(const int diameter_pixels) noexcept {
    if (diameter_pixels_ == diameter_pixels && bitmap_ != nullptr && pixels_ != nullptr) {
        return true;
    }

    if (memory_dc_ == nullptr) {
        memory_dc_ = CreateCompatibleDC(nullptr);
        if (memory_dc_ == nullptr) {
            return false;
        }
    }

    if (bitmap_ != nullptr) {
        if (previous_bitmap_ != nullptr) {
            SelectObject(memory_dc_, previous_bitmap_);
        }
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
        previous_bitmap_ = nullptr;
        pixels_ = nullptr;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = diameter_pixels;
    bitmap_info.bmiHeader.biHeight = -diameter_pixels;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    bitmap_ = CreateDIBSection(
        memory_dc_, &bitmap_info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
    if (bitmap_ == nullptr || pixels_ == nullptr) {
        bitmap_ = nullptr;
        pixels_ = nullptr;
        return false;
    }

    previous_bitmap_ = SelectObject(memory_dc_, bitmap_);
    if (previous_bitmap_ == nullptr || previous_bitmap_ == HGDI_ERROR) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
        previous_bitmap_ = nullptr;
        pixels_ = nullptr;
        return false;
    }

    diameter_pixels_ = diameter_pixels;
    surface_presented_ = false;
    RenderSurface();
    return true;
}

void ClickPositionIndicator::RenderSurface() noexcept {
    if (pixels_ == nullptr || diameter_pixels_ <= 0) {
        return;
    }

    auto* destination = static_cast<std::uint32_t*>(pixels_);
    const std::size_t pixel_count = static_cast<std::size_t>(diameter_pixels_) *
                                    static_cast<std::size_t>(diameter_pixels_);
    std::fill(destination, destination + pixel_count, 0U);

    const double center = static_cast<double>(diameter_pixels_ - 1) / 2.0;
    const double outer_radius = center - 0.75;
    const double outline_width = std::max(1.5, static_cast<double>(diameter_pixels_) / 13.0);
    const double inner_radius = std::max(0.0, outer_radius - outline_width);
    const std::uint32_t fill = PremultipliedBgra(45, 135, 255, 58);
    const std::uint32_t outline = PremultipliedBgra(40, 130, 255, 205);

    for (int y = 0; y < diameter_pixels_; ++y) {
        for (int x = 0; x < diameter_pixels_; ++x) {
            const double delta_x = static_cast<double>(x) - center;
            const double delta_y = static_cast<double>(y) - center;
            const double distance = std::sqrt(delta_x * delta_x + delta_y * delta_y);
            const std::size_t pixel_index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(diameter_pixels_) +
                static_cast<std::size_t>(x);
            if (distance <= inner_radius) {
                destination[pixel_index] = fill;
            } else if (distance <= outer_radius) {
                destination[pixel_index] = outline;
            }
        }
    }
}

// UpdateLayeredWindow publishes the complete indicator atomically, including
// its per-pixel alpha, before the popup is positioned above ordinary windows.
bool ClickPositionIndicator::PresentAt(const int left, const int top) noexcept {
    if (window_ == nullptr || memory_dc_ == nullptr || bitmap_ == nullptr || diameter_pixels_ <= 0) {
        return false;
    }

    const HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        return false;
    }

    POINT destination{left, top};
    SIZE size{diameter_pixels_, diameter_pixels_};
    POINT source{};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    const BOOL updated = UpdateLayeredWindow(
        window_, screen_dc, &destination, &size, memory_dc_, &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen_dc);
    if (updated == FALSE) {
        return false;
    }

    SetWindowPos(window_,
                 HWND_TOPMOST,
                 left,
                 top,
                 diameter_pixels_,
                 diameter_pixels_,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    surface_presented_ = true;
    return true;
}

} // namespace vectorclick::win

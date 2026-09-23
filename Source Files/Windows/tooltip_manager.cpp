#include "Windows/tooltip_manager.h"

#include "Windows/capture_exclusion.h"
#include "Windows/shared_image_loader.h"
#include "Windows/ui_theme.h"

#include <dwmapi.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace vectorclick::win {
namespace {

constexpr wchar_t TooltipWindowClassName[] = L"VectorClickTooltipWindow";
constexpr UINT_PTR TooltipSubclassId = 0x56435454U; // "VCTT"
constexpr UINT_PTR GeneratedTimerIdFirst = 0x1000;
constexpr UINT InitialDelayMilliseconds = 500;
constexpr UINT AutoPopMilliseconds = 40'000;
constexpr int TooltipCornerSupersample = 8;

int Scale(const int value, const UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

std::uint8_t PremultiplyChannel(const std::uint8_t component,
                                const std::uint8_t alpha) noexcept {
    return static_cast<std::uint8_t>(
        (static_cast<unsigned>(component) * static_cast<unsigned>(alpha) + 127U) /
        255U);
}

void ApplyRoundedAlphaMask(std::uint32_t* const pixels,
                           const int width,
                           const int height,
                           const int radius) noexcept {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }

    constexpr int sample_count = TooltipCornerSupersample * TooltipCornerSupersample;
    constexpr int sample_scale = TooltipCornerSupersample * 2;
    const int clamped_radius = std::clamp(radius, 0, std::min(width, height) / 2);
    const std::int64_t radius_units =
        static_cast<std::int64_t>(clamped_radius) * sample_scale;
    const std::int64_t radius_squared = radius_units * radius_units;
    const std::int64_t right_center =
        static_cast<std::int64_t>(width) * sample_scale - radius_units;
    const std::int64_t bottom_center =
        static_cast<std::int64_t>(height) * sample_scale - radius_units;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool fully_inside =
                clamped_radius == 0 ||
                (x >= clamped_radius && x < width - clamped_radius) ||
                (y >= clamped_radius && y < height - clamped_radius);
            if (fully_inside) {
                const std::size_t pixel_index =
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x);
                const std::uint32_t pixel = pixels[pixel_index];
                pixels[pixel_index] = (pixel & 0x00FFFFFFU) | 0xFF000000U;
                continue;
            }

            int covered_samples = 0;
            for (int sample_y = 0; sample_y < TooltipCornerSupersample; ++sample_y) {
                const std::int64_t y_units =
                    static_cast<std::int64_t>(y) * sample_scale + sample_y * 2 + 1;
                const std::int64_t closest_y =
                    std::clamp(y_units, radius_units, bottom_center);
                const std::int64_t delta_y = y_units - closest_y;

                for (int sample_x = 0; sample_x < TooltipCornerSupersample; ++sample_x) {
                    const std::int64_t x_units =
                        static_cast<std::int64_t>(x) * sample_scale + sample_x * 2 + 1;
                    const std::int64_t closest_x =
                        std::clamp(x_units, radius_units, right_center);
                    const std::int64_t delta_x = x_units - closest_x;
                    if ((delta_x * delta_x) + (delta_y * delta_y) <= radius_squared) {
                        ++covered_samples;
                    }
                }
            }

            const std::uint8_t alpha = static_cast<std::uint8_t>(
                (covered_samples * 255 + sample_count / 2) / sample_count);
            const std::size_t pixel_index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);
            const std::uint32_t pixel = pixels[pixel_index];
            const auto blue = static_cast<std::uint8_t>(pixel & 0xFFU);
            const auto green = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
            const auto red = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
            pixels[pixel_index] =
                static_cast<std::uint32_t>(PremultiplyChannel(blue, alpha)) |
                (static_cast<std::uint32_t>(PremultiplyChannel(green, alpha)) << 8U) |
                (static_cast<std::uint32_t>(PremultiplyChannel(red, alpha)) << 16U) |
                (static_cast<std::uint32_t>(alpha) << 24U);
        }
    }
}

} // namespace

TooltipManager::~TooltipManager() {
    for (const auto& tool : tools_) {
        if (tool.control != nullptr && IsWindow(tool.control)) {
            RemoveWindowSubclass(tool.control, ToolSubclassProc, TooltipSubclassId);
        }
    }
    if (tooltip_window_ != nullptr && IsWindow(tooltip_window_)) {
        DestroyWindow(tooltip_window_);
    }
}

bool TooltipManager::Create(const HWND owner) {
    owner_ = owner;
    instance_ = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (instance_ == nullptr) {
        instance_ = GetModuleHandleW(nullptr);
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    // Use only the application-painted outer outline for the tooltip.
    // The system drop shadow can introduce a second, rougher edge around the
    // popup corners that does not match the smoothed GDI+ outline.
    window_class.style = 0;
    window_class.lpfnWndProc = TooltipWindowProc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = TooltipWindowClassName;
    RegisterClassExW(&window_class);

    tooltip_window_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
            WS_EX_LAYERED,
        TooltipWindowClassName,
        L"",
        WS_POPUP,
        0,
        0,
        0,
        0,
        owner_,
        nullptr,
        instance_,
        this);

    if (tooltip_window_ != nullptr && IsCaptureExclusionRequested() &&
        ApplyRequestedCaptureExclusion(tooltip_window_).result !=
            CaptureExclusionResult::Applied) {
        DestroyWindow(tooltip_window_);
        tooltip_window_ = nullptr;
        return false;
    }

    if (tooltip_window_ != nullptr) {
        // The tooltip uses an application-owned per-pixel rounded alpha mask
        // and application-painted outlines. Prevent Windows 11 from layering a
        // separate compositor corner radius over that geometry, which can make
        // the outer edge overshoot the blue outline near the corners.
        constexpr DWORD RoundCornerPreferenceAttribute = 33;
        constexpr DWORD DoNotRoundCornerPreference = 1;
        (void)DwmSetWindowAttribute(tooltip_window_,
                                    RoundCornerPreferenceAttribute,
                                    &DoNotRoundCornerPreference,
                                    sizeof(DoNotRoundCornerPreference));
    }

    return tooltip_window_ != nullptr;
}

void TooltipManager::Add(const HWND control, std::wstring text) {
    if (tooltip_window_ == nullptr || control == nullptr || !IsWindow(control)) {
        return;
    }

    tools_.push_back({control, std::move(text), true});
    SetWindowSubclass(control,
                      ToolSubclassProc,
                      TooltipSubclassId,
                      reinterpret_cast<DWORD_PTR>(this));
}

void TooltipManager::SetText(const HWND control, std::wstring text) {
    const auto iterator = std::find_if(tools_.begin(), tools_.end(), [control](const Tool& tool) {
        return tool.control == control;
    });
    if (iterator == tools_.end()) {
        return;
    }
    iterator->text = std::move(text);
    if (visible_ && current_control_ == control && tooltip_window_ != nullptr) {
        RECT window_rect{};
        if (GetWindowRect(tooltip_window_, &window_rect) != FALSE) {
            const int width = window_rect.right - window_rect.left;
            const int height = window_rect.bottom - window_rect.top;
            const int inner_outline_inset = std::max(5, Scale(5, current_dpi_));
            const int blue_corner_radius = std::max(7, Scale(10, current_dpi_));
            const int outer_corner_radius = blue_corner_radius + inner_outline_inset;
            (void)PresentCurrentSurface(window_rect.left,
                                        window_rect.top,
                                        width,
                                        height,
                                        outer_corner_radius);
        }
    }
}

void TooltipManager::RefreshVisible(const HWND control) {
    if (visible_ && current_control_ == control && tooltip_window_ != nullptr) {
        ShowCurrent();
    }
}

void TooltipManager::ReassertTopmost() noexcept {
    if (!visible_ || tooltip_window_ == nullptr ||
        IsWindow(tooltip_window_) == FALSE) {
        return;
    }

    // Promoting the already-visible main window into the topmost band can
    // place it above a tooltip that was shown before the transition. Restore
    // only the tooltip's Z-order; do not move, resize, activate, or recreate it.
    (void)SetWindowPos(tooltip_window_,
                       HWND_TOPMOST,
                       0,
                       0,
                       0,
                       0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                           SWP_NOOWNERZORDER);
}

void TooltipManager::Dismiss(const HWND control) noexcept {
    if (current_control_ == control) {
        EndHover(control);
    }
}

void TooltipManager::SetActive(const HWND control, const bool active) noexcept {
    const auto iterator = std::find_if(tools_.begin(), tools_.end(), [control](const Tool& tool) {
        return tool.control == control;
    });
    if (iterator == tools_.end() || iterator->active == active) {
        return;
    }

    iterator->active = active;
    if (!active && current_control_ == control) {
        EndHover(control);
    }
}

bool TooltipManager::HasPendingOrVisible() const noexcept {
    return current_control_ != nullptr || visible_;
}

bool TooltipManager::DismissAll() noexcept {
    const bool dismissed = HasPendingOrVisible();
    Hide();
    current_control_ = nullptr;
    auto_popped_ = false;
    keyboard_focus_ = false;
    return dismissed;
}

void TooltipManager::Hide() noexcept {
    StopGeneratedTimer(initial_delay_timer_id_);
    StopGeneratedTimer(auto_pop_timer_id_);
    if (tooltip_window_ == nullptr) {
        return;
    }
    ShowWindow(tooltip_window_, SW_HIDE);
    visible_ = false;
}

LRESULT CALLBACK TooltipManager::TooltipWindowProc(const HWND window,
                                                    const UINT message,
                                                    const WPARAM w_param,
                                                    const LPARAM l_param) {
    TooltipManager* self = reinterpret_cast<TooltipManager*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<TooltipManager*>(create->lpCreateParams);
        self->tooltip_window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (self != nullptr) {
        return self->HandleTooltipMessage(message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT CALLBACK TooltipManager::ToolSubclassProc(const HWND window,
                                                  const UINT message,
                                                  const WPARAM w_param,
                                                  const LPARAM l_param,
                                                  const UINT_PTR subclass_id,
                                                  const DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<TooltipManager*>(reference_data);
    if (self != nullptr) {
        switch (message) {
        case WM_MOUSEMOVE: {
            // A successful TME_LEAVE request stays active until Windows emits
            // WM_MOUSELEAVE. Re-arming it for every generated / synthetic mouse
            // move adds avoidable User32 work on one of the hottest self-target
            // paths. Keep one tracked control and retry only after failure or
            // after the prior tracking request has been canceled by leave.
            if (self->mouse_leave_tracking_control_ != window) {
                TRACKMOUSEEVENT tracking{};
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE;
                tracking.hwndTrack = window;
                if (TrackMouseEvent(&tracking) != FALSE) {
                    self->mouse_leave_tracking_control_ = window;
                }
            }
            self->BeginHover(window, false);
            break;
        }
        case WM_MOUSELEAVE:
            if (self->mouse_leave_tracking_control_ == window) {
                self->mouse_leave_tracking_control_ = nullptr;
            }
            self->EndHover(window);
            break;
        case WM_SETFOCUS:
            // Programmatic focus changes and mouse selection should not open a
            // tooltip. Preserve keyboard accessibility only when focus moved
            // through actual Tab or Shift + Tab navigation.
            if ((GetKeyState(VK_TAB) & 0x8000) != 0) {
                self->BeginHover(window, true);
            }
            break;
        case WM_KILLFOCUS:
            self->EndHover(window);
            break;
        case WM_NCDESTROY:
            if (self->mouse_leave_tracking_control_ == window) {
                self->mouse_leave_tracking_control_ = nullptr;
            }
            self->EndHover(window);
            RemoveWindowSubclass(window, ToolSubclassProc, subclass_id);
            break;
        default:
            break;
        }
    }
    return DefSubclassProc(window, message, w_param, l_param);
}

LRESULT TooltipManager::HandleTooltipMessage(const UINT message,
                                             const WPARAM w_param,
                                             const LPARAM l_param) {
    (void)l_param;
    switch (message) {
    case WM_TIMER:
        if (initial_delay_timer_id_ != 0 &&
            w_param == initial_delay_timer_id_) {
            StopGeneratedTimer(initial_delay_timer_id_);
            ShowCurrent();
            return 0;
        }
        if (auto_pop_timer_id_ != 0 &&
            w_param == auto_pop_timer_id_) {
            StopGeneratedTimer(auto_pop_timer_id_);
            ShowWindow(tooltip_window_, SW_HIDE);
            visible_ = false;
            auto_popped_ = true;
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(tooltip_window_, &paint);
        EndPaint(tooltip_window_, &paint);
        return 0;
    }

    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        initial_delay_timer_id_ = 0;
        auto_pop_timer_id_ = 0;
        tooltip_window_ = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(tooltip_window_, message, w_param, l_param);
}

void TooltipManager::DrawCurrentSurface(const HDC dc,
                                        const RECT& client) const noexcept {
    if (dc == nullptr || client.right <= client.left || client.bottom <= client.top) {
        return;
    }

    const int blue_outline_width = std::max(2, Scale(1, current_dpi_));
    const int outer_outline_width = std::max(1, Scale(1, current_dpi_));
    const int blue_corner_radius = std::max(7, Scale(10, current_dpi_));
    // Keep the accepted nested tooltip artwork unchanged. Only the final outer
    // window alpha mask is antialiased now; the grey and blue outline geometry
    // remains consistent with the established tooltip geometry.
    const int outer_outline_inset = std::max(2, Scale(2, current_dpi_));
    const int inner_outline_inset = std::max(5, Scale(5, current_dpi_));
    const int outer_corner_radius = blue_corner_radius + inner_outline_inset;
    const int outer_outline_radius =
        std::max(4, outer_corner_radius - outer_outline_inset);

    ui::Fill(dc, client, ui::Window);

    RECT outer_outline = client;
    InflateRect(&outer_outline, -outer_outline_inset, -outer_outline_inset);
    ui::DrawRoundedOutline(dc,
                           outer_outline,
                           ui::BorderSoft,
                           outer_outline_radius,
                           outer_outline_width);

    RECT inner_outline = client;
    InflateRect(&inner_outline, -inner_outline_inset, -inner_outline_inset);
    ui::DrawRoundedOutline(dc,
                           inner_outline,
                           ui::AccentHover,
                           blue_corner_radius,
                           blue_outline_width);

    const auto* text = FindText(current_control_);
    if (text == nullptr) {
        return;
    }

    const int padding = Scale(11, current_dpi_);
    RECT text_rect{inner_outline.left + padding,
                   inner_outline.top + padding,
                   inner_outline.right - padding,
                   inner_outline.bottom - padding};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::Text);
    const HGDIOBJ tooltip_font =
        font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ old_font = SelectObject(dc, tooltip_font);
    DrawTextW(dc,
              text->c_str(),
              -1,
              &text_rect,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old_font);
}

bool TooltipManager::PresentCurrentSurface(const int x,
                                           const int y,
                                           const int width,
                                           const int height,
                                           const int outer_corner_radius) const noexcept {
    if (tooltip_window_ == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* pixel_memory = nullptr;
    const HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        return false;
    }
    UniqueGdiObject bitmap(CreateDIBSection(screen_dc,
                                            &bitmap_info,
                                            DIB_RGB_COLORS,
                                            &pixel_memory,
                                            nullptr,
                                            0));
    if (bitmap.get() == nullptr || pixel_memory == nullptr) {
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    const HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (memory_dc == nullptr) {
        ReleaseDC(nullptr, screen_dc);
        return false;
    }
    const HGDIOBJ previous_bitmap = SelectObject(memory_dc, bitmap.get());
    if (previous_bitmap == nullptr || previous_bitmap == HGDI_ERROR) {
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    RECT client{0, 0, width, height};
    DrawCurrentSurface(memory_dc, client);
    ApplyRoundedAlphaMask(static_cast<std::uint32_t*>(pixel_memory),
                          width,
                          height,
                          outer_corner_radius);

    POINT destination{x, y};
    SIZE size{width, height};
    POINT source{};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL updated = UpdateLayeredWindow(tooltip_window_,
                                             screen_dc,
                                             &destination,
                                             &size,
                                             memory_dc,
                                             &source,
                                             0,
                                             &blend,
                                             ULW_ALPHA);

    SelectObject(memory_dc, previous_bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return updated != FALSE;
}

UINT_PTR TooltipManager::StartGeneratedTimer(UINT_PTR& active_timer_id,
                                              const UINT milliseconds) noexcept {
    StopGeneratedTimer(active_timer_id);
    if (tooltip_window_ == nullptr || IsWindow(tooltip_window_) == FALSE) {
        return 0;
    }

    if (next_timer_id_ < GeneratedTimerIdFirst) {
        next_timer_id_ = GeneratedTimerIdFirst;
    }
    const UINT_PTR requested_id = next_timer_id_++;
    const UINT_PTR timer_id =
        SetTimer(tooltip_window_, requested_id, milliseconds, nullptr);
    if (timer_id != 0) {
        active_timer_id = timer_id;
    }
    return timer_id;
}

void TooltipManager::StopGeneratedTimer(UINT_PTR& active_timer_id) noexcept {
    if (active_timer_id != 0 && tooltip_window_ != nullptr &&
        IsWindow(tooltip_window_) != FALSE) {
        KillTimer(tooltip_window_, active_timer_id);
    }
    active_timer_id = 0;
}

void TooltipManager::BeginHover(const HWND control, const bool keyboard_focus) {
    if (control == nullptr || current_control_ == control) {
        return;
    }
    if (IsWindowVisible(control) == FALSE || FindText(control) == nullptr) {
        return;
    }

    Hide();
    current_control_ = control;
    keyboard_focus_ = keyboard_focus;
    auto_popped_ = false;
    if (StartGeneratedTimer(initial_delay_timer_id_,
                            InitialDelayMilliseconds) == 0) {
        ShowCurrent();
    }
}

void TooltipManager::EndHover(const HWND control) noexcept {
    if (current_control_ != control) {
        return;
    }

    Hide();
    current_control_ = nullptr;
    auto_popped_ = false;
    keyboard_focus_ = false;
}

void TooltipManager::ShowCurrent() {
    if (tooltip_window_ == nullptr || current_control_ == nullptr || auto_popped_ ||
        !IsWindow(current_control_) || IsWindowVisible(current_control_) == FALSE) {
        return;
    }

    if (!keyboard_focus_) {
        POINT client_point{};
        GetCursorPos(&client_point);
        ScreenToClient(current_control_, &client_point);
        RECT client{};
        GetClientRect(current_control_, &client);
        if (!PtInRect(&client, client_point)) {
            EndHover(current_control_);
            return;
        }
    } else if (GetFocus() != current_control_) {
        EndHover(current_control_);
        return;
    }

    const auto* text = FindText(current_control_);
    if (text == nullptr) {
        EndHover(current_control_);
        return;
    }

    current_dpi_ = GetDpiForWindow(current_control_);
    if (current_dpi_ == 0) {
        current_dpi_ = 96;
    }
    RecreateFont(current_dpi_);

    HDC dc = GetDC(tooltip_window_);
    const HGDIOBJ tooltip_font = font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ old_font = SelectObject(dc, tooltip_font);
    const int maximum_text_width = Scale(460, current_dpi_);
    RECT text_bounds{0, 0, maximum_text_width, 0};
    DrawTextW(dc,
              text->c_str(),
              -1,
              &text_bounds,
              DT_CALCRECT | DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old_font);
    ReleaseDC(tooltip_window_, dc);

    const int padding = Scale(11, current_dpi_);
    const int inner_outline_inset = std::max(5, Scale(5, current_dpi_));
    const int tooltip_width =
        std::max(Scale(180, current_dpi_), static_cast<int>(text_bounds.right) + padding * 2) +
        inner_outline_inset * 2;
    const int tooltip_height = text_bounds.bottom + padding * 2 + inner_outline_inset * 2;

    POINT anchor_position{};
    if (keyboard_focus_) {
        RECT control_rect{};
        GetWindowRect(current_control_, &control_rect);
        anchor_position.x = control_rect.left + Scale(12, current_dpi_);
        anchor_position.y = control_rect.bottom + Scale(8, current_dpi_);
    } else {
        GetCursorPos(&anchor_position);
        anchor_position.x += Scale(16, current_dpi_);
        anchor_position.y += Scale(20, current_dpi_);
    }

    const HMONITOR monitor = MonitorFromPoint(anchor_position, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoW(monitor, &monitor_info);

    int tooltip_x = anchor_position.x - inner_outline_inset;
    int tooltip_y = anchor_position.y - inner_outline_inset;
    const int minimum_x = static_cast<int>(monitor_info.rcWork.left);
    const int maximum_x = std::max(minimum_x,
                                   static_cast<int>(monitor_info.rcWork.right) - tooltip_width);
    const int minimum_y = static_cast<int>(monitor_info.rcWork.top);
    const int maximum_y = std::max(minimum_y,
                                   static_cast<int>(monitor_info.rcWork.bottom) - tooltip_height);
    tooltip_x = std::clamp(tooltip_x, minimum_x, maximum_x);
    tooltip_y = std::clamp(tooltip_y, minimum_y, maximum_y);

    SetWindowPos(tooltip_window_,
                 HWND_TOPMOST,
                 tooltip_x,
                 tooltip_y,
                 tooltip_width,
                 tooltip_height,
                 SWP_NOACTIVATE | SWP_NOREDRAW);

    const int blue_corner_radius = std::max(7, Scale(10, current_dpi_));
    const int outer_corner_radius = blue_corner_radius + inner_outline_inset;

    const CaptureExclusionOutcome tooltip_affinity =
        SynchronizeRequestedCaptureExclusion(tooltip_window_);
    if (IsCaptureExclusionRequested() &&
        tooltip_affinity.result != CaptureExclusionResult::Applied) {
        return;
    }

    if (!PresentCurrentSurface(tooltip_x,
                               tooltip_y,
                               tooltip_width,
                               tooltip_height,
                               outer_corner_radius)) {
        return;
    }

    ShowWindow(tooltip_window_, SW_SHOWNOACTIVATE);
    visible_ = true;
    if (StartGeneratedTimer(auto_pop_timer_id_, AutoPopMilliseconds) == 0) {
        ShowWindow(tooltip_window_, SW_HIDE);
        visible_ = false;
        auto_popped_ = true;
    }
}

const std::wstring* TooltipManager::FindText(const HWND control) const noexcept {
    const auto iterator = std::find_if(tools_.begin(), tools_.end(), [control](const Tool& tool) {
        return tool.control == control;
    });
    return iterator != tools_.end() && iterator->active ? &iterator->text : nullptr;
}

void TooltipManager::RecreateFont(const UINT dpi) {
    UniqueGdiObject new_font(CreateFontW(
        -MulDiv(9, static_cast<int>(dpi), 72),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"));
    if (new_font.get() != nullptr) {
        font_ = std::move(new_font);
    }
}

} // namespace vectorclick::win

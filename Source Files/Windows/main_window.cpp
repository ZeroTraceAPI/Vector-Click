#include "Windows/main_window.h"
#include "Windows/numeric_field_presenter.h"
#include "Windows/numpad_keys.h"
#include "Windows/safety_hotkey_catalog.h"
#include "Windows/shared_image_loader.h"
#include "Windows/shutdown_policy.h"

#include "Core/validation.h"
#include "Windows/admin_utils.h"
#include "Windows/app_identity.h"
#include "Windows/app_messages.h"
#include "Windows/centered_message_box.h"
#include "Windows/capture_exclusion.h"
#include "Windows/diagnostics_display.h"
#include "Windows/force_exit.h"
#include "Windows/input_backend_dispatcher.h"
#include "Windows/keyboard_character.h"
#include "Windows/startup_options.h"
#include "Windows/ui_layout.h"
#include "Windows/ui_presentation_state.h"
#include "Windows/ui_theme.h"
#include "version_config.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace vectorclick::win {
namespace {

constexpr UINT_PTR DiagnosticsTimerId = 1;
constexpr UINT_PTR TargetStatusTimerId = 3;
constexpr UINT_PTR GeneratedTimerIdFirst = 0x1000;
constexpr UINT DiagnosticsTimerMilliseconds = 250;
constexpr UINT PositionCaptureTimerMilliseconds = 1'000;
constexpr UINT TargetStatusTimerMilliseconds = 1'000;
constexpr UINT SettingsSaveDebounceMilliseconds = 1'000;
constexpr UINT StatusLayoutDebounceMilliseconds = 160;
constexpr UINT SupportEmailCopiedMilliseconds = 3'000;
constexpr UINT DiagnosticReportCopiedMilliseconds = 3'000;
constexpr UINT RunNotificationCleanupMilliseconds = 12'000;
constexpr UINT AdvancedScrollSettleMilliseconds = 72;
constexpr int PositionCaptureSeconds = 4;
constexpr int StatusTextVerticalPadding = 14;
constexpr int DiagnosticsTextVerticalPadding = 10;
constexpr std::size_t MaximumNumericEditCharacters = 32U;
constexpr wchar_t ContentHostClassName[] = L"VectorClickContentHost";
constexpr wchar_t ComboPopupClassName[] = L"VectorClickComboPopup";
constexpr wchar_t ChoiceControlClassName[] = L"VectorClickChoice";
constexpr wchar_t ResizeOverlayClassName[] = L"VectorClickResizeOverlay";
constexpr int ResizeOverlayCornerRadiusLogical = 8;
constexpr int ResizeOverlayCornerSupersample = 8;

struct BufferedPaintSurface final {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous_bitmap{};
};

void DestroyBufferedPaintSurface(BufferedPaintSurface& surface) noexcept {
    if (surface.dc != nullptr && surface.previous_bitmap != nullptr &&
        surface.previous_bitmap != HGDI_ERROR) {
        (void)SelectObject(surface.dc, surface.previous_bitmap);
    }
    if (surface.bitmap != nullptr) {
        (void)DeleteObject(surface.bitmap);
    }
    if (surface.dc != nullptr) {
        (void)DeleteDC(surface.dc);
    }
    surface = {};
}

[[nodiscard]] bool CreateBufferedPaintSurface(const HDC reference_dc,
                                              const int width,
                                              const int height,
                                              BufferedPaintSurface& surface) noexcept {
    surface = {};
    if (reference_dc == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    const HDC memory_dc = CreateCompatibleDC(reference_dc);
    if (memory_dc == nullptr) {
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(reference_dc,
                                            &info,
                                            DIB_RGB_COLORS,
                                            &pixels,
                                            nullptr,
                                            0);
    if (bitmap == nullptr || pixels == nullptr) {
        if (bitmap != nullptr) {
            (void)DeleteObject(bitmap);
        }
        (void)DeleteDC(memory_dc);
        return false;
    }

    const HGDIOBJ previous = SelectObject(memory_dc, bitmap);
    if (previous == nullptr || previous == HGDI_ERROR) {
        (void)DeleteObject(bitmap);
        (void)DeleteDC(memory_dc);
        return false;
    }

    surface.dc = memory_dc;
    surface.bitmap = bitmap;
    surface.previous_bitmap = previous;
    return true;
}

template <typename Render>
[[nodiscard]] bool PaintBufferedRegion(const HDC target_dc,
                                       const RECT& target_bounds,
                                       const COLORREF background,
                                       Render&& render) noexcept {
    const int width = target_bounds.right - target_bounds.left;
    const int height = target_bounds.bottom - target_bounds.top;
    if (target_dc == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    BufferedPaintSurface surface{};
    if (!CreateBufferedPaintSurface(target_dc, width, height, surface)) {
        return false;
    }

    const RECT local_bounds{0, 0, width, height};
    ui::Fill(surface.dc, local_bounds, background);
    render(surface.dc, local_bounds);
    (void)GdiFlush();

    const bool copied =
        BitBlt(target_dc,
               target_bounds.left,
               target_bounds.top,
               width,
               height,
               surface.dc,
               0,
               0,
               SRCCOPY) != FALSE;
    (void)GdiFlush();
    DestroyBufferedPaintSurface(surface);
    return copied;
}

[[nodiscard]] bool ClientRectInScreen(const HWND window,
                                      RECT& bounds) noexcept {
    bounds = {};
    if (window == nullptr || IsWindow(window) == FALSE) {
        return false;
    }

    RECT client{};
    POINT origin{};
    if (GetClientRect(window, &client) == FALSE ||
        ClientToScreen(window, &origin) == FALSE) {
        return false;
    }

    bounds.left = origin.x;
    bounds.top = origin.y;
    bounds.right = origin.x + (client.right - client.left);
    bounds.bottom = origin.y + (client.bottom - client.top);
    return bounds.right > bounds.left && bounds.bottom > bounds.top;
}

struct RetainedFocusPatch final {
    std::array<HWND, 12> controls{};
    std::array<RECT, 12> rects{};
    std::size_t count{};
    UniqueGdiObject bitmap;
};

[[nodiscard]] bool IsWindowWithinHost(const HWND host,
                                      const HWND candidate) noexcept {
    return host != nullptr && candidate != nullptr &&
           IsWindow(host) != FALSE && IsWindow(candidate) != FALSE &&
           (candidate == host || IsChild(host, candidate) != FALSE);
}

[[nodiscard]] bool RetainedControlRectInHost(const HWND host,
                                             const HWND control,
                                             RECT& bounds) noexcept {
    bounds = {};
    if (!IsWindowWithinHost(host, control) || control == host ||
        IsWindowVisible(control) == FALSE ||
        GetWindowRect(control, &bounds) == FALSE) {
        return false;
    }

    (void)MapWindowPoints(HWND_DESKTOP,
                          host,
                          reinterpret_cast<POINT*>(&bounds),
                          2);

    RECT client{};
    if (GetClientRect(host, &client) == FALSE) {
        return false;
    }

    RECT clipped{};
    if (IntersectRect(&clipped, &bounds, &client) == FALSE ||
        IsRectEmpty(&clipped)) {
        return false;
    }

    bounds = clipped;
    return true;
}

[[nodiscard]] bool AppendRetainedPatchControl(RetainedFocusPatch& patch,
                                              const HWND host,
                                              const HWND control) noexcept {
    if (control == nullptr) {
        return true;
    }
    if (!IsWindowWithinHost(host, control) || control == host) {
        return false;
    }

    for (std::size_t index = 0; index < patch.count; ++index) {
        if (patch.controls[index] == control) {
            return true;
        }
    }
    if (patch.count >= patch.controls.size()) {
        return false;
    }

    RECT bounds{};
    if (!RetainedControlRectInHost(host, control, bounds)) {
        return false;
    }

    patch.controls[patch.count] = control;
    patch.rects[patch.count] = bounds;
    ++patch.count;
    return true;
}

[[nodiscard]] bool CloneRetainedBitmap(const HWND reference,
                                       const HGDIOBJ source_bitmap,
                                       const int width,
                                       const int height,
                                       UniqueGdiObject& destination) noexcept {
    destination.reset();
    if (reference == nullptr || source_bitmap == nullptr ||
        width <= 0 || height <= 0) {
        return false;
    }

    const HDC reference_dc = GetDC(reference);
    if (reference_dc == nullptr) {
        return false;
    }
    const HDC source_dc = CreateCompatibleDC(reference_dc);
    const HDC destination_dc = CreateCompatibleDC(reference_dc);
    if (source_dc == nullptr || destination_dc == nullptr) {
        if (source_dc != nullptr) {
            DeleteDC(source_dc);
        }
        if (destination_dc != nullptr) {
            DeleteDC(destination_dc);
        }
        ReleaseDC(reference, reference_dc);
        return false;
    }

    const HBITMAP bitmap = CreateCompatibleBitmap(reference_dc, width, height);
    if (bitmap == nullptr) {
        DeleteDC(source_dc);
        DeleteDC(destination_dc);
        ReleaseDC(reference, reference_dc);
        return false;
    }

    const HGDIOBJ previous_source = SelectObject(source_dc, source_bitmap);
    const HGDIOBJ previous_destination =
        SelectObject(destination_dc, bitmap);
    bool copied =
        previous_source != nullptr && previous_source != HGDI_ERROR &&
        previous_destination != nullptr &&
        previous_destination != HGDI_ERROR;
    if (copied) {
        copied = BitBlt(destination_dc,
                        0,
                        0,
                        width,
                        height,
                        source_dc,
                        0,
                        0,
                        SRCCOPY) != FALSE;
    }

    if (previous_source != nullptr && previous_source != HGDI_ERROR) {
        SelectObject(source_dc, previous_source);
    }
    if (previous_destination != nullptr &&
        previous_destination != HGDI_ERROR) {
        SelectObject(destination_dc, previous_destination);
    }
    DeleteDC(source_dc);
    DeleteDC(destination_dc);
    ReleaseDC(reference, reference_dc);

    if (!copied) {
        DeleteObject(bitmap);
        return false;
    }

    destination.reset(bitmap);
    return true;
}

[[nodiscard]] bool RenderRetainedFocusPatch(const HWND host,
                                            RetainedFocusPatch& patch) noexcept {
    if (host == nullptr || patch.bitmap.get() == nullptr ||
        patch.count == 0U) {
        return false;
    }

    const HDC reference_dc = GetDC(host);
    if (reference_dc == nullptr) {
        return false;
    }
    const HDC destination_dc = CreateCompatibleDC(reference_dc);
    if (destination_dc == nullptr) {
        ReleaseDC(host, reference_dc);
        return false;
    }

    const HGDIOBJ previous_bitmap =
        SelectObject(destination_dc, patch.bitmap.get());
    if (previous_bitmap == nullptr || previous_bitmap == HGDI_ERROR) {
        DeleteDC(destination_dc);
        ReleaseDC(host, reference_dc);
        return false;
    }

    HRGN combined = CreateRectRgn(0, 0, 0, 0);
    bool rendered = combined != nullptr;
    if (rendered) {
        for (std::size_t index = 0; index < patch.count; ++index) {
            const RECT& bounds = patch.rects[index];
            HRGN region =
                CreateRectRgn(bounds.left, bounds.top, bounds.right, bounds.bottom);
            if (region == nullptr ||
                CombineRgn(combined, combined, region, RGN_OR) == ERROR) {
                rendered = false;
            }
            if (region != nullptr) {
                DeleteObject(region);
            }
            if (!rendered) {
                break;
            }
        }
    }

    if (rendered) {
        rendered = SelectClipRgn(destination_dc, combined) != ERROR;
    }
    if (rendered) {
        SendMessageW(host,
                     WM_PRINT,
                     reinterpret_cast<WPARAM>(destination_dc),
                     PRF_CLIENT | PRF_CHILDREN | PRF_CHECKVISIBLE);
        rendered = GdiFlush() != FALSE;
    }

    (void)SelectClipRgn(destination_dc, nullptr);
    if (combined != nullptr) {
        DeleteObject(combined);
    }
    SelectObject(destination_dc, previous_bitmap);
    DeleteDC(destination_dc);
    ReleaseDC(host, reference_dc);
    return rendered;
}


[[nodiscard]] bool RenderRetainedFocusPatchDirectControls(
    const HWND host,
    RetainedFocusPatch& patch) noexcept {
    if (host == nullptr || IsWindow(host) == FALSE ||
        patch.bitmap.get() == nullptr || patch.count == 0U) {
        return false;
    }

    const HDC reference_dc = GetDC(host);
    if (reference_dc == nullptr) {
        return false;
    }
    const HDC destination_dc = CreateCompatibleDC(reference_dc);
    if (destination_dc == nullptr) {
        ReleaseDC(host, reference_dc);
        return false;
    }

    const HGDIOBJ previous_bitmap =
        SelectObject(destination_dc, patch.bitmap.get());
    bool rendered = previous_bitmap != nullptr &&
                    previous_bitmap != HGDI_ERROR;

    for (std::size_t index = 0; rendered && index < patch.count; ++index) {
        const HWND control = patch.controls[index];
        if (!IsWindowWithinHost(host, control) || control == host ||
            IsWindowVisible(control) == FALSE) {
            rendered = false;
            break;
        }

        RECT full_bounds{};
        if (GetWindowRect(control, &full_bounds) == FALSE) {
            rendered = false;
            break;
        }
        (void)MapWindowPoints(HWND_DESKTOP,
                              host,
                              reinterpret_cast<POINT*>(&full_bounds),
                              2);
        const int width = full_bounds.right - full_bounds.left;
        const int height = full_bounds.bottom - full_bounds.top;
        if (width <= 0 || height <= 0) {
            rendered = false;
            break;
        }

        const int saved = SaveDC(destination_dc);
        if (saved == 0) {
            rendered = false;
            break;
        }

        POINT previous_origin{};
        if (SetViewportOrgEx(destination_dc,
                             full_bounds.left,
                             full_bounds.top,
                             &previous_origin) == FALSE) {
            (void)RestoreDC(destination_dc, saved);
            rendered = false;
            break;
        }

        const RECT& clipped = patch.rects[index];
        const int clip_left = clipped.left - full_bounds.left;
        const int clip_top = clipped.top - full_bounds.top;
        const int clip_right = clipped.right - full_bounds.left;
        const int clip_bottom = clipped.bottom - full_bounds.top;
        if (clip_right <= clip_left || clip_bottom <= clip_top ||
            IntersectClipRect(destination_dc,
                              clip_left,
                              clip_top,
                              clip_right,
                              clip_bottom) == ERROR) {
            (void)RestoreDC(destination_dc, saved);
            rendered = false;
            break;
        }

        (void)SendMessageW(control,
                           WM_PRINT,
                           reinterpret_cast<WPARAM>(destination_dc),
                           PRF_CLIENT | PRF_CHILDREN | PRF_CHECKVISIBLE);
        (void)RestoreDC(destination_dc, saved);
    }

    if (rendered) {
        rendered = GdiFlush() != FALSE;
    }
    if (previous_bitmap != nullptr && previous_bitmap != HGDI_ERROR) {
        SelectObject(destination_dc, previous_bitmap);
    }
    DeleteDC(destination_dc);
    ReleaseDC(host, reference_dc);
    return rendered;
}

std::uint8_t PremultiplyOverlayChannel(const std::uint8_t component,
                                       const std::uint8_t alpha) noexcept {
    return static_cast<std::uint8_t>(
        (static_cast<unsigned>(component) * static_cast<unsigned>(alpha) + 127U) /
        255U);
}

void ApplyBottomRoundedOverlayAlpha(std::uint32_t* const pixels,
                                    const int width,
                                    const int height,
                                    const int left_radius,
                                    const int right_radius) noexcept {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }

    constexpr std::uint32_t OpaqueAlpha = 0xFF000000U;
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    // GDI drawing into a 32-bit BI_RGB DIB does not establish a useful alpha
    // byte. Almost the entire retained resize surface is fully opaque, so make
    // that common case a single cheap alpha-byte pass. Extracting and
    // premultiplying all three color channels for pixels whose alpha is already
    // 255 would add millions of needless integer operations on a large frame.
    for (std::size_t index = 0; index < pixel_count; ++index) {
        pixels[index] |= OpaqueAlpha;
    }

    constexpr int sample_count =
        ResizeOverlayCornerSupersample * ResizeOverlayCornerSupersample;
    constexpr int sample_scale = ResizeOverlayCornerSupersample * 2;
    const int left = std::clamp(left_radius, 0, std::min(width, height) / 2);
    const int right = std::clamp(right_radius, 0, std::min(width, height) / 2);

    const auto apply_corner = [&](const bool left_corner, const int radius) noexcept {
        if (radius <= 0) {
            return;
        }
        const int start_x = left_corner ? 0 : width - radius;
        const int end_x = left_corner ? radius : width;
        const int start_y = height - radius;
        const std::int64_t center_x = static_cast<std::int64_t>(
            left_corner ? radius : width - radius) * sample_scale;
        const std::int64_t center_y =
            static_cast<std::int64_t>(height - radius) * sample_scale;
        const std::int64_t radius_units =
            static_cast<std::int64_t>(radius) * sample_scale;
        const std::int64_t radius_squared = radius_units * radius_units;

        for (int y = start_y; y < height; ++y) {
            for (int x = start_x; x < end_x; ++x) {
                int covered_samples = 0;
                for (int sy = 0; sy < ResizeOverlayCornerSupersample; ++sy) {
                    const std::int64_t py =
                        static_cast<std::int64_t>(y) * sample_scale + sy * 2 + 1;
                    for (int sx = 0; sx < ResizeOverlayCornerSupersample; ++sx) {
                        const std::int64_t px =
                            static_cast<std::int64_t>(x) * sample_scale + sx * 2 + 1;
                        const std::int64_t dx = px - center_x;
                        const std::int64_t dy = py - center_y;
                        if ((dx * dx) + (dy * dy) <= radius_squared) {
                            ++covered_samples;
                        }
                    }
                }

                const auto alpha = static_cast<std::uint8_t>(
                    (covered_samples * 255 + sample_count / 2) / sample_count);
                if (alpha == 255U) {
                    continue;
                }

                const std::size_t index =
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(x);
                const std::uint32_t pixel = pixels[index];
                const auto blue = static_cast<std::uint8_t>(pixel & 0xFFU);
                const auto green = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
                const auto red = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
                pixels[index] =
                    static_cast<std::uint32_t>(PremultiplyOverlayChannel(blue, alpha)) |
                    (static_cast<std::uint32_t>(PremultiplyOverlayChannel(green, alpha)) << 8U) |
                    (static_cast<std::uint32_t>(PremultiplyOverlayChannel(red, alpha)) << 16U) |
                    (static_cast<std::uint32_t>(alpha) << 24U);
            }
        }
    };

    apply_corner(true, left);
    apply_corner(false, right);
}
constexpr wchar_t AdvancedScrollSnapshotClassName[] = L"VectorClickAdvancedScrollSnapshot";
constexpr wchar_t AdvancedScrollbarClassName[] = L"VectorClickAdvancedScrollbar";
constexpr DWORD ChoiceRadioStyle = 0x0001U;
constexpr DWORD ChoiceCheckStyle = 0x0002U;
constexpr LONG_PTR ChoiceCheckedState = 0x0001;
constexpr LONG_PTR ChoicePressedState = 0x0002;
constexpr LONG_PTR ChoiceHotState = 0x0004;
constexpr int ChoiceOwnerOffset = 0;
constexpr int ChoiceStateOffset = static_cast<int>(sizeof(LONG_PTR));
constexpr wchar_t KeyCapturePrompt[] = L"Press a key...";
constexpr wchar_t SupportEmailCopiedStatus[] =
    L"Status: Support email copied to the clipboard.";
constexpr wchar_t SupportEmailCopiedTooltip[] =
    L"ZeroTraceAPI@proton.me is available on the Windows clipboard.";
constexpr wchar_t DiagnosticReportCopiedStatus[] =
    L"Status: Diagnostic report copied to the clipboard.";
constexpr wchar_t DiagnosticReportCopiedTooltip[] =
    L"The privacy-filtered report was generated locally and copied to the Windows clipboard. Vector Click did not transmit it.";
constexpr wchar_t OrdinaryStatusDefaultTooltip[] =
    L"Shows whether Vector Click is ready, running, stopping, or needs a setting corrected.";
constexpr wchar_t DiagnosticsStatusTooltip[] =
    L"Shows live action counts, generated inputs, measured rates, elapsed time, and readiness details. The receiving application may process fewer inputs than Vector Click sends.";
constexpr wchar_t GeneratedKeyTooltip[] =
    L"Choose the keyboard key Vector Click will press. Click the field and press a supported key, or open the list. Shifted symbols are separate choices. Numpad keys follow the current Windows Num Lock state when generated. The physical key cannot match either safety hotkey.";
constexpr wchar_t StartHotkeyTooltip[] =
    L"Choose the global hotkey used to start Vector Click when idle and request a normal stop while running. Shifted symbols include Shift automatically. It must use a different physical key from Emergency Stop and generated keyboard input.";
constexpr wchar_t EmergencyHotkeyTooltip[] =
    L"Choose the global hotkey for Emergency Stop. It cancels pending input and releases input Vector Click still tracks as held. Function keys are recommended because they rarely conflict with typing.";

[[nodiscard]] bool CapturePrintedClient(const HWND source,
                                        UniqueGdiObject& destination,
                                        int& width,
                                        int& height) noexcept {
    destination.reset();
    width = 0;
    height = 0;

    if (source == nullptr || IsWindow(source) == FALSE ||
        IsWindowVisible(source) == FALSE) {
        return false;
    }

    RECT client{};
    if (GetClientRect(source, &client) == FALSE || IsRectEmpty(&client)) {
        return false;
    }

    const int captured_width = client.right - client.left;
    const int captured_height = client.bottom - client.top;
    if (captured_width <= 0 || captured_height <= 0) {
        return false;
    }

    const HDC source_dc = GetDC(source);
    if (source_dc == nullptr) {
        return false;
    }
    const HDC memory_dc = CreateCompatibleDC(source_dc);
    if (memory_dc == nullptr) {
        ReleaseDC(source, source_dc);
        return false;
    }
    const HBITMAP bitmap =
        CreateCompatibleBitmap(source_dc, captured_width, captured_height);
    if (bitmap == nullptr) {
        DeleteDC(memory_dc);
        ReleaseDC(source, source_dc);
        return false;
    }

    const HGDIOBJ previous_bitmap = SelectObject(memory_dc, bitmap);
    if (previous_bitmap == nullptr || previous_bitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(source, source_dc);
        return false;
    }

    // WM_PRINT asks the existing window procedures and visible child controls
    // to draw into application-owned memory. Unlike a desktop BitBlt, this path
    // does not sample DWM's protected composed surface and remains usable while
    // WDA_EXCLUDEFROMCAPTURE is active.
    PatBlt(memory_dc, 0, 0, captured_width, captured_height, BLACKNESS);
    SendMessageW(source,
                 WM_PRINT,
                 reinterpret_cast<WPARAM>(memory_dc),
                 PRF_CLIENT | PRF_CHILDREN | PRF_CHECKVISIBLE);
    GdiFlush();

    SelectObject(memory_dc, previous_bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(source, source_dc);

    destination.reset(bitmap);
    width = captured_width;
    height = captured_height;
    return true;
}

template <typename Painter>
[[nodiscard]] bool PaintControlSnapshotLocally(const HDC destination,
                                                const int width,
                                                const int height,
                                                Painter&& painter) {
    if (destination == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    const HDC buffer = CreateCompatibleDC(destination);
    if (buffer == nullptr) {
        return false;
    }
    const HBITMAP bitmap = CreateCompatibleBitmap(destination, width, height);
    if (bitmap == nullptr) {
        DeleteDC(buffer);
        return false;
    }
    const HGDIOBJ previous = SelectObject(buffer, bitmap);
    if (previous == nullptr || previous == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(buffer);
        return false;
    }

    // WM_PRINT can supply a parent DC whose viewport is translated to the
    // child. GDI honors that translation, but GDI+ glyph and rounded-outline
    // rendering is not reliably equivalent on every Windows compositor path.
    // Paint each custom field in a zero-origin control-sized buffer, then copy
    // the complete frame through the destination DC's ordinary GDI mapping.
    std::forward<Painter>(painter)(buffer);
    const BOOL copied = BitBlt(destination,
                               0,
                               0,
                               width,
                               height,
                               buffer,
                               0,
                               0,
                               SRCCOPY);

    SelectObject(buffer, previous);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    return copied != FALSE;
}

bool OpenExternalDestination(const HWND owner,
                             const wchar_t* const destination,
                             const wchar_t* const description) {
    if (destination == nullptr || destination[0] == L'\0') {
        return false;
    }

    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        owner, L"open", destination, nullptr, nullptr, SW_SHOWNORMAL));
    if (result > 32) {
        return true;
    }

    std::wstring message = L"Vector Click could not open ";
    message += description != nullptr ? description : L"the selected destination";
    message += L" through the default Windows application.";
    ShowCenteredMessageBox(owner,
                           message.c_str(),
                           L"Could not open destination",
                           MB_OK | MB_ICONWARNING);
    return false;
}

bool CopyUnicodeTextToClipboard(const HWND owner,
                                const std::wstring_view text) noexcept {
    if (text.empty() ||
        text.size() > (std::numeric_limits<SIZE_T>::max() / sizeof(wchar_t)) - 1U) {
        return false;
    }

    if (OpenClipboard(owner) == FALSE) {
        return false;
    }

    bool copied = false;
    HGLOBAL memory = nullptr;
    if (EmptyClipboard() != FALSE) {
        const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
        memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
            if (destination != nullptr) {
                std::copy(text.begin(), text.end(), destination);
                destination[text.size()] = L'\0';
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                    copied = true;
                    memory = nullptr;
                }
            }
        }
    }

    if (memory != nullptr) {
        GlobalFree(memory);
    }
    CloseClipboard();
    return copied;
}

std::wstring SafeAsciiDiagnosticText(const char* text,
                                     const std::size_t maximum_length = 64U) {
    if (text == nullptr) {
        return {};
    }
    std::wstring result;
    result.reserve(maximum_length);
    for (std::size_t index = 0; index < maximum_length && text[index] != '\0'; ++index) {
        const unsigned char value = static_cast<unsigned char>(text[index]);
        if (value < 0x20U || value > 0x7EU) {
            result.push_back(L'?');
        } else {
            result.push_back(static_cast<wchar_t>(value));
        }
    }
    return result;
}

core::DiagnosticArchitecture DiagnosticArchitectureFromMachine(const WORD machine) noexcept {
    switch (machine) {
    case PROCESSOR_ARCHITECTURE_INTEL: return core::DiagnosticArchitecture::X86;
    case PROCESSOR_ARCHITECTURE_AMD64: return core::DiagnosticArchitecture::X64;
    case PROCESSOR_ARCHITECTURE_ARM: return core::DiagnosticArchitecture::Arm;
    case PROCESSOR_ARCHITECTURE_ARM64: return core::DiagnosticArchitecture::Arm64;
    default: return core::DiagnosticArchitecture::Unknown;
    }
}

core::DiagnosticArchitecture ApplicationDiagnosticArchitecture() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    return core::DiagnosticArchitecture::X64;
#elif defined(_M_IX86) || defined(__i386__)
    return core::DiagnosticArchitecture::X86;
#elif defined(_M_ARM64) || defined(__aarch64__)
    return core::DiagnosticArchitecture::Arm64;
#elif defined(_M_ARM) || defined(__arm__)
    return core::DiagnosticArchitecture::Arm;
#else
    return core::DiagnosticArchitecture::Unknown;
#endif
}

core::DiagnosticHostOs DiagnosticHostOsFromWine(const char* sysname) noexcept {
    if (sysname == nullptr || sysname[0] == '\0') {
        return core::DiagnosticHostOs::Unknown;
    }
    if (_stricmp(sysname, "Linux") == 0) {
        return core::DiagnosticHostOs::Linux;
    }
    if (_stricmp(sysname, "Darwin") == 0) {
        return core::DiagnosticHostOs::MacOs;
    }
    if (_stricmp(sysname, "FreeBSD") == 0) {
        return core::DiagnosticHostOs::FreeBsd;
    }
    return core::DiagnosticHostOs::OtherUnix;
}

core::DiagnosticEnvironment CaptureDiagnosticEnvironment(const HWND window,
                                                         const bool elevated) noexcept {
    core::DiagnosticEnvironment environment;
    environment.application_elevated = elevated;
    environment.application_architecture = ApplicationDiagnosticArchitecture();

    SYSTEM_INFO system_info{};
    GetNativeSystemInfo(&system_info);
    environment.native_architecture =
        DiagnosticArchitectureFromMachine(system_info.wProcessorArchitecture);

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (GetVersionExW(&version) != FALSE) {
        environment.windows_version_available = true;
        environment.windows_major = version.dwMajorVersion;
        environment.windows_minor = version.dwMinorVersion;
        environment.windows_build = version.dwBuildNumber;
    }

    const UINT dpi = window != nullptr && IsWindow(window) != FALSE
                         ? GetDpiForWindow(window)
                         : 0U;
    if (dpi != 0U) {
        environment.ui_scaling_percent =
            static_cast<std::uint32_t>(MulDiv(static_cast<int>(dpi), 100, 96));
    }

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        using WineGetVersion = const char* (__cdecl*)();
        using WineGetHostVersion = void (__cdecl*)(const char**, const char**);
        const auto get_wine_version = reinterpret_cast<WineGetVersion>(
            GetProcAddress(ntdll, "wine_get_version"));
        if (get_wine_version != nullptr) {
            environment.wine_detected = true;
            environment.wine_version = SafeAsciiDiagnosticText(get_wine_version());
            environment.host_os = core::DiagnosticHostOs::Unknown;
            const auto get_host_version = reinterpret_cast<WineGetHostVersion>(
                GetProcAddress(ntdll, "wine_get_host_version"));
            if (get_host_version != nullptr) {
                const char* sysname = nullptr;
                get_host_version(&sysname, nullptr);
                environment.host_os = DiagnosticHostOsFromWine(sysname);
            }
        }
    }
    return environment;
}

core::DiagnosticEngineState ToDiagnosticEngineState(const EngineState state) noexcept {
    switch (state) {
    case EngineState::Ready: return core::DiagnosticEngineState::Ready;
    case EngineState::Running: return core::DiagnosticEngineState::Running;
    case EngineState::Stopping: return core::DiagnosticEngineState::Stopping;
    case EngineState::Disarmed: return core::DiagnosticEngineState::Disarmed;
    case EngineState::Faulted: return core::DiagnosticEngineState::Faulted;
    }
    return core::DiagnosticEngineState::Faulted;
}

core::DiagnosticTargetElevation ToDiagnosticTargetElevation(
    const TargetElevation elevation) noexcept {
    switch (elevation) {
    case TargetElevation::Standard: return core::DiagnosticTargetElevation::Standard;
    case TargetElevation::Elevated: return core::DiagnosticTargetElevation::Elevated;
    case TargetElevation::Unknown: return core::DiagnosticTargetElevation::Unknown;
    }
    return core::DiagnosticTargetElevation::Unknown;
}

bool DiagnosticBackendRequiresTarget(const core::InputBackend backend) noexcept {
    return backend == core::InputBackend::ForegroundTargetInput ||
           backend == core::InputBackend::TargetedWindowMessages ||
           backend == core::InputBackend::TargetedUnicodeText;
}

bool IsNumericTextValid(const std::wstring_view text,
                        const bool allow_sign,
                        const bool allow_decimal) noexcept {
    if (text.empty()) {
        return true;
    }

    std::size_t index = 0;
    if (allow_sign && text.front() == L'-') {
        index = 1;
        if (index == text.size()) {
            return true;
        }
    }

    bool decimal_seen = false;
    std::size_t fractional_digits = 0;
    for (; index < text.size(); ++index) {
        const wchar_t character = text[index];
        if (character >= L'0' && character <= L'9') {
            if (decimal_seen && ++fractional_digits > 3U) {
                return false;
            }
            continue;
        }
        if (allow_decimal && character == L'.' && !decimal_seen) {
            decimal_seen = true;
            continue;
        }
        return false;
    }
    return true;
}

std::wstring ReplaceSelectedText(const HWND edit,
                                 const std::wstring_view inserted) {
    std::wstring current;
    const int length = GetWindowTextLengthW(edit);
    if (length > 0) {
        current.resize(static_cast<std::size_t>(length) + 1U);
        GetWindowTextW(edit, current.data(), length + 1);
        current.resize(static_cast<std::size_t>(length));
    }

    DWORD selection_start = 0;
    DWORD selection_end = 0;
    SendMessageW(edit,
                 EM_GETSEL,
                 reinterpret_cast<WPARAM>(&selection_start),
                 reinterpret_cast<LPARAM>(&selection_end));
    selection_start = std::min<DWORD>(selection_start, static_cast<DWORD>(current.size()));
    selection_end = std::min<DWORD>(selection_end, static_cast<DWORD>(current.size()));
    if (selection_start > selection_end) {
        std::swap(selection_start, selection_end);
    }

    std::wstring candidate;
    candidate.reserve(current.size() - (selection_end - selection_start) + inserted.size());
    candidate.append(current, 0, selection_start);
    candidate.append(inserted);
    candidate.append(current, selection_end, std::wstring::npos);
    return candidate;
}

std::wstring_view TrimNumericText(std::wstring_view text) noexcept {
    while (!text.empty() && std::iswspace(text.front()) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::iswspace(text.back()) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

bool TryParseUnsignedIntegerText(std::wstring_view text,
                                 std::uint64_t& output) noexcept {
    text = TrimNumericText(text);
    if (text.empty()) {
        return false;
    }

    std::uint64_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = (value * 10U) + digit;
    }

    output = value;
    return true;
}

bool TryParseSignedIntegerText(std::wstring_view text,
                               std::int64_t& output) noexcept {
    text = TrimNumericText(text);
    if (text.empty()) {
        return false;
    }

    bool negative = false;
    if (text.front() == L'-') {
        negative = true;
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }

    constexpr std::uint64_t negative_limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
    const std::uint64_t limit = negative
                                    ? negative_limit
                                    : static_cast<std::uint64_t>(
                                          std::numeric_limits<std::int64_t>::max());
    std::uint64_t magnitude = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - L'0');
        if (magnitude > (limit - digit) / 10U) {
            return false;
        }
        magnitude = (magnitude * 10U) + digit;
    }

    if (!negative) {
        output = static_cast<std::int64_t>(magnitude);
    } else if (magnitude == negative_limit) {
        output = std::numeric_limits<std::int64_t>::min();
    } else {
        output = -static_cast<std::int64_t>(magnitude);
    }
    return true;
}

bool TryParseMillisecondsText(std::wstring_view text,
                              std::uint64_t& output_microseconds) noexcept {
    text = TrimNumericText(text);
    if (text.empty()) {
        return false;
    }

    std::uint64_t whole_milliseconds = 0;
    std::uint32_t fractional_microseconds = 0;
    std::size_t fractional_digits = 0;
    bool decimal_seen = false;
    bool digit_seen = false;

    constexpr std::uint64_t maximum_microseconds =
        std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t maximum_whole_milliseconds =
        maximum_microseconds / 1'000U;

    for (const wchar_t character : text) {
        if (character == L'.' && !decimal_seen) {
            decimal_seen = true;
            continue;
        }
        if (character < L'0' || character > L'9') {
            return false;
        }

        digit_seen = true;
        const std::uint32_t digit =
            static_cast<std::uint32_t>(character - L'0');
        if (!decimal_seen) {
            if (whole_milliseconds >
                (maximum_whole_milliseconds - digit) / 10U) {
                return false;
            }
            whole_milliseconds = (whole_milliseconds * 10U) + digit;
            continue;
        }

        if (fractional_digits >= 3U) {
            return false;
        }
        fractional_microseconds =
            (fractional_microseconds * 10U) + digit;
        ++fractional_digits;
    }

    if (!digit_seen) {
        return false;
    }
    while (fractional_digits < 3U) {
        fractional_microseconds *= 10U;
        ++fractional_digits;
    }

    std::uint64_t microseconds = whole_milliseconds * 1'000U;
    if (microseconds > maximum_microseconds - fractional_microseconds) {
        return false;
    }
    microseconds += fractional_microseconds;

    output_microseconds = microseconds;
    return true;
}

bool SetEnabledIfChanged(const HWND control, const bool enabled) noexcept {
    if (control == nullptr || IsWindow(control) == FALSE) {
        return false;
    }
    const bool currently_enabled = IsWindowEnabled(control) != FALSE;
    if (currently_enabled == enabled) {
        return false;
    }
    EnableWindow(control, enabled ? TRUE : FALSE);
    return true;
}

int Scale(const int value, const UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

PresentationEngineState ToPresentationEngineState(const EngineState state) noexcept {
    switch (state) {
    case EngineState::Ready:
        return PresentationEngineState::Ready;
    case EngineState::Running:
        return PresentationEngineState::Running;
    case EngineState::Stopping:
        return PresentationEngineState::Stopping;
    case EngineState::Disarmed:
        return PresentationEngineState::Disarmed;
    case EngineState::Faulted:
        return PresentationEngineState::Faulted;
    }
    return PresentationEngineState::Faulted;
}

} // namespace

MainWindow::MainWindow(
    const HINSTANCE instance,
    std::optional<TargetWindowIdentity> startup_target_restore,
    const std::optional<int> startup_page_restore,
    std::optional<core::RunSettings> startup_settings_restore,
    std::optional<std::wstring> startup_profile_restore,
    const bool startup_settings_restore_invalid)
    : instance_(instance),
      running_as_administrator_(IsRunningAsAdministrator()),
      startup_target_restore_(std::move(startup_target_restore)),
      startup_settings_restore_(std::move(startup_settings_restore)),
      startup_profile_restore_(std::move(startup_profile_restore)),
      startup_settings_restore_invalid_(startup_settings_restore_invalid) {
    selected_page_index_ = std::clamp(startup_page_restore.value_or(0), 0, 2);

    const auto add_hotkey = [this](const std::wstring& name,
                                   const std::uint16_t virtual_key,
                                   const std::uint16_t modifiers = 0) {
        const core::HotkeyBinding binding{virtual_key, modifiers};
        assert(IsSupportedSafetyHotkeyBinding(binding));
        hotkey_choices_.emplace_back(name, binding);
    };
    const auto add_generated_key = [this](const std::wstring& name,
                                          const std::uint16_t virtual_key,
                                          const std::uint16_t modifiers = 0) {
        generated_key_choices_.emplace_back(
            name, core::HotkeyBinding{virtual_key, modifiers});
    };

    for (std::uint16_t key = VK_F1; key <= VK_F12; ++key) {
        add_hotkey(L"F" + std::to_wstring(key - VK_F1 + 1), key);
    }
    for (wchar_t key = L'A'; key <= L'Z'; ++key) {
        add_hotkey(std::wstring(1, key), static_cast<std::uint16_t>(key));
    }
    for (wchar_t key = L'0'; key <= L'9'; ++key) {
        add_hotkey(std::wstring(1, key), static_cast<std::uint16_t>(key));
    }

    const auto add_shifted_number_row = [&](auto&& add_choice) {
        for (const auto& choice :
             std::initializer_list<std::tuple<const wchar_t*, std::uint16_t>>{
                 {L"!", L'1'},
                 {L"@", L'2'},
                 {L"#", L'3'},
                 {L"$", L'4'},
                 {L"%", L'5'},
                 {L"^", L'6'},
                 {L"&", L'7'},
                 {L"*", L'8'},
                 {L"(", L'9'},
                 {L")", L'0'},
             }) {
            add_choice(std::get<0>(choice),
                       std::get<1>(choice),
                       core::KeyModifierShift);
        }
    };

    const auto add_us_punctuation = [&](auto&& add_choice) {
        for (const auto& choice :
             std::initializer_list<
                 std::tuple<const wchar_t*, std::uint16_t, std::uint16_t>>{
                 {L"`", VK_OEM_3, 0},
                 {L"~", VK_OEM_3, core::KeyModifierShift},
                 {L"-", VK_OEM_MINUS, 0},
                 {L"_", VK_OEM_MINUS, core::KeyModifierShift},
                 {L"=", VK_OEM_PLUS, 0},
                 {L"+", VK_OEM_PLUS, core::KeyModifierShift},
                 {L"[", VK_OEM_4, 0},
                 {L"{", VK_OEM_4, core::KeyModifierShift},
                 {L"]", VK_OEM_6, 0},
                 {L"}", VK_OEM_6, core::KeyModifierShift},
                 {L"\\", VK_OEM_5, 0},
                 {L"|", VK_OEM_5, core::KeyModifierShift},
                 {L";", VK_OEM_1, 0},
                 {L":", VK_OEM_1, core::KeyModifierShift},
                 {L"'", VK_OEM_7, 0},
                 {L"\"", VK_OEM_7, core::KeyModifierShift},
                 {L",", VK_OEM_COMMA, 0},
                 {L"<", VK_OEM_COMMA, core::KeyModifierShift},
                 {L".", VK_OEM_PERIOD, 0},
                 {L">", VK_OEM_PERIOD, core::KeyModifierShift},
                 {L"/", VK_OEM_2, 0},
                 {L"?", VK_OEM_2, core::KeyModifierShift},
             }) {
            add_choice(std::get<0>(choice),
                       std::get<1>(choice),
                       std::get<2>(choice));
        }
    };

    add_shifted_number_row(add_hotkey);
    add_us_punctuation(add_hotkey);
    for (const auto& choice :
         std::initializer_list<std::pair<const wchar_t*, std::uint16_t>>{
             {L"Numpad Multiply", VK_MULTIPLY},
             {L"Numpad Add", VK_ADD},
             {L"Numpad Subtract", VK_SUBTRACT},
             {L"Numpad Divide", VK_DIVIDE},
         }) {
        add_hotkey(choice.first, choice.second);
    }

    for (wchar_t key = L'A'; key <= L'Z'; ++key) {
        add_generated_key(std::wstring(1, key), static_cast<std::uint16_t>(key));
    }
    for (wchar_t key = L'0'; key <= L'9'; ++key) {
        add_generated_key(std::wstring(1, key), static_cast<std::uint16_t>(key));
    }
    add_shifted_number_row(add_generated_key);
    add_us_punctuation(add_generated_key);

    for (std::uint16_t key = VK_F1; key <= VK_F12; ++key) {
        add_generated_key(L"F" + std::to_wstring(key - VK_F1 + 1), key);
    }
    for (const auto& choice :
         std::initializer_list<std::pair<const wchar_t*, std::uint16_t>>{
             {L"Space", VK_SPACE},
             {L"Enter", VK_RETURN},
             {L"Tab", VK_TAB},
             {L"Backspace", VK_BACK},
             {L"Escape", VK_ESCAPE},
             {L"Up Arrow", VK_UP},
             {L"Down Arrow", VK_DOWN},
             {L"Left Arrow", VK_LEFT},
             {L"Right Arrow", VK_RIGHT},
             {L"Home", VK_HOME},
             {L"End", VK_END},
             {L"Page Up", VK_PRIOR},
             {L"Page Down", VK_NEXT},
             {L"Insert", VK_INSERT},
             {L"Delete", VK_DELETE},
             {L"Numpad 0", VK_NUMPAD0},
             {L"Numpad 1", VK_NUMPAD1},
             {L"Numpad 2", VK_NUMPAD2},
             {L"Numpad 3", VK_NUMPAD3},
             {L"Numpad 4", VK_NUMPAD4},
             {L"Numpad 5", VK_NUMPAD5},
             {L"Numpad 6", VK_NUMPAD6},
             {L"Numpad 7", VK_NUMPAD7},
             {L"Numpad 8", VK_NUMPAD8},
             {L"Numpad 9", VK_NUMPAD9},
             {L"Numpad Multiply", VK_MULTIPLY},
             {L"Numpad Add", VK_ADD},
             {L"Numpad Subtract", VK_SUBTRACT},
             {L"Numpad Decimal", VK_DECIMAL},
             {L"Numpad Divide", VK_DIVIDE},
         }) {
        add_generated_key(choice.first, choice.second);
    }

    for (const auto& choice :
         std::initializer_list<std::pair<const wchar_t*, std::uint16_t>>{
             {L"Numpad Insert", VK_NUMPAD0},
             {L"Numpad End", VK_NUMPAD1},
             {L"Numpad Down Arrow", VK_NUMPAD2},
             {L"Numpad Page Down", VK_NUMPAD3},
             {L"Numpad Left Arrow", VK_NUMPAD4},
             {L"Numpad Clear", VK_NUMPAD5},
             {L"Numpad Right Arrow", VK_NUMPAD6},
             {L"Numpad Home", VK_NUMPAD7},
             {L"Numpad Up Arrow", VK_NUMPAD8},
             {L"Numpad Page Up", VK_NUMPAD9},
             {L"Numpad Delete", VK_DECIMAL},
         }) {
        add_generated_key(choice.first, choice.second, core::KeyModifierShift);
    }

}

MainWindow::~MainWindow() {
    if (window_ != nullptr && IsWindow(window_)) {
        DestroyWindow(window_);
    }
}

bool MainWindow::Create() {
    window_background_brush_.reset(CreateSolidBrush(ui::Window));
    content_background_brush_.reset(CreateSolidBrush(ui::WindowAlt));

    WNDCLASSEXW content_class{};
    content_class.cbSize = sizeof(content_class);
    content_class.lpfnWndProc = ContentHostProc;
    content_class.hInstance = instance_;
    content_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    content_class.hbrBackground = reinterpret_cast<HBRUSH>(content_background_brush_.get());
    content_class.lpszClassName = ContentHostClassName;
    RegisterClassExW(&content_class);

    WNDCLASSEXW combo_popup_class{};
    combo_popup_class.cbSize = sizeof(combo_popup_class);
    combo_popup_class.lpfnWndProc = ComboPopupProc;
    combo_popup_class.hInstance = instance_;
    combo_popup_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    combo_popup_class.hbrBackground = reinterpret_cast<HBRUSH>(content_background_brush_.get());
    combo_popup_class.lpszClassName = ComboPopupClassName;
    RegisterClassExW(&combo_popup_class);

    WNDCLASSEXW choice_class{};
    choice_class.cbSize = sizeof(choice_class);
    choice_class.lpfnWndProc = ChoiceControlProc;
    choice_class.cbWndExtra = static_cast<int>(sizeof(LONG_PTR) * 2U);
    choice_class.hInstance = instance_;
    choice_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    choice_class.hbrBackground = reinterpret_cast<HBRUSH>(content_background_brush_.get());
    choice_class.lpszClassName = ChoiceControlClassName;
    RegisterClassExW(&choice_class);

    WNDCLASSEXW resize_overlay_class{};
    resize_overlay_class.cbSize = sizeof(resize_overlay_class);
    resize_overlay_class.lpfnWndProc = ResizeOverlayProc;
    resize_overlay_class.hInstance = instance_;
    resize_overlay_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    // The overlay paints a complete double-buffered frame. A class
    // background brush would erase to black before WM_PAINT and become
    // visible when Windows repeats rejected minimum-size resize messages.
    resize_overlay_class.hbrBackground = nullptr;
    resize_overlay_class.lpszClassName = ResizeOverlayClassName;
    RegisterClassExW(&resize_overlay_class);

    WNDCLASSEXW advanced_scroll_snapshot_class{};
    advanced_scroll_snapshot_class.cbSize = sizeof(advanced_scroll_snapshot_class);
    advanced_scroll_snapshot_class.lpfnWndProc = AdvancedScrollSnapshotProc;
    advanced_scroll_snapshot_class.hInstance = instance_;
    advanced_scroll_snapshot_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    // The snapshot paints one complete retained viewport frame. Do not let a
    // class brush erase behind that frame before WM_PAINT.
    advanced_scroll_snapshot_class.hbrBackground = nullptr;
    advanced_scroll_snapshot_class.lpszClassName =
        AdvancedScrollSnapshotClassName;
    RegisterClassExW(&advanced_scroll_snapshot_class);

    WNDCLASSEXW advanced_scrollbar_class{};
    advanced_scrollbar_class.cbSize = sizeof(advanced_scrollbar_class);
    advanced_scrollbar_class.lpfnWndProc = AdvancedScrollbarProc;
    advanced_scrollbar_class.hInstance = instance_;
    advanced_scrollbar_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_HAND);
    advanced_scrollbar_class.hbrBackground = nullptr;
    advanced_scrollbar_class.lpszClassName = AdvancedScrollbarClassName;
    RegisterClassExW(&advanced_scrollbar_class);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    window_class.hIcon = LoadSharedDefaultIcon(
        instance_, MAKEINTRESOURCEW(VectorClickIconResourceId));
    window_class.hIconSm = LoadSharedDefaultIcon(
        instance_, MAKEINTRESOURCEW(VectorClickIconResourceId));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(window_background_brush_.get());
    window_class.lpszClassName = MainWindowClassName;
    RegisterClassExW(&window_class);

    std::wstring window_title = ProductDisplayName;
    if (running_as_administrator_) {
        window_title += L" (Administrator)";
    }

    window_ = CreateWindowExW(
        0,
        MainWindowClassName,
        window_title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        layout::InitialWindowWidth,
        layout::InitialWindowHeight,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) {
        return false;
    }

    // Permit only the benign single-instance handshake through UIPI so an
    // unelevated duplicate can notify an elevated Vector Click instance. No
    // settings, pointers, or arbitrary payload are accepted through this route.
    CHANGEFILTERSTRUCT notice_filter{};
    notice_filter.cbSize = sizeof(notice_filter);
    (void)ChangeWindowMessageFilterEx(
        window_,
        WM_APP_SINGLE_INSTANCE_NOTICE_REQUEST,
        MSGFLT_ALLOW,
        &notice_filter);

    ui::ApplyDarkTitleBar(window_);
    dpi_ = GetDpiForWindow(window_);
    const int window_width = Scale(layout::InitialWindowWidth, dpi_);
    const int window_height = Scale(layout::InitialWindowHeight, dpi_);

    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info)) {
        const int work_width = monitor_info.rcWork.right - monitor_info.rcWork.left;
        const int work_height = monitor_info.rcWork.bottom - monitor_info.rcWork.top;
        const int window_x = monitor_info.rcWork.left + std::max(0, (work_width - window_width) / 2);
        const int window_y = monitor_info.rcWork.top + std::max(0, (work_height - window_height) / 2);

        SetWindowPos(window_,
                     nullptr,
                     window_x,
                     window_y,
                     window_width,
                     window_height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(window_,
                     nullptr,
                     0,
                     0,
                     window_width,
                     window_height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return true;
}

LRESULT CALLBACK MainWindow::WindowProc(const HWND window,
                                        const UINT message,
                                        const WPARAM w_param,
                                        const LPARAM l_param) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self != nullptr) {
        return self->HandleMessage(message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT CALLBACK MainWindow::ContentHostProc(const HWND window,
                                             const UINT message,
                                             const WPARAM w_param,
                                             const LPARAM l_param) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (self != nullptr) {
        const auto paint_client = [self, window](const HDC dc) {
            if (dc == nullptr) {
                return;
            }

            RECT client{};
            if (GetClientRect(window, &client) == FALSE) {
                return;
            }
            const bool outer_host = window == self->content_host_;
            const bool startup_cover = window == self->startup_cover_;
            const COLORREF host_background =
                (startup_cover || outer_host) ? ui::Window : ui::WindowAlt;
            ui::Fill(dc, client, host_background);
            if (!outer_host) {
                return;
            }

            RECT shell = client;
            shell.top += Scale(layout::TabDockTop, self->dpi_);
            const int shell_radius = Scale(14, self->dpi_);

            int dock_left = shell.right - shell_radius;
            int dock_right = shell.left + shell_radius;
            const auto include_tab = [&](const HWND tab) {
                if (tab == nullptr || IsWindow(tab) == FALSE) {
                    return;
                }
                RECT tab_bounds{};
                if (GetWindowRect(tab, &tab_bounds) == FALSE) {
                    return;
                }
                MapWindowPoints(HWND_DESKTOP,
                                window,
                                reinterpret_cast<POINT*>(&tab_bounds),
                                2);
                dock_left = std::min(dock_left,
                                     static_cast<int>(tab_bounds.left));
                dock_right = std::max(dock_right,
                                      static_cast<int>(tab_bounds.right));
            };
            include_tab(self->basic_tab_button_);
            include_tab(self->advanced_tab_button_);
            include_tab(self->about_tab_button_);
            if (dock_right < dock_left) {
                dock_left = shell.left + shell_radius;
                dock_right = shell.right - shell_radius;
            }

            // Above the shell, retain the main-window surface rather than
            // painting a full-width secondary strip. The shell outline is open
            // only beneath the actual tabs so the tab dock and page body read as
            // one connected component.
            const int local_dock_left = dock_left - shell.left;
            const int local_dock_right = dock_right - shell.left;
            const bool buffered = PaintBufferedRegion(
                dc,
                shell,
                ui::Window,
                [&](const HDC buffered_dc, const RECT& local_bounds) noexcept {
                    ui::FillOutsideRoundedPanel(buffered_dc,
                                                local_bounds,
                                                ui::Window,
                                                shell_radius);
                    ui::DrawDockedPageShell(buffered_dc,
                                            local_bounds,
                                            ui::WindowAlt,
                                            ui::BorderSoft,
                                            shell_radius,
                                            local_dock_left,
                                            local_dock_right);
                });
            if (!buffered) {
                // Preserve the accepted direct-HDC path if an application-owned
                // memory surface cannot be prepared or published.
                ui::FillOutsideRoundedPanel(dc,
                                            shell,
                                            ui::Window,
                                            shell_radius);
                ui::DrawDockedPageShell(dc,
                                        shell,
                                        ui::WindowAlt,
                                        ui::BorderSoft,
                                        shell_radius,
                                        dock_left,
                                        dock_right);
            }
        };

        switch (message) {
        case WM_MOUSEWHEEL:
            if (window == self->advanced_page_host_ &&
                self->ScrollAdvancedWheel(w_param, l_param)) {
                return 0;
            }
            break;
        case WM_COMMAND:
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_NOTIFY:
            return SendMessageW(self->window_, message, w_param, l_param);
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            paint_client(dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint_client(reinterpret_cast<HDC>(w_param));
            return 0;
        case WM_ERASEBKGND: {
            const HDC dc = reinterpret_cast<HDC>(w_param);
            RECT client{};
            GetClientRect(window, &client);
            ui::Fill(dc,
                     client,
                     (window == self->startup_cover_ ||
                      window == self->content_host_)
                         ? ui::Window
                         : ui::WindowAlt);
            return 1;
        }
        default:
            break;
        }
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT CALLBACK MainWindow::ResizeOverlayProc(const HWND window,
                                                const UINT message,
                                                const WPARAM w_param,
                                                const LPARAM l_param) {
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }

    if (self != nullptr) {
        const auto paint_overlay = [self, window](const HDC dc) {
            if (dc == nullptr) {
                return;
            }

            RECT client{};
            if (GetClientRect(window, &client) == FALSE) {
                return;
            }
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            if (width <= 0 || height <= 0) {
                return;
            }

            if (window == self->resize_overlay_guard_) {
                // The guard is a static child underlay. It never carries
                // retained controls or participates in per-frame geometry;
                // it only supplies the same outer-window background when a
                // parent growth step temporarily exposes client pixels beyond
                // the previous retained frame.
                ui::Fill(dc, client, ui::Window);
                return;
            }

            // Build the background and retained UI frame off-screen, then
            // publish them with one BitBlt. This prevents the black erase
            // frame from becoming visible when the user keeps dragging past
            // an enforced minimum width or height.
            const HDC buffer = CreateCompatibleDC(dc);
            const HBITMAP buffer_bitmap =
                buffer != nullptr ? CreateCompatibleBitmap(dc, width, height)
                                  : nullptr;
            HGDIOBJ previous_buffer = nullptr;
            if (buffer != nullptr && buffer_bitmap != nullptr) {
                previous_buffer = SelectObject(buffer, buffer_bitmap);
            }
            const bool buffered = previous_buffer != nullptr &&
                                  previous_buffer != HGDI_ERROR;
            const HDC target = buffered ? buffer : dc;
            ui::Fill(target, client, ui::Window);

            if (self->move_cover_bitmap_.get() != nullptr &&
                self->move_cover_width_ > 0 &&
                self->move_cover_height_ > 0) {
                const HDC source = CreateCompatibleDC(target);
                if (source != nullptr) {
                    const HGDIOBJ previous =
                        SelectObject(source, self->move_cover_bitmap_.get());
                    if (previous != nullptr && previous != HGDI_ERROR) {
                        BitBlt(target,
                               self->resize_overlay_content_x_,
                               self->resize_overlay_content_y_,
                               self->move_cover_width_,
                               self->move_cover_height_,
                               source,
                               0,
                               0,
                               SRCCOPY);
                        SelectObject(source, previous);
                    }
                    DeleteDC(source);
                }
            }

            if (buffered) {
                BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, previous_buffer);
            }
            if (buffer_bitmap != nullptr) {
                DeleteObject(buffer_bitmap);
            }
            if (buffer != nullptr) {
                DeleteDC(buffer);
            }
        };

        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            paint_overlay(dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint_overlay(reinterpret_cast<HDC>(w_param));
            return 0;
        case WM_ERASEBKGND:
            // WM_PAINT publishes the complete background and UI frame in one
            // buffered copy, so no separate erase is required.
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            break;
        }
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT CALLBACK MainWindow::AdvancedScrollSnapshotProc(
    const HWND window,
    const UINT message,
    const WPARAM w_param,
    const LPARAM l_param) {
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }

    if (self == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    const auto paint_snapshot = [self, window](const HDC dc) {
        if (dc == nullptr) {
            return;
        }

        RECT client{};
        if (GetClientRect(window, &client) == FALSE) {
            return;
        }
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0) {
            return;
        }

        const HDC buffer = CreateCompatibleDC(dc);
        const HBITMAP buffer_bitmap =
            buffer != nullptr ? CreateCompatibleBitmap(dc, width, height)
                              : nullptr;
        HGDIOBJ previous_buffer = nullptr;
        if (buffer != nullptr && buffer_bitmap != nullptr) {
            previous_buffer = SelectObject(buffer, buffer_bitmap);
        }
        const bool buffered = previous_buffer != nullptr &&
                              previous_buffer != HGDI_ERROR;
        const HDC target = buffered ? buffer : dc;
        ui::Fill(target, client, ui::WindowAlt);

        if (self->advanced_scroll_snapshot_bitmap_.get() != nullptr &&
            self->advanced_scroll_snapshot_width_ > 0 &&
            self->advanced_scroll_snapshot_height_ > 0) {
            const int source_y = std::clamp(
                Scale(self->advanced_scroll_offset_logical_, self->dpi_),
                0,
                self->advanced_scroll_snapshot_height_);
            const int copy_width = std::min(
                width, self->advanced_scroll_snapshot_width_);
            const int copy_height = std::min(
                height,
                self->advanced_scroll_snapshot_height_ - source_y);
            if (copy_width > 0 && copy_height > 0) {
                const HDC source = CreateCompatibleDC(target);
                if (source != nullptr) {
                    const HGDIOBJ previous = SelectObject(
                        source,
                        self->advanced_scroll_snapshot_bitmap_.get());
                    if (previous != nullptr && previous != HGDI_ERROR) {
                        BitBlt(target,
                               0,
                               0,
                               copy_width,
                               copy_height,
                               source,
                               0,
                               source_y,
                               SRCCOPY);
                        SelectObject(source, previous);
                    }
                    DeleteDC(source);
                }
            }
        }

        if (buffered) {
            BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previous_buffer);
        }
        if (buffer_bitmap != nullptr) {
            DeleteObject(buffer_bitmap);
        }
        if (buffer != nullptr) {
            DeleteDC(buffer);
        }
    };

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        paint_snapshot(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_snapshot(reinterpret_cast<HDC>(w_param));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        // The retained frame is visual only. The live controls have already
        // moved to the same offset underneath it, so input authority remains
        // with the actual Advanced child windows.
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT CALLBACK MainWindow::AdvancedScrollbarProc(
    const HWND window,
    const UINT message,
    const WPARAM w_param,
    const LPARAM l_param) {
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }

    if (self == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    const auto paint_scrollbar = [self, window](const HDC dc,
                                                const bool stable_snapshot) {
        if (dc == nullptr) {
            return;
        }
        RECT client{};
        if (GetClientRect(window, &client) == FALSE) {
            return;
        }
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0) {
            return;
        }

        // The thumb changes position for every scroll step. Painting the track
        // and thumb directly into the visible window DC can expose an
        // intermediate background-only frame. Build the complete narrow
        // scrollbar off-screen and publish it with one copy instead.
        const HDC buffer = CreateCompatibleDC(dc);
        const HBITMAP buffer_bitmap =
            buffer != nullptr ? CreateCompatibleBitmap(dc, width, height)
                              : nullptr;
        HGDIOBJ previous_buffer = nullptr;
        if (buffer != nullptr && buffer_bitmap != nullptr) {
            previous_buffer = SelectObject(buffer, buffer_bitmap);
        }
        const bool buffered = previous_buffer != nullptr &&
                              previous_buffer != HGDI_ERROR;
        const HDC target = buffered ? buffer : dc;

        ui::Fill(target, client, ui::WindowAlt);
        if (self->AdvancedMaximumScrollLogical() > 0) {
            const RECT track = self->AdvancedScrollbarTrackRect();
            const RECT thumb =
                self->AdvancedScrollbarThumbRect(stable_snapshot);
            ui::DrawRoundedPanel(
                target,
                track,
                ui::ScrollbarTrack,
                ui::ScrollbarTrack,
                std::max(1, static_cast<int>((track.right - track.left) / 2)));
            const ui::ScrollbarVisualState scrollbar_state =
                stable_snapshot
                    ? ui::ScrollbarVisualState::Idle
                    : ui::ScrollbarState(self->advanced_scroll_hovered_,
                                         self->advanced_scroll_dragging_);
            const COLORREF thumb_color =
                ui::ScrollbarThumbColor(scrollbar_state);
            ui::DrawRoundedPanel(
                target,
                thumb,
                thumb_color,
                thumb_color,
                std::max(1, static_cast<int>((thumb.right - thumb.left) / 2)));
        }

        if (buffered) {
            BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previous_buffer);
        }
        if (buffer_bitmap != nullptr) {
            DeleteObject(buffer_bitmap);
        }
        if (buffer != nullptr) {
            DeleteDC(buffer);
        }
    };

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        paint_scrollbar(dc, false);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_scrollbar(reinterpret_cast<HDC>(w_param), true);
        return 0;
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        if (self->AdvancedMaximumScrollLogical() <= 0) {
            return 0;
        }
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const RECT track = self->AdvancedScrollbarTrackRect();
        const RECT thumb = self->AdvancedScrollbarThumbRect();

        // The hovered thumb is deliberately a little wider than the visible
        // track. Give every visible thumb pixel the same drag authority that
        // its highlight advertises. Only non-thumb clicks are constrained to
        // the narrower track so a near-miss beside the track remains inert.
        if (PtInRect(&thumb, point) != FALSE) {
            self->advanced_scroll_dragging_ = true;
            self->advanced_scroll_drag_offset_ = point.y - thumb.top;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        } else {
            // The scrollbar window is intentionally wider than its visible
            // track so the narrow control has a little breathing room in the
            // gutter. Treat those side margins as inert: a near-miss beside
            // the visible track must not trigger a page-up / page-down jump.
            if (PtInRect(&track, point) == FALSE) {
                return 0;
            }

            const int page_delta = std::max(
                1,
                (self->advanced_viewport_height_logical_ * 4) / 5);
            self->ScrollAdvancedBy(point.y < thumb.top ? -page_delta
                                                       : page_delta);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!self->advanced_scroll_hovered_) {
            self->advanced_scroll_hovered_ = true;
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window;
            (void)TrackMouseEvent(&tracking);
            InvalidateRect(window, nullptr, FALSE);
        }
        if (self->advanced_scroll_dragging_ && GetCapture() == window) {
            const RECT track = self->AdvancedScrollbarTrackRect();
            const RECT thumb = self->AdvancedScrollbarThumbRect();
            const int thumb_height = thumb.bottom - thumb.top;
            const int travel = (track.bottom - track.top) - thumb_height;
            if (travel > 0) {
                const int requested_top =
                    GET_Y_LPARAM(l_param) - self->advanced_scroll_drag_offset_;
                const int thumb_top = std::clamp(
                    requested_top,
                    static_cast<int>(track.top),
                    static_cast<int>(track.bottom) - thumb_height);
                const int maximum = self->AdvancedMaximumScrollLogical();
                const int offset = static_cast<int>(
                    (static_cast<long long>(thumb_top - track.top) * maximum +
                     (travel / 2)) /
                    travel);
                (void)self->SetAdvancedScrollOffset(offset, false);
            }
            return 0;
        }
        break;
    }

    case WM_MOUSELEAVE:
        self->advanced_scroll_hovered_ = false;
        if (!self->advanced_scroll_dragging_) {
            InvalidateRect(window, nullptr, FALSE);
        }

        // The movement cover can be refreshed immediately after thumb drag
        // release while the pointer is still over the scrollbar. That exact
        // screen frame legitimately contains the blue hover thumb, but it is
        // no longer a valid retained movement frame after the pointer leaves.
        // Mark only the movement cache stale here so a later title-bar drag
        // cannot replay that transient hover state. The live scrollbar itself
        // keeps the ordinary mouse-leave repaint above.
        self->MarkMoveCoverPresentationDirty(true);
        return 0;

    case WM_LBUTTONUP:
        if (self->advanced_scroll_dragging_) {
            self->advanced_scroll_dragging_ = false;
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            InvalidateRect(window, nullptr, FALSE);
            self->FinishAdvancedScrollSnapshot();
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (self->advanced_scroll_dragging_) {
            self->advanced_scroll_dragging_ = false;
            InvalidateRect(window, nullptr, FALSE);
            self->FinishAdvancedScrollSnapshot();
        }
        return 0;

    case WM_MOUSEWHEEL:
        return self->ScrollAdvancedWheel(w_param, l_param) ? 0 :
               DefWindowProcW(window, message, w_param, l_param);

    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT CALLBACK MainWindow::ChoiceControlProc(const HWND window,
                                                   const UINT message,
                                                   const WPARAM w_param,
                                                   const LPARAM l_param) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(window,
                          ChoiceOwnerOffset,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        SetWindowLongPtrW(window, ChoiceStateOffset, 0);
    }

    auto* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, ChoiceOwnerOffset));
    if (self == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    const auto state = [window]() noexcept {
        return GetWindowLongPtrW(window, ChoiceStateOffset);
    };
    const auto set_state = [window](const LONG_PTR value) noexcept {
        SetWindowLongPtrW(window, ChoiceStateOffset, value);
    };
    const auto update_flag = [&state, &set_state](const LONG_PTR flag,
                                                  const bool enabled) noexcept {
        LONG_PTR value = state();
        value = enabled ? (value | flag) : (value & ~flag);
        set_state(value);
    };
    const auto radio = [window]() noexcept {
        return (static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)) &
                ChoiceRadioStyle) != 0U;
    };
    const auto checked = [&state]() noexcept {
        return (state() & ChoiceCheckedState) != 0;
    };
    const auto notify_clicked = [window, &radio, &checked, &update_flag]() {
        if (IsWindowEnabled(window) == FALSE) {
            return;
        }
        if (radio()) {
            update_flag(ChoiceCheckedState, true);
        } else {
            update_flag(ChoiceCheckedState, !checked());
        }
        InvalidateRect(window, nullptr, FALSE);
        const HWND parent = GetParent(window);
        if (parent != nullptr && IsWindow(parent) != FALSE) {
            SendMessageW(parent,
                         WM_COMMAND,
                         MAKEWPARAM(GetDlgCtrlID(window), BN_CLICKED),
                         reinterpret_cast<LPARAM>(window));
        }
    };
    const auto paint = [self, window, &state, &radio](const HDC target_dc) {
        if (target_dc == nullptr || IsWindow(window) == FALSE) {
            return;
        }
        RECT bounds{};
        if (GetClientRect(window, &bounds) == FALSE || IsRectEmpty(&bounds)) {
            return;
        }

        HDC memory_dc = CreateCompatibleDC(target_dc);
        HBITMAP bitmap = memory_dc != nullptr
                             ? CreateCompatibleBitmap(target_dc,
                                                      bounds.right - bounds.left,
                                                      bounds.bottom - bounds.top)
                             : nullptr;
        HGDIOBJ old_bitmap = nullptr;
        HDC draw_dc = target_dc;
        if (memory_dc != nullptr && bitmap != nullptr) {
            old_bitmap = SelectObject(memory_dc, bitmap);
            draw_dc = memory_dc;
        }

        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_BUTTON;
        draw.CtlID = static_cast<UINT>(GetDlgCtrlID(window));
        draw.hwndItem = window;
        draw.hDC = draw_dc;
        draw.rcItem = bounds;
        const LONG_PTR current = state();
        if ((current & ChoicePressedState) != 0) {
            draw.itemState |= ODS_SELECTED;
        }
        if ((current & ChoiceHotState) != 0) {
            draw.itemState |= ODS_HOTLIGHT;
        }
        if (GetFocus() == window) {
            draw.itemState |= ODS_FOCUS;
        }
        if (IsWindowEnabled(window) == FALSE) {
            draw.itemState |= ODS_DISABLED;
        }
        self->DrawChoiceControl(draw, radio());

        if (draw_dc != target_dc) {
            BitBlt(target_dc,
                   0,
                   0,
                   bounds.right - bounds.left,
                   bounds.bottom - bounds.top,
                   draw_dc,
                   0,
                   0,
                   SRCCOPY);
            SelectObject(memory_dc, old_bitmap);
            DeleteObject(bitmap);
            DeleteDC(memory_dc);
        } else if (memory_dc != nullptr) {
            DeleteDC(memory_dc);
        }
    };

    switch (message) {
    case WM_NCDESTROY:
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        SetWindowLongPtrW(window, ChoiceOwnerOffset, 0);
        SetWindowLongPtrW(window, ChoiceStateOffset, 0);
        return DefWindowProcW(window, message, w_param, l_param);

    case WM_GETDLGCODE:
        return radio() ? (DLGC_RADIOBUTTON | DLGC_WANTARROWS) : DLGC_BUTTON;

    case BM_GETCHECK:
        return checked() ? BST_CHECKED : BST_UNCHECKED;

    case BM_SETCHECK:
        update_flag(ChoiceCheckedState, w_param == BST_CHECKED);
        InvalidateRect(window, nullptr, FALSE);
        return 0;

    case BM_GETSTATE: {
        LRESULT result = 0;
        if ((state() & ChoicePressedState) != 0) {
            result |= BST_PUSHED;
        }
        if (GetFocus() == window) {
            result |= BST_FOCUS;
        }
        return result;
    }

    case BM_SETSTATE:
        update_flag(ChoicePressedState, w_param != 0);
        InvalidateRect(window, nullptr, FALSE);
        return 0;

    case BM_CLICK:
        notify_clicked();
        return 0;

    case WM_LBUTTONDOWN:
        if (IsWindowEnabled(window) != FALSE) {
            SetFocus(window);
            SetCapture(window);
            update_flag(ChoicePressedState, true);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP: {
        const bool was_pressed = (state() & ChoicePressedState) != 0;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        update_flag(ChoicePressedState, false);
        RECT bounds{};
        GetClientRect(window, &bounds);
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        InvalidateRect(window, nullptr, FALSE);
        if (was_pressed && PtInRect(&bounds, point) != FALSE) {
            notify_clicked();
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        update_flag(ChoicePressedState, false);
        InvalidateRect(window, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE:
        if ((state() & ChoiceHotState) == 0) {
            update_flag(ChoiceHotState, true);
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window;
            TrackMouseEvent(&tracking);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSELEAVE:
        update_flag(ChoiceHotState, false);
        InvalidateRect(window, nullptr, FALSE);
        return 0;

    case WM_KEYDOWN:
        if (w_param == VK_SPACE && IsWindowEnabled(window) != FALSE) {
            update_flag(ChoicePressedState, true);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (radio() && IsWindowEnabled(window) != FALSE &&
            (w_param == VK_LEFT || w_param == VK_UP ||
             w_param == VK_RIGHT || w_param == VK_DOWN)) {
            HWND peer = nullptr;
            if (window == self->current_cursor_radio_) {
                peer = self->fixed_position_radio_;
            } else if (window == self->fixed_position_radio_) {
                peer = self->current_cursor_radio_;
            } else if (window == self->unlimited_radio_) {
                peer = self->limited_radio_;
            } else if (window == self->limited_radio_) {
                peer = self->unlimited_radio_;
            }
            if (peer != nullptr && IsWindowEnabled(peer) != FALSE) {
                SetFocus(peer);
                SendMessageW(peer, BM_CLICK, 0, 0);
            }
            return 0;
        }
        break;

    case WM_KEYUP:
        if (w_param == VK_SPACE) {
            const bool was_pressed = (state() & ChoicePressedState) != 0;
            update_flag(ChoicePressedState, false);
            InvalidateRect(window, nullptr, FALSE);
            if (was_pressed) {
                notify_clicked();
            }
            return 0;
        }
        break;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SETFONT:
        InvalidateRect(window, nullptr, FALSE);
        return DefWindowProcW(window, message, w_param, l_param);

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint_structure{};
        const HDC dc = BeginPaint(window, &paint_structure);
        paint(dc);
        EndPaint(window, &paint_structure);
        return 0;
    }

    case WM_PRINTCLIENT:
        paint(reinterpret_cast<HDC>(w_param));
        return 0;

    default:
        break;
    }

    const LRESULT result = DefWindowProcW(window, message, w_param, l_param);
    if (message == WM_SETTEXT) {
        InvalidateRect(window, nullptr, FALSE);
    }
    return result;
}

LRESULT CALLBACK MainWindow::ComboPopupProc(const HWND window,
                                             const UINT message,
                                             const WPARAM w_param,
                                             const LPARAM l_param) {
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (self == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const RECT scrollbar_gutter = self->ComboPopupScrollbarGutter();
        const bool scrollbar_hovered =
            IsRectEmpty(&scrollbar_gutter) == FALSE &&
            PtInRect(&scrollbar_gutter, point) != FALSE;
        bool redraw = false;
        if (scrollbar_hovered != self->combo_popup_scroll_hovered_) {
            self->combo_popup_scroll_hovered_ = scrollbar_hovered;
            redraw = true;
        }

        if (self->combo_popup_scroll_dragging_) {
            const RECT track = self->ComboPopupScrollbarTrack();
            const RECT thumb = self->ComboPopupScrollbarThumb();
            const int thumb_height = std::max(
                1,
                static_cast<int>(thumb.bottom - thumb.top));
            const int travel = std::max(
                0,
                static_cast<int>(track.bottom - track.top) - thumb_height);
            const int highest_top = std::max(
                0,
                self->combo_popup_item_count_ - self->combo_popup_visible_rows_);
            if (travel > 0 && highest_top > 0) {
                const int thumb_top = std::clamp(
                    point.y - self->combo_popup_scroll_drag_offset_,
                    track.top,
                    track.bottom - thumb_height);
                const int top_index = MulDiv(
                    thumb_top - track.top,
                    highest_top,
                    travel);
                if (top_index != self->combo_popup_top_index_) {
                    self->combo_popup_top_index_ = top_index;
                    self->combo_popup_hover_ = -1;
                    redraw = true;
                }
            }
            if (redraw) {
                (void)self->RenderComboPopup();
            }
            return 0;
        }

        const int hover = self->ComboPopupItemAtPoint(point);
        if (hover != self->combo_popup_hover_) {
            self->combo_popup_hover_ = hover;
            redraw = true;
        }
        if (redraw) {
            (void)self->RenderComboPopup();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        RECT client{};
        GetClientRect(window, &client);
        if (PtInRect(&client, point) == FALSE) {
            self->CloseComboPopup(false);
            return 0;
        }

        const RECT thumb = self->ComboPopupScrollbarThumb();
        const RECT track = self->ComboPopupScrollbarTrack();
        if (PtInRect(&thumb, point) != FALSE && !IsRectEmpty(&thumb)) {
            self->combo_popup_scroll_hovered_ = true;
            self->combo_popup_scroll_dragging_ = true;
            self->combo_popup_scroll_drag_offset_ = point.y - thumb.top;
            (void)self->RenderComboPopup();
            return 0;
        }
        if (PtInRect(&track, point) != FALSE && !IsRectEmpty(&track)) {
            const int page = std::max(1, self->combo_popup_visible_rows_ - 1);
            self->ScrollComboPopup(point.y < thumb.top ? -page : page);
            return 0;
        }

        const int item = self->ComboPopupItemAtPoint(point);
        if (item >= 0) {
            self->MoveComboPopupHighlight(item);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        if (self->combo_popup_scroll_dragging_) {
            self->combo_popup_scroll_dragging_ = false;
            const RECT scrollbar_gutter = self->ComboPopupScrollbarGutter();
            self->combo_popup_scroll_hovered_ =
                IsRectEmpty(&scrollbar_gutter) == FALSE &&
                PtInRect(&scrollbar_gutter, point) != FALSE;
            (void)self->RenderComboPopup();
            return 0;
        }
        const int item = self->ComboPopupItemAtPoint(point);
        if (item >= 0) {
            self->MoveComboPopupHighlight(item);
            self->CloseComboPopup(true);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        const int wheel_delta = GET_WHEEL_DELTA_WPARAM(w_param);
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL) {
            lines = static_cast<UINT>(std::max(1, self->combo_popup_visible_rows_ - 1));
        }
        const int row_delta = -
            (wheel_delta / WHEEL_DELTA) * static_cast<int>(std::max<UINT>(1, lines));
        self->ScrollComboPopup(row_delta);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(l_param) != window &&
            self->combo_popup_owner_ != nullptr) {
            self->CloseComboPopup(false);
        }
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, w_param, l_param);

    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT CALLBACK MainWindow::ComboSubclassProc(const HWND window,
                                                const UINT message,
                                                const WPARAM w_param,
                                                const LPARAM l_param,
                                                const UINT_PTR subclass_id,
                                                const DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<MainWindow*>(reference_data);

    if (message == WM_NCDESTROY) {
        if (self != nullptr && self->IsKeyCaptureCombo(window)) {
            self->EndKeyCapture();
        }
        if (self != nullptr && self->IsComboPopupOpenFor(window)) {
            self->combo_popup_owner_ = nullptr;
            self->combo_popup_scroll_hovered_ = false;
            self->combo_popup_scroll_dragging_ = false;
            if (GetCapture() == self->combo_popup_) {
                ReleaseCapture();
            }
            if (self->combo_popup_ != nullptr &&
                IsWindow(self->combo_popup_) != FALSE) {
                ShowWindow(self->combo_popup_, SW_HIDE);
            }
        }
        RemoveWindowSubclass(window, ComboSubclassProc, subclass_id);
        return DefSubclassProc(window, message, w_param, l_param);
    }

    if (self != nullptr && message == CB_GETDROPPEDSTATE) {
        if (self->IsComboPopupOpenFor(window)) {
            return TRUE;
        }
        return DefSubclassProc(window, message, w_param, l_param);
    }

    if (self != nullptr && message == CB_SHOWDROPDOWN) {
        if (w_param != FALSE) {
            if (self->IsKeyCaptureCombo(window)) {
                self->EndKeyCapture();
            }
            if (self->OpenComboPopup(window)) {
                InvalidateRect(window, nullptr, FALSE);
                return TRUE;
            }
            return DefSubclassProc(window, message, w_param, l_param);
        } else if (self->IsComboPopupOpenFor(window)) {
            self->CloseComboPopup(false);
        }
        InvalidateRect(window, nullptr, FALSE);
        return TRUE;
    }

    if (self != nullptr && IsWindowEnabled(window) != FALSE) {
        const bool pending_hotkey_selector =
            self->hotkey_registration_pending_ &&
            (window == self->start_hotkey_combo_ ||
             window == self->emergency_hotkey_combo_);
        if (pending_hotkey_selector) {
            switch (message) {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            case WM_MOUSEWHEEL:
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_CHAR:
            case WM_SYSCHAR:
                return 0;
            default:
                break;
            }
        }

        const bool popup_open = self->IsComboPopupOpenFor(window);
        const UINT key = static_cast<UINT>(w_param);
        const bool key_message = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool alt_down = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool captures_keys = window == self->generated_key_combo_ ||
                                   window == self->start_hotkey_combo_ ||
                                   window == self->emergency_hotkey_combo_;

        if (self->IsKeyCaptureCombo(window)) {
            if (message == WM_KILLFOCUS) {
                self->EndKeyCapture();
            } else if (message == WM_CHAR || message == WM_SYSCHAR) {
                return 0;
            } else if (key_message) {
                if (self->CaptureKeyForCombo(window, key)) {
                    return 0;
                }
                if (key == VK_ESCAPE && window != self->generated_key_combo_) {
                    self->EndKeyCapture();
                    return 0;
                }
                if (key == VK_TAB && window != self->generated_key_combo_) {
                    self->EndKeyCapture();
                } else if (key != VK_SHIFT && key != VK_CONTROL &&
                           key != VK_MENU && key != VK_LWIN && key != VK_RWIN) {
                    MessageBeep(MB_OK);
                    return 0;
                } else {
                    return 0;
                }
            }
        }

        if (popup_open && message == WM_CHAR) {
            const wchar_t typed = static_cast<wchar_t>(w_param);
            if (std::iswalnum(typed) != 0 && self->combo_popup_item_count_ > 0) {
                const wchar_t target = static_cast<wchar_t>(std::towupper(typed));
                const int start = std::max(0, self->combo_popup_highlight_ + 1);
                for (int offset = 0; offset < self->combo_popup_item_count_; ++offset) {
                    const int index = (start + offset) % self->combo_popup_item_count_;
                    const LRESULT length = SendMessageW(window,
                                                        CB_GETLBTEXTLEN,
                                                        static_cast<WPARAM>(index),
                                                        0);
                    if (length <= 0) {
                        continue;
                    }
                    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
                    if (SendMessageW(window,
                                     CB_GETLBTEXT,
                                     static_cast<WPARAM>(index),
                                     reinterpret_cast<LPARAM>(text.data())) == CB_ERR) {
                        continue;
                    }
                    if (static_cast<wchar_t>(std::towupper(text.front())) == target) {
                        self->MoveComboPopupHighlight(index);
                        break;
                    }
                }
            }
            return 0;
        }

        if (popup_open && key_message) {
            switch (key) {
            case VK_ESCAPE:
                self->CloseComboPopup(false);
                return 0;
            case VK_RETURN:
            case VK_SPACE:
                self->CloseComboPopup(true);
                return 0;
            case VK_F4:
                self->CloseComboPopup(true);
                return 0;
            case VK_UP:
                if (alt_down) {
                    self->CloseComboPopup(true);
                } else {
                    self->MoveComboPopupHighlight(self->combo_popup_highlight_ - 1);
                }
                return 0;
            case VK_DOWN:
                if (alt_down) {
                    self->CloseComboPopup(true);
                } else {
                    self->MoveComboPopupHighlight(self->combo_popup_highlight_ + 1);
                }
                return 0;
            case VK_HOME:
                self->MoveComboPopupHighlight(0);
                return 0;
            case VK_END:
                self->MoveComboPopupHighlight(self->combo_popup_item_count_ - 1);
                return 0;
            case VK_PRIOR:
                self->MoveComboPopupHighlight(
                    self->combo_popup_highlight_ -
                    std::max(1, self->combo_popup_visible_rows_ - 1));
                return 0;
            case VK_NEXT:
                self->MoveComboPopupHighlight(
                    self->combo_popup_highlight_ +
                    std::max(1, self->combo_popup_visible_rows_ - 1));
                return 0;
            case VK_TAB:
            {
                const bool previous = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const HWND next = GetNextDlgTabItem(self->content_host_,
                                                    window,
                                                    previous ? TRUE : FALSE);
                self->CloseComboPopup(true);
                if (next != nullptr && IsWindow(next) != FALSE &&
                    IsWindowEnabled(next) != FALSE && IsWindowVisible(next) != FALSE) {
                    SetFocus(next);
                }
                return 0;
            }
            default:
                break;
            }
        }

        if (!popup_open) {
            const bool opening_key = key_message &&
                (key == VK_F4 || key == VK_SPACE ||
                 (alt_down && (key == VK_DOWN || key == VK_UP)));
            const bool mouse_open = message == WM_LBUTTONDOWN ||
                                    message == WM_LBUTTONDBLCLK;
            if (mouse_open && captures_keys) {
                RECT bounds{};
                GetClientRect(window, &bounds);
                const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(window));
                const int arrow_width = Scale(28, control_dpi);
                const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
                if (point.x < bounds.right - arrow_width) {
                    self->BeginKeyCapture(window);
                    return 0;
                }
                self->EndKeyCapture();
            }
            if (mouse_open || opening_key) {
                SetFocus(window);
                if (self->OpenComboPopup(window)) {
                    return 0;
                }
            }
        }
    }

    if (message == WM_ERASEBKGND) {
        const HDC dc = reinterpret_cast<HDC>(w_param);
        RECT bounds{};
        GetClientRect(window, &bounds);
        ui::Fill(dc, bounds, ui::SurfaceAlt);
        return 1;
    }

    if (message == WM_PRINTCLIENT && self != nullptr) {
        self->DrawPrintedCombo(window, reinterpret_cast<HDC>(w_param));
        return 0;
    }

    if (message == WM_PAINT && self != nullptr) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        self->DrawPrintedCombo(window, dc);
        EndPaint(window, &paint);
        return 0;
    }

    bool input_type_precaptured = false;
    LRESULT input_type_selection_before = CB_ERR;
    if (self != nullptr && window == self->action_type_combo_) {
        const UINT key = static_cast<UINT>(w_param);
        const bool selection_key =
            (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
            (key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END ||
             key == VK_PRIOR || key == VK_NEXT || key == VK_F4 ||
             key == VK_RETURN || key == VK_SPACE);
        const bool may_change_selection =
            message == WM_LBUTTONDOWN || message == WM_MOUSEWHEEL ||
            selection_key || message == WM_CHAR;
        if (may_change_selection) {
            // Query combo state only for input messages that can actually alter
            // the selection. Querying it for every subclassed message causes
            // CB_GETDROPPEDSTATE to re-enter this subclass recursively.
            const bool list_open =
                SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
            if (!list_open) {
                // Capture the last stable frame before the native combo box
                // changes its selected text. The deferred mode update can then
                // cover every intermediate child-window state with the old
                // frame rather than a mixed Mouse / Keyboard frame.
                input_type_selection_before =
                    SendMessageW(window, CB_GETCURSEL, 0, 0);
                input_type_precaptured = self->CaptureBasicTopSnapshot();
            }
        }
    }

    const LRESULT result = DefSubclassProc(window, message, w_param, l_param);
    if (self == nullptr) {
        return result;
    }

    if (input_type_precaptured && window == self->action_type_combo_) {
        const bool list_open_after =
            SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
        const LRESULT selection_after =
            SendMessageW(window, CB_GETCURSEL, 0, 0);
        if (!list_open_after &&
            selection_after == input_type_selection_before &&
            !self->input_type_update_pending_ &&
            !self->input_type_update_posted_) {
            // Keys or wheel movement at the end of the list can leave the
            // selection unchanged and therefore produce no combo notification.
            // Discard that unused bitmap instead of retaining a stale frame.
            self->HideBasicTopSnapshot();
        }
    }

    // Native closed-combo wheel handling can repaint the selected-text portion
    // after Vector Click's custom arrow frame has already been drawn. Repaint
    // ordinary selectors once the wheel message completes so the divider and
    // border remain intact. Input type has its own covered transition repaint.
    if (message == WM_MOUSEWHEEL && window != self->action_type_combo_) {
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
    }

    if (message == WM_ENABLE || message == WM_SETFOCUS ||
        message == WM_KILLFOCUS || message == CB_SETCURSEL ||
        message == CB_SHOWDROPDOWN) {
        InvalidateRect(window, nullptr, FALSE);
    }

    return result;
}

LRESULT CALLBACK MainWindow::AdvancedFocusSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM w_param,
    const LPARAM l_param,
    const UINT_PTR subclass_id,
    const DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<MainWindow*>(reference_data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, AdvancedFocusSubclassProc, subclass_id);
        return DefSubclassProc(window, message, w_param, l_param);
    }

    if (self != nullptr && message == WM_SETFOCUS &&
        !self->suppress_advanced_focus_scroll_ && !self->target_picker_open_) {
        // Some custom and owner-drawn controls do not send the parent-level
        // BN_SETFOCUS / CBN_SETFOCUS / EN_SETFOCUS notifications used by the
        // older scroll path. Observe focus at the HWND itself so every
        // keyboard-focusable Advanced control is brought into the viewport.
        self->EnsureAdvancedControlVisible(window);
    }
    return DefSubclassProc(window, message, w_param, l_param);
}

LRESULT CALLBACK MainWindow::EditSubclassProc(const HWND window,
                                               const UINT message,
                                               const WPARAM w_param,
                                               const LPARAM l_param,
                                               const UINT_PTR subclass_id,
                                               const DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<MainWindow*>(reference_data);

    if (message == WM_NCDESTROY) {
        if (self != nullptr) {
            if (self->numeric_hot_edit_ == window) {
                self->numeric_hot_edit_ = nullptr;
                self->numeric_hot_part_ = 0;
            }
            if (self->numeric_pressed_edit_ == window) {
                self->numeric_pressed_edit_ = nullptr;
                self->numeric_pressed_part_ = 0;
            }
        }
        RemoveWindowSubclass(window, EditSubclassProc, subclass_id);
        return DefSubclassProc(window, message, w_param, l_param);
    }

    const bool signed_field = self != nullptr &&
        (window == self->fixed_x_edit_ || window == self->fixed_y_edit_);
    const bool decimal_field =
        self != nullptr && self->IsDurationMillisecondsEdit(window);

    const auto set_hot_part = [self](const HWND edit, const int part) noexcept {
        if (self == nullptr) {
            return;
        }
        const HWND old_hot = self->numeric_hot_edit_;
        const int old_part = self->numeric_hot_part_;
        const HWND new_hot = part != 0 ? edit : nullptr;
        if (old_hot == new_hot && old_part == part) {
            return;
        }
        self->numeric_hot_edit_ = new_hot;
        self->numeric_hot_part_ = part;
        numeric_field::InvalidateArrow(old_hot);
        if (edit != old_hot) {
            numeric_field::InvalidateArrow(edit);
        }
    };

    const auto draw_numeric_chrome = [self, window](const HDC dc) noexcept {
        numeric_field::VisualState state{};
        if (self != nullptr) {
            state.hot_edit = self->numeric_hot_edit_;
            state.hot_part = self->numeric_hot_part_;
            state.pressed_edit = self->numeric_pressed_edit_;
            state.pressed_part = self->numeric_pressed_part_;
        }
        numeric_field::DrawChrome(window, dc, state);
    };

    const auto paint_numeric_background = [window](const HDC dc) noexcept {
        if (dc == nullptr) {
            return;
        }
        RECT bounds{};
        if (GetClientRect(window, &bounds) == FALSE || IsRectEmpty(&bounds)) {
            return;
        }
        const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(window));
        ui::Fill(dc, bounds, ui::Surface);
        ui::DrawRoundedPanel(dc,
                             bounds,
                             ui::SurfaceAlt,
                             ui::SurfaceAlt,
                             Scale(8, control_dpi));
    };

    const auto restore_print_dc = [](const HDC dc,
                                     const int saved_state) noexcept {
        if (dc == nullptr) {
            return;
        }
        if (saved_state != 0) {
            RestoreDC(dc, saved_state);
            return;
        }

        // A standard memory or paint DC should support SaveDC. Keep a bounded
        // fallback so a misbehaving native EDIT print does not leave the
        // application-owned chrome clipped to the text formatting rectangle.
        SetMapMode(dc, MM_TEXT);
        SetWindowOrgEx(dc, 0, 0, nullptr);
        SetViewportOrgEx(dc, 0, 0, nullptr);
        SelectClipRgn(dc, nullptr);
    };

    const auto reject_invalid_input = []() {
        MessageBeep(MB_OK);
        return static_cast<LRESULT>(0);
    };

    if (message == WM_CHAR) {
        if (w_param == static_cast<WPARAM>(L'\r') ||
            w_param == static_cast<WPARAM>(L'\n') ||
            w_param == static_cast<WPARAM>(L'\t')) {
            return 0;
        }
        if (w_param >= static_cast<WPARAM>(L' ')) {
            const wchar_t character = static_cast<wchar_t>(w_param);
            const std::wstring candidate = ReplaceSelectedText(
                window, std::wstring_view(&character, 1));
            if (!IsNumericTextValid(candidate, signed_field, decimal_field)) {
                return reject_invalid_input();
            }
        }
    }

    if (message == WM_PASTE) {
        bool valid_paste = false;
        if (OpenClipboard(window) != FALSE) {
            const HGLOBAL data =
                static_cast<HGLOBAL>(GetClipboardData(CF_UNICODETEXT));
            if (data != nullptr) {
                const SIZE_T byte_size = GlobalSize(data);
                if (byte_size >= sizeof(wchar_t) &&
                    byte_size % sizeof(wchar_t) == 0U) {
                    const auto* text =
                        static_cast<const wchar_t*>(GlobalLock(data));
                    if (text != nullptr) {
                        const std::size_t available_characters =
                            byte_size / sizeof(wchar_t);
                        const std::size_t inspection_characters =
                            std::min<std::size_t>(
                                available_characters,
                                MaximumNumericEditCharacters + 1U);
                        const wchar_t* const terminator =
                            std::find(text,
                                      text + inspection_characters,
                                      L'\0');
                        if (terminator != text + inspection_characters) {
                            const std::size_t length =
                                static_cast<std::size_t>(terminator - text);
                            if (length > 0U &&
                                length <= MaximumNumericEditCharacters) {
                                valid_paste = IsNumericTextValid(
                                    ReplaceSelectedText(
                                        window,
                                        std::wstring_view(text, length)),
                                    signed_field,
                                    decimal_field);
                            }
                        }
                        GlobalUnlock(data);
                    }
                }
            }
            CloseClipboard();
        }
        if (!valid_paste) {
            return reject_invalid_input();
        }
    }

    if (message == WM_SETCURSOR && LOWORD(l_param) == HTCLIENT &&
        IsWindowEnabled(window) != FALSE) {
        POINT point{};
        if (GetCursorPos(&point) != FALSE &&
            ScreenToClient(window, &point) != FALSE &&
            numeric_field::ArrowPartAt(window, point) != 0) {
            // Keep the I-beam over editable numeric text, but present the
            // standard Windows arrow over the up and down button hit regions.
            // The arrow regions behave as buttons, not as text insertion areas.
            SetCursor(LoadSharedDefaultCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
    }

    if (self != nullptr && message == WM_MOUSEMOVE) {
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const int part = numeric_field::ArrowPartAt(window, point);
        set_hot_part(window, part);
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window;
        TrackMouseEvent(&tracking);
    }

    if (self != nullptr && message == WM_MOUSELEAVE) {
        set_hot_part(window, 0);
        return 0;
    }

    if (self != nullptr &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) &&
        IsWindowEnabled(window) != FALSE) {
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const int part = numeric_field::ArrowPartAt(window, point);
        if (part != 0) {
            SetFocus(window);
            SetCapture(window);
            self->numeric_pressed_edit_ = window;
            self->numeric_pressed_part_ = part;
            numeric_field::InvalidateArrow(window);
            return 0;
        }
    }

    if (self != nullptr && message == WM_LBUTTONUP &&
        self->numeric_pressed_edit_ == window) {
        const int pressed_part = self->numeric_pressed_part_;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        self->numeric_pressed_edit_ = nullptr;
        self->numeric_pressed_part_ = 0;
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const int released_part = numeric_field::ArrowPartAt(window, point);
        numeric_field::InvalidateArrow(window);
        set_hot_part(window, released_part);
        if (pressed_part != 0 && released_part == pressed_part &&
            IsWindowEnabled(window) != FALSE) {
            self->StepNumericEdit(window, pressed_part == 1 ? 1 : -1);
        }
        return 0;
    }

    if (self != nullptr &&
        (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) &&
        self->numeric_pressed_edit_ == window) {
        self->numeric_pressed_edit_ = nullptr;
        self->numeric_pressed_part_ = 0;
        numeric_field::InvalidateArrow(window);
        set_hot_part(window, 0);
    }

    if (self != nullptr &&
        ((message == WM_ENABLE && w_param == FALSE) ||
         (message == WM_SHOWWINDOW && w_param == FALSE))) {
        if (self->numeric_pressed_edit_ == window) {
            self->numeric_pressed_edit_ = nullptr;
            self->numeric_pressed_part_ = 0;
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            numeric_field::InvalidateArrow(window);
        }
        set_hot_part(window, 0);
    }

    if (message == WM_GETDLGCODE) {
        LRESULT code = DefSubclassProc(window, message, w_param, l_param);
        const auto* key_message = reinterpret_cast<const MSG*>(l_param);
        if (key_message != nullptr && key_message->message == WM_KEYDOWN &&
            key_message->wParam == VK_TAB) {
            code &= ~(DLGC_WANTTAB | DLGC_WANTALLKEYS | DLGC_WANTMESSAGE);
        }
        return code;
    }

    if (message == WM_KEYDOWN && w_param == static_cast<WPARAM>(L'A') &&
        (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (GetKeyState(VK_MENU) & 0x8000) == 0) {
        // The numeric validation subclass must preserve ordinary edit-control
        // selection behavior. Handle Ctrl+A explicitly because the filtered
        // numeric path otherwise receives the translated control character as
        // rejected input and Windows emits a warning beep.
        SendMessageW(window, EM_SETSEL, 0, -1);
        return 0;
    }

    if (message == WM_CHAR && w_param == 1U) {
        // Consume the translated Ctrl+A control character after the selection
        // has already been applied by WM_KEYDOWN.
        return 0;
    }

    if (self != nullptr && message == WM_KEYDOWN &&
        (w_param == VK_UP || w_param == VK_DOWN) &&
        (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
        (GetKeyState(VK_MENU) & 0x8000) == 0) {
        self->StepNumericEdit(window, w_param == VK_UP ? 1 : -1);
        return 0;
    }

    if (self != nullptr && message == WM_MOUSEWHEEL &&
        IsWindowEnabled(window) != FALSE) {
        const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
        if (delta != 0) {
            self->StepNumericEdit(window, delta > 0 ? 1 : -1);
        }
        return 0;
    }

    if (message == WM_ERASEBKGND) {
        // WM_PAINT composes the native EDIT client and VectorClick chrome in
        // one off-screen frame. A separate erase would expose an intermediate
        // borderless rectangle and recreate the flicker this path prevents.
        return 1;
    }

    if (message == WM_PRINTCLIENT && self != nullptr) {
        self->DrawPrintedEdit(window, reinterpret_cast<HDC>(w_param));
        return 0;
    }

    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC destination = BeginPaint(window, &paint);
        if (destination != nullptr && self != nullptr &&
            IsWindowEnabled(window) == FALSE) {
            // Disabled numeric fields are presentation-only. Paint their
            // complete frame through Vector Click's application-owned path
            // instead of asking the native EDIT to print itself. During page
            // visibility transitions, native EDIT painting can briefly use its
            // default enabled text color before the disabled palette is
            // reapplied, which exposes a one-frame white value. Enabled fields
            // continue through the native path below so selection, caret, and
            // ordinary edit behavior remain unchanged.
            self->DrawPrintedEdit(window, destination);
            EndPaint(window, &paint);
            return 0;
        }
        if (destination != nullptr) {
            RECT bounds{};
            if (GetClientRect(window, &bounds) != FALSE &&
                !IsRectEmpty(&bounds)) {
                const int width = bounds.right - bounds.left;
                const int height = bounds.bottom - bounds.top;
                HDC buffer = CreateCompatibleDC(destination);
                HBITMAP bitmap = buffer != nullptr
                                     ? CreateCompatibleBitmap(destination,
                                                              width,
                                                              height)
                                     : nullptr;
                HGDIOBJ old_bitmap = nullptr;
                if (buffer != nullptr && bitmap != nullptr) {
                    old_bitmap = SelectObject(buffer, bitmap);
                    paint_numeric_background(buffer);

                    // Native EDIT printing may leave a restrictive clip or
                    // viewport in the supplied DC. Restore its state before
                    // drawing the border and arrows, otherwise the chrome can
                    // disappear even though the text remains visible.
                    const int native_state = SaveDC(buffer);
                    (void)DefSubclassProc(
                        window,
                        WM_PRINTCLIENT,
                        reinterpret_cast<WPARAM>(buffer),
                        PRF_CLIENT);
                    restore_print_dc(buffer, native_state);
                    draw_numeric_chrome(buffer);

                    BitBlt(destination,
                           0,
                           0,
                           width,
                           height,
                           buffer,
                           0,
                           0,
                           SRCCOPY);

                    SelectObject(buffer, old_bitmap);
                    DeleteObject(bitmap);
                    DeleteDC(buffer);
                } else {
                    if (bitmap != nullptr) {
                        DeleteObject(bitmap);
                    }
                    if (buffer != nullptr) {
                        DeleteDC(buffer);
                    }

                    // Allocation failure is non-fatal. Use the same composed
                    // order directly on the paint DC rather than returning a
                    // blank control.
                    paint_numeric_background(destination);
                    const int native_state = SaveDC(destination);
                    (void)DefSubclassProc(
                        window,
                        WM_PRINTCLIENT,
                        reinterpret_cast<WPARAM>(destination),
                        PRF_CLIENT);
                    restore_print_dc(destination, native_state);
                    draw_numeric_chrome(destination);
                }
            }
        }
        EndPaint(window, &paint);
        return 0;
    }

    const LRESULT result = DefSubclassProc(window, message, w_param, l_param);
    if (self == nullptr) {
        return result;
    }

    if (message == WM_SETFONT || message == WM_SIZE ||
        message == WM_DPICHANGED) {
        self->UpdateNumericEditFormatting(window);
    }
    if (message == WM_SETFOCUS) {
        self->BeginNumericEditHistorySession(window);
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_ENABLE || message == WM_SETTEXT ||
        message == WM_SETFONT || message == WM_SIZE ||
        message == WM_DPICHANGED) {
        InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_KILLFOCUS) {
        self->FlushNumericPresentationRefresh();
        self->EndNumericEditHistorySession(window);
    }
    return result;
}

void MainWindow::PresentAlreadyRunningNotice() {
    if (window_ == nullptr || IsWindow(window_) == FALSE) {
        return;
    }

    if (IsIconic(window_) != FALSE) {
        ShowWindow(window_, SW_RESTORE);
    } else if (IsWindowVisible(window_) == FALSE) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
    }

    // Do not re-show an already visible main window here. The secondary
    // process has already requested foreground activation, and forcing the
    // owner through SW_SHOW can momentarily reorder it above an owned modal
    // popup. Preserve the established owner / popup z-order and attach the
    // notice to the deepest enabled Vector Click popup instead.
    //
    // Attach the notice to the deepest currently enabled Vector Click popup,
    // if one exists. This preserves the same owner / modal hierarchy used by
    // the rest of the application instead of bypassing an open Profile
    // Manager, target picker, or other owned dialog.
    HWND notice_owner = window_;
    for (int depth = 0; depth < 8; ++depth) {
        const HWND popup = GetWindow(notice_owner, GW_ENABLEDPOPUP);
        if (popup == nullptr || popup == notice_owner ||
            IsWindow(popup) == FALSE || IsWindowVisible(popup) == FALSE) {
            break;
        }
        notice_owner = popup;
    }

    ShowCenteredMessageBox(
        notice_owner,
        L"Vector Click is already running. Only one instance should run at a time so global hotkeys, portable settings, and generated input do not conflict. The existing window has been restored.",
        L"Vector Click is already running",
        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

LRESULT MainWindow::HandleMessage(const UINT message, const WPARAM w_param, const LPARAM l_param) {
    if (message == WM_APP_SINGLE_INSTANCE_NOTICE_REQUEST) {
        if (w_param != SingleInstanceNoticeRequestTag ||
            l_param != SingleInstanceNoticeRequestCheck) {
            return 0;
        }

        // Acknowledge synchronously so the short-lived secondary process can
        // prove it reached a compatible Vector Click build. Coalesce requests
        // both before presentation and while the notice's nested modal loop is
        // active, so a burst of launches cannot leave queued dialogs behind.
        // The secondary process already foregrounds the deepest enabled popup,
        // which also brings an existing notice forward when one is presenting.
        if (single_instance_notice_presenting_ ||
            single_instance_notice_pending_) {
            return SingleInstanceNoticeAck;
        }

        single_instance_notice_pending_ = true;
        if (PostMessageW(
                window_, WM_APP_PRESENT_SINGLE_INSTANCE_NOTICE, 0, 0) == FALSE) {
            single_instance_notice_pending_ = false;
            return 0;
        }
        return SingleInstanceNoticeAck;
    }

    switch (message) {
    case WM_CREATE:
        return OnCreate() ? 0 : -1;

    case WM_NCLBUTTONDOWN:
        // Remember whether this native size / move loop originated from the
        // title bar. Windows can emit a transient WM_SIZING while restoring a
        // snapped window during a caption drag. That message is not a manual
        // edge resize and must not create the independent resize overlay.
        caption_move_active_ = w_param == HTCAPTION;

        // Dragging a maximized title bar asks Windows to restore the window
        // and enter the native movement loop in one operation. Mark that
        // boundary before DefWindowProc begins its modal loop so the later
        // movement cover is rendered only from Vector Click's own hierarchy.
        // The composed desktop can still contain the old maximized frame or
        // pixels from windows underneath while DWM is completing the restore.
        if (w_param == HTCAPTION) {
            if (IsZoomed(window_) != FALSE) {
                maximized_caption_drag_pending_ = true;
            } else if (restored_down_exact_move_pending_) {
                // Restore Down can leave the application-rendered WM_PRINT
                // movement bitmap in a transient scale state even though the
                // live restored hierarchy is already correct. Mark only the
                // first later caption drag so it uses an exact composed frame
                // or the live hierarchy rather than that bitmap.
                restored_down_caption_drag_pending_ = true;
            }
        }
        break;

    case WM_NCLBUTTONUP:
    case WM_CANCELMODE:
        if (!interactive_resize_) {
            maximized_caption_drag_pending_ = false;
            restored_down_caption_drag_pending_ = false;
            caption_move_active_ = false;
        }
        break;

    case WM_ENTERSIZEMOVE: {
        const bool restored_from_maximized_caption =
            maximized_caption_drag_pending_;
        const bool first_caption_drag_after_restore_down =
            restored_down_caption_drag_pending_;
        maximized_caption_drag_pending_ = false;
        restored_down_caption_drag_pending_ = false;
        if (first_caption_drag_after_restore_down) {
            restored_down_exact_move_pending_ = false;
        }
        FinishAdvancedScrollSnapshot(false);
        CloseComboPopup(false);

        // A retained movement frame must never preserve an application tooltip
        // or a mode-dependent status presentation from an earlier stable UI
        // state. Dismiss both visible and delayed tooltips before selecting the
        // movement frame. If a tooltip was active, rebuild through Vector
        // Click's own hierarchy so the first drag frame is immediately clean
        // without waiting for the desktop compositor to retire the popup.
        const bool dismissed_tooltip = tooltips_.DismissAll();
        if (dismissed_tooltip) {
            if (CaptureMoveCoverFromWindow() &&
                !IsCaptureExclusionRequested()) {
                move_cover_pending_exact_screen_refresh_ = true;
            }
        }

        interactive_resize_ = true;
        interactive_sizing_ = false;
        restored_caption_drag_active_ = restored_from_maximized_caption;
        programmatic_resize_overlay_ = false;
        resize_overlay_finish_posted_ = false;
        HideResizeOverlay();
        {
            RECT client{};
            if (GetClientRect(window_, &client) != FALSE) {
                size_move_client_width_ = client.right - client.left;
                size_move_client_height_ = client.bottom - client.top;
            } else {
                size_move_client_width_ = 0;
                size_move_client_height_ = 0;
            }
        }

        // A maximized title-bar drag crosses a restore boundary while DWM may
        // still be retiring the maximized composition. Never sample the
        // desktop there. Rebuild from Vector Click's current restored layout
        // and keep that application-owned frame authoritative for the complete
        // move. WM_SIZE repeats the same safe refresh if the restore dimensions
        // arrive after WM_ENTERSIZEMOVE.
        if (restored_caption_drag_active_) {
            (void)RefreshMoveCoverForRestoredCaptionDrag();
            return 0;
        }

        if (first_caption_drag_after_restore_down) {
            // The live restored window is already correct at this point. The
            // problematic route is specifically the application-rendered
            // retained bitmap created across the prior maximize / restore
            // boundary. Prefer one exact visible frame after DWM has published
            // the restored hierarchy. If guarded desktop capture is unavailable
            // because the window is protected or partly off-screen, show no
            // movement cover for this one drag and let Windows move the live
            // hierarchy. Never fall back to the suspect WM_PRINT bitmap.
            HideMoveCover();
            (void)DwmFlush();
            if (CanCaptureMoveCoverFromScreen() &&
                CaptureMoveCoverFromScreen()) {
                (void)ShowMoveCover();
            } else {
                move_cover_pending_exact_screen_refresh_ = true;
            }
            return 0;
        }

        // Reuse the most recent complete stable frame for ordinary movement.
        // Capturing the desktop or printing the complete child hierarchy inside
        // WM_ENTERSIZEMOVE makes the first frame of every normal drag visibly
        // pause. A route-appropriate refresh is queued after stable UI changes
        // instead. Focus is a rendered property, though, so a focus mismatch
        // must be rebuilt from the current HWND hierarchy rather than sampled
        // from DWM before the focus-loss repaint has necessarily composed.
        const bool move_cover_focus_changed =
            move_cover_bitmap_.get() != nullptr &&
            move_cover_focus_ != GetFocus();
        if (!HasReusableMoveCover()) {
            if (move_cover_focus_changed ||
                move_cover_pending_exact_screen_refresh_ ||
                IsCaptureExclusionRequested()) {
                // A clean startup or guarded off-screen fallback is rendered
                // from Vector Click's final HWND layout. This prevents a stale
                // page bitmap from being replayed when desktop capture is not
                // currently safe.
                (void)CaptureMoveCoverFromWindow();
            } else if (CanCaptureMoveCoverFromScreen()) {
                (void)CaptureMoveCoverFromScreen();
            } else if (CaptureMoveCoverFromWindow()) {
                // The application-owned fallback is visually complete but the
                // ordinary route still prefers an exact composed frame once
                // the window returns fully inside the monitor work area.
                move_cover_pending_exact_screen_refresh_ = true;
            }
        }
        const bool move_cover_shown = ShowMoveCover();
        if (move_cover_shown && move_cover_exact_refresh_deferred_) {
            // A complete application-owned frame has now carried the first
            // post-Snap movement boundary. Any exact composed replacement can
            // wait for this movement session to finish. If Windows restores a
            // snapped geometry during the drag, WM_SIZE re-arms the deferral.
            move_cover_exact_refresh_deferred_ = false;
        }
        return 0;
    }

    case WM_SIZING: {
        const bool documented_sizing_edge =
            w_param == WMSZ_LEFT || w_param == WMSZ_RIGHT ||
            w_param == WMSZ_TOP || w_param == WMSZ_BOTTOM ||
            w_param == WMSZ_TOPLEFT || w_param == WMSZ_TOPRIGHT ||
            w_param == WMSZ_BOTTOMLEFT || w_param == WMSZ_BOTTOMRIGHT;
        if (caption_move_active_ || !documented_sizing_edge) {
            // Pulling a snapped window away by its title bar can produce a
            // transient WM_SIZING even though the native interaction is still
            // a move. Keep the movement cover authoritative and never leave a
            // resize overlay parked at the old snapped client rectangle.
            break;
        }
        if (!interactive_sizing_) {
            // WM_SIZING describes a proposed future rectangle. Moving the
            // independent retained popup there can put it ahead of the real
            // client during fast resizing and can also follow transient
            // Windows 11 Snap preview geometry. Start at the current committed
            // client instead. WM_SIZE publishes each geometry only after
            // Windows actually commits that resize step. Pass only the stable
            // native sizing edge so BeginResizeOverlay can choose the safest
            // already-supported presentation route for that interaction.
            interactive_sizing_ = BeginResizeOverlay(nullptr, w_param);
            if (!interactive_sizing_) {
                // Preserve the accepted native fallback if the independent
                // client-area surface cannot be created for any reason.
                HideMoveCover();
            }
        }
        break;
    }

    case WM_EXITSIZEMOVE: {
        interactive_resize_ = false;
        restored_caption_drag_active_ = false;
        maximized_caption_drag_pending_ = false;
        caption_move_active_ = false;
        bool defer_exact_move_cover_refresh = false;
        {
            RECT client{};
            const bool have_client = GetClientRect(window_, &client) != FALSE;
            const int client_width = have_client ? client.right - client.left : 0;
            const int client_height = have_client ? client.bottom - client.top : 0;
            const bool resized = have_client &&
                (client_width != size_move_client_width_ ||
                 client_height != size_move_client_height_);
            size_move_client_width_ = 0;
            size_move_client_height_ = 0;

            // A sizing session can finish without a client-size delta when
            // Windows clamps the proposed rectangle to the enforced minimum.
            // An active resize overlay still has to be finalized in that case;
            // otherwise its client-area popup remains above the main window
            // and absorbs later clicks. Test the overlay itself before the
            // size delta so a rejected minimum-size drag cannot be mistaken
            // for an ordinary window move.
            if (resize_overlay_ != nullptr &&
                IsWindow(resize_overlay_) != FALSE) {
                (void)UpdateResizeOverlayForCurrentClient();
                FinishResizeOverlay();
            } else if (resized) {
                // A Snap restore can resize the client during a caption move.
                // Keep the retained movement frame visible across the final
                // hierarchy repair so the release does not expose a transient
                // live-layout frame. The bridge is used only at release and
                // does not participate in active movement or resize geometry.
                RetireMoveCoverAfterResize();

                // A caption drag can end in a Windows 11 Snap Layout resize
                // without ever using Vector Click's edge-resize overlay. The
                // Snap Assist / snap-preview surface can still be retiring when
                // WM_EXITSIZEMOVE returns. Do not immediately sample the
                // composed desktop at that boundary: a transparent system
                // overlay can pass the WindowFromPoint ownership guard while
                // still contributing pixels to BitBlt, baking the Snap preview
                // or transient DWM afterimages into the next movement cover.
                // Seed the new geometry from Vector Click's own hierarchy and
                // keep that frame authoritative through the next drag. A later
                // ordinary move completion is a safe boundary for the existing
                // exact composed-desktop replacement.
                (void)CaptureMoveCoverFromWindow();
                if (IsCaptureExclusionRequested()) {
                    move_cover_pending_exact_screen_refresh_ = false;
                    move_cover_exact_refresh_deferred_ = false;
                } else {
                    move_cover_pending_exact_screen_refresh_ = true;
                    move_cover_exact_refresh_deferred_ = true;
                    defer_exact_move_cover_refresh = true;
                }
            } else {
                // Transfer from the child movement cover to a short-lived,
                // fully opaque layered popup at the final content position.
                // Because the popup is a separate top-level surface, the real
                // child hierarchy can repaint underneath it without exposing
                // intermediate states. The popup is never repositioned during
                // movement and contains only the already verified VectorClick
                // frame, so it cannot add drag lag or capture external pixels.
                RetireMoveCoverAfterMove();
            }
        }
        interactive_sizing_ = false;
        if (move_cover_pending_exact_screen_refresh_ &&
            !defer_exact_move_cover_refresh) {
            // A completed ordinary move is a deterministic stabilization
            // boundary. Snap-resize completion deliberately defers this exact
            // desktop replacement by one movement session so Windows' Snap UI
            // cannot become part of the retained Vector Click frame.
            QueueMoveCoverRefresh();
        }
        return 0;
    }

    case WM_SIZE: {
        CloseComboPopup(false);

        const bool restored_down_from_maximized =
            w_param == SIZE_RESTORED && window_was_maximized_ &&
            !interactive_resize_ && !maximized_caption_drag_pending_;
        if (w_param == SIZE_MAXIMIZED) {
            window_was_maximized_ = true;
            restored_down_exact_move_pending_ = false;
            restored_down_caption_drag_pending_ = false;
        } else if (w_param == SIZE_RESTORED) {
            window_was_maximized_ = false;
            if (restored_down_from_maximized) {
                restored_down_exact_move_pending_ = true;
                restored_down_caption_drag_pending_ = false;

                // A maximized and restored Basic / About content host can have
                // identical dimensions, so dimension validation alone cannot
                // reject a malformed application-rendered restore bitmap. Make
                // every previous frame unusable and request one exact composed
                // replacement after the restored hierarchy is repaired.
                HideMoveCover();
                move_cover_bitmap_.reset();
                move_cover_width_ = 0;
                move_cover_height_ = 0;
                move_cover_page_index_ = -1;
                move_cover_action_type_index_ = -1;
                move_cover_advanced_scroll_offset_ = -1;
                move_cover_pointer_control_ = nullptr;
                move_cover_bitmap_revision_ = 0U;
                move_cover_pending_exact_screen_refresh_ = true;
            }
        }

        if (startup_cover_ != nullptr && IsWindow(startup_cover_) != FALSE) {
            RECT client{};
            GetClientRect(window_, &client);
            SetWindowPos(startup_cover_,
                         HWND_TOP,
                         0,
                         0,
                         client.right - client.left,
                         client.bottom - client.top,
                         SWP_NOACTIVATE);
        }
        if (w_param != SIZE_MINIMIZED) {
            // A dynamic status-height change resizes the outer window itself.
            // The status-layout transaction positions and repaints only the
            // footer afterward. Running the ordinary WM_SIZE repair here
            // repainted the complete control hierarchy twice and caused the
            // visible full-interface flash reported while typing numbers.
            if (status_layout_update_in_progress_) {
                return 0;
            }
            if (interactive_resize_) {
                if (restored_caption_drag_active_) {
                    // Some Windows builds deliver the restored WM_SIZE after
                    // WM_ENTERSIZEMOVE. Replace the earlier maximized-sized
                    // frame immediately, still without reading composed desktop
                    // pixels from the unstable restore boundary.
                    (void)RefreshMoveCoverForRestoredCaptionDrag();
                } else if (interactive_sizing_ && resize_overlay_ != nullptr &&
                           IsWindow(resize_overlay_) != FALSE) {
                    // Publish only committed manual-resize geometry. The
                    // retained popup follows the real current client rather
                    // than predicting a future WM_SIZING rectangle. Advanced
                    // resize content is composed from cached bitmaps, so this
                    // does not relayout or print the live child hierarchy.
                    (void)UpdateResizeOverlayForCurrentClient();
                } else if (!interactive_sizing_) {
                    // Pulling a snapped window away from its Snap Layout can
                    // restore a different client size inside the native caption
                    // movement loop. This is the same kind of unstable DWM
                    // boundary already handled explicitly for maximized caption
                    // restores, but IsZoomed() cannot identify snapped windows.
                    // Re-layout and rebuild the movement surface strictly from
                    // Vector Click's own HWND hierarchy whenever Windows changes
                    // geometry during a caption move. Never desktop-BitBlt the
                    // Snap restore animation or its transient afterimages.
                    HideMoveCover();
                    PositionContentHost(false);
                    if (CaptureMoveCoverFromWindow()) {
                        move_cover_pending_exact_screen_refresh_ =
                            !IsCaptureExclusionRequested();
                        move_cover_exact_refresh_deferred_ =
                            !IsCaptureExclusionRequested();
                        (void)ShowMoveCover();
                    } else if (!IsCaptureExclusionRequested()) {
                        move_cover_pending_exact_screen_refresh_ = true;
                        move_cover_exact_refresh_deferred_ = true;
                    }
                }
            } else if (programmatic_resize_overlay_ &&
                       resize_overlay_ != nullptr &&
                       IsWindow(resize_overlay_) != FALSE) {
                (void)UpdateResizeOverlayForCurrentClient();
                PositionContentHost(false);
                if (!resize_overlay_finish_posted_) {
                    resize_overlay_finish_posted_ = true;
                    if (PostMessageW(window_,
                                     WM_APP_FINISH_RESIZE_OVERLAY,
                                     0,
                                     0) == FALSE) {
                        resize_overlay_finish_posted_ = false;
                        FinishResizeOverlay();
                    }
                }
            } else {
                PositionContentHost(true);
                QueueSurfaceRepair();
                if (restored_down_from_maximized) {
                    // QueueSurfaceRepair is intentionally posted first. The
                    // replacement capture will therefore observe only the
                    // fully restored visible hierarchy.
                    QueueMoveCoverRefresh();
                }
            }
        }
        return 0;
    }

    case WM_WINDOWPOSCHANGING:
        // Manual edge or corner sizing is presented exclusively from the
        // committed WM_SIZE client geometry. Keep this path for programmatic
        // resize transitions such as maximize and restore, but never let a
        // proposed WINDOWPOS compete with the committed manual-resize surface.
        if (!(interactive_resize_ && interactive_sizing_) &&
            resize_overlay_ != nullptr &&
            IsWindow(resize_overlay_) != FALSE) {
            const auto* position = reinterpret_cast<const WINDOWPOS*>(l_param);
            if (position != nullptr) {
                RECT proposed{};
                if (GetWindowRect(window_, &proposed) != FALSE) {
                    int width = proposed.right - proposed.left;
                    int height = proposed.bottom - proposed.top;
                    if ((position->flags & SWP_NOMOVE) == 0U) {
                        proposed.left = position->x;
                        proposed.top = position->y;
                    }
                    if ((position->flags & SWP_NOSIZE) == 0U) {
                        width = position->cx;
                        height = position->cy;
                    }
                    proposed.right = proposed.left + width;
                    proposed.bottom = proposed.top + height;
                    (void)UpdateResizeOverlayForOuterRect(proposed);
                }
            }
        }
        break;

    case WM_SYSCOMMAND: {
        const WPARAM command = w_param & 0xFFF0U;
        if ((command == SC_MAXIMIZE || command == SC_RESTORE) &&
            startup_presentation_complete_ && !interactive_resize_) {
            CloseComboPopup(false);
            programmatic_resize_overlay_ = BeginResizeOverlay(nullptr);
        }
        break;
    }

    case WM_APP_FINISH_RESIZE_OVERLAY:
        resize_overlay_finish_posted_ = false;
        if (programmatic_resize_overlay_ && !interactive_resize_) {
            FinishResizeOverlay();
        }
        return 0;

    case WM_SHOWWINDOW:
        if (w_param != FALSE) {
            QueueSurfaceRepair();
        }
        break;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(l_param);
        info->ptMinTrackSize.x = Scale(layout::MinimumWindowWidth, dpi_);
        info->ptMinTrackSize.y =
            Scale(layout::MinimumOuterHeight(status_height_logical_, CurrentLayoutPage()), dpi_);
        return 0;
    }

    case WM_DPICHANGED: {
        CloseComboPopup(false);
        dpi_ = HIWORD(w_param);
        RecreateFontForDpi();
        const auto* suggested = reinterpret_cast<RECT*>(l_param);
        SetWindowPos(window_, nullptr,
                     suggested->left,
                     suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        PositionContentHost(true);
        Relayout();
        run_feedback_presenter_.RefreshRunningIndicator();
        UpdateStatusAreaLayout(false);
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(w_param) == WA_INACTIVE) {
            CloseComboPopup(false);
        }
        break;

    case WM_MOUSEWHEEL:
        if (ScrollAdvancedWheel(w_param, l_param)) {
            return 0;
        }
        break;

    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(w_param);
        RECT client{};
        GetClientRect(window_, &client);
        ui::Fill(dc, client, ui::Window);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        ui::Fill(dc, client, ui::Window);
        EndPaint(window_, &paint);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(w_param);
        const HWND control = reinterpret_cast<HWND>(l_param);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, IsWindowEnabled(control) ? ui::Text : ui::Disabled);
        static HBRUSH surface_brush = CreateSolidBrush(ui::Surface);
        return reinterpret_cast<LRESULT>(surface_brush);
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(w_param);
        const HWND control = reinterpret_cast<HWND>(l_param);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, ui::SurfaceAlt);
        SetTextColor(dc, IsWindowEnabled(control) ? ui::Text : ui::Disabled);
        static HBRUSH edit_brush = CreateSolidBrush(ui::SurfaceAlt);
        return reinterpret_cast<LRESULT>(edit_brush);
    }

    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(l_param);
        if (measure != nullptr && measure->CtlType == ODT_COMBOBOX) {
            measure->itemHeight = static_cast<UINT>(Scale(22, dpi_));
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
        if (draw == nullptr) {
            break;
        }
        if (draw->CtlType == ODT_COMBOBOX) {
            DrawComboItem(*draw);
            return TRUE;
        }
        if (draw->hwndItem == basic_tab_button_ ||
            draw->hwndItem == advanced_tab_button_ ||
            draw->hwndItem == about_tab_button_) {
            DrawTabControl(*draw);
            return TRUE;
        }
        if (IsGroupControl(draw->hwndItem)) {
            DrawGroupControl(*draw);
            return TRUE;
        }
        if (IsRadioControl(draw->hwndItem)) {
            DrawChoiceControl(*draw, true);
            return TRUE;
        }
        if (IsCheckControl(draw->hwndItem)) {
            DrawChoiceControl(*draw, false);
            return TRUE;
        }
        if (IsPushButtonControl(draw->hwndItem)) {
            DrawPushButtonControl(*draw);
            return TRUE;
        }
        if (IsCompactTextControl(draw->hwndItem)) {
            DrawCompactText(*draw);
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(l_param);
        if (header != nullptr && header->code == NM_CUSTOMDRAW &&
            (IsCheckControl(header->hwndFrom) || IsRadioControl(header->hwndFrom))) {
            auto* custom = reinterpret_cast<NMCUSTOMDRAW*>(l_param);
            if (custom->dwDrawStage == CDDS_PREPAINT) {
                DRAWITEMSTRUCT draw{};
                draw.CtlType = ODT_BUTTON;
                draw.hwndItem = header->hwndFrom;
                draw.hDC = custom->hdc;
                draw.rcItem = custom->rc;
                if ((custom->uItemState & CDIS_SELECTED) != 0) {
                    draw.itemState |= ODS_SELECTED;
                }
                if ((custom->uItemState & CDIS_DISABLED) != 0) {
                    draw.itemState |= ODS_DISABLED;
                }
                if ((custom->uItemState & CDIS_FOCUS) != 0) {
                    draw.itemState |= ODS_FOCUS;
                }
                if ((custom->uItemState & CDIS_HOT) != 0) {
                    draw.itemState |= ODS_HOTLIGHT;
                }
                DrawChoiceControl(draw, IsRadioControl(header->hwndFrom));
                return CDRF_SKIPDEFAULT;
            }
        }
        break;
    }

    case WM_COMMAND: {
        const int id = LOWORD(w_param);
        const int notification = HIWORD(w_param);
        const HWND command_control = reinterpret_cast<HWND>(l_param);
        if (suppress_control_events_ || numeric_text_update_in_progress_) {
            return 0;
        }
        if (notification == EN_CHANGE && IsNumericEditControl(command_control)) {
            NoteNumericEditHistoryTextChanged(command_control);
        }
        if (!suppress_advanced_focus_scroll_ && !target_picker_open_ &&
            (notification == BN_SETFOCUS || notification == CBN_SETFOCUS ||
             notification == EN_SETFOCUS)) {
            // A target-picker modal transaction can synchronously transfer
            // focus among disabled / re-enabled owner controls. That is not
            // Advanced-page navigation, so keep the user's existing scroll
            // position while the picker owns the interaction.
            EnsureAdvancedControlVisible(reinterpret_cast<HWND>(l_param));
        }
        switch (id) {
        case BasicPageTab:
            if (notification == BN_CLICKED) {
                SelectPage(0);
            }
            break;
        case AdvancedPageTab:
            if (notification == BN_CLICKED) {
                SelectPage(1);
            }
            break;
        case AboutPageTab:
            if (notification == BN_CLICKED) {
                SelectPage(2);
            }
            break;
        case StartButton:
            StartFromControls();
            break;
        case StopButton:
            if (CleanupRequired()) {
                RetryEmergencyCleanup();
            } else {
                StopNormally();
            }
            break;
        case EmergencyButton:
            EmergencyStop();
            break;
        case AdminButton:
            RestartElevated();
            break;
        case OfficialDownloadsButton:
            if (notification == BN_CLICKED) {
                (void)OpenExternalDestination(window_, ReleasesUrl, L"the official downloads page");
            }
            break;
        case SourceCodeButton:
            if (notification == BN_CLICKED) {
                (void)OpenExternalDestination(window_, RepositoryUrl, L"the source-code repository");
            }
            break;
        case ReportBugButton:
            if (notification == BN_CLICKED) {
                (void)OpenExternalDestination(window_, IssuesUrl, L"the issue page");
            }
            break;
        case CopySupportEmailButton:
            if (notification == BN_CLICKED) {
                if (CopyUnicodeTextToClipboard(window_, SupportEmail)) {
                    const EngineState state =
                        controller_ ? controller_->State() : EngineState::Ready;
                    SetStatusPresentation({
                        StatusCategoryForEngineState(
                            ToPresentationEngineState(state)),
                        SupportEmailCopiedStatus,
                        SupportEmailCopiedTooltip,
                    }, true);
                    if (StartGeneratedTimer(support_email_copied_timer_id_,
                                            SupportEmailCopiedMilliseconds) == 0) {
                        // The clipboard operation already succeeded, but a
                        // failed timer must not strand this temporary message
                        // indefinitely. Fall back immediately to the canonical
                        // engine / status presentation.
                        const std::uint64_t completed =
                            controller_ ? controller_->CompletedActions() : 0U;
                        UpdateStatus(state, completed);
                    }
                } else {
                    ShowCenteredMessageBox(
                        window_,
                        L"Vector Click could not copy the support email to the Windows clipboard. Try again, or copy the address shown on the About page.",
                        L"Could not copy support email",
                        MB_OK | MB_ICONWARNING);
                }
            }
            break;
        case ViewLicenseButton:
            if (notification == BN_CLICKED) {
                (void)OpenExternalDestination(window_, LicenseUrl, L"the license page");
            }
            break;
        case CopyDiagnosticReportButton:
            if (notification == BN_CLICKED) {
                CopyDiagnosticReport();
            }
            break;
        case CapturePositionButton:
            if (notification == BN_CLICKED) {
                BeginPositionCapture();
            }
            break;
        case SelectTargetButton:
            if (notification == BN_CLICKED) {
                ChooseTargetWindow();
            }
            break;
        case ClearTargetButton:
            if (notification == BN_CLICKED) {
                ClearTargetWindow();
            }
            break;
        case BackgroundInputCheck:
            if (notification == BN_CLICKED) {
                RefreshEnabledState();
                ShowInputMethodTargetRoutingStatus();
                ScheduleSettingsSave();
            }
            break;
        case ActionTypeCombo:
            if (notification == CBN_SELCHANGE) {
                // The stable pre-change frame was captured by the combo
                // subclass before native selection processing began. When the
                // list is open, wait until it closes before presenting that
                // frame so the popup itself is never covered or captured.
                input_type_update_pending_ = true;
                if (SendMessageW(action_type_combo_, CB_GETDROPPEDSTATE, 0, 0) == FALSE) {
                    // Present the saved frame in the same notification turn,
                    // before Windows has an opportunity to paint the new combo
                    // text beside the old mode-dependent rows.
                    (void)ShowBasicTopSnapshot();
                    QueueInputTypeUpdate();
                }
            }
            if (notification == CBN_CLOSEUP) {
                if (input_type_update_pending_) {
                    // The list window is now gone, so it is safe to cover the
                    // upper Basic surface while the posted update completes.
                    (void)ShowBasicTopSnapshot();
                    QueueInputTypeUpdate();
                } else {
                    // A dropdown that was opened and then cancelled may have a
                    // pre-change bitmap but no pending mode update.
                    HideBasicTopSnapshot();
                }
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case ActionPatternCombo:
            if (notification == CBN_SELCHANGE) {
                UpdateActionPatternControls();
                RefreshEnabledState();
                ScheduleSettingsSave();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case BackendCombo:
            if (notification == CBN_SELCHANGE) {
                RefreshEnabledState();
                ShowInputMethodTargetRoutingStatus();
                ScheduleSettingsSave();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case RandomIntervalStyleCombo:
            if (notification == CBN_SELCHANGE) {
                ScheduleSettingsSave();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case DownDurationBehaviorCombo:
            if (notification == CBN_SELCHANGE) {
                RefreshEnabledState();
                ScheduleSettingsSave();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case MouseButtonCombo:
            if (notification == CBN_SELCHANGE) {
                ScheduleSettingsSave();
            }
            if (notification == CBN_CLOSEUP) {
                SetFocus(window_);
                InvalidateRect(mouse_button_combo_, nullptr, TRUE);
            }
            break;
        case GeneratedKeyCombo:
            if (notification == CBN_SELCHANGE) {
                ValidateGeneratedKeySelection(true);
                UpdateKeySelectorTooltip(generated_key_combo_);
                RefreshEnabledState();
                ScheduleSettingsSave();
            }
            if (notification == CBN_CLOSEUP) {
                SetFocus(window_);
                InvalidateRect(generated_key_combo_, nullptr, TRUE);
            }
            break;
        case IntervalMinutesEdit:
        case IntervalSecondsEdit:
        case IntervalEdit:
        case ButtonDownMinutesEdit:
        case ButtonDownSecondsEdit:
        case ButtonDownEdit:
        case ActionSpacingMinutesEdit:
        case ActionSpacingSecondsEdit:
        case ActionSpacingEdit:
        case MinimumIntervalMinutesEdit:
        case MinimumIntervalSecondsEdit:
        case MinimumIntervalEdit:
        case MaximumIntervalMinutesEdit:
        case MaximumIntervalSecondsEdit:
        case MaximumIntervalEdit:
            if (notification == EN_CHANGE) {
                QueueNumericPresentationRefresh(true);
                ScheduleSettingsSave();
            }
            break;
        case RandomIntervalCheck:
            if (notification == BN_CLICKED) {
                RefreshEnabledState();
                UpdateRateLabel();
                ScheduleSettingsSave();
            }
            break;
        case CurrentCursorRadio:
        case FixedPositionRadio:
            if (notification == BN_CLICKED) {
                SetRadioPair(current_cursor_radio_,
                             fixed_position_radio_,
                             id == FixedPositionRadio ? fixed_position_radio_
                                                      : current_cursor_radio_);
                RefreshEnabledState();
                ScheduleSettingsSave();
            }
            break;
        case ClickPositionIndicatorCheck:
            if (notification == BN_CLICKED) {
                if (!IsChecked(click_position_indicator_check_)) {
                    ResetClickPositionIndicator();
                }
                ScheduleSettingsSave();
            }
            break;
        case FixedXEdit:
        case FixedYEdit:
        case RepeatCountEdit:
            if (notification == EN_CHANGE) {
                QueueNumericPresentationRefresh(false);
                ScheduleSettingsSave();
            }
            break;
        case RunTimeHoursEdit:
        case RunTimeMinutesEdit:
        case RunTimeSecondsEdit:
            if (notification == EN_CHANGE) {
                QueueNumericPresentationRefresh(true);
                ScheduleSettingsSave();
            }
            break;
        case BurstCountEdit:
            if (notification == EN_CHANGE) {
                QueueNumericPresentationRefresh(true);
                ScheduleSettingsSave();
            }
            break;
        case UnlimitedRadio:
        case LimitedRadio:
            if (notification == BN_CLICKED) {
                SetRadioPair(unlimited_radio_,
                             limited_radio_,
                             id == LimitedRadio ? limited_radio_
                                                : unlimited_radio_);
                RefreshEnabledState();
                ScheduleSettingsSave();
            }
            break;
        case StartHotkeyCombo:
        case EmergencyHotkeyCombo:
            if (notification == CBN_SELCHANGE) {
                ConfigureHotkeysFromControls(true);
                UpdateKeySelectorTooltip(
                    id == StartHotkeyCombo ? start_hotkey_combo_
                                           : emergency_hotkey_combo_);
            }
            if (notification == CBN_CLOSEUP) {
                const HWND combo = id == StartHotkeyCombo ? start_hotkey_combo_ : emergency_hotkey_combo_;
                SetFocus(window_);
                InvalidateRect(combo, nullptr, FALSE);
            }
            break;
        case DiagnosticsCheck: {
            const bool diagnostics_enabled = IsChecked(diagnostics_check_);
            if (diagnostics_enabled) {
                SetTimer(window_, DiagnosticsTimerId, DiagnosticsTimerMilliseconds, nullptr);
                last_diagnostics_text_.clear();
            } else {
                KillTimer(window_, DiagnosticsTimerId);
            }

            // Ordinary status and live diagnostics share one visible, buffered
            // footer surface. The retained diagnostics text child stays hidden
            // as an internal text store, so overlapping child windows never
            // paint the same footer rectangle.
            ShowWindow(status_text_, SW_SHOW);
            ShowWindow(diagnostics_text_, SW_HIDE);

            if (controller_) {
                UpdateStatus(controller_->State(), controller_->CompletedActions());
                if (diagnostics_enabled) {
                    RefreshPresentation(false, false, false, true);
                }
            }
            InvalidateRect(status_text_, nullptr, FALSE);
            UpdateStatusAreaLayout();
            UpdateVisibleStatusTooltip();
            ScheduleSettingsSave();
            break;
        }
        case SafetyShieldCheck:
            safety_shield_requested_.store(
                IsChecked(safety_shield_check_), std::memory_order_release);
            // This preference only controls whether Emergency Stop requests
            // the optional shield. It does not participate in control
            // availability. Running the full enabled-state pass here used to
            // expose intermediate enable / disable frames in unrelated
            // conditionally disabled controls, most visibly the Variation
            // style combo while random intervals were off.
            ScheduleSettingsSave();
            break;
        case ForceExitOnEmergencyStopCheck:
            force_exit_on_emergency_stop_requested_.store(
                IsChecked(force_exit_on_emergency_stop_check_),
                std::memory_order_release);
            ScheduleSettingsSave();
            break;
        case CaptureExclusionCheck:
            if (notification == BN_CLICKED) {
                HandleCaptureExclusionToggle();
            }
            break;
        case KeepOnTopCheck:
            if (notification == BN_CLICKED) {
                HandleKeepOnTopToggle();
            }
            break;
        case RememberSettingsCheck:
            HandleRememberSettingsToggle();
            break;
        case ImportSettingsButton:
            if (notification == BN_CLICKED) {
                ImportSettingsFromFile();
            }
            break;
        case ProfileCombo:
            if (notification == CBN_SELCHANGE && !refreshing_profile_selector_) {
                HandleProfileSelection();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case ManageProfilesButton:
            if (notification == BN_CLICKED) {
                OpenProfileManager();
            }
            break;
        case ProcessPriorityCombo:
            if (notification == CBN_SELCHANGE) {
                HandleProcessPrioritySelection();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case TimingWorkerPriorityCombo:
            if (notification == CBN_SELCHANGE) {
                HandleTimingWorkerPrioritySelection();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case HotkeyControlPriorityCombo:
            if (notification == CBN_SELCHANGE) {
                HandleHotkeyControlPrioritySelection();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case TimingWorkerQosCombo:
            if (notification == CBN_SELCHANGE) {
                HandleTimingWorkerQosSelection();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case WindowsNotificationCombo:
            if (notification == CBN_SELCHANGE) {
                settings_cache_.windows_notification_mode =
                    SelectedRunFeedbackMode(windows_notification_combo_);
                ScheduleSettingsSave();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case SystemSoundCombo:
            if (notification == CBN_SELCHANGE) {
                settings_cache_.system_sound_mode =
                    SelectedRunFeedbackMode(system_sound_combo_);
                ScheduleSettingsSave();
            }
            if (notification == CBN_CLOSEUP) {
                PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
            }
            break;
        case RunningIndicatorCheck:
            if (notification == BN_CLICKED) {
                settings_cache_.show_running_indicator =
                    IsChecked(running_indicator_check_);
                ScheduleSettingsSave();
            }
            break;
        default:
            break;
        }

        bool history_commit = false;
        if (notification == EN_KILLFOCUS &&
            IsNumericEditControl(command_control)) {
            history_commit = true;
        } else if (notification == CBN_SELCHANGE) {
            switch (id) {
            case ActionPatternCombo:
            case BackendCombo:
            case RandomIntervalStyleCombo:
            case DownDurationBehaviorCombo:
            case MouseButtonCombo:
            case GeneratedKeyCombo:
            case WindowsNotificationCombo:
            case SystemSoundCombo:
                history_commit = true;
                break;
            default:
                break;
            }
        } else if (notification == BN_CLICKED) {
            switch (id) {
            case BackgroundInputCheck:
            case RandomIntervalCheck:
            case CurrentCursorRadio:
            case FixedPositionRadio:
            case ClickPositionIndicatorCheck:
            case UnlimitedRadio:
            case LimitedRadio:
            case DiagnosticsCheck:
            case SafetyShieldCheck:
            case ForceExitOnEmergencyStopCheck:
            case RunningIndicatorCheck:
                history_commit = true;
                break;
            default:
                break;
            }
        }
        if (history_commit) {
            CommitSettingsHistoryFromControls();
        }

        // Any committed control change makes the retained movement frame
        // visually obsolete even when the selected page, Input type, scroll
        // offset, and dimensions remain the same. Combo selection changes are
        // marked immediately but refreshed only after the popup closes, so an
        // application-owned list can never become part of the cached frame.
        if (notification == BN_CLICKED || notification == EN_CHANGE ||
            notification == CBN_CLOSEUP) {
            MarkMoveCoverPresentationDirty(true);
        } else if (notification == CBN_SELCHANGE) {
            MarkMoveCoverPresentationDirty(false);
        }
        return 0;
    }

    case WM_TIMER:
        if (w_param == DiagnosticsTimerId && controller_) {
            RefreshPresentation(false, false, false, true);
            return 0;
        }
        if (position_capture_timer_id_ != 0 &&
            w_param == position_capture_timer_id_) {
            // A generated WM_TIMER can already be waiting in the queue when
            // KillTimer() cancels the countdown. Never let a late message
            // revive or finish a capture whose owning UI state has ended.
            const bool keyboard =
                SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;
            if (position_capture_seconds_remaining_ <= 0 || keyboard) {
                CancelPositionCapture();
                return 0;
            }

            --position_capture_seconds_remaining_;
            if (position_capture_seconds_remaining_ <= 0) {
                FinishPositionCapture();
            } else {
                SetCapturePositionButtonText(
                    L"Capturing in " +
                    std::to_wstring(position_capture_seconds_remaining_) + L"...");
            }
            return 0;
        }
        if (w_param == TargetStatusTimerId) {
            UpdateTargetDisplay();
            return 0;
        }
        if (settings_save_timer_id_ != 0 &&
            w_param == settings_save_timer_id_) {
            StopGeneratedTimer(settings_save_timer_id_);
            settings_save_pending_ = false;
            SaveSettingsIfEnabled();
            return 0;
        }
        if (status_layout_timer_id_ != 0 &&
            w_param == status_layout_timer_id_) {
            StopGeneratedTimer(status_layout_timer_id_);
            UpdateStatusAreaLayout();
            return 0;
        }
        if (support_email_copied_timer_id_ != 0 &&
            w_param == support_email_copied_timer_id_) {
            StopGeneratedTimer(support_email_copied_timer_id_);
            if (status_text_ != nullptr &&
                IsWindow(status_text_) != FALSE &&
                GetControlText(status_text_) == SupportEmailCopiedStatus) {
                const EngineState state =
                    controller_ ? controller_->State() : EngineState::Ready;
                const std::uint64_t completed =
                    controller_ ? controller_->CompletedActions() : 0U;
                UpdateStatus(state, completed);
            }
            return 0;
        }
        if (diagnostic_report_copied_timer_id_ != 0 &&
            w_param == diagnostic_report_copied_timer_id_) {
            StopGeneratedTimer(diagnostic_report_copied_timer_id_);
            if (status_text_ != nullptr &&
                IsWindow(status_text_) != FALSE &&
                GetControlText(status_text_) == DiagnosticReportCopiedStatus) {
                const EngineState state =
                    controller_ ? controller_->State() : EngineState::Ready;
                const std::uint64_t completed =
                    controller_ ? controller_->CompletedActions() : 0U;
                UpdateStatus(state, completed);
            }
            return 0;
        }
        if (run_notification_cleanup_timer_id_ != 0 &&
            w_param == run_notification_cleanup_timer_id_) {
            StopGeneratedTimer(run_notification_cleanup_timer_id_);
            run_feedback_presenter_.RemoveNotificationIcon();
            return 0;
        }
        if (advanced_scroll_settle_timer_id_ != 0 &&
            w_param == advanced_scroll_settle_timer_id_) {
            StopGeneratedTimer(advanced_scroll_settle_timer_id_);
            FinishAdvancedScrollSnapshot();
            return 0;
        }
        break;

    case WM_APP_PRESENT_SINGLE_INSTANCE_NOTICE:
        single_instance_notice_pending_ = false;
        if (single_instance_notice_presenting_) {
            return 0;
        }
        single_instance_notice_presenting_ = true;
        PresentAlreadyRunningNotice();
        single_instance_notice_presenting_ = false;
        return 0;

    case WM_APP_REQUEST_START:
        // Only the global Start / Stop hotkey posts this message. A later Stop
        // increments the generation before cancellation, making any older
        // queued Start request stale even if the worker reaches Ready quickly.
        if (w_param != 0 &&
            static_cast<std::uint32_t>(w_param) ==
                hotkey_toggle_generation_.load(std::memory_order_acquire)) {
            StartFromControls();
        }
        return 0;

    case WM_APP_HOTKEY_NORMAL_STOP:
        // The hotkey thread already published cancellation so it remains free
        // for Emergency Stop. Finish only the UI-owned semantics here.
        RemoveQueuedStartRequests();
        ResetClickPositionIndicator();
        if (last_run_diagnostic_.available &&
            last_run_diagnostic_.outcome == core::DiagnosticRunOutcome::InProgress) {
            last_run_diagnostic_.outcome = core::DiagnosticRunOutcome::NormalStop;
        }
        return 0;

    case WM_APP_FORCE_STOP_EXIT:
        BeginForceStopAndExit(w_param != 0);
        return 0;

    case WM_APP_RETRY_CLEANUP:
        RetryEmergencyCleanup();
        return 0;

    case WM_APP_REFRESH_MOVE_COVER:
        move_cover_refresh_posted_ = false;
        RefreshMoveCoverCache();
        return 0;

    case WM_APP_EMERGENCY_UI:
        emergency_ui_pending_.store(false, std::memory_order_release);
        RemoveQueuedStartRequests();
        CancelPositionCapture();
        DismissActiveMessageBoxForEmergency();
        if (!emergency_ui_presented_.exchange(true, std::memory_order_acq_rel)) {
            ShowSafetyShield();
        }
        // Resolve a completed, no-shield Emergency Stop before applying enabled
        // states. This avoids an otherwise unnecessary disabled-then-enabled
        // frame across the entire main UI. If cleanup is still pending, the
        // Safety Shield is visible, or recovery is unresolved, the latch stays
        // active and RefreshEnabledState() still publishes the required lock.
        AdvanceEmergencyCompletion();
        RefreshEnabledState();
        return 0;

    case WM_APP_SAFETY_SHIELD_CLOSED:
        AdvanceEmergencyCompletion();
        RefreshEnabledState();
        return 0;

    case WM_APP_APPLY_INPUT_TYPE:
        input_type_update_posted_ = false;
        ApplyInputTypeUpdate();
        return 0;

    case WM_APP_FINISH_STARTUP:
        FinishStartupPresentation();
        return 0;

    case WM_APP_REPAIR_SURFACES:
        surface_repair_posted_ = false;
        RepairVisibleSurfaces();
        return 0;

    case WM_APP_CAPTURE_KEY:
        if (key_capture_combo_ != nullptr) {
            (void)CaptureKeyForCombo(
                key_capture_combo_,
                static_cast<UINT>(w_param),
                static_cast<std::uint16_t>(l_param));
        }
        return 0;

    case WM_APP_REFRESH_NUMERIC_PRESENTATION: {
        numeric_presentation_refresh_posted_ = false;
        if (!numeric_presentation_refresh_pending_) {
            return 0;
        }
        const bool update_rate = numeric_rate_refresh_pending_;
        numeric_presentation_refresh_pending_ = false;
        numeric_rate_refresh_pending_ = false;
        RefreshPresentation(update_rate, true, false, true);
        return 0;
    }

    case WM_APP_CLICK_INDICATOR: {
        click_indicator_update_pending_.store(false, std::memory_order_release);
        const std::uint64_t packed_point =
            latest_click_indicator_point_.load(std::memory_order_acquire);
        const ScreenPoint point{
            static_cast<std::int32_t>(static_cast<std::uint32_t>(packed_point >> 32U)),
            static_cast<std::int32_t>(static_cast<std::uint32_t>(packed_point))};
        if (controller_ && controller_->State() == EngineState::Running &&
            IsChecked(click_position_indicator_check_) &&
            SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 0) {
            if (!click_position_indicator_.Create(instance_)) {
                click_position_indicator_available_ = false;
                SetChecked(click_position_indicator_check_, false);
                ScheduleSettingsSave();
                RefreshEnabledState();
                return 0;
            }
            click_position_indicator_.ShowAt(point, dpi_);
        }
        return 0;
    }

    case WM_APP_RUN_SESSION_READY:
        if (!shutdown_in_progress_.load(std::memory_order_acquire)) {
            BeginRunFeedback();
        }
        return 0;

    case WM_APP_RECONCILE_ENGINE_STATE: {
        if (controller_ == nullptr || window_ == nullptr ||
            IsWindow(window_) == FALSE) {
            return 0;
        }

        // PostMessageW failure can occur after an older state transition was
        // already queued successfully. A nonqueued reconciliation therefore
        // removes obsolete queued engine-state transitions before applying the
        // controller's current authoritative atomic state. This prevents an old
        // Running or Stopping message from regressing the recovered UI later.
        MSG pending_state{};
        while (PeekMessageW(&pending_state,
                            window_,
                            WM_APP_ENGINE_STATE,
                            WM_APP_ENGINE_STATE,
                            PM_REMOVE) != FALSE) {
        }

        const EngineState current_state = controller_->State();
        if (current_state != EngineState::Running) {
            // Run-session-ready is presentation-only and is posted after the
            // Running state. If the authoritative state already advanced past
            // Running, discard any readiness notification from that old run so
            // it cannot reactivate start feedback after terminal reconciliation.
            MSG pending_ready{};
            while (PeekMessageW(&pending_ready,
                                window_,
                                WM_APP_RUN_SESSION_READY,
                                WM_APP_RUN_SESSION_READY,
                                PM_REMOVE) != FALSE) {
            }
        }

        return HandleMessage(
            WM_APP_ENGINE_STATE,
            static_cast<WPARAM>(current_state),
            static_cast<LPARAM>(controller_->CompletedActions()));
    }

    case WM_APP_ENGINE_STATE: {
        const auto state = static_cast<EngineState>(w_param);
        if (state != EngineState::Running) {
            ResetClickPositionIndicator();
        }

        // A run-state transaction updates many independent child HWNDs. Each
        // EnableWindow call synchronously delivers WM_ENABLE, so DWM can expose
        // an intermediate mixture of old and new control presentations even
        // though the final logical state is correct. Vector Click already uses
        // a retained complete client frame for settings / page transactions that
        // have the same multi-HWND publication problem. Reuse that established
        // presentation boundary only while this window is visibly foreground.
        // A global hotkey may change state while another application owns the
        // foreground; never create the independent retained popup in that case.
        const bool terminal_state =
            state == EngineState::Ready || state == EngineState::Disarmed ||
            state == EngineState::Faulted;
        bool retained_state_frame_active = false;
        if ((state == EngineState::Running || terminal_state) &&
            startup_presentation_complete_ && window_ != nullptr &&
            IsWindow(window_) != FALSE && IsWindowVisible(window_) != FALSE &&
            IsIconic(window_) == FALSE && GetForegroundWindow() == window_ &&
            !interactive_resize_ && !programmatic_resize_overlay_ &&
            !shutdown_in_progress_.load(std::memory_order_acquire) &&
            !emergency_latched_.load(std::memory_order_acquire) &&
            !safety_shield_.IsVisible()) {
            retained_state_frame_active =
                BeginResizeOverlay(nullptr, 0, false);
        }

        const auto completed = static_cast<std::uint64_t>(l_param);
        if (state == EngineState::Running) {
        } else if (state == EngineState::Stopping) {
        } else if (state == EngineState::Ready) {
        } else if (state == EngineState::Disarmed) {
        } else if (state == EngineState::Faulted) {
        }
        UpdateStatus(state, completed);
        if (state == EngineState::Ready && controller_ && controller_->LastRunHadBackendFailure()) {
            core::InputBackend requested_backend = core::InputBackend::Automatic;
            const LRESULT backend_selection = SendMessageW(backend_combo_, CB_GETCURSEL, 0, 0);
            if (backend_selection == 1) {
                requested_backend = core::InputBackend::StandardInput;
            } else if (backend_selection == 2) {
                requested_backend = core::InputBackend::ForegroundTargetInput;
            } else if (backend_selection == 3) {
                requested_backend = core::InputBackend::TargetedWindowMessages;
            } else if (backend_selection == 4) {
                requested_backend = core::InputBackend::UnicodeTextInput;
            } else if (backend_selection == 5) {
                requested_backend = core::InputBackend::TargetedUnicodeText;
            }
            const core::InputBackend effective_backend =
                InputBackendDispatcher::EffectiveBackend(
                    requested_backend,
                    target_window_,
                    IsChecked(background_input_check_));
            const bool foreground_target =
                effective_backend == core::InputBackend::ForegroundTargetInput;
            const bool targeted =
                effective_backend == core::InputBackend::TargetedWindowMessages;
            const bool targeted_unicode =
                effective_backend == core::InputBackend::TargetedUnicodeText;
            std::wstring text = targeted_unicode
                                    ? L"Status: Targeted Unicode text stopped safely"
                                    : targeted
                                          ? L"Status: Targeted input stopped safely"
                                          : foreground_target
                                                ? L"Status: Foreground target input stopped safely"
                                                : L"Status: Generated input stopped safely";
            const bool keyboard = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;
            text += L" | Completed " + ActionUnitText(SelectedActionPattern(), keyboard) + L": ";
            text += std::to_wstring(completed);
            const std::wstring details = targeted_unicode
                ? L"Vector Click could not confirm completion of a targeted text character because the selected target became unavailable, was not foreground while background input was off, stopped responding, or no compatible text recipient could be resolved inside that target. A timed-out Windows message may already have begun processing, so the run stopped instead of continuing with additional characters."
                : targeted
                      ? L"Vector Click could not confirm a targeted input operation because the selected target became unavailable, was not foreground while background input was off, was minimized for mouse input, stopped responding, or the configured mouse point was outside that target. Release cleanup was performed for any press that Windows may already have begun processing."
                      : foreground_target
                            ? L"No new foreground-target press was sent because the selected target became unavailable, left the foreground, was minimized, or the current or fixed mouse point was outside that target. Vector Click stopped and performed release cleanup instead of sending to another application."
                            : L"Windows rejected a generated input submission. Vector Click stopped the run and performed release cleanup rather than retrying in a burst.";
            SetStatusPresentation({
                StatusCategory::Ready,
                std::move(text),
                details,
            }, true);
        }
        // A natural / ordinary terminal state means the worker has completed
        // its release path. Emergency Stop keeps its active boundary until the
        // emergency latch is cleared after synchronous release cleanup.
        if (!emergency_latched_.load(std::memory_order_acquire) &&
            (state == EngineState::Ready || state == EngineState::Disarmed ||
             state == EngineState::Faulted)) {
            DisableActiveRunHotkeyGuard();
            HandleRunFeedbackTerminal(state, false);
            EndProcessPriorityActive(false);
        }
        // Engine-state transitions can disable the currently focused Advanced
        // control. Windows may then move focus synchronously to another control,
        // which normally asks the Advanced page to scroll that new focus into
        // view. A run-state change is not a navigation request, so suppress only
        // that focus-driven auto-scroll while the enabled-state transaction is
        // applied. Normal keyboard / mouse focus navigation keeps its existing
        // ensure-visible behavior.
        suppress_advanced_focus_scroll_ = true;
        UpdateEnabledState(state);
        suppress_advanced_focus_scroll_ = false;
        AdvanceEmergencyCompletion();

        if (retained_state_frame_active) {
            // The prior complete frame still covers the visible client while
            // every child reaches its final enabled / text presentation below it.
            // Paint that live hierarchy synchronously, let DWM commit it, then
            // retire the retained frame in one visual boundary. This avoids
            // exposing the sequential WM_ENABLE paints that can otherwise
            // flicker intermittently across native control classes.
            RedrawWindow(window_,
                         nullptr,
                         nullptr,
                         RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                             RDW_UPDATENOW);
            (void)DwmFlush();
            HideResizeOverlay();
            MarkMoveCoverPresentationDirty(false);
            QueueMoveCoverRefresh();
        } else if (state == EngineState::Running &&
                   window_ != nullptr && IsWindow(window_) != FALSE &&
                   IsIconic(window_) == FALSE) {
            // Preserve the fail-safe path when a retained foreground frame
            // cannot be used. This prevents generated self-targeted input from
            // starving the Running-state paint behind higher-priority message
            // traffic.
            RedrawWindow(window_,
                         nullptr,
                         nullptr,
                         RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE |
                             RDW_UPDATENOW);
        }
        return 0;
    }

    case WM_APP_HOTKEY_REGISTRATION_RESULT: {
        std::optional<HotkeyRegistrationResult> result;
        {
            std::scoped_lock lock(hotkey_registration_result_mutex_);
            if (pending_hotkey_registration_result_.has_value()) {
                result.emplace(std::move(*pending_hotkey_registration_result_));
                pending_hotkey_registration_result_.reset();
            }
        }
        if (result.has_value() &&
            !shutdown_in_progress_.load(std::memory_order_acquire)) {
            HandleHotkeyRegistrationResult(std::move(*result));
        }
        return 0;
    }

    case WM_APP_HOTKEY_ERROR:
        ShowCenteredMessageBox(
            window_,
            L"A hotkey callback failed safely. No generated input was started.",
            L"Hotkey unavailable",
            MB_OK | MB_ICONWARNING);
        return 0;

    case WM_APP_CLEAR_TRANSIENT_FOCUS:
        // Let the control receiving WM_KILLFOCUS repaint itself. Invalidating
        // every combo box here caused a visible delayed flash after selections.
        tooltips_.Hide();
        SetFocus(window_);
        return 0;

    case WM_QUERYENDSESSION:
        if (force_exit_in_progress_.load(std::memory_order_acquire)) {
            return TRUE;
        }
        if (!session_end_query_pending_) {
            session_end_query_pending_ = true;
            session_end_cleanup_succeeded_ =
                PrepareSafeShutdown(true, false);
        }
        return TRUE;

    case WM_ENDSESSION:
        // Force Stop and Exit is already a terminal, bounded process-exit
        // transaction. Keep the main window and Controller alive until its
        // watchdog terminates the process; destroying them here could release
        // Controller while the detached best-effort cleanup worker is still
        // using it. WM_QUERYENDSESSION already approves the session end.
        if (force_exit_in_progress_.load(std::memory_order_acquire)) {
            return 0;
        }
        if (w_param != FALSE) {
            if (!session_end_query_pending_) {
                session_end_query_pending_ = true;
                session_end_cleanup_succeeded_ =
                    PrepareSafeShutdown(true, false);
            }
            if (window_ != nullptr && IsWindow(window_) != FALSE) {
                DestroyWindow(window_);
            }
        } else {
            CancelPendingSessionEnd();
        }
        return 0;

    case WM_CLOSE:
        if (force_exit_in_progress_.load(std::memory_order_acquire) ||
            shutdown_in_progress_.load(std::memory_order_acquire)) {
            return 0;
        }
        if (PrepareSafeShutdown(true, true)) {
            DestroyWindow(window_);
        } else {
            PresentShutdownRecovery();
        }
        return 0;

    case WM_DESTROY:
        OnDestroy();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window_, message, w_param, l_param);
}

bool MainWindow::OnCreate() {
    dpi_ = GetDpiForWindow(window_);
    RecreateFontForDpi();

    content_host_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        ContentHostClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0,
        0,
        Scale(layout::FixedContentWidth, dpi_),
        Scale(CurrentContentHeightLogical(), dpi_),
        window_,
        nullptr,
        instance_,
        this);
    if (content_host_ == nullptr) {
        return false;
    }

    combo_popup_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        ComboPopupClassName,
        L"",
        WS_POPUP,
        0,
        0,
        1,
        1,
        window_,
        nullptr,
        instance_,
        this);
    if (combo_popup_ == nullptr) {
        return false;
    }

    CreateControls();
    if (basic_tab_button_ == nullptr || advanced_tab_button_ == nullptr ||
        about_tab_button_ == nullptr || basic_page_host_ == nullptr ||
        advanced_page_host_ == nullptr ||
        advanced_scroll_content_host_ == nullptr ||
        advanced_scrollbar_ == nullptr ||
        about_page_host_ == nullptr || basic_top_host_ == nullptr) {
        return false;
    }
    // The indicator window is created lazily on the first enabled mouse press.
    // Keeping the feature off therefore creates no extra window or GDI surface.
    click_position_indicator_available_ = true;
    LayoutControls();
    PositionContentHost(false);

    if (!tooltips_.Create(window_)) {
        ShowCenteredMessageBox(window_, L"Tooltips could not be initialized.", L"Vector Click", MB_OK | MB_ICONWARNING);
    }

    auto add_help = [this](const std::wstring& text, const std::initializer_list<HWND> controls) {
        for (const HWND control : controls) {
            tooltips_.Add(control, text);
        }
    };

    add_help(L"Choose whether Vector Click sends mouse clicks or keyboard presses.",
             {action_type_label_, action_type_combo_});
    add_help(L"Choose how many inputs each action sends. Single sends 1, Double 2, Triple 3, Burst uses Burst count, and Hold keeps the selected button or key down until stopped.",
             {action_pattern_label_, action_pattern_combo_});
    add_help(L"Set the number of clicks or key presses in each Burst. Available only when Action pattern is Burst. Range: 2 to 100.",
             {burst_count_label_, burst_count_edit_});
    add_help(L"Set the delay between inputs inside a Double, Triple, or Burst action. Lower values place them closer together. Fixed and Natural (configured center) require Action spacing to be at least as long as the configured Down duration. Natural (automatic center) adapts generated press duration to the available spacing.",
             {action_spacing_label_, action_spacing_minutes_edit_,
              action_spacing_seconds_edit_, action_spacing_edit_});
    add_help(L"Choose how input is sent. Automatic uses Standard input with no target, Foreground target input when a target is selected, and Targeted window messages only when Allow background input is enabled. Unicode text input sends printable characters to the current foreground application as text. Targeted Unicode text sends printable characters directly to the selected target and is compatibility-dependent. Standard and Foreground target input preserve physical-key semantics.",
             {backend_label_, backend_combo_});
    add_help(L"Shows the selected target window. Automatic, Foreground target input, Targeted window messages, and Targeted Unicode text use it; Standard input and Unicode text input send to the currently active window instead. Targeted message methods may not work with every application.",
             {target_group_, target_label_, target_status_text_});
    tooltips_.Add(select_target_button_,
        L"Choose the exact application window to use for targeted input.");
    tooltips_.Add(clear_target_button_,
        L"Clear the selected target window. Target selections are not saved between launches.");
    tooltips_.Add(background_input_check_,
        L"Allow Automatic, Targeted window messages, or Targeted Unicode text to use direct background delivery while the selected window is not in front. When this is off, Automatic uses Foreground target input instead, and explicit targeted methods require the selected target to be in front. Some applications ignore background messages, and minimized windows may not accept mouse clicks.");
    add_help(L"Choose which mouse button Vector Click will click.",
             {mouse_button_label_, mouse_button_combo_});
    tooltips_.Add(generated_key_label_, GeneratedKeyTooltip);
    tooltips_.Add(generated_key_combo_, GeneratedKeyTooltip);
    add_help(L"Set how often a new action starts when random interval is off. Lower values run faster. Double, Triple, and Burst send multiple inputs per action, so the displayed click or key rate can be higher. Vector Click enforces a 10,000-input-per-second safety limit.",
             {interval_label_, basic_interval_minutes_header_,
              basic_interval_seconds_header_,
              basic_interval_milliseconds_header_, interval_minutes_edit_,
              interval_seconds_edit_, interval_edit_, rate_text_});
    add_help(L"Set the configured Down duration. Fixed uses this value exactly. Natural (configured center) varies mouse-button or key presses around this value. Natural (automatic center) chooses its own typical press duration and temporarily disables these fields. Hold mode ignores this setting.",
             {button_down_label_, button_down_minutes_edit_,
              button_down_seconds_edit_, button_down_edit_});
    add_help(
        L"Choose how Down duration is produced. Fixed preserves the configured value. Natural (configured center) varies around that value using measured human press-duration characteristics. Natural (automatic center) chooses a typical duration from the configured action pace and then varies it naturally. Natural timing uses input-specific mouse and keyboard calibration from recorded physical press / release timing.",
        {down_duration_behavior_label_, down_duration_behavior_combo_});
    tooltips_.Add(
        random_interval_check_,
        L"Choose a new delay between Minimum interval and Maximum interval before each action after the first. The first action still begins immediately. Action spacing is not randomized.");
    add_help(
        L"Independent chooses every interval separately from the full range, so longer runs tend toward the range average. Drifting holds irregular fast, middle, or slow timing periods. Natural variation uses measured human-timing characteristics with input-specific mouse and keyboard calibration: local pace changes on shorter and longer time scales with asymmetric short-term variation, while always staying inside the same Minimum and Maximum limits.",
        {random_interval_style_label_, random_interval_style_combo_});
    add_help(
        L"Set the inclusive random interval range with Minutes, Seconds, and Milliseconds. Each visible component accepts 0 through 1,000,000 and remains exactly as entered. Minimum must be greater than 0 and Maximum cannot be lower than Minimum. Fixed and Natural (configured center) require the configured Down duration to be no longer than Minimum. Natural (automatic center) adapts generated press duration to the available interval. These fields are used only while random interval is enabled.",
        {minimum_interval_label_, minimum_interval_minutes_edit_,
         minimum_interval_seconds_edit_, minimum_interval_edit_,
         maximum_interval_label_, maximum_interval_minutes_edit_,
         maximum_interval_seconds_edit_, maximum_interval_edit_});
    tooltips_.Add(position_group_,
        L"Choose where generated mouse clicks occur. These controls do not apply to keyboard input.");
    tooltips_.Add(current_cursor_radio_,
        L"Click at the pointer's current location when each mouse click begins. This option does not apply to keyboard input.");
    tooltips_.Add(fixed_position_radio_,
        L"Always click the saved X and Y screen coordinate. This option does not apply to keyboard input.");
    tooltips_.Add(position_unavailable_text_,
        L"Position controls apply only to mouse input.");
    add_help(L"Set the fixed screen coordinates. Negative values are valid for monitors positioned left of or above the primary monitor.",
             {fixed_x_label_, fixed_x_edit_, fixed_y_label_, fixed_y_edit_});
    tooltips_.Add(capture_position_button_,
        L"After four seconds, save the pointer's current location and select Fixed screen position.");
    tooltips_.Add(click_position_indicator_check_,
        L"Show a temporary blue circle at each generated mouse-click location. It does not receive input and is hidden when the run stops.");
    tooltips_.Add(repeat_group_,
        L"Configure the action-count limit and optional run time limit.");
    tooltips_.Add(unlimited_radio_,
        L"Run without an action-count limit. An enabled time limit can still stop the run.");
    add_help(
        L"Stop after 1 through 1,000,000 complete actions. If a time limit is also enabled, the first limit reached stops the run. Hold ignores the action-count limit.",
        {limited_radio_, repeat_count_edit_, repeat_unit_label_});
    add_help(
        L"Set a run time limit using Hours, Minutes, and Seconds from 0 through 1,000,000. 0 / 0 / 0 turns the time limit off. If both limits are active, the first limit reached stops the run. Hold can use the time limit.",
        {run_time_hours_header_, run_time_minutes_header_, run_time_seconds_header_,
         run_time_hours_edit_, run_time_minutes_edit_, run_time_seconds_edit_});
    tooltips_.Add(start_hotkey_label_, StartHotkeyTooltip);
    tooltips_.Add(start_hotkey_combo_, StartHotkeyTooltip);
    tooltips_.Add(emergency_hotkey_label_, EmergencyHotkeyTooltip);
    tooltips_.Add(emergency_hotkey_combo_, EmergencyHotkeyTooltip);
    tooltips_.Add(hotkeys_group_,
        L"Safety hotkeys are being registered with Windows. Start remains unavailable until both are confirmed.");
    tooltips_.Add(diagnostics_check_,
        L"Show live action counts, generated inputs, measured rates, elapsed time, and readiness details. Off by default.");
    tooltips_.Add(diagnostics_text_, DiagnosticsStatusTooltip);
    tooltips_.Add(safety_shield_check_,
        L"After Emergency Stop, cover the virtual desktop with a dark Safety Shield while cleanup finishes. It keeps physical clicks from reaching other windows and remains open until you close it. It is not used for a normal Stop.");
    tooltips_.Add(force_exit_on_emergency_stop_check_,
        L"When enabled, Emergency Stop immediately arms a hard two-second process-exit deadline, cancels active input, and makes bounded release-cleanup attempts before Vector Click terminates. This is a last-resort safety option. Cleanup is best effort, and unsaved in-memory changes may be lost.");
    tooltips_.Add(capture_exclusion_check_,
        L"Ask Windows to hide Vector Click windows from supported screenshots, recordings, and screen sharing. The window may still appear in a capture app's selection list. This does not block every capture method, external capture devices, or cameras.");
    tooltips_.Add(keep_on_top_check_,
        L"Keep the Vector Click window above normal application windows even when Vector Click is not active. Other topmost windows and Windows system UI may still appear above it. This does not prevent other applications from becoming active.");
    add_help(
        L"Choose a named local profile. Profiles are separate validated snapshots of portable Vector Click settings. Loading a profile does not overwrite it when you make temporary changes. Target-window identity and the session-only scheduling controls are not stored in profiles.",
        {profile_label_, profile_combo_});
    tooltips_.Add(
        manage_profiles_button_,
        L"Create, save, rename, duplicate, import, export, or delete local profiles. Profiles are stored separately in the local Vector Click Profiles folder beside the application.");
    tooltips_.Add(remember_settings_check_,
        L"Save the current options to 'VectorClick settings.json' beside the executable. Vector Click asks before creating the file. Target-window selections are never saved.");
    tooltips_.Add(
        import_settings_button_,
        L"Load a Vector Click settings JSON file from any location. The file is validated before use. Importing does not move or rename the selected file and does not enable Remember settings. If Remember settings is already enabled, the imported configuration is saved to the normal portable settings file.");
    tooltips_.Add(
        performance_group_,
        L"Adjust how Windows schedules Vector Click. System default / System managed is recommended for most systems because higher settings are not always faster.");
    tooltips_.Add(
        performance_guidance_text_,
        L"Higher scheduling settings can help under heavy CPU load on some systems, but they can make little difference or perform worse on others.");
    add_help(
        L"Controls how strongly Windows prioritizes Vector Click compared with other running programs. System default is recommended for most systems. Above Normal is the first step to try if heavy CPU load affects timing. High may help some systems, but can make other applications less responsive. While active modes apply the higher priority only during a run and restore it afterward.",
        {process_priority_label_, process_priority_combo_});
    add_help(
        L"Controls the priority of the thread responsible for timing and generated input. System default is recommended for most systems. +1 or +2 may improve timing under heavy CPU load, but can make little difference or perform worse on some systems. +2 is blocked with High process priority.",
        {timing_worker_priority_label_, timing_worker_priority_combo_});
    add_help(
        L"Controls how Windows manages performance and efficiency for the timing worker. System managed is recommended for most systems. HighQoS favors performance behavior, while EcoQoS lets Windows favor efficient scheduling. Either setting may help, do nothing, or perform worse depending on the system.",
        {timing_worker_qos_label_, timing_worker_qos_combo_});
    add_help(
        L"Controls the priority of the thread that receives global Start / Stop and Emergency Stop hotkeys. System default is recommended for most systems. Above Normal (+1) may improve hotkey responsiveness under severe CPU load, but most systems should not need it.",
        {hotkey_control_priority_label_, hotkey_control_priority_combo_});
    tooltips_.Add(
        notifications_group_,
        L"Choose optional Windows notifications, system sounds, and an active icon indicator for run start and stop events. All feedback is off by default.");
    add_help(
        L"Show a silent Windows notification when a run starts, stops, or both. The separate System sound setting controls audio. Windows notifications are shell-owned and may remain visible during screenshots, recordings, or screen sharing.",
        {windows_notification_label_, windows_notification_combo_});
    add_help(
        L"Play a Windows system sound when a run starts, stops, or both. The current Windows sound scheme controls the actual sound. Sound playback is best effort and never changes run behavior.",
        {system_sound_label_, system_sound_combo_});
    tooltips_.Add(
        running_indicator_check_,
        L"Add a violet status dot to the upper-left of the Vector Click application icon after a run has initialized successfully. The normal icon returns after release cleanup completes. The indicator may appear in the taskbar, title bar, and Alt+Tab and is not hidden by Vector Click's screen-capture exclusion.");
    tooltips_.Add(start_button_,
        L"Start the selected action after all settings, the target, and both safety hotkeys are ready. Start remains unavailable during hotkey registration, Emergency Stop cleanup, or while the Safety Shield is open.");
    tooltips_.Add(stop_button_,
        L"Stop the current run normally. If release cleanup remains unresolved, this button becomes Retry cleanup and resubmits releases without generating a new press.");
    tooltips_.Add(emergency_button_,
        L"Cancel the current run immediately and release any input Vector Click still tracks as held. Input already accepted by Windows or another application cannot be recalled.");
    tooltips_.Add(admin_button_,
        L"Restart Vector Click through the Windows administrator prompt. This may be needed to control a target that is also running as administrator.");
    tooltips_.Add(copy_diagnostic_report_button_,
        L"Generate a privacy-filtered diagnostic report from Vector Click's current state and active or last run, then copy it to the Windows clipboard. The report is generated locally and Vector Click does not transmit it.");
    if (ordinary_status_tooltip_.empty()) {
        ordinary_status_tooltip_ = OrdinaryStatusDefaultTooltip;
    }
    tooltips_.Add(status_text_, ordinary_status_tooltip_);

    // Keep keyboard focus visible throughout the scrollable Advanced page.
    // Install this subclass after tooltip subclasses so focus-driven scrolling
    // completes first in the subclass chain. SetAdvancedScrollOffset()
    // intentionally dismisses pending tooltips while the viewport moves; the
    // tooltip subclass then observes the same WM_SETFOCUS against the control's
    // final on-screen position and can start its keyboard tooltip normally.
    // Every direct WS_TABSTOP child is covered because custom choices and
    // owner-drawn buttons do not all publish the same parent focus notifications.
    if (advanced_scroll_content_host_ != nullptr &&
        IsWindow(advanced_scroll_content_host_) != FALSE) {
        for (HWND child = GetWindow(advanced_scroll_content_host_, GW_CHILD);
             child != nullptr;
             child = GetWindow(child, GW_HWNDNEXT)) {
            const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
            if ((style & WS_TABSTOP) != 0) {
                (void)SetWindowSubclass(
                    child,
                    AdvancedFocusSubclassProc,
                    1,
                    reinterpret_cast<DWORD_PTR>(this));
            }
        }
    }

    core::RunSettings initial = core::DefaultRunSettings();
    std::optional<std::wstring> profile_to_associate;
    std::wstring load_error;
    if (settings_store_.Exists()) {
        core::RunSettings loaded = core::DefaultRunSettings();
        if (settings_store_.Load(loaded, load_error)) {
            last_saved_settings_ = loaded;
            std::optional<std::wstring> stored_profile_id;
            if (settings_store_.LoadActiveProfileId(stored_profile_id)) {
                last_saved_profile_id_ = stored_profile_id;
            } else {
                last_saved_profile_id_.reset();
            }
            if (loaded.remember_settings) {
                initial = loaded;
                profile_to_associate = stored_profile_id;
            }
        } else if (!load_error.empty()) {
            const std::wstring message =
                load_error +
                L"\n\nVector Click will use default settings for this launch. The existing file is left unchanged unless new settings are saved.";
            ShowCenteredMessageBox(window_, message.c_str(), L"Settings not loaded", MB_OK | MB_ICONWARNING);
        }
    }

    // An administrator restart is a process transition, not a persistence
    // decision. Restore the exact validated live controls from the standard
    // process for this launch even when Remember settings is off. The handoff
    // is carried only in the bounded startup argument and is never written to
    // the portable settings file merely because elevation was requested.
    if (startup_settings_restore_.has_value()) {
        initial = *startup_settings_restore_;
        profile_to_associate = startup_profile_restore_;
        startup_settings_restore_.reset();
        startup_profile_restore_.reset();
    } else if (startup_settings_restore_invalid_) {
        ShowCenteredMessageBox(
            window_,
            L"Vector Click restarted as administrator, but the transient settings handoff was invalid or incomplete. The target and page can still be restored, but this launch will use the normal remembered or default settings.",
            L"Settings were not restored",
            MB_OK | MB_ICONWARNING);
    }
    startup_settings_restore_invalid_ = false;
    startup_profile_restore_.reset();

    settings_cache_ = initial;
    settings_cache_.start_stop_hotkey = {};
    settings_cache_.emergency_hotkey = {};
    hotkey_registration_pending_ = true;
    safety_hotkeys_confirmed_ = false;
    pending_hotkey_request_id_ = HotkeyThread::InitialRequestId;
    confirmed_start_hotkey_ = {};
    confirmed_emergency_hotkey_ = {};
    active_start_hotkey_key_.store(0, std::memory_order_release);
    active_start_hotkey_modifiers_.store(0, std::memory_order_release);
    active_emergency_hotkey_key_.store(0, std::memory_order_release);
    active_emergency_hotkey_modifiers_.store(0, std::memory_order_release);
    ApplySettings(initial);

    // Restore capture exclusion while the main window and its existing popup
    // surfaces are still hidden. This prevents the first ordinary frame from
    // being capturable when remembered settings request the option.
    SetCaptureExclusionRequested(false);
    if (initial.hide_from_screen_capture) {
        const CaptureExclusionOutcome main_outcome =
            ApplyCaptureExclusion(window_, true);
        if (main_outcome.result == CaptureExclusionResult::Applied) {
            SetCaptureExclusionRequested(true);
        } else {
            initial.hide_from_screen_capture = false;
            settings_cache_.hide_from_screen_capture = false;
            suppress_control_events_ = true;
            SetChecked(capture_exclusion_check_, false);
            suppress_control_events_ = false;

            std::wstring message;
            if (main_outcome.result == CaptureExclusionResult::Unsupported) {
                message =
                    L"Complete screen-capture exclusion requires Windows 10 version 2004 (build 19041) or newer. The remembered option was left off for this launch.";
            } else {
                message =
                    L"Windows could not restore screen-capture hiding for Vector Click. The remembered option was left off for this launch.";
                if (main_outcome.error != ERROR_SUCCESS) {
                    message += L"\n\nWindows error code: " +
                               std::to_wstring(main_outcome.error);
                }
            }
            ShowCenteredMessageBox(window_,
                                   message.c_str(),
                                   L"Screen-capture hiding",
                                   MB_OK | MB_ICONWARNING);
        }
    }

    // Restore the optional topmost state while the main window is still hidden.
    // This is ordinary Windows topmost Z-order only: it does not activate the
    // window, steal focus, or repeatedly force itself to the foreground.
    if (initial.keep_window_on_top && !SetMainWindowTopmost(true)) {
        initial.keep_window_on_top = false;
        settings_cache_.keep_window_on_top = false;
        suppress_control_events_ = true;
        SetChecked(keep_on_top_check_, false);
        suppress_control_events_ = false;
        ShowCenteredMessageBox(
            window_,
            L"Windows could not restore the remembered Keep Vector Click on top setting. The option was left off for this launch.",
            L"Keep on top",
            MB_OK | MB_ICONWARNING);
    }

    suppress_control_events_ = true;
    SelectHotkey(start_hotkey_combo_, {});
    SelectHotkey(emergency_hotkey_combo_, {});
    suppress_control_events_ = false;
    UpdateKeySelectorTooltips();
    if (profile_to_associate.has_value()) {
        core::RunSettings saved_profile{};
        ProfileInfo profile{};
        std::wstring profile_error;
        if (profile_store_.Load(*profile_to_associate,
                                saved_profile, profile, profile_error)) {
            active_profile_info_ = std::move(profile);
            active_profile_saved_settings_ =
                ProfileStore::NormalizeSettingsForProfile(saved_profile);
        }
    }
    RefreshProfileSelector();
    UpdateSafetyHotkeyIndicator();
    RestoreStartupTarget();
    UpdateTargetDisplay();

    if (!safety_shield_.Initialize(instance_, window_)) {
        safety_shield_requested_.store(false, std::memory_order_release);
        SetChecked(safety_shield_check_, false);
        EnableWindow(safety_shield_check_, FALSE);
        ShowCenteredMessageBox(
            window_,
            L"The Safety Shield UI thread could not be initialized. Emergency Stop remains available, but the optional full-desktop shield is disabled for this launch.",
            L"Safety Shield unavailable",
            MB_OK | MB_ICONWARNING);
    }

    (void)run_feedback_presenter_.Initialize(window_, instance_);

    controller_ = std::make_unique<Controller>(
        [this](const EngineState state, const std::uint64_t completed) {
            const HWND notification_window = window_;
            if (notification_window == nullptr ||
                IsWindow(notification_window) == FALSE) {
                return;
            }

            if (PostMessageW(notification_window,
                             WM_APP_ENGINE_STATE,
                             static_cast<WPARAM>(state),
                             static_cast<LPARAM>(completed)) != FALSE) {
                return;
            }

            // State publication has already succeeded inside ClickWorker. If
            // the queued UI notification cannot be posted, request a nonqueued
            // reconciliation instead of waiting synchronously. Cross-thread
            // SendNotifyMessageW delivery returns immediately, so worker and
            // hotkey notification paths do not wait on the main UI while a
            // lifecycle transition is held. Same-thread delivery is direct but
            // creates no cross-thread wait or lifecycle-lock cycle. The
            // reconciliation handler removes older queued state messages before
            // applying the controller's current authoritative state.
            if (shutdown_in_progress_.load(std::memory_order_acquire) ||
                IsWindow(notification_window) == FALSE) {
                return;
            }
            (void)SendNotifyMessageW(
                notification_window, WM_APP_RECONCILE_ENGINE_STATE, 0, 0);
        },
        [this](const ScreenPoint point) {
            const std::uint64_t packed_point =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(point.x)) << 32U) |
                static_cast<std::uint32_t>(point.y);
            latest_click_indicator_point_.store(packed_point, std::memory_order_release);

            bool expected = false;
            if (!click_indicator_update_pending_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return;
            }
            if (window_ == nullptr || IsWindow(window_) == FALSE ||
                PostMessageW(window_, WM_APP_CLICK_INDICATOR, 0, 0) == FALSE) {
                click_indicator_update_pending_.store(false, std::memory_order_release);
            }
        },
        [this] {
            if (window_ != nullptr && IsWindow(window_) != FALSE) {
                (void)PostMessageW(window_, WM_APP_RUN_SESSION_READY, 0, 0);
            }
        });

    hotkey_thread_ = std::make_unique<HotkeyThread>(
        initial.start_stop_hotkey,
        initial.emergency_hotkey,
        [this] {
            if (key_capture_active_.load(std::memory_order_acquire)) {
                PostMessageW(window_,
                             WM_APP_CAPTURE_KEY,
                             static_cast<WPARAM>(active_start_hotkey_key_.load(
                                 std::memory_order_acquire)),
                             static_cast<LPARAM>(active_start_hotkey_modifiers_.load(
                                 std::memory_order_acquire)));
                return;
            }
            if (shutdown_in_progress_.load(std::memory_order_acquire) ||
                emergency_latched_.load(std::memory_order_acquire) ||
                controller_ == nullptr) {
                return;
            }

            const EngineState state = controller_->State();
            if (state == EngineState::Running) {
                // Invalidate every older queued hotkey Start before publishing
                // cancellation. Without this guard, a delayed Start request can
                // become actionable after a fast Stop and accidentally run the
                // Start-only foreground checks when the user intended only to
                // stop the current session.
                hotkey_toggle_generation_.fetch_add(1U, std::memory_order_acq_rel);

                // Cancellation-only by design. Release cleanup remains on the
                // worker thread so this global hotkey thread can immediately
                // process a following Emergency Stop request.
                if (controller_->RequestStop()) {
                    run_stop_intent_.store(
                        core::DiagnosticRunOutcome::NormalStop,
                        std::memory_order_release);
                    if (window_ != nullptr && IsWindow(window_) != FALSE) {
                        (void)PostMessageW(
                            window_, WM_APP_HOTKEY_NORMAL_STOP, 0, 0);
                    }
                }
                return;
            }

            if (state == EngineState::Stopping) {
                hotkey_toggle_generation_.fetch_add(1U, std::memory_order_acq_rel);
                return;
            }

            if (state == EngineState::Ready || state == EngineState::Disarmed) {
                const std::uint32_t generation =
                    hotkey_toggle_generation_.fetch_add(
                        1U, std::memory_order_acq_rel) + 1U;
                (void)PostMessageW(
                    window_,
                    WM_APP_REQUEST_START,
                    static_cast<WPARAM>(generation),
                    0);
            }
        },
        [this] {
            if (key_capture_active_.load(std::memory_order_acquire)) {
                PostMessageW(window_,
                             WM_APP_CAPTURE_KEY,
                             static_cast<WPARAM>(active_emergency_hotkey_key_.load(
                                 std::memory_order_acquire)),
                             static_cast<LPARAM>(active_emergency_hotkey_modifiers_.load(
                                 std::memory_order_acquire)));
                return;
            }
            const bool force_exit_requested =
                force_exit_on_emergency_stop_requested_.load(
                    std::memory_order_acquire);
            bool force_exit_claimed = false;
            if (force_exit_requested) {
                bool expected = false;
                force_exit_claimed = force_exit_in_progress_.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
                if (force_exit_claimed) {
                    // Arm the hard process-exit deadline on the hotkey thread
                    // before relying on the main UI or any release-ledger call.
                    // If either path becomes unresponsive, Vector Click still
                    // terminates at the bounded force-exit deadline.
                    force_exit::StartWatchdogOrTerminate();
                }
            }

            const bool first_request = !emergency_latched_.exchange(
                true, std::memory_order_acq_rel);
            if (first_request) {
                emergency_cleanup_complete_.store(false, std::memory_order_release);

                // Cancellation and the optional protective overlay are
                // requested before any release-ledger call that could wait on
                // a backend lock or slow target. Force-exit mode does not
                // enable the shield implicitly; its own watchdog is the final
                // escape when the shield preference is off.
                bool was_running = false;
                if (controller_) {
                    was_running = controller_->RequestEmergencyStop();
                }
                if (was_running) {
                    run_stop_intent_.store(
                        core::DiagnosticRunOutcome::EmergencyStop,
                        std::memory_order_release);
                }
                if (safety_shield_requested_.load(std::memory_order_acquire) ||
                    (!force_exit_requested && CleanupRequired())) {
                    (void)safety_shield_.Show();
                    if (force_exit_requested) {
                        // Show and ForceStopping are queued to the independent
                        // shield thread in order, so a stalled main window does
                        // not leave an enabled force-exit session presenting as
                        // ordinary Emergency Stop recovery.
                        safety_shield_.BeginForceExit();
                    }
                }
                DismissActiveMessageBoxForEmergency();
                QueueEmergencyUi();

                bool releases_submitted = false;
                if (controller_) {
                    releases_submitted =
                        controller_->CompleteEmergencyStop(was_running);
                }
                const bool cleanup_required = CleanupRequired();
                emergency_release_failed_.store(
                    !releases_submitted || cleanup_required,
                    std::memory_order_release);
                emergency_cleanup_complete_.store(true, std::memory_order_release);
                if (safety_shield_.IsVisible()) {
                    if (releases_submitted && !cleanup_required) {
                        safety_shield_.MarkComplete();
                    } else {
                        safety_shield_.MarkFailed();
                    }
                }
            } else if (safety_shield_requested_.load(std::memory_order_acquire) ||
                       (!force_exit_requested && CleanupRequired())) {
                (void)safety_shield_.Show();
            }

            if (force_exit_claimed && window_ != nullptr &&
                IsWindow(window_) != FALSE) {
                // The watchdog is already authoritative. This message only
                // asks the UI thread to enter the existing force-stop state and
                // launch the additional bounded cleanup passes.
                (void)PostMessageW(
                    window_, WM_APP_FORCE_STOP_EXIT, static_cast<WPARAM>(1), 0);
            }
            DismissActiveMessageBoxForEmergency();
            QueueEmergencyUi();
        },
        [this] {
            const HWND notification_window = window_;
            if (shutdown_in_progress_.load(std::memory_order_acquire) ||
                notification_window == nullptr ||
                IsWindow(notification_window) == FALSE) {
                return;
            }

            if (PostMessageW(notification_window, WM_APP_HOTKEY_ERROR, 0, 0) != FALSE) {
                return;
            }

            // Callback-failure reporting carries no pointer payload. If the
            // ordinary queue is unavailable, use a nonqueued asynchronous wake
            // so the hotkey thread never waits on the UI while reporting an
            // already-contained callback failure.
            if (!shutdown_in_progress_.load(std::memory_order_acquire) &&
                IsWindow(notification_window) != FALSE) {
                (void)SendNotifyMessageW(notification_window,
                                         WM_APP_HOTKEY_ERROR,
                                         0,
                                         0);
            }
        },
        [this](HotkeyRegistrationResult result) {
            {
                std::scoped_lock lock(hotkey_registration_result_mutex_);
                pending_hotkey_registration_result_.emplace(std::move(result));
            }

            const HWND notification_window = window_;
            if (shutdown_in_progress_.load(std::memory_order_acquire) ||
                notification_window == nullptr ||
                IsWindow(notification_window) == FALSE) {
                return;
            }

            if (PostMessageW(notification_window,
                             WM_APP_HOTKEY_REGISTRATION_RESULT,
                             0,
                             0) != FALSE) {
                return;
            }

            // The registration transaction has already completed on the hotkey
            // thread. If the ordinary queued wake-up cannot be posted, use a
            // pointer-free nonqueued notification so the UI can consume the
            // stored authoritative result without making the hotkey thread wait.
            if (!shutdown_in_progress_.load(std::memory_order_acquire) &&
                IsWindow(notification_window) != FALSE) {
                (void)SendNotifyMessageW(
                    notification_window, WM_APP_HOTKEY_REGISTRATION_RESULT, 0, 0);
            }
        },
        initial.hotkey_control_priority_mode);

    UpdateRateLabel();
    UpdateStatus(EngineState::Ready, 0);
    UpdateEnabledState(EngineState::Ready);
    if (startup_target_restored_) {
        SetStatusPresentation({
            StatusCategory::Ready,
            L"Status: Administrator restart complete. Target restored: " +
                target_window_.title,
            {},
        }, true);
    } else if (!startup_target_restore_error_.empty()) {
        SetStatusPresentation({
            StatusCategory::Ready,
            L"Status: Administrator restart complete. Select the target again",
            {},
        }, true);
    }

    RECT startup_bounds{};
    GetClientRect(window_, &startup_bounds);
    startup_cover_ = CreateWindowExW(
        0,
        ContentHostClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0,
        0,
        startup_bounds.right - startup_bounds.left,
        startup_bounds.bottom - startup_bounds.top,
        window_,
        nullptr,
        instance_,
        this);
    if (startup_cover_ != nullptr) {
        SetWindowPos(startup_cover_,
                     HWND_TOP,
                     0,
                     0,
                     startup_bounds.right - startup_bounds.left,
                     startup_bounds.bottom - startup_bounds.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (PostMessageW(window_, WM_APP_FINISH_STARTUP, 0, 0) == FALSE) {
            FinishStartupPresentation();
        }
    } else {
        startup_presentation_complete_ = true;
    }

    PostMessageW(window_, WM_APP_CLEAR_TRANSIENT_FOCUS, 0, 0);
    return true;
}

void MainWindow::OnDestroy() {
    // Force Stop owns a detached cleanup worker whose lifetime is deliberately
    // bounded by the hard process watchdog. If an unexpected path destroys the
    // main window after that terminal transaction begins, terminate immediately
    // rather than releasing Controller while the cleanup worker may still be
    // executing against it.
    if (force_exit_in_progress_.load(std::memory_order_acquire)) {
        force_exit::TerminateCurrentProcess();
    }

    shutdown_in_progress_.store(true, std::memory_order_release);

    // Publish final cancellation while the dedicated hotkey thread is still
    // alive. Normal close, administrator restart, and session-end paths should
    // already have completed this boundary, but WM_DESTROY remains a final
    // idempotent fallback for unexpected destruction.
    if (controller_) {
        const bool was_running = controller_->RequestEmergencyStop();
        (void)controller_->CompleteEmergencyStop(was_running);
    }
    EndProcessPriorityActive(false);

    KillTimer(window_, DiagnosticsTimerId);
    KillTimer(window_, TargetStatusTimerId);
    StopGeneratedTimer(settings_save_timer_id_);
    StopGeneratedTimer(status_layout_timer_id_);
    StopGeneratedTimer(support_email_copied_timer_id_);
    StopGeneratedTimer(diagnostic_report_copied_timer_id_);
    StopGeneratedTimer(run_notification_cleanup_timer_id_);
    StopGeneratedTimer(advanced_scroll_settle_timer_id_);
    run_feedback_presenter_.Shutdown();
    run_feedback_active_ = false;
    HideAdvancedScrollSnapshot();
    settings_save_pending_ = false;
    surface_repair_posted_ = false;
    numeric_presentation_refresh_posted_ = false;
    numeric_presentation_refresh_pending_ = false;
    numeric_rate_refresh_pending_ = false;
    EndKeyCapture();
    CancelPositionCapture();

    hotkey_thread_.reset();

    // The registration-result payload lives in this object rather than in the
    // asynchronous message. Once the hotkey thread is joined, release a result
    // that teardown no longer needs. Any leftover wake-up carries no pointer.
    {
        std::scoped_lock lock(hotkey_registration_result_mutex_);
        pending_hotkey_registration_result_.reset();
    }
    controller_.reset();
    click_position_indicator_.Destroy();
    safety_shield_.Shutdown();
    HideResizeOverlay();
    HideMoveCover();
    move_cover_bitmap_.reset();
    move_cover_width_ = 0;
    move_cover_height_ = 0;
    move_cover_page_index_ = -1;
    move_cover_advanced_scroll_offset_ = -1;
    move_cover_pointer_control_ = nullptr;
    move_cover_bitmap_revision_ = 0U;
    HideBasicTopSnapshot();
    combo_popup_owner_ = nullptr;
    if (GetCapture() == combo_popup_) {
        ReleaseCapture();
    }
    if (combo_popup_ != nullptr && IsWindow(combo_popup_) != FALSE) {
        ShowWindow(combo_popup_, SW_HIDE);
    }
    if (combo_popup_ != nullptr && IsWindow(combo_popup_) != FALSE) {
        DestroyWindow(combo_popup_);
    }
    combo_popup_ = nullptr;
    startup_cover_ = nullptr;
    window_ = nullptr;
    PostQuitMessage(0);
}

void MainWindow::FinishStartupPresentation() {
    if (startup_cover_ == nullptr || IsWindow(startup_cover_) == FALSE) {
        startup_cover_ = nullptr;
        startup_presentation_complete_ = true;
        move_cover_pending_exact_screen_refresh_ =
            !IsCaptureExclusionRequested();
        (void)CaptureMoveCoverFromWindow();
        if (deferred_hotkey_notice_.has_value()) {
            std::wstring notice = std::move(*deferred_hotkey_notice_);
            deferred_hotkey_notice_.reset();
            std::wstring title = L"Safety hotkeys";
            const std::size_t separator = notice.find(L'\n');
            if (separator != std::wstring::npos) {
                title = notice.substr(0, separator);
                notice.erase(0, separator + 1);
            }
            ShowCenteredMessageBox(window_,
                                   notice.c_str(),
                                   title.c_str(),
                                   MB_OK | MB_ICONWARNING);
        }
        return;
    }

    // The cover previously remained above the real controls while the repaint
    // was requested. Windows therefore clipped the child controls out of that
    // paint, and removing the cover exposed only its retained dark bitmap.
    // Move the cover behind the content host first. It still supplies a dark
    // background around the fixed interface while the real hierarchy paints.
    SetWindowPos(startup_cover_,
                 HWND_BOTTOM,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER | SWP_NOREDRAW);

    RedrawWindow(window_,
                 nullptr,
                 nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN |
                     RDW_UPDATENOW);
    (void)DwmFlush();

    DestroyWindow(startup_cover_);
    startup_cover_ = nullptr;
    startup_presentation_complete_ = true;
    surface_repair_posted_ = false;

    // Seed the first movement frame from Vector Click's final child-window
    // layout. The composed desktop can still contain Windows' opening scale
    // transform even though the HWND rectangles already have their final
    // dimensions. A dimension-only cache check cannot detect that mismatch.
    move_cover_pending_exact_screen_refresh_ =
        !IsCaptureExclusionRequested();
    (void)CaptureMoveCoverFromWindow();

    if (deferred_hotkey_notice_.has_value()) {
        std::wstring notice = std::move(*deferred_hotkey_notice_);
        deferred_hotkey_notice_.reset();
        std::wstring title = L"Safety hotkeys";
        const std::size_t separator = notice.find(L'\n');
        if (separator != std::wstring::npos) {
            title = notice.substr(0, separator);
            notice.erase(0, separator + 1);
        }
        ShowCenteredMessageBox(window_,
                               notice.c_str(),
                               title.c_str(),
                               MB_OK | MB_ICONWARNING);
    }
}


bool MainWindow::PreTranslateMessage(const MSG& message) {
    const bool key_capture_active =
        key_capture_active_.load(std::memory_order_acquire);
    if (!key_capture_active) {
        // Native EDIT controls keep keyboard focus after a click on an
        // unfocusable child surface. That is useful while editing, but it is
        // surprising after the user deliberately clicks elsewhere on the
        // current Vector Click page. Treat a click on any non-tab-stop
        // application surface as an explicit end to the numeric editing
        // transaction. Interactive controls still receive the click normally
        // and take focus through their established Windows behavior.
        if (message.message == WM_LBUTTONDOWN) {
            const HWND focused = GetFocus();
            const HWND target = message.hwnd;
            const bool target_in_main_window =
                target != nullptr &&
                (target == window_ || IsChild(window_, target) != FALSE);
            const LONG_PTR target_style =
                target_in_main_window
                    ? GetWindowLongPtrW(target, GWL_STYLE)
                    : 0;
            const bool target_takes_focus =
                target_in_main_window &&
                (target_style & WS_TABSTOP) != 0 &&
                IsWindowEnabled(target) != FALSE;
            if (IsNumericEditControl(focused) && target != focused &&
                target_in_main_window && !target_takes_focus) {
                SetFocus(window_);
            }
        }
        const bool key_message =
            message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
        const bool control_down =
            (GetKeyState(VK_CONTROL) & static_cast<SHORT>(0x8000)) != 0;
        const bool alt_down =
            (GetKeyState(VK_MENU) & static_cast<SHORT>(0x8000)) != 0;
        const bool shift_down =
            (GetKeyState(VK_SHIFT) & static_cast<SHORT>(0x8000)) != 0;
        const UINT virtual_key = static_cast<UINT>(message.wParam);
        const bool settings_history_shortcut =
            key_message && control_down && !alt_down && !shift_down &&
            (virtual_key == static_cast<UINT>(L'Z') ||
             virtual_key == static_cast<UINT>(L'Y'));
        if (!settings_history_shortcut) {
            return false;
        }

        // Numeric fields use one application-owned edit transaction while
        // focused. Native EDIT undo works at the character-operation level and
        // can expose intermediate text such as an empty field after replacing
        // a selected value. Vector Click instead restores the field value that
        // existed when focus entered the numeric edit. Ctrl+Y reapplies that
        // same in-progress edit. Once the field is back at its local baseline,
        // another Ctrl+Z / Ctrl+Y can continue through semantic settings
        // history normally.
        const HWND focused = GetFocus();
        if (IsNumericEditControl(focused)) {
            const bool redo = virtual_key == static_cast<UINT>(L'Y');
            if (HandleNumericEditHistoryShortcut(focused, redo)) {
                return true;
            }
        }

        // Invalid numeric text is never committed into semantic history. If
        // focus has already left that field, Ctrl+Z must still restore the
        // current committed snapshot before older history is traversed. This
        // preserves the warning-state recovery behavior even after a blank-area
        // click has ended the numeric edit session.
        if (virtual_key == static_cast<UINT>(L'Z')) {
            core::RunSettings candidate{};
            std::wstring validation_error;
            if (!ReadSettings(candidate, validation_error)) {
                (void)RestoreCurrentSettingsHistoryState();
                return true;
            }
        }

        (void)HandleSettingsHistoryShortcut(
            virtual_key == static_cast<UINT>(L'Y'));
        // Ctrl+Z / Ctrl+Y are application-reserved settings shortcuts outside
        // active numeric editing, even when no corresponding history step is
        // currently available. Consuming the message also prevents a plain Z
        // or Y safety hotkey from being interpreted through dialog handling.
        return true;
    }

    const HWND combo = key_capture_combo_;
    if (combo == nullptr || IsWindow(combo) == FALSE ||
        IsWindowEnabled(combo) == FALSE) {
        EndKeyCapture();
        return false;
    }

    // IsDialogMessage and native CBS_DROPDOWNLIST handling can consume or
    // reinterpret these messages before the combo subclass receives them.
    // Capture the physical virtual key at the application message-loop
    // boundary so S remains S, D remains D, Enter remains Enter, and so on.
    if (message.message == WM_CHAR || message.message == WM_SYSCHAR) {
        return true;
    }
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
        return false;
    }

    const UINT virtual_key = static_cast<UINT>(
        numpad::NormalizeCapturedVirtualKey(
            static_cast<UINT>(message.wParam), message.lParam));
    const bool shift_is_down =
        (GetKeyState(VK_SHIFT) & static_cast<SHORT>(0x8000)) != 0;
    const bool control_is_down =
        (GetKeyState(VK_CONTROL) & static_cast<SHORT>(0x8000)) != 0;
    if (control_is_down &&
        (virtual_key == static_cast<UINT>(L'Z') ||
         virtual_key == static_cast<UINT>(L'Y'))) {
        // Ctrl+Z / Ctrl+Y are disabled while a key selector is capturing.
        // Control is not a supported generated / safety modifier, so do not
        // accidentally capture the following Z or Y as an unmodified key.
        return true;
    }
    const std::uint16_t modifiers =
        shift_is_down ? core::KeyModifierShift : 0;
    if (CaptureKeyForCombo(combo, virtual_key, modifiers)) {
        return true;
    }

    // Escape and Tab are valid generated keys, but they remain cancel / focus
    // navigation keys for the more restricted safety-hotkey fields.
    if (combo != generated_key_combo_ && virtual_key == VK_ESCAPE) {
        EndKeyCapture();
        if (window_ != nullptr && IsWindow(window_) != FALSE) {
            SetFocus(window_);
        }
        return true;
    }
    if (combo != generated_key_combo_ && virtual_key == VK_TAB) {
        EndKeyCapture();
        return false;
    }

    if (virtual_key != VK_SHIFT && virtual_key != VK_CONTROL &&
        virtual_key != VK_MENU && virtual_key != VK_LWIN &&
        virtual_key != VK_RWIN) {
        MessageBeep(MB_OK);
    }
    return true;
}

bool MainWindow::IsKeyCaptureCombo(const HWND combo) const noexcept {
    return combo != nullptr && combo == key_capture_combo_ &&
           key_capture_active_.load(std::memory_order_acquire);
}

void MainWindow::BeginKeyCapture(const HWND combo) {
    if (combo == nullptr || IsWindow(combo) == FALSE ||
        (combo != generated_key_combo_ && combo != start_hotkey_combo_ &&
         combo != emergency_hotkey_combo_)) {
        return;
    }
    if (hotkey_registration_pending_ &&
        (combo == start_hotkey_combo_ || combo == emergency_hotkey_combo_)) {
        return;
    }

    CloseComboPopup(false);
    if (key_capture_combo_ != nullptr && key_capture_combo_ != combo) {
        EndKeyCapture();
    }
    key_capture_combo_ = combo;
    key_capture_active_.store(true, std::memory_order_release);
    tooltips_.Dismiss(combo);
    UpdateKeySelectorTooltip(combo);
    SetFocus(combo);
    InvalidateRect(combo, nullptr, FALSE);
    UpdateWindow(combo);
}

void MainWindow::EndKeyCapture() {
    const HWND previous = key_capture_combo_;
    key_capture_active_.store(false, std::memory_order_release);
    key_capture_combo_ = nullptr;
    if (previous != nullptr && IsWindow(previous) != FALSE) {
        UpdateKeySelectorTooltip(previous);
        InvalidateRect(previous, nullptr, FALSE);
    }
}

bool MainWindow::CaptureKeyForCombo(const HWND combo,
                                    const UINT virtual_key,
                                    const std::uint16_t modifiers) {
    if (!IsKeyCaptureCombo(combo) || virtual_key == 0 || virtual_key > 0xFFFFU) {
        return false;
    }

    const auto& choices = combo == generated_key_combo_
                              ? generated_key_choices_
                              : hotkey_choices_;
    const core::HotkeyBinding requested{
        static_cast<std::uint16_t>(virtual_key),
        static_cast<std::uint16_t>(modifiers & core::SupportedKeyModifiers),
    };
    auto choice = std::ranges::find_if(
        choices,
        [&requested](const auto& item) { return item.second == requested; });

    // Shift is meaningful only for choices that explicitly expose a shifted
    // entry. Holding Shift while capturing A, F5, Enter, or another ordinary
    // key should still select that ordinary key rather than reject capture.
    if (choice == choices.end() && requested.modifiers != 0) {
        const core::HotkeyBinding unmodified{requested.virtual_key, 0};
        choice = std::ranges::find_if(
            choices,
            [&unmodified](const auto& item) { return item.second == unmodified; });
    }
    if (choice == choices.end()) {
        return false;
    }

    const int index = static_cast<int>(std::distance(choices.begin(), choice));
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    EndKeyCapture();
    NotifyCombo(combo, CBN_SELCHANGE);
    if (window_ != nullptr && IsWindow(window_) != FALSE) {
        SetFocus(window_);
    }
    return true;
}

void MainWindow::UpdateNumericEditFormatting(const HWND edit) const noexcept {
    numeric_field::UpdateFormatting(edit);
}

MainWindow::DurationEditSet MainWindow::IntervalDurationEdits() const noexcept {
    return {interval_minutes_edit_, interval_seconds_edit_, interval_edit_};
}

MainWindow::DurationEditSet MainWindow::ButtonDownDurationEdits() const noexcept {
    return {button_down_minutes_edit_, button_down_seconds_edit_,
            button_down_edit_};
}

MainWindow::DurationEditSet MainWindow::ActionSpacingDurationEdits() const noexcept {
    return {action_spacing_minutes_edit_, action_spacing_seconds_edit_,
            action_spacing_edit_};
}

MainWindow::DurationEditSet MainWindow::MinimumIntervalDurationEdits() const noexcept {
    return {minimum_interval_minutes_edit_, minimum_interval_seconds_edit_,
            minimum_interval_edit_};
}

MainWindow::DurationEditSet MainWindow::MaximumIntervalDurationEdits() const noexcept {
    return {maximum_interval_minutes_edit_, maximum_interval_seconds_edit_,
            maximum_interval_edit_};
}

MainWindow::RunTimeLimitEditSet MainWindow::RunTimeLimitEdits() const noexcept {
    return {run_time_hours_edit_, run_time_minutes_edit_,
            run_time_seconds_edit_};
}

bool MainWindow::IsDurationMinutesEdit(const HWND edit) const noexcept {
    return edit == interval_minutes_edit_ ||
           edit == button_down_minutes_edit_ ||
           edit == action_spacing_minutes_edit_ ||
           edit == minimum_interval_minutes_edit_ ||
           edit == maximum_interval_minutes_edit_;
}

bool MainWindow::IsDurationSecondsEdit(const HWND edit) const noexcept {
    return edit == interval_seconds_edit_ ||
           edit == button_down_seconds_edit_ ||
           edit == action_spacing_seconds_edit_ ||
           edit == minimum_interval_seconds_edit_ ||
           edit == maximum_interval_seconds_edit_;
}

bool MainWindow::IsDurationMillisecondsEdit(const HWND edit) const noexcept {
    return edit == interval_edit_ || edit == button_down_edit_ ||
           edit == action_spacing_edit_ || edit == minimum_interval_edit_ ||
           edit == maximum_interval_edit_;
}

bool MainWindow::IsDurationEdit(const HWND edit) const noexcept {
    return IsDurationMinutesEdit(edit) || IsDurationSecondsEdit(edit) ||
           IsDurationMillisecondsEdit(edit);
}

bool MainWindow::IsRunTimeLimitEdit(const HWND edit) const noexcept {
    return edit == run_time_hours_edit_ ||
           edit == run_time_minutes_edit_ ||
           edit == run_time_seconds_edit_;
}

bool MainWindow::TryReadDurationEdits(
    const DurationEditSet& edits,
    core::DurationComponents& components,
    std::uint64_t& total_microseconds) const noexcept {
    std::uint64_t minutes = 0;
    std::uint64_t seconds = 0;
    std::uint64_t milliseconds_microseconds = 0;
    if (!TryParseUnsignedIntegerText(
            GetControlText(edits.minutes), minutes) ||
        !TryParseUnsignedIntegerText(
            GetControlText(edits.seconds), seconds) ||
        !TryParseMillisecondsText(
            GetControlText(edits.milliseconds),
            milliseconds_microseconds) ||
        minutes > core::MaximumTimeComponent ||
        seconds > core::MaximumTimeComponent ||
        milliseconds_microseconds >
            core::MaximumMillisecondsComponentMicroseconds) {
        return false;
    }

    components = {static_cast<std::uint32_t>(minutes),
                  static_cast<std::uint32_t>(seconds),
                  milliseconds_microseconds};
    return core::ComposeDurationMicroseconds(
        components, total_microseconds);
}

void MainWindow::SetDurationEdits(
    const DurationEditSet& edits,
    const core::DurationComponents& components) {
    SetControlText(edits.minutes, std::to_wstring(components.minutes));
    SetControlText(edits.seconds, std::to_wstring(components.seconds));
    const std::string milliseconds =
        core::FormatMilliseconds(components.milliseconds_microseconds);
    SetControlText(edits.milliseconds,
                   std::wstring(milliseconds.begin(), milliseconds.end()));
}

bool MainWindow::TryReadRunTimeLimitEdits(
    core::RunTimeLimitComponents& components,
    std::uint64_t& total_microseconds) const noexcept {
    const RunTimeLimitEditSet edits = RunTimeLimitEdits();
    std::uint64_t hours = 0;
    std::uint64_t minutes = 0;
    std::uint64_t seconds = 0;
    if (!TryParseUnsignedIntegerText(GetControlText(edits.hours), hours) ||
        !TryParseUnsignedIntegerText(GetControlText(edits.minutes), minutes) ||
        !TryParseUnsignedIntegerText(GetControlText(edits.seconds), seconds) ||
        hours > core::MaximumTimeComponent ||
        minutes > core::MaximumTimeComponent ||
        seconds > core::MaximumTimeComponent) {
        return false;
    }

    components = {static_cast<std::uint32_t>(hours),
                  static_cast<std::uint32_t>(minutes),
                  static_cast<std::uint32_t>(seconds)};
    return core::ComposeRunTimeLimitMicroseconds(
        components, total_microseconds);
}

void MainWindow::SetRunTimeLimitEdits(
    const core::RunTimeLimitComponents& components) {
    const RunTimeLimitEditSet edits = RunTimeLimitEdits();
    SetControlText(edits.hours, std::to_wstring(components.hours));
    SetControlText(edits.minutes, std::to_wstring(components.minutes));
    SetControlText(edits.seconds, std::to_wstring(components.seconds));
}

void MainWindow::StepNumericEdit(const HWND edit, const int direction) {
    if (edit == nullptr || IsWindow(edit) == FALSE ||
        IsWindowEnabled(edit) == FALSE || direction == 0) {
        return;
    }

    const std::wstring text = GetControlText(edit);
    std::wstring replacement;

    if (IsDurationMillisecondsEdit(edit)) {
        std::uint64_t microseconds = 0;
        if (!TryParseMillisecondsText(text, microseconds)) {
            return;
        }

        constexpr std::uint64_t step_microseconds =
            core::MicrosecondsPerMillisecond;
        if (direction > 0) {
            microseconds = std::min<std::uint64_t>(
                core::MaximumMillisecondsComponentMicroseconds,
                microseconds <=
                        core::MaximumMillisecondsComponentMicroseconds -
                            step_microseconds
                    ? microseconds + step_microseconds
                    : core::MaximumMillisecondsComponentMicroseconds);
        } else {
            microseconds = microseconds >= step_microseconds
                               ? microseconds - step_microseconds
                               : 0U;
        }
        const std::string formatted = core::FormatMilliseconds(microseconds);
        replacement.assign(formatted.begin(), formatted.end());
    } else if (IsDurationMinutesEdit(edit) ||
               IsDurationSecondsEdit(edit)) {
        std::uint64_t value = 0;
        if (!TryParseUnsignedIntegerText(text, value)) {
            return;
        }
        value = std::min(value, core::MaximumTimeComponent);
        if (direction > 0) {
            value = std::min<std::uint64_t>(
                core::MaximumTimeComponent, value + 1U);
        } else {
            value = value > 0U ? value - 1U : 0U;
        }
        replacement = std::to_wstring(value);
    } else if (IsRunTimeLimitEdit(edit)) {
        std::uint64_t value = 0;
        if (!TryParseUnsignedIntegerText(text, value)) {
            return;
        }
        value = std::min(value, core::MaximumTimeComponent);
        if (direction > 0) {
            value = std::min<std::uint64_t>(
                core::MaximumTimeComponent, value + 1U);
        } else {
            value = value > 0U ? value - 1U : 0U;
        }
        replacement = std::to_wstring(value);
    } else if (edit == fixed_x_edit_ || edit == fixed_y_edit_) {
        std::int64_t value = 0;
        if (!TryParseSignedIntegerText(text, value)) {
            return;
        }
        value = std::clamp<std::int64_t>(
            value,
            static_cast<std::int64_t>(
                std::numeric_limits<std::int32_t>::min()),
            static_cast<std::int64_t>(
                std::numeric_limits<std::int32_t>::max()));
        if (direction > 0) {
            if (value < std::numeric_limits<std::int32_t>::max()) {
                ++value;
            }
        } else if (value > std::numeric_limits<std::int32_t>::min()) {
            --value;
        }
        replacement = std::to_wstring(value);
    } else if (edit == burst_count_edit_) {
        std::uint64_t value = 0;
        if (!TryParseUnsignedIntegerText(text, value)) {
            return;
        }
        if (direction > 0) {
            value = std::min<std::uint64_t>(
                core::MaximumBurstCount,
                value < std::numeric_limits<std::uint64_t>::max()
                    ? value + 1U
                    : value);
        } else {
            value = value > core::MinimumBurstCount
                        ? value - 1U
                        : core::MinimumBurstCount;
        }
        value = std::clamp<std::uint64_t>(
            value, core::MinimumBurstCount, core::MaximumBurstCount);
        replacement = std::to_wstring(value);
    } else if (edit == repeat_count_edit_) {
        std::uint64_t value = 0;
        if (!TryParseUnsignedIntegerText(text, value)) {
            return;
        }
        if (direction > 0) {
            if (value < std::numeric_limits<std::uint64_t>::max()) {
                ++value;
            }
        } else if (value > 1U) {
            --value;
        } else {
            value = 1U;
        }
        value = std::clamp<std::uint64_t>(value, 1U, core::MaximumRepeatCount);
        replacement = std::to_wstring(value);
    } else {
        return;
    }

    if (replacement.empty()) {
        return;
    }

    const bool text_changed = GetControlText(edit) != replacement;
    const bool refresh_rate_after_step =
        IsDurationEdit(edit) || IsRunTimeLimitEdit(edit) ||
        edit == burst_count_edit_;
    if (text_changed) {
        const bool previous_update_state = numeric_text_update_in_progress_;
        numeric_text_update_in_progress_ = true;
        SetControlText(edit, replacement);
        numeric_text_update_in_progress_ = previous_update_state;
    }
    SendMessageW(edit,
                 EM_SETSEL,
                 static_cast<WPARAM>(replacement.size()),
                 static_cast<LPARAM>(replacement.size()));
    InvalidateRect(edit, nullptr, FALSE);

    if (!text_changed || suppress_control_events_ ||
        numeric_text_update_in_progress_) {
        return;
    }
    numeric_presentation_refresh_pending_ = false;
    numeric_rate_refresh_pending_ = false;
    RefreshPresentation(refresh_rate_after_step, true, false, true);
    MarkMoveCoverPresentationDirty();
    ScheduleSettingsSave();
}

void MainWindow::QueueNumericPresentationRefresh(const bool update_rate) {
    MarkMoveCoverPresentationDirty();
    numeric_presentation_refresh_pending_ = true;
    numeric_rate_refresh_pending_ = numeric_rate_refresh_pending_ || update_rate;
    if (numeric_presentation_refresh_posted_ || window_ == nullptr ||
        IsWindow(window_) == FALSE) {
        return;
    }

    numeric_presentation_refresh_posted_ =
        PostMessageW(window_, WM_APP_REFRESH_NUMERIC_PRESENTATION, 0, 0) != FALSE;
    if (!numeric_presentation_refresh_posted_) {
        // Posting can fail only during teardown or severe queue failure. Apply
        // the current final text synchronously so presentation cannot remain
        // stale merely because the coalescing message could not be queued.
        const bool refresh_rate = numeric_rate_refresh_pending_;
        numeric_presentation_refresh_pending_ = false;
        numeric_rate_refresh_pending_ = false;
        RefreshPresentation(refresh_rate, true, false, true);
    }
}

void MainWindow::FlushNumericPresentationRefresh() {
    if (!numeric_presentation_refresh_pending_) {
        return;
    }
    const bool update_rate = numeric_rate_refresh_pending_;
    numeric_presentation_refresh_pending_ = false;
    numeric_rate_refresh_pending_ = false;
    RefreshPresentation(update_rate, true, false, true);
}

HWND MainWindow::MakeControl(const wchar_t* class_name,
                             const wchar_t* text,
                             const DWORD style,
                             const DWORD extended_style,
                             const int id) {
    HWND parent = control_parent_;
    if (parent == nullptr) {
        parent = content_host_ != nullptr ? content_host_ : window_;
    }

    HWND control = CreateWindowExW(
        extended_style,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0, 0, 100, 24,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance_,
        nullptr);
    ApplyFont(control);
    return control;
}

HWND MainWindow::MakeChoice(const wchar_t* text,
                            const bool radio,
                            const DWORD style,
                            const int id) {
    HWND parent = control_parent_;
    if (parent == nullptr) {
        parent = content_host_ != nullptr ? content_host_ : window_;
    }

    HWND control = CreateWindowExW(
        0,
        ChoiceControlClassName,
        text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | style |
            (radio ? ChoiceRadioStyle : ChoiceCheckStyle),
        0,
        0,
        100,
        24,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance_,
        this);
    ApplyFont(control);
    return control;
}

HWND MainWindow::MakeLabel(const wchar_t* text) {
    HWND label = MakeControl(L"STATIC", text, SS_OWNERDRAW | SS_NOTIFY, 0, -1);
    labels_.push_back(label);
    return label;
}

HWND MainWindow::MakeGroup(const wchar_t* text) {
    return MakeControl(L"STATIC", text, SS_OWNERDRAW | WS_CLIPSIBLINGS, 0, -1);
}

bool MainWindow::IsComboPopupOpenFor(const HWND combo) const noexcept {
    return combo != nullptr && combo_popup_owner_ == combo &&
           combo_popup_ != nullptr && IsWindow(combo_popup_) != FALSE &&
           IsWindowVisible(combo_popup_) != FALSE;
}

void MainWindow::NotifyCombo(const HWND combo, const int notification) const noexcept {
    if (combo == nullptr || IsWindow(combo) == FALSE) {
        return;
    }
    const HWND parent = GetParent(combo);
    if (parent == nullptr || IsWindow(parent) == FALSE) {
        return;
    }
    SendMessageW(parent,
                 WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(combo), notification),
                 reinterpret_cast<LPARAM>(combo));
}

bool MainWindow::OpenComboPopup(const HWND combo) {
    if (combo == nullptr || IsWindow(combo) == FALSE ||
        IsWindowEnabled(combo) == FALSE || combo_popup_ == nullptr ||
        IsWindow(combo_popup_) == FALSE) {
        return false;
    }
    if (hotkey_registration_pending_ &&
        (combo == start_hotkey_combo_ || combo == emergency_hotkey_combo_)) {
        return false;
    }

    if (IsComboPopupOpenFor(combo)) {
        CloseComboPopup(false);
        return true;
    }
    if (combo_popup_owner_ != nullptr) {
        CloseComboPopup(false);
    }
    SetFocus(combo);
    tooltips_.Hide();

    const LRESULT count_result = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    if (count_result == CB_ERR || count_result <= 0) {
        return false;
    }

    const UINT popup_dpi = std::max<UINT>(96, GetDpiForWindow(combo));
    RECT combo_bounds{};
    if (GetWindowRect(combo, &combo_bounds) == FALSE) {
        return false;
    }

    const int field_width = combo_bounds.right - combo_bounds.left;
    if (field_width <= 0) {
        return false;
    }

    const LRESULT selection_height_result =
        SendMessageW(combo, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0);
    const int field_height = std::max(
        Scale(28, popup_dpi),
        selection_height_result == CB_ERR
            ? 0
            : static_cast<int>(selection_height_result) + Scale(4, popup_dpi));
    const LRESULT native_item_height_result =
        SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);
    const int item_height = std::max(
        Scale(27, popup_dpi),
        native_item_height_result == CB_ERR
            ? 0
            : static_cast<int>(native_item_height_result) + Scale(4, popup_dpi));
    const int item_count = static_cast<int>(count_result);
    const int maximum_rows = combo == generated_key_combo_ ? 10 : 8;
    const int visible_rows = std::max(1, std::min(item_count, maximum_rows));
    const int border_width = std::max(1, Scale(1, popup_dpi));
    const int height = item_height * visible_rows + border_width * 2;

    RECT anchor{
        combo_bounds.left,
        combo_bounds.top,
        combo_bounds.right,
        combo_bounds.top + field_height,
    };
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &monitor_info) == FALSE) {
        return false;
    }

    const int work_left = static_cast<int>(monitor_info.rcWork.left);
    const int work_top = static_cast<int>(monitor_info.rcWork.top);
    const int work_right = static_cast<int>(monitor_info.rcWork.right);
    const int work_bottom = static_cast<int>(monitor_info.rcWork.bottom);

    // Keep each option on one line, but let the application-owned popup grow
    // wider than the closed field when necessary. This preserves a compact
    // page layout while showing entries such as "Numpad Page Down" in full.
    int widest_text = 0;
    const HDC combo_dc = GetDC(combo);
    if (combo_dc != nullptr) {
        const HFONT combo_font = reinterpret_cast<HFONT>(
            SendMessageW(combo, WM_GETFONT, 0, 0));
        const HGDIOBJ old_font = combo_font != nullptr
                                     ? SelectObject(combo_dc, combo_font)
                                     : nullptr;
        for (int index = 0; index < item_count; ++index) {
            const LRESULT length_result = SendMessageW(
                combo,
                CB_GETLBTEXTLEN,
                static_cast<WPARAM>(index),
                0);
            if (length_result < 0) {
                continue;
            }
            std::wstring text(static_cast<std::size_t>(length_result) + 1U,
                              L'\0');
            if (length_result > 0) {
                SendMessageW(combo,
                             CB_GETLBTEXT,
                             static_cast<WPARAM>(index),
                             reinterpret_cast<LPARAM>(text.data()));
            }
            SIZE extent{};
            if (GetTextExtentPoint32W(combo_dc,
                                      text.data(),
                                      static_cast<int>(length_result),
                                      &extent) != FALSE) {
                widest_text = std::max(widest_text,
                                       static_cast<int>(extent.cx));
            }
        }
        if (old_font != nullptr && old_font != HGDI_ERROR) {
            SelectObject(combo_dc, old_font);
        }
        ReleaseDC(combo, combo_dc);
    }

    const bool needs_scrollbar = item_count > visible_rows;
    const int text_chrome = Scale(needs_scrollbar ? 39 : 25, popup_dpi);
    const int desired_popup_width = std::max(
        field_width,
        widest_text > 0 ? widest_text + text_chrome : field_width);
    const int work_width = std::max(1, work_right - work_left);
    const int maximum_popup_width = std::max(
        field_width,
        std::min(Scale(420, popup_dpi), work_width));
    const int popup_width = std::clamp(desired_popup_width,
                                       field_width,
                                       maximum_popup_width);

    int popup_x = anchor.left;
    if (popup_x + popup_width > work_right) {
        popup_x = anchor.right - popup_width;
    }
    int popup_y = anchor.bottom - border_width;
    if (popup_y + height > monitor_info.rcWork.bottom &&
        anchor.top - height >= monitor_info.rcWork.top) {
        popup_y = anchor.top - height + border_width;
    }
    popup_x = std::clamp(popup_x,
                         work_left,
                         std::max(work_left, work_right - popup_width));
    popup_y = std::clamp(popup_y,
                         work_top,
                         std::max(work_top, work_bottom - height));

    const LRESULT selected_result = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    const int selected = selected_result == CB_ERR
                             ? 0
                             : std::clamp(static_cast<int>(selected_result),
                                          0,
                                          item_count - 1);

    combo_popup_owner_ = combo;
    combo_popup_highlight_ = selected;
    combo_popup_hover_ = -1;
    combo_popup_item_count_ = item_count;
    combo_popup_visible_rows_ = visible_rows;
    combo_popup_item_height_ = item_height;
    combo_popup_x_ = popup_x;
    combo_popup_y_ = popup_y;
    combo_popup_width_ = popup_width;
    combo_popup_height_ = height;
    combo_popup_scroll_hovered_ = false;
    combo_popup_scroll_dragging_ = false;
    combo_popup_scroll_drag_offset_ = 0;

    const int highest_top = std::max(0, item_count - visible_rows);
    combo_popup_top_index_ = std::clamp(
        selected - visible_rows / 2,
        0,
        highest_top);

    if (!RenderComboPopup()) {
        combo_popup_owner_ = nullptr;
        combo_popup_highlight_ = -1;
        combo_popup_hover_ = -1;
        combo_popup_item_count_ = 0;
        combo_popup_visible_rows_ = 0;
        combo_popup_item_height_ = 0;
        combo_popup_scroll_hovered_ = false;
        combo_popup_scroll_dragging_ = false;
        return false;
    }
    // The application-owned dropdown is an independent popup rather than a
    // child of the closed combo field. Keep it in the same Z-order band as
    // the main window so a topmost Vector Click window cannot cover its own
    // expanded options. HWND_NOTOPMOST is intentional here: the popup can
    // survive hidden across later opens, so explicitly demote it again after
    // Keep Vector Click on top is turned off.
    const bool main_window_topmost =
        (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    SetWindowPos(combo_popup_,
                 main_window_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 popup_x,
                 popup_y,
                 popup_width,
                 height,
                 SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOOWNERZORDER);
    const CaptureExclusionOutcome popup_affinity =
        SynchronizeRequestedCaptureExclusion(combo_popup_);
    if (IsCaptureExclusionRequested() &&
        popup_affinity.result != CaptureExclusionResult::Applied) {
        combo_popup_owner_ = nullptr;
        combo_popup_highlight_ = -1;
        combo_popup_hover_ = -1;
        combo_popup_item_count_ = 0;
        combo_popup_visible_rows_ = 0;
        combo_popup_item_height_ = 0;
        combo_popup_scroll_hovered_ = false;
        combo_popup_scroll_dragging_ = false;
        return false;
    }
    ShowWindow(combo_popup_, SW_SHOWNOACTIVATE);
    SetCapture(combo_popup_);
    InvalidateRect(combo, nullptr, FALSE);
    UpdateWindow(combo);
    return true;
}

void MainWindow::CommitComboPopupSelection() {
    const HWND combo = combo_popup_owner_;
    if (combo == nullptr || IsWindow(combo) == FALSE ||
        combo_popup_highlight_ < 0 ||
        combo_popup_highlight_ >= combo_popup_item_count_) {
        return;
    }

    const LRESULT current_result = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    const int current = current_result == CB_ERR
                            ? -1
                            : static_cast<int>(current_result);
    if (current == combo_popup_highlight_) {
        return;
    }

    SendMessageW(combo,
                 CB_SETCURSEL,
                 static_cast<WPARAM>(combo_popup_highlight_),
                 0);
    const bool safety_hotkey_combo =
        combo == start_hotkey_combo_ || combo == emergency_hotkey_combo_;
    if (!safety_hotkey_combo) {
        InvalidateRect(combo, nullptr, FALSE);
        UpdateWindow(combo);
    }
    NotifyCombo(combo, CBN_SELCHANGE);
}

void MainWindow::ApplyInputTypeSelectionCovered(const int selection) {
    if (action_type_combo_ == nullptr || IsWindow(action_type_combo_) == FALSE) {
        return;
    }

    const LRESULT count_result = SendMessageW(action_type_combo_, CB_GETCOUNT, 0, 0);
    if (count_result == CB_ERR || count_result <= 0) {
        return;
    }
    const int bounded_selection = std::clamp(
        selection, 0, static_cast<int>(count_result) - 1);
    const LRESULT current_result = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0);
    if (current_result != CB_ERR &&
        static_cast<int>(current_result) == bounded_selection) {
        return;
    }

    // The application-owned list has already been hidden. Capture and present
    // the last stable Mouse or Keyboard frame before changing the selected
    // text or any mode-dependent controls. The real controls are then updated
    // completely behind that frame and exposed only after the final repaint.
    input_type_update_pending_ = false;
    input_type_update_posted_ = false;
    HideBasicTopSnapshot();

    const bool visible = basic_top_host_ != nullptr &&
                         IsWindow(basic_top_host_) != FALSE &&
                         IsWindowVisible(basic_top_host_) != FALSE;
    if (visible && CaptureBasicTopSnapshot()) {
        (void)ShowBasicTopSnapshot();
    }

    if (basic_top_host_ != nullptr && IsWindow(basic_top_host_) != FALSE) {
        SendMessageW(basic_top_host_, WM_SETREDRAW, FALSE, 0);
    }

    SendMessageW(action_type_combo_,
                 CB_SETCURSEL,
                 static_cast<WPARAM>(bounded_selection),
                 0);
    UpdateActionControls(false);
    if (bounded_selection == 1) {
        ResetClickPositionIndicator();
    }
    ValidateGeneratedKeySelection(true);
    RefreshEnabledState();

    if (basic_top_host_ != nullptr && IsWindow(basic_top_host_) != FALSE) {
        SendMessageW(basic_top_host_, WM_SETREDRAW, TRUE, 0);
    }
    if (visible) {
        RedrawBasicTopHost();
        (void)DwmFlush();
    }

    HideBasicTopSnapshot();
    QueueMoveCoverRefresh();
    ScheduleSettingsSave();
    CommitSettingsHistoryFromControls();
}

void MainWindow::CloseComboPopup(const bool commit_selection) {
    const HWND combo = combo_popup_owner_;
    if (combo == nullptr) {
        return;
    }

    const bool action_type_commit =
        commit_selection && combo == action_type_combo_ &&
        combo_popup_highlight_ >= 0 &&
        combo_popup_highlight_ < combo_popup_item_count_ &&
        SendMessageW(combo, CB_GETCURSEL, 0, 0) != combo_popup_highlight_;
    const int action_type_selection = action_type_commit
                                          ? combo_popup_highlight_
                                          : -1;

    // For Input type, close the application-owned popup before touching the
    // selected combo text. This prevents the old / new text alternation seen
    // when the snapshot was shown only after the new text had already painted.
    if (!action_type_commit && commit_selection) {
        CommitComboPopupSelection();
    }

    combo_popup_owner_ = nullptr;
    combo_popup_highlight_ = -1;
    combo_popup_hover_ = -1;
    combo_popup_top_index_ = 0;
    combo_popup_item_count_ = 0;
    combo_popup_visible_rows_ = 0;
    combo_popup_item_height_ = 0;
    combo_popup_scroll_hovered_ = false;
    combo_popup_scroll_dragging_ = false;
    combo_popup_scroll_drag_offset_ = 0;

    if (GetCapture() == combo_popup_) {
        ReleaseCapture();
    }
    if (combo_popup_ != nullptr && IsWindow(combo_popup_) != FALSE) {
        ShowWindow(combo_popup_, SW_HIDE);
    }

    if (action_type_commit) {
        // The protected frame is application-rendered and never contains the
        // independent popup, so no compositor round trip is needed before the
        // complete mode transition begins.
        ApplyInputTypeSelectionCovered(action_type_selection);
    }

    if (IsWindow(combo) != FALSE) {
        InvalidateRect(combo, nullptr, FALSE);
        UpdateWindow(combo);
        NotifyCombo(combo, CBN_CLOSEUP);
    }
}

void MainWindow::MoveComboPopupHighlight(int index) {
    if (combo_popup_owner_ == nullptr || combo_popup_item_count_ <= 0) {
        return;
    }
    index = std::clamp(index, 0, combo_popup_item_count_ - 1);
    if (index == combo_popup_highlight_ &&
        index >= combo_popup_top_index_ &&
        index < combo_popup_top_index_ + combo_popup_visible_rows_) {
        return;
    }

    combo_popup_highlight_ = index;
    combo_popup_hover_ = -1;
    if (index < combo_popup_top_index_) {
        combo_popup_top_index_ = index;
    } else if (index >= combo_popup_top_index_ + combo_popup_visible_rows_) {
        combo_popup_top_index_ = index - combo_popup_visible_rows_ + 1;
    }
    const int highest_top = std::max(
        0,
        combo_popup_item_count_ - combo_popup_visible_rows_);
    combo_popup_top_index_ = std::clamp(combo_popup_top_index_, 0, highest_top);
    (void)RenderComboPopup();
}

void MainWindow::ScrollComboPopup(const int row_delta) {
    if (combo_popup_owner_ == nullptr || combo_popup_item_count_ <= 0 ||
        row_delta == 0) {
        return;
    }
    const int highest_top = std::max(
        0,
        combo_popup_item_count_ - combo_popup_visible_rows_);
    const int next_top = std::clamp(
        combo_popup_top_index_ + row_delta,
        0,
        highest_top);
    if (next_top == combo_popup_top_index_) {
        return;
    }
    combo_popup_top_index_ = next_top;
    combo_popup_hover_ = -1;
    (void)RenderComboPopup();
}

int MainWindow::ComboPopupItemAtPoint(const POINT point) const noexcept {
    if (combo_popup_owner_ == nullptr || combo_popup_item_height_ <= 0 ||
        combo_popup_visible_rows_ <= 0) {
        return -1;
    }
    const UINT popup_dpi = std::max<UINT>(96, GetDpiForWindow(combo_popup_owner_));
    const int border = std::max(1, Scale(1, popup_dpi));
    const int scrollbar_width = combo_popup_item_count_ > combo_popup_visible_rows_
                                    ? Scale(13, popup_dpi)
                                    : 0;
    if (point.x < border || point.x >= combo_popup_width_ - border - scrollbar_width ||
        point.y < border || point.y >= combo_popup_height_ - border) {
        return -1;
    }
    const int row = (point.y - border) / combo_popup_item_height_;
    if (row < 0 || row >= combo_popup_visible_rows_) {
        return -1;
    }
    const int index = combo_popup_top_index_ + row;
    return index >= 0 && index < combo_popup_item_count_ ? index : -1;
}

RECT MainWindow::ComboPopupScrollbarGutter() const noexcept {
    RECT empty{};
    if (combo_popup_owner_ == nullptr ||
        combo_popup_item_count_ <= combo_popup_visible_rows_) {
        return empty;
    }
    const UINT popup_dpi = std::max<UINT>(96, GetDpiForWindow(combo_popup_owner_));
    const int border = std::max(1, Scale(1, popup_dpi));
    const int scrollbar_width = Scale(13, popup_dpi);
    return RECT{
        combo_popup_width_ - border - scrollbar_width,
        border,
        combo_popup_width_ - border,
        combo_popup_height_ - border,
    };
}

RECT MainWindow::ComboPopupScrollbarTrack() const noexcept {
    RECT track = ComboPopupScrollbarGutter();
    if (IsRectEmpty(&track) != FALSE) {
        return RECT{};
    }
    const UINT popup_dpi = std::max<UINT>(96, GetDpiForWindow(combo_popup_owner_));
    const int desired_width = std::max(1, Scale(4, popup_dpi));
    const int available_width = std::max(
        1, static_cast<int>(track.right - track.left));
    const int track_width = std::min(desired_width, available_width);
    const int track_left = track.left + (available_width - track_width) / 2;
    const int vertical_inset = std::max(1, Scale(1, popup_dpi));
    track.left = track_left;
    track.right = track_left + track_width;
    track.top += vertical_inset;
    track.bottom = std::max(track.top + 1, track.bottom - vertical_inset);
    return track;
}

RECT MainWindow::ComboPopupScrollbarThumb() const noexcept {
    const RECT track = ComboPopupScrollbarTrack();
    if (IsRectEmpty(&track) != FALSE) {
        return RECT{};
    }
    const UINT popup_dpi = std::max<UINT>(96, GetDpiForWindow(combo_popup_owner_));
    const int track_height = track.bottom - track.top;
    const int minimum_thumb = std::min(Scale(18, popup_dpi), track_height);
    const int thumb_height = std::clamp(
        MulDiv(track_height,
               combo_popup_visible_rows_,
               std::max(1, combo_popup_item_count_)),
        minimum_thumb,
        track_height);
    const int highest_top = std::max(
        0,
        combo_popup_item_count_ - combo_popup_visible_rows_);
    const int travel = std::max(0, track_height - thumb_height);
    const int offset = highest_top > 0
                           ? MulDiv(travel, combo_popup_top_index_, highest_top)
                           : 0;
    const ui::ScrollbarVisualState scrollbar_state = ui::ScrollbarState(
        combo_popup_scroll_hovered_, combo_popup_scroll_dragging_);
    const int desired_width = std::max(
        1,
        Scale(ui::ScrollbarThumbLogicalWidth(scrollbar_state), popup_dpi));
    const RECT gutter = ComboPopupScrollbarGutter();
    const int available_width = std::max(
        1, static_cast<int>(gutter.right - gutter.left));
    const int thumb_width = std::min(desired_width, available_width);
    const int thumb_left = gutter.left + (available_width - thumb_width) / 2;
    return RECT{
        thumb_left,
        track.top + offset,
        thumb_left + thumb_width,
        track.top + offset + thumb_height,
    };
}

bool MainWindow::RenderComboPopup() {
    if (combo_popup_owner_ == nullptr || combo_popup_ == nullptr ||
        IsWindow(combo_popup_owner_) == FALSE || IsWindow(combo_popup_) == FALSE ||
        combo_popup_width_ <= 0 || combo_popup_height_ <= 0) {
        return false;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = combo_popup_width_;
    bitmap_info.bmiHeader.biHeight = -combo_popup_height_;
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
    const HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap.get());
    if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) {
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    RECT client{0, 0, combo_popup_width_, combo_popup_height_};
    ui::Fill(memory_dc, client, ui::SurfaceAlt);

    const UINT popup_dpi = std::max<UINT>(96, GetDpiForWindow(combo_popup_owner_));
    const int border = std::max(1, Scale(1, popup_dpi));
    const bool has_scrollbar = combo_popup_item_count_ > combo_popup_visible_rows_;
    const int scrollbar_width = has_scrollbar ? Scale(13, popup_dpi) : 0;
    const int item_right = combo_popup_width_ - border - scrollbar_width;
    const HFONT popup_font = reinterpret_cast<HFONT>(
        font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));

    for (int row = 0; row < combo_popup_visible_rows_; ++row) {
        const int index = combo_popup_top_index_ + row;
        if (index >= combo_popup_item_count_) {
            break;
        }
        RECT item{
            border,
            border + row * combo_popup_item_height_,
            item_right,
            std::min(combo_popup_height_ - border,
                     border + (row + 1) * combo_popup_item_height_),
        };

        if (index == combo_popup_highlight_) {
            ui::Fill(memory_dc, item, ui::SurfacePressed);
        } else if (index == combo_popup_hover_) {
            ui::Fill(memory_dc, item, ui::SurfaceHover);
        }

        const LRESULT length_result = SendMessageW(combo_popup_owner_,
                                                   CB_GETLBTEXTLEN,
                                                   static_cast<WPARAM>(index),
                                                   0);
        std::wstring text;
        if (length_result >= 0) {
            text.resize(static_cast<std::size_t>(length_result) + 1U);
            if (length_result > 0) {
                SendMessageW(combo_popup_owner_,
                             CB_GETLBTEXT,
                             static_cast<WPARAM>(index),
                             reinterpret_cast<LPARAM>(text.data()));
            }
            text.resize(static_cast<std::size_t>(length_result));
        }

        RECT text_bounds = item;
        text_bounds.left += Scale(11, popup_dpi);
        text_bounds.right -= Scale(8, popup_dpi);
        ui::DrawTextLine(memory_dc,
                         text,
                         text_bounds,
                         popup_font,
                         IsWindowEnabled(combo_popup_owner_) != FALSE
                             ? ui::Text
                             : ui::Disabled,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (has_scrollbar) {
        const RECT gutter = ComboPopupScrollbarGutter();
        ui::Fill(memory_dc, gutter, ui::Surface);
        const RECT track = ComboPopupScrollbarTrack();
        ui::DrawRoundedPanel(
            memory_dc,
            track,
            ui::ScrollbarTrack,
            ui::ScrollbarTrack,
            std::max(1, static_cast<int>((track.right - track.left) / 2)));
        const RECT thumb = ComboPopupScrollbarThumb();
        const COLORREF thumb_color = ui::ScrollbarThumbColor(
            ui::ScrollbarState(combo_popup_scroll_hovered_,
                               combo_popup_scroll_dragging_));
        ui::DrawRoundedPanel(
            memory_dc,
            thumb,
            thumb_color,
            thumb_color,
            std::max(1, static_cast<int>((thumb.right - thumb.left) / 2)));
    }

    const HPEN border_pen = CreatePen(PS_SOLID, border, ui::Border);
    if (border_pen != nullptr) {
        const HGDIOBJ old_pen = SelectObject(memory_dc, border_pen);
        const HGDIOBJ old_brush = SelectObject(memory_dc, GetStockObject(NULL_BRUSH));
        Rectangle(memory_dc,
                  0,
                  0,
                  combo_popup_width_,
                  combo_popup_height_);
        SelectObject(memory_dc, old_brush);
        SelectObject(memory_dc, old_pen);
        DeleteObject(border_pen);
    }

    auto* pixels = static_cast<std::uint32_t*>(pixel_memory);
    const std::size_t pixel_count =
        static_cast<std::size_t>(combo_popup_width_) *
        static_cast<std::size_t>(combo_popup_height_);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        pixels[index] |= 0xFF000000U;
    }

    POINT destination{combo_popup_x_, combo_popup_y_};
    SIZE size{combo_popup_width_, combo_popup_height_};
    POINT source{};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL updated = UpdateLayeredWindow(combo_popup_,
                                             screen_dc,
                                             &destination,
                                             &size,
                                             memory_dc,
                                             &source,
                                             0,
                                             &blend,
                                             ULW_ALPHA);

    SelectObject(memory_dc, old_bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return updated != FALSE;
}

void MainWindow::CreateControls() {
    control_parent_ = content_host_;
    basic_tab_button_ = MakeControl(
        L"BUTTON", L"Basic", BS_OWNERDRAW | WS_TABSTOP | WS_GROUP, 0, BasicPageTab);
    advanced_tab_button_ = MakeControl(
        L"BUTTON", L"Advanced", BS_OWNERDRAW | WS_TABSTOP, 0, AdvancedPageTab);
    about_tab_button_ = MakeControl(
        L"BUTTON", L"About", BS_OWNERDRAW | WS_TABSTOP, 0, AboutPageTab);

    auto create_page_host = [this](const bool visible) -> HWND {
        return CreateWindowExW(
            WS_EX_CONTROLPARENT,
            ContentHostClassName,
            L"",
            WS_CHILD | (visible ? WS_VISIBLE : 0) | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0,
            0,
            100,
            100,
            content_host_,
            nullptr,
            instance_,
            this);
    };

    basic_page_host_ = create_page_host(selected_page_index_ == 0);
    advanced_page_host_ = create_page_host(selected_page_index_ == 1);
    about_page_host_ = create_page_host(selected_page_index_ == 2);
    advanced_scroll_content_host_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        ContentHostClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0,
        0,
        Scale(layout::FixedContentWidth, dpi_),
        Scale(layout::AdvancedPageHeight, dpi_),
        advanced_page_host_,
        nullptr,
        instance_,
        this);
    advanced_scrollbar_ = CreateWindowExW(
        0,
        AdvancedScrollbarClassName,
        L"",
        WS_CHILD | WS_CLIPSIBLINGS,
        0,
        0,
        Scale(layout::AdvancedScrollbarWidth, dpi_),
        Scale(layout::BasicPageHeight, dpi_),
        content_host_,
        nullptr,
        instance_,
        this);

    // The two upper Basic cards and every control inside them share one
    // opaque child surface. Input type changes can therefore update and
    // present this complete region as one unit without repainting Repeat,
    // Hotkeys, status, or the action buttons. Card backgrounds and their
    // controls share the same parent and z-order domain.
    basic_top_host_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        ContentHostClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0,
        0,
        100,
        100,
        basic_page_host_,
        nullptr,
        instance_,
        this);

    control_parent_ = basic_top_host_;
    input_group_ = MakeGroup(L"Input");
    position_group_ = MakeGroup(L"Position");

    action_type_label_ = MakeLabel(L"Input type");
    action_type_combo_ = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, 0, ActionTypeCombo);
    SendMessageW(action_type_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Mouse click"));
    SendMessageW(action_type_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Keyboard press"));

    mouse_button_label_ = MakeLabel(L"Mouse button");
    mouse_button_combo_ = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, 0, MouseButtonCombo);
    for (const wchar_t* button : {L"Left", L"Right", L"Middle", L"X1", L"X2"}) {
        SendMessageW(mouse_button_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(button));
    }

    generated_key_label_ = MakeLabel(L"Keyboard key");
    generated_key_combo_ = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, 0, GeneratedKeyCombo);
    for (const auto& [name, key] : generated_key_choices_) {
        (void)key;
        SendMessageW(generated_key_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }

    action_pattern_label_ = MakeLabel(L"Action pattern");
    action_pattern_combo_ = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, 0, ActionPatternCombo);
    for (const wchar_t* pattern : {L"Single", L"Double", L"Triple", L"Burst", L"Hold"}) {
        SendMessageW(action_pattern_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pattern));
    }

    constexpr DWORD numeric_edit_style =
        ES_MULTILINE | ES_AUTOHSCROLL | ES_CENTER | WS_TABSTOP;

    interval_label_ = MakeLabel(L"Input interval");
    basic_interval_minutes_header_ = MakeLabel(L"Minutes");
    basic_interval_seconds_header_ = MakeLabel(L"Seconds");
    basic_interval_milliseconds_header_ = MakeLabel(L"Milliseconds");
    interval_minutes_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, IntervalMinutesEdit);
    interval_seconds_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, IntervalSecondsEdit);
    interval_edit_ = MakeControl(L"EDIT", L"10", numeric_edit_style, 0, IntervalEdit);
    rate_text_ = MakeControl(L"STATIC", L"Click rate: 100.0 CPS", SS_OWNERDRAW | SS_NOTIFY, 0, RateText);

    current_cursor_radio_ = MakeChoice(L"Current cursor", true, WS_GROUP, CurrentCursorRadio);
    fixed_position_radio_ = MakeChoice(L"Fixed screen position", true, 0, FixedPositionRadio);
    fixed_x_label_ = MakeLabel(L"X");
    fixed_x_edit_ = MakeControl(L"EDIT", L"0", numeric_edit_style, 0, FixedXEdit);
    fixed_y_label_ = MakeLabel(L"Y");
    fixed_y_edit_ = MakeControl(L"EDIT", L"0", numeric_edit_style, 0, FixedYEdit);
    capture_position_button_ = MakeControl(L"BUTTON", L"Capture in 4 seconds", BS_OWNERDRAW | WS_TABSTOP, 0, CapturePositionButton);
    position_unavailable_text_ = MakeLabel(L"Position controls are not used for keyboard input.");

    // The lower Basic cards remain direct children of the Basic page. They do
    // not participate in Input type transitions and are intentionally outside
    // the upper-surface repaint boundary.
    control_parent_ = basic_page_host_;
    repeat_group_ = MakeGroup(L"Repeat / time limit");
    hotkeys_group_ = MakeGroup(L"Hotkeys");

    unlimited_radio_ = MakeChoice(L"Unlimited", true, WS_GROUP, UnlimitedRadio);
    limited_radio_ = MakeChoice(L"Limited to", true, 0, LimitedRadio);
    repeat_count_edit_ = MakeControl(L"EDIT", L"100", numeric_edit_style, 0, RepeatCountEdit);
    repeat_unit_label_ = MakeLabel(L"clicks");
    run_time_hours_header_ = MakeLabel(L"Hours");
    run_time_minutes_header_ = MakeLabel(L"Minutes");
    run_time_seconds_header_ = MakeLabel(L"Seconds");
    run_time_hours_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, RunTimeHoursEdit);
    run_time_minutes_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, RunTimeMinutesEdit);
    run_time_seconds_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, RunTimeSecondsEdit);

    start_hotkey_label_ = MakeLabel(L"Start / Stop");
    start_hotkey_combo_ = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, 0, StartHotkeyCombo);
    emergency_hotkey_label_ = MakeLabel(L"Emergency Stop");
    emergency_hotkey_combo_ = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, 0, EmergencyHotkeyCombo);
    for (const auto& [name, key] : hotkey_choices_) {
        (void)key;
        SendMessageW(start_hotkey_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        SendMessageW(emergency_hotkey_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }

    control_parent_ = advanced_scroll_content_host_;
    advanced_timing_group_ = MakeGroup(L"Timing and input method");
    target_group_ = MakeGroup(L"Target window and administrator compatibility");
    performance_group_ = MakeGroup(L"Scheduling & Performance");
    notifications_group_ = MakeGroup(L"Notifications & Indicators");
    options_group_ = MakeGroup(L"Safety, display, and settings");

    advanced_minutes_header_ = MakeLabel(L"Minutes");
    advanced_seconds_header_ = MakeLabel(L"Seconds");
    advanced_milliseconds_header_ = MakeLabel(L"Milliseconds");
    button_down_label_ = MakeLabel(L"Down duration");
    button_down_minutes_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, ButtonDownMinutesEdit);
    button_down_seconds_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, ButtonDownSecondsEdit);
    button_down_edit_ = MakeControl(L"EDIT", L"1", numeric_edit_style, 0, ButtonDownEdit);
    down_duration_behavior_label_ = MakeLabel(L"Down duration behavior");
    down_duration_behavior_combo_ = MakeControl(
        WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        0, DownDurationBehaviorCombo);
    SendMessageW(down_duration_behavior_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Fixed (configured value)"));
    SendMessageW(down_duration_behavior_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Natural (configured center)"));
    SendMessageW(down_duration_behavior_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Natural (automatic center)"));
    action_spacing_label_ = MakeLabel(L"Action spacing");
    action_spacing_minutes_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, ActionSpacingMinutesEdit);
    action_spacing_seconds_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, ActionSpacingSecondsEdit);
    action_spacing_edit_ = MakeControl(L"EDIT", L"10", numeric_edit_style, 0, ActionSpacingEdit);
    burst_count_label_ = MakeLabel(L"Burst count");
    burst_count_edit_ = MakeControl(L"EDIT", L"4", numeric_edit_style, 0, BurstCountEdit);

    backend_label_ = MakeLabel(L"Input method");
    backend_combo_ = MakeControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, 0, BackendCombo);
    SendMessageW(backend_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic (recommended)"));
    SendMessageW(backend_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Standard input"));
    SendMessageW(backend_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Foreground target input"));
    SendMessageW(backend_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Targeted window messages"));
    SendMessageW(backend_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Unicode text input"));
    SendMessageW(backend_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Targeted Unicode text"));

    random_interval_check_ = MakeChoice(
        L"Use random input interval", false, 0, RandomIntervalCheck);
    random_interval_style_label_ = MakeLabel(L"Variation style");
    random_interval_style_combo_ = MakeControl(
        WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        0, RandomIntervalStyleCombo);
    SendMessageW(random_interval_style_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Independent (balanced)"));
    SendMessageW(random_interval_style_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Drifting (more varied)"));
    SendMessageW(random_interval_style_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Natural variation"));
    minimum_interval_label_ = MakeLabel(L"Minimum interval");
    minimum_interval_minutes_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, MinimumIntervalMinutesEdit);
    minimum_interval_seconds_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, MinimumIntervalSecondsEdit);
    minimum_interval_edit_ = MakeControl(
        L"EDIT", L"10", numeric_edit_style, 0, MinimumIntervalEdit);
    maximum_interval_label_ = MakeLabel(L"Maximum interval");
    maximum_interval_minutes_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, MaximumIntervalMinutesEdit);
    maximum_interval_seconds_edit_ = MakeControl(
        L"EDIT", L"0", numeric_edit_style, 0, MaximumIntervalSecondsEdit);
    maximum_interval_edit_ = MakeControl(
        L"EDIT", L"100", numeric_edit_style, 0, MaximumIntervalEdit);

    target_label_ = MakeLabel(L"Target window");
    target_status_text_ = MakeControl(L"STATIC", L"No target selected (optional)", SS_OWNERDRAW | SS_NOTIFY, 0, -1);
    select_target_button_ = MakeControl(L"BUTTON", L"Choose window...", BS_OWNERDRAW | WS_TABSTOP, 0, SelectTargetButton);
    clear_target_button_ = MakeControl(L"BUTTON", L"Clear", BS_OWNERDRAW | WS_TABSTOP, 0, ClearTargetButton);
    background_input_check_ = MakeChoice(L"Allow background input", false, 0, BackgroundInputCheck);
    admin_button_ = MakeControl(L"BUTTON", L"Restart as administrator", BS_OWNERDRAW | WS_TABSTOP, 0, AdminButton);

    // Create Advanced controls in the same top-to-bottom, left-to-right order
    // in which they are presented. Native dialog traversal follows child
    // window order, so keeping creation order aligned with the layout keeps
    // Tab / Shift+Tab navigation spatially predictable.
    performance_guidance_text_ = MakeLabel(
        L"System default / System managed is recommended for most systems. Other settings may improve performance on some systems, but may make little difference or perform worse on others.");
    process_priority_label_ = MakeLabel(L"Process priority");
    process_priority_combo_ = MakeControl(
        WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        0, ProcessPriorityCombo);
    for (const wchar_t* priority : {L"System default",
                                    L"Above Normal while active",
                                    L"Above Normal",
                                    L"High while active",
                                    L"High"}) {
        SendMessageW(process_priority_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(priority));
    }
    timing_worker_qos_label_ = MakeLabel(L"Timing worker QoS");
    timing_worker_qos_combo_ = MakeControl(
        WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        0, TimingWorkerQosCombo);
    for (const wchar_t* qos : {L"System managed",
                               L"High performance (HighQoS)",
                               L"Efficiency (EcoQoS)"}) {
        SendMessageW(timing_worker_qos_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(qos));
    }
    timing_worker_priority_label_ = MakeLabel(L"Timing worker priority");
    timing_worker_priority_combo_ = MakeControl(
        WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        0, TimingWorkerPriorityCombo);
    for (const wchar_t* priority : {L"System default",
                                    L"Above Normal (+1)",
                                    L"Highest (+2)"}) {
        SendMessageW(timing_worker_priority_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(priority));
    }
    hotkey_control_priority_label_ = MakeLabel(L"Hotkey / control priority");
    hotkey_control_priority_combo_ = MakeControl(
        WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        0, HotkeyControlPriorityCombo);
    for (const wchar_t* priority : {L"System default", L"Above Normal (+1)"}) {
        SendMessageW(hotkey_control_priority_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(priority));
    }

    windows_notification_label_ = MakeLabel(L"Windows notification");
    windows_notification_combo_ = MakeControl(
        WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        0, WindowsNotificationCombo);
    system_sound_label_ = MakeLabel(L"System sound");
    system_sound_combo_ = MakeControl(
        WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        0, SystemSoundCombo);
    for (const wchar_t* mode : {L"Off", L"Started", L"Stopped", L"Started and stopped"}) {
        SendMessageW(windows_notification_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(mode));
        SendMessageW(system_sound_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(mode));
    }
    running_indicator_check_ = MakeChoice(
        L"Show active indicator while running", false, 0, RunningIndicatorCheck);

    diagnostics_check_ = MakeChoice(L"Enable live diagnostics", false, 0, DiagnosticsCheck);
    safety_shield_check_ = MakeChoice(
        L"Show Safety Shield after Emergency Stop", false, 0, SafetyShieldCheck);
    click_position_indicator_check_ = MakeChoice(
        L"Show click position indicator", false, 0, ClickPositionIndicatorCheck);
    force_exit_on_emergency_stop_check_ = MakeChoice(
        L"Force exit on Emergency Stop", false, 0, ForceExitOnEmergencyStopCheck);
    capture_exclusion_check_ = MakeChoice(
        L"Hide Vector Click from screen capture", false, 0, CaptureExclusionCheck);
    keep_on_top_check_ = MakeChoice(
        L"Keep Vector Click on top", false, 0, KeepOnTopCheck);
    profile_label_ = MakeLabel(L"Local profile");
    profile_combo_ = MakeControl(
        WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
        0, ProfileCombo);
    manage_profiles_button_ = MakeControl(
        L"BUTTON", L"Manage Profiles...", BS_OWNERDRAW | WS_TABSTOP, 0,
        ManageProfilesButton);
    remember_settings_check_ = MakeChoice(L"Remember settings", false, 0, RememberSettingsCheck);
    import_settings_button_ = MakeControl(
        L"BUTTON", L"Import settings...", BS_OWNERDRAW | WS_TABSTOP, 0,
        ImportSettingsButton);

    control_parent_ = about_page_host_;
    about_identity_group_ = MakeGroup(L"About Vector Click");
    about_name_text_ = MakeLabel(ProductDisplayName);
    const std::wstring about_version = std::wstring(L"Version ") + ApplicationVersion;
    about_version_text_ = MakeLabel(about_version.c_str());
    about_description_text_ = MakeLabel(
        L"Native Windows mouse and keyboard automation utility focused on safety, clarity, and predictable behavior.");
    std::wstring about_details = L"Developed by ";
    about_details += DeveloperDisplayName;
    about_details += L"\nMozilla Public License 2.0";
    about_details_text_ = MakeLabel(about_details.c_str());

    about_links_group_ = MakeGroup(L"Support, diagnostics, and official links");
    about_links_description_text_ = MakeLabel(
        L"Open official Vector Click pages, report issues, or copy privacy-safe support information.");
    std::wstring about_support = L"Support: ";
    about_support += SupportEmail;
    about_support_text_ = MakeLabel(about_support.c_str());
    official_downloads_button_ = MakeControl(
        L"BUTTON", L"Official Downloads", BS_OWNERDRAW | WS_TABSTOP, 0, OfficialDownloadsButton);
    source_code_button_ = MakeControl(
        L"BUTTON", L"View Source Code", BS_OWNERDRAW | WS_TABSTOP, 0, SourceCodeButton);
    view_license_button_ = MakeControl(
        L"BUTTON", L"View License", BS_OWNERDRAW | WS_TABSTOP, 0, ViewLicenseButton);
    copy_support_email_button_ = MakeControl(
        L"BUTTON", L"Copy Support Email", BS_OWNERDRAW | WS_TABSTOP, 0, CopySupportEmailButton);
    copy_diagnostic_report_button_ = MakeControl(
        L"BUTTON", L"Copy Diagnostic Report", BS_OWNERDRAW | WS_TABSTOP, 0, CopyDiagnosticReportButton);
    report_bug_button_ = MakeControl(
        L"BUTTON", L"Report a Bug", BS_OWNERDRAW | WS_TABSTOP, 0, ReportBugButton);

    control_parent_ = content_host_;
    start_button_ = MakeControl(L"BUTTON", L"Start", BS_OWNERDRAW | WS_TABSTOP, 0, StartButton);
    stop_button_ = MakeControl(L"BUTTON", L"Stop", BS_OWNERDRAW | WS_TABSTOP, 0, StopButton);
    emergency_button_ = MakeControl(L"BUTTON", L"Emergency Stop", BS_OWNERDRAW | WS_TABSTOP, 0, EmergencyButton);
    status_text_ = MakeControl(L"STATIC", L"Status: Ready", SS_OWNERDRAW | SS_NOTIFY, 0, StatusText);
    diagnostics_text_ = DiagnosticsDisplay::Create(instance_, content_host_, DiagnosticsText);
    ApplyFont(diagnostics_text_);

    for (const HWND edit : {interval_minutes_edit_,
                            interval_seconds_edit_,
                            interval_edit_,
                            fixed_x_edit_,
                            fixed_y_edit_,
                            repeat_count_edit_,
                            run_time_hours_edit_,
                            run_time_minutes_edit_,
                            run_time_seconds_edit_,
                            button_down_minutes_edit_,
                            button_down_seconds_edit_,
                            button_down_edit_,
                            action_spacing_minutes_edit_,
                            action_spacing_seconds_edit_,
                            action_spacing_edit_,
                            minimum_interval_minutes_edit_,
                            minimum_interval_seconds_edit_,
                            minimum_interval_edit_,
                            maximum_interval_minutes_edit_,
                            maximum_interval_seconds_edit_,
                            maximum_interval_edit_,
                            burst_count_edit_}) {
        if (edit != nullptr) {
            SendMessageW(edit, EM_SETLIMITTEXT, MaximumNumericEditCharacters, 0);
            (void)SetWindowSubclass(edit,
                                    EditSubclassProc,
                                    1,
                                    reinterpret_cast<DWORD_PTR>(this));
            UpdateNumericEditFormatting(edit);
        }
    }

    EnumChildWindows(
        content_host_,
        [](const HWND child, const LPARAM) -> BOOL {
            ui::ApplyDarkControlTheme(child);
            return TRUE;
        },
        0);

    for (const HWND combo : {action_type_combo_,
                             action_pattern_combo_,
                             mouse_button_combo_,
                             generated_key_combo_,
                             start_hotkey_combo_,
                             emergency_hotkey_combo_,
                             backend_combo_,
                             random_interval_style_combo_,
                             down_duration_behavior_combo_,
                             process_priority_combo_,
                             timing_worker_priority_combo_,
                             timing_worker_qos_combo_,
                             hotkey_control_priority_combo_,
                             windows_notification_combo_,
                             system_sound_combo_,
                             profile_combo_}) {
        if (combo != nullptr) {
            (void)SetWindowSubclass(combo,
                                    ComboSubclassProc,
                                    1,
                                    reinterpret_cast<DWORD_PTR>(this));
        }
    }

    control_parent_ = nullptr;
}

layout::LayoutMetrics MainWindow::BuildLayoutMetrics() const {
    const auto compact_width = [this](const HWND control, const int minimum) {
        const int icon_reserve = GlyphForControl(control) == ui::Glyph::None ? 18 : 44;
        return PreferredControlWidth(control, icon_reserve, minimum);
    };

    return {
        .action_type_label_width = compact_width(action_type_label_, 118),
        .mouse_button_label_width = compact_width(mouse_button_label_, 118),
        .generated_key_label_width = compact_width(generated_key_label_, 118),
        .action_pattern_label_width = compact_width(action_pattern_label_, 118),
        .interval_label_width = compact_width(interval_label_, 128),
        .current_cursor_width = PreferredControlWidth(current_cursor_radio_, 34, 142),
        .fixed_position_width = PreferredControlWidth(fixed_position_radio_, 34, 184),
        .unlimited_width = PreferredControlWidth(unlimited_radio_, 34, 126),
        .limited_width = PreferredControlWidth(limited_radio_, 34, 116),
        .start_hotkey_label_width = compact_width(start_hotkey_label_, 132),
        .emergency_hotkey_label_width = compact_width(emergency_hotkey_label_, 142),
        .background_input_width = PreferredControlWidth(background_input_check_, 66, 232),
        .random_interval_width = PreferredControlWidth(random_interval_check_, 34, 228),
        .diagnostics_width = PreferredControlWidth(diagnostics_check_, 34, 206),
        .click_position_indicator_width =
            PreferredControlWidth(click_position_indicator_check_, 34, 238),
        .safety_shield_width = PreferredControlWidth(safety_shield_check_, 34, 288),
        .force_exit_on_emergency_stop_width =
            PreferredControlWidth(force_exit_on_emergency_stop_check_, 34, 278),
        .capture_exclusion_width =
            PreferredControlWidth(capture_exclusion_check_, 34, 306),
        .keep_on_top_width = PreferredControlWidth(keep_on_top_check_, 34, 220),
        .remember_settings_width = PreferredControlWidth(remember_settings_check_, 34, 194),
        .running_indicator_width =
            PreferredControlWidth(running_indicator_check_, 34, 286),
    };
}

void MainWindow::LayoutControls(const bool redraw) {
    if (content_host_ == nullptr || basic_tab_button_ == nullptr ||
        advanced_tab_button_ == nullptr || about_tab_button_ == nullptr ||
        action_type_combo_ == nullptr ||
        advanced_scroll_content_host_ == nullptr) {
        return;
    }
    FinishAdvancedScrollSnapshot(false);
    advanced_viewport_height_logical_ =
        layout::ClampAdvancedViewportHeight(advanced_viewport_height_logical_);
    advanced_scroll_offset_logical_ = std::clamp(
        advanced_scroll_offset_logical_, 0, AdvancedMaximumScrollLogical());
    const layout::MainLayout main_layout = layout::CalculateMainLayout(
        BuildLayoutMetrics(),
        status_height_logical_,
        CurrentLayoutPage(),
        advanced_viewport_height_logical_);

    struct Binding final {
        HWND window{};
        layout::Control control{};
    };

    // Win32 positions child windows in their parent's coordinate space. Keep
    // each layout group explicit so a future batching pass cannot mix windows
    // from different parents in one deferred-position transaction.
    const auto content_bindings = std::to_array<Binding>({
        {basic_tab_button_, layout::Control::BasicTab},
        {advanced_tab_button_, layout::Control::AdvancedTab},
        {about_tab_button_, layout::Control::AboutTab},
        {basic_page_host_, layout::Control::BasicPageHost},
        {advanced_page_host_, layout::Control::AdvancedPageHost},
        {about_page_host_, layout::Control::AboutPageHost},
        {status_text_, layout::Control::StatusText},
        {diagnostics_text_, layout::Control::DiagnosticsText},
        {start_button_, layout::Control::StartButton},
        {stop_button_, layout::Control::StopButton},
        {emergency_button_, layout::Control::EmergencyButton},
    });

    const auto basic_page_bindings = std::to_array<Binding>({
        {basic_top_host_, layout::Control::BasicTopHost},
        {repeat_group_, layout::Control::RepeatGroup},
        {hotkeys_group_, layout::Control::HotkeysGroup},
        {unlimited_radio_, layout::Control::UnlimitedRadio},
        {limited_radio_, layout::Control::LimitedRadio},
        {repeat_count_edit_, layout::Control::RepeatCountEdit},
        {repeat_unit_label_, layout::Control::RepeatUnitLabel},
        {run_time_hours_header_, layout::Control::RunTimeHoursHeader},
        {run_time_minutes_header_, layout::Control::RunTimeMinutesHeader},
        {run_time_seconds_header_, layout::Control::RunTimeSecondsHeader},
        {run_time_hours_edit_, layout::Control::RunTimeHoursEdit},
        {run_time_minutes_edit_, layout::Control::RunTimeMinutesEdit},
        {run_time_seconds_edit_, layout::Control::RunTimeSecondsEdit},
        {start_hotkey_label_, layout::Control::StartHotkeyLabel},
        {start_hotkey_combo_, layout::Control::StartHotkeyCombo},
        {emergency_hotkey_label_, layout::Control::EmergencyHotkeyLabel},
        {emergency_hotkey_combo_, layout::Control::EmergencyHotkeyCombo},
    });

    const auto basic_top_bindings = std::to_array<Binding>({
        {input_group_, layout::Control::InputGroup},
        {position_group_, layout::Control::PositionGroup},
        {action_type_label_, layout::Control::ActionTypeLabel},
        {action_type_combo_, layout::Control::ActionTypeCombo},
        {mouse_button_label_, layout::Control::MouseButtonLabel},
        {generated_key_label_, layout::Control::GeneratedKeyLabel},
        {mouse_button_combo_, layout::Control::MouseButtonCombo},
        {generated_key_combo_, layout::Control::GeneratedKeyCombo},
        {action_pattern_label_, layout::Control::ActionPatternLabel},
        {action_pattern_combo_, layout::Control::ActionPatternCombo},
        {interval_label_, layout::Control::IntervalLabel},
        {basic_interval_minutes_header_, layout::Control::BasicIntervalMinutesHeader},
        {basic_interval_seconds_header_, layout::Control::BasicIntervalSecondsHeader},
        {basic_interval_milliseconds_header_, layout::Control::BasicIntervalMillisecondsHeader},
        {interval_minutes_edit_, layout::Control::IntervalMinutesEdit},
        {interval_seconds_edit_, layout::Control::IntervalSecondsEdit},
        {interval_edit_, layout::Control::IntervalEdit},
        {rate_text_, layout::Control::RateText},
        {current_cursor_radio_, layout::Control::CurrentCursorRadio},
        {fixed_position_radio_, layout::Control::FixedPositionRadio},
        {fixed_x_label_, layout::Control::FixedXLabel},
        {fixed_x_edit_, layout::Control::FixedXEdit},
        {fixed_y_label_, layout::Control::FixedYLabel},
        {fixed_y_edit_, layout::Control::FixedYEdit},
        {capture_position_button_, layout::Control::CapturePositionButton},
        {position_unavailable_text_, layout::Control::PositionUnavailableText},
    });

    const auto advanced_page_bindings = std::to_array<Binding>({
        {advanced_timing_group_, layout::Control::AdvancedTimingGroup},
        {advanced_minutes_header_, layout::Control::AdvancedMinutesHeader},
        {advanced_seconds_header_, layout::Control::AdvancedSecondsHeader},
        {advanced_milliseconds_header_, layout::Control::AdvancedMillisecondsHeader},
        {button_down_label_, layout::Control::ButtonDownLabel},
        {button_down_minutes_edit_, layout::Control::ButtonDownMinutesEdit},
        {button_down_seconds_edit_, layout::Control::ButtonDownSecondsEdit},
        {button_down_edit_, layout::Control::ButtonDownEdit},
        {down_duration_behavior_label_, layout::Control::DownDurationBehaviorLabel},
        {down_duration_behavior_combo_, layout::Control::DownDurationBehaviorCombo},
        {action_spacing_label_, layout::Control::ActionSpacingLabel},
        {action_spacing_minutes_edit_, layout::Control::ActionSpacingMinutesEdit},
        {action_spacing_seconds_edit_, layout::Control::ActionSpacingSecondsEdit},
        {action_spacing_edit_, layout::Control::ActionSpacingEdit},
        {burst_count_label_, layout::Control::BurstCountLabel},
        {burst_count_edit_, layout::Control::BurstCountEdit},
        {backend_label_, layout::Control::BackendLabel},
        {backend_combo_, layout::Control::BackendCombo},
        {random_interval_check_, layout::Control::RandomIntervalCheck},
        {random_interval_style_label_, layout::Control::RandomIntervalStyleLabel},
        {random_interval_style_combo_, layout::Control::RandomIntervalStyleCombo},
        {minimum_interval_label_, layout::Control::MinimumIntervalLabel},
        {minimum_interval_minutes_edit_, layout::Control::MinimumIntervalMinutesEdit},
        {minimum_interval_seconds_edit_, layout::Control::MinimumIntervalSecondsEdit},
        {minimum_interval_edit_, layout::Control::MinimumIntervalEdit},
        {maximum_interval_label_, layout::Control::MaximumIntervalLabel},
        {maximum_interval_minutes_edit_, layout::Control::MaximumIntervalMinutesEdit},
        {maximum_interval_seconds_edit_, layout::Control::MaximumIntervalSecondsEdit},
        {maximum_interval_edit_, layout::Control::MaximumIntervalEdit},
        {target_group_, layout::Control::TargetGroup},
        {target_label_, layout::Control::TargetLabel},
        {target_status_text_, layout::Control::TargetStatusText},
        {select_target_button_, layout::Control::SelectTargetButton},
        {clear_target_button_, layout::Control::ClearTargetButton},
        {background_input_check_, layout::Control::BackgroundInputCheck},
        {admin_button_, layout::Control::AdminButton},
        {performance_group_, layout::Control::PerformanceGroup},
        {performance_guidance_text_, layout::Control::PerformanceGuidanceText},
        {options_group_, layout::Control::OptionsGroup},
        {diagnostics_check_, layout::Control::DiagnosticsCheck},
        {click_position_indicator_check_, layout::Control::ClickPositionIndicatorCheck},
        {safety_shield_check_, layout::Control::SafetyShieldCheck},
        {force_exit_on_emergency_stop_check_,
         layout::Control::ForceExitOnEmergencyStopCheck},
        {capture_exclusion_check_, layout::Control::CaptureExclusionCheck},
        {keep_on_top_check_, layout::Control::KeepOnTopCheck},
        {profile_label_, layout::Control::ProfileLabel},
        {profile_combo_, layout::Control::ProfileCombo},
        {manage_profiles_button_, layout::Control::ManageProfilesButton},
        {remember_settings_check_, layout::Control::RememberSettingsCheck},
        {import_settings_button_, layout::Control::ImportSettingsButton},
        {process_priority_label_, layout::Control::ProcessPriorityLabel},
        {process_priority_combo_, layout::Control::ProcessPriorityCombo},
        {timing_worker_priority_label_, layout::Control::TimingWorkerPriorityLabel},
        {timing_worker_priority_combo_, layout::Control::TimingWorkerPriorityCombo},
        {timing_worker_qos_label_, layout::Control::TimingWorkerQosLabel},
        {timing_worker_qos_combo_, layout::Control::TimingWorkerQosCombo},
        {hotkey_control_priority_label_, layout::Control::HotkeyControlPriorityLabel},
        {hotkey_control_priority_combo_, layout::Control::HotkeyControlPriorityCombo},
        {notifications_group_, layout::Control::NotificationsGroup},
        {windows_notification_label_, layout::Control::WindowsNotificationLabel},
        {windows_notification_combo_, layout::Control::WindowsNotificationCombo},
        {system_sound_label_, layout::Control::SystemSoundLabel},
        {system_sound_combo_, layout::Control::SystemSoundCombo},
        {running_indicator_check_, layout::Control::RunningIndicatorCheck},
    });


    const auto about_page_bindings = std::to_array<Binding>({
        {about_identity_group_, layout::Control::AboutIdentityGroup},
        {about_name_text_, layout::Control::AboutNameText},
        {about_version_text_, layout::Control::AboutVersionText},
        {about_description_text_, layout::Control::AboutDescriptionText},
        {about_details_text_, layout::Control::AboutDetailsText},
        {about_links_group_, layout::Control::AboutLinksGroup},
        {about_links_description_text_, layout::Control::AboutLinksDescriptionText},
        {about_support_text_, layout::Control::AboutSupportText},
        {official_downloads_button_, layout::Control::OfficialDownloadsButton},
        {source_code_button_, layout::Control::SourceCodeButton},
        {report_bug_button_, layout::Control::ReportBugButton},
        {copy_support_email_button_, layout::Control::CopySupportEmailButton},
        {view_license_button_, layout::Control::ViewLicenseButton},
        {copy_diagnostic_report_button_, layout::Control::CopyDiagnosticReportButton},
    });

    static_assert(content_bindings.size() + basic_page_bindings.size() +
                      basic_top_bindings.size() + advanced_page_bindings.size() +
                      about_page_bindings.size() == layout::ControlCount);

    const UINT placement_flags =
        SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOOWNERZORDER | SWP_NOZORDER |
        (redraw ? 0U : SWP_NOREDRAW);

    const auto apply_group = [this, &main_layout, placement_flags](
                                 const auto& bindings,
                                 const int vertical_offset_logical = 0) {
        for (const Binding& binding : bindings) {
            if (binding.window == nullptr || IsWindow(binding.window) == FALSE) {
                continue;
            }

            const layout::Rect& logical = main_layout[binding.control];
            SetWindowPos(binding.window,
                         nullptr,
                         Scale(logical.x, dpi_),
                         Scale(logical.y + vertical_offset_logical, dpi_),
                         Scale(logical.width, dpi_),
                         Scale(logical.height, dpi_),
                         placement_flags);
        }
    };

    apply_group(content_bindings);

    RECT advanced_page_client{};
    if (GetClientRect(advanced_page_host_, &advanced_page_client) != FALSE) {
        const int content_height = std::max(
            static_cast<int>(advanced_page_client.bottom),
            Scale(layout::AdvancedPageHeight, dpi_));
        SetWindowPos(advanced_scroll_content_host_,
                     nullptr,
                     0,
                     -Scale(advanced_scroll_offset_logical_, dpi_),
                     std::max(1, static_cast<int>(advanced_page_client.right)),
                     std::max(1, content_height),
                     placement_flags);
    }
    apply_group(basic_page_bindings);
    apply_group(basic_top_bindings);
    apply_group(advanced_page_bindings);
    apply_group(about_page_bindings);

    for (const HWND edit : {interval_minutes_edit_,
                            interval_seconds_edit_,
                            interval_edit_,
                            fixed_x_edit_,
                            fixed_y_edit_,
                            repeat_count_edit_,
                            run_time_hours_edit_,
                            run_time_minutes_edit_,
                            run_time_seconds_edit_,
                            button_down_minutes_edit_,
                            button_down_seconds_edit_,
                            button_down_edit_,
                            action_spacing_minutes_edit_,
                            action_spacing_seconds_edit_,
                            action_spacing_edit_,
                            minimum_interval_minutes_edit_,
                            minimum_interval_seconds_edit_,
                            minimum_interval_edit_,
                            maximum_interval_minutes_edit_,
                            maximum_interval_seconds_edit_,
                            maximum_interval_edit_,
                            burst_count_edit_}) {
        UpdateNumericEditFormatting(edit);
    }

    const auto apply_tab_region = [this](const HWND tab) {
        if (tab == nullptr || IsWindow(tab) == FALSE) {
            return;
        }
        RECT bounds{};
        if (GetClientRect(tab, &bounds) == FALSE ||
            bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
            return;
        }

        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int top_radius =
            std::max(1, Scale(layout::TabCornerRadius, dpi_));
        const int bottom_radius =
            std::max(1, Scale(layout::TabDockJunctionRadius, dpi_));
        const int top_diameter = std::max(2, top_radius * 2);
        const int bottom_diameter = std::max(2, bottom_radius * 2);

        HRGN region = CreateRoundRectRgn(0,
                                         0,
                                         width + 1,
                                         std::min(height + 1,
                                                  top_diameter + 1),
                                         top_diameter,
                                         top_diameter);
        HRGN middle_region = CreateRectRgn(0,
                                           std::min(top_radius, height),
                                           width + 1,
                                           std::max(std::min(top_radius, height) + 1,
                                                    height - bottom_radius + 1));
        HRGN bottom_region = CreateRoundRectRgn(0,
                                                std::max(0, height - bottom_diameter),
                                                width + 1,
                                                height + 1,
                                                bottom_diameter,
                                                bottom_diameter);
        if (region == nullptr || middle_region == nullptr ||
            bottom_region == nullptr ||
            CombineRgn(region, region, middle_region, RGN_OR) == ERROR ||
            CombineRgn(region, region, bottom_region, RGN_OR) == ERROR) {
            if (bottom_region != nullptr) {
                DeleteObject(bottom_region);
            }
            if (middle_region != nullptr) {
                DeleteObject(middle_region);
            }
            if (region != nullptr) {
                DeleteObject(region);
            }
            return;
        }
        DeleteObject(bottom_region);
        DeleteObject(middle_region);
        if (SetWindowRgn(tab, region, FALSE) == 0) {
            DeleteObject(region);
        }
    };
    apply_tab_region(basic_tab_button_);
    apply_tab_region(advanced_tab_button_);
    apply_tab_region(about_tab_button_);

    UpdateKeySelectorTooltips();
    UpdateProfileSelectorDroppedWidth();
    UpdateProfileSelectorTooltip();

    const HWND selected_page = selected_page_index_ == 2
                                   ? about_page_host_
                                   : selected_page_index_ == 1
                                         ? advanced_page_host_
                                         : basic_page_host_;
    SetWindowPos(selected_page,
                 HWND_TOP,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    // The scrollbar is a sibling in the content host's right gutter. Restore
    // it above the selected Advanced page after that page takes the top z-order.
    UpdateAdvancedScrollbar(false);

    if (redraw) {
        RedrawWindow(content_host_,
                     nullptr,
                     nullptr,
                     RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

void MainWindow::SelectPage(int index) {
    if (basic_tab_button_ == nullptr || advanced_tab_button_ == nullptr ||
        about_tab_button_ == nullptr || basic_page_host_ == nullptr ||
        advanced_page_host_ == nullptr || about_page_host_ == nullptr) {
        return;
    }

    FinishAdvancedScrollSnapshot(false);
    CloseComboPopup(false);

    index = std::clamp(index, 0, 2);
    const std::array<HWND, 3> pages{
        basic_page_host_, advanced_page_host_, about_page_host_};
    const HWND selected_page = pages[static_cast<std::size_t>(index)];
    bool already_selected = IsWindowVisible(selected_page) != FALSE;
    for (std::size_t page_index = 0; page_index < pages.size(); ++page_index) {
        if (page_index != static_cast<std::size_t>(index) &&
            IsWindowVisible(pages[page_index]) != FALSE) {
            already_selected = false;
            break;
        }
    }
    if (already_selected) {
        return;
    }

    RECT previous_content_rect{};
    const bool have_previous_content_rect =
        GetWindowRect(content_host_, &previous_content_rect) != FALSE;
    if (have_previous_content_rect) {
        (void)MapWindowPoints(HWND_DESKTOP,
                              window_,
                              reinterpret_cast<POINT*>(&previous_content_rect),
                              2);
    }

    bool transition_overlay_active = false;
    if (startup_presentation_complete_ && window_ != nullptr &&
        IsWindowVisible(window_) != FALSE) {
        // Preserve the previous complete frame while the selected page and its
        // compact or enlarged Advanced viewport are laid out underneath it.
        transition_overlay_active =
            BeginResizeOverlay(nullptr, 0, true);
    }

    // Same-geometry page switches can preserve direct persistent controls
    // whose geometry and invalid state remain unchanged. Record their content
    // host-relative state before layout so an unexpected visibility or bounds
    // change is still treated as repaint work.
    struct PersistentVisualState final {
        HWND window{};
        RECT bounds{};
        bool bounds_valid{};
        bool visible{};
    };
    const std::array<HWND, 6> persistent_windows{
        start_button_,
        stop_button_,
        emergency_button_,
        status_text_,
        diagnostics_text_,
        advanced_scrollbar_,
    };
    const auto capture_persistent_state =
        [this](const HWND control) -> PersistentVisualState {
        PersistentVisualState state{};
        state.window = control;
        if (control == nullptr || IsWindow(control) == FALSE) {
            return state;
        }
        state.visible = IsWindowVisible(control) != FALSE;
        state.bounds_valid = GetWindowRect(control, &state.bounds) != FALSE;
        if (state.bounds_valid) {
            (void)MapWindowPoints(HWND_DESKTOP,
                                  content_host_,
                                  reinterpret_cast<POINT*>(&state.bounds),
                                  2);
        }
        return state;
    };
    std::array<PersistentVisualState, persistent_windows.size()>
        persistent_before{};
    for (std::size_t control_index = 0;
         control_index < persistent_windows.size();
         ++control_index) {
        persistent_before[control_index] =
            capture_persistent_state(persistent_windows[control_index]);
    }

    selected_page_index_ = index;

    InvalidateControl(basic_tab_button_);
    InvalidateControl(advanced_tab_button_);
    InvalidateControl(about_tab_button_);

    SendMessageW(content_host_, WM_SETREDRAW, FALSE, 0);
    const int previous_advanced_viewport = advanced_viewport_height_logical_;
    PositionContentHost(false);
    if (advanced_viewport_height_logical_ == previous_advanced_viewport) {
        // PositionContentHost already performs the full layout when switching
        // to or from an Advanced viewport with a different height. Avoid
        // immediately repeating that same control-positioning pass. Basic /
        // About transitions still need the explicit layout below because their
        // shared fixed viewport does not trigger the internal layout path.
        LayoutControls(false);
    }
    for (std::size_t page_index = 0; page_index < pages.size(); ++page_index) {
        const bool show = page_index == static_cast<std::size_t>(index);
        SetWindowPos(pages[page_index],
                     show ? HWND_TOP : nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_NOOWNERZORDER | SWP_NOREDRAW |
                         (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW) |
                         (show ? 0U : SWP_NOZORDER));
    }
    SendMessageW(content_host_, WM_SETREDRAW, TRUE, 0);

    RECT current_content_rect{};
    const bool have_current_content_rect =
        GetWindowRect(content_host_, &current_content_rect) != FALSE;
    if (have_current_content_rect) {
        (void)MapWindowPoints(HWND_DESKTOP,
                              window_,
                              reinterpret_cast<POINT*>(&current_content_rect),
                              2);
    }

    const bool content_rect_changed =
        have_previous_content_rect && have_current_content_rect &&
        EqualRect(&previous_content_rect, &current_content_rect) == FALSE;
    if (content_rect_changed) {
        // Enlarged Advanced and compact Basic / About use different centered
        // host rectangles. Moving the WS_CLIPCHILDREN content host does not
        // guarantee that its vacated parent pixels are repainted, especially
        // while a transition popup is covering the client. Explicitly clear
        // the previous rectangle before exposing the new page so maximized tab
        // switches cannot leave duplicated tabs, cards, status areas, or footer
        // buttons behind. The current child is clipped out by the main window.
        RedrawWindow(window_,
                     &previous_content_rect,
                     nullptr,
                     RDW_ERASE | RDW_INVALIDATE | RDW_NOCHILDREN |
                         RDW_UPDATENOW);
    }

    std::array<PersistentVisualState, persistent_windows.size()>
        persistent_after{};
    std::array<bool, persistent_windows.size()> persistent_needs_redraw{};
    bool shell_dirty = false;

    if (!content_rect_changed) {
        for (std::size_t control_index = 0;
             control_index < persistent_windows.size();
             ++control_index) {
            persistent_after[control_index] =
                capture_persistent_state(persistent_windows[control_index]);
            const PersistentVisualState& before =
                persistent_before[control_index];
            const PersistentVisualState& after = persistent_after[control_index];

            const bool geometry_changed =
                before.bounds_valid != after.bounds_valid ||
                before.visible != after.visible ||
                (before.bounds_valid && after.bounds_valid &&
                 EqualRect(&before.bounds, &after.bounds) == FALSE);

            RECT update{};
            const bool dirty =
                after.visible && after.window != nullptr &&
                GetUpdateRect(after.window, &update, FALSE) != FALSE;
            persistent_needs_redraw[control_index] =
                geometry_changed || dirty;
        }

        RECT shell_update{};
        shell_dirty =
            GetUpdateRect(content_host_, &shell_update, FALSE) != FALSE;
    }

    if (content_rect_changed) {
        // Geometry-changing transitions, currently switches to or from
        // Advanced, are faster and simpler as one coordinated full hierarchy
        // repaint. Same-geometry Basic / About switches use the narrower path
        // below so already-valid shell and persistent pixels are preserved.
        RedrawWindow(content_host_,
                     nullptr,
                     nullptr,
                     RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                         RDW_UPDATENOW);
    } else {
        if (shell_dirty) {
            RedrawWindow(content_host_,
                         nullptr,
                         nullptr,
                         RDW_ERASE | RDW_INVALIDATE | RDW_NOCHILDREN |
                             RDW_UPDATENOW);
        }

        // The replacement page still completes synchronously beneath the
        // retained frame. Only unchanged persistent controls are skipped.
        RedrawWindow(selected_page,
                     nullptr,
                     nullptr,
                     RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                         RDW_UPDATENOW);

        for (std::size_t control_index = 0;
             control_index < persistent_after.size();
             ++control_index) {
            const PersistentVisualState& state =
                persistent_after[control_index];
            if (!persistent_needs_redraw[control_index] || !state.visible ||
                state.window == nullptr || IsWindow(state.window) == FALSE) {
                continue;
            }
            RedrawWindow(state.window,
                         nullptr,
                         nullptr,
                         RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN |
                             RDW_UPDATENOW);
        }

        // Keep a narrow correctness guard around the optimized path. If
        // Windows still reports pending paint anywhere in the visible content
        // hierarchy, fall back to the established full synchronous repaint
        // before the retained transition frame is removed.
        struct PendingPaintValidation final {
            HWND basic_tab{};
            HWND advanced_tab{};
            HWND about_tab{};
            unsigned pending{};
        } validation{basic_tab_button_,
                     advanced_tab_button_,
                     about_tab_button_,
                     0U};

        RECT pending_update{};
        if (GetUpdateRect(content_host_, &pending_update, FALSE) != FALSE) {
            ++validation.pending;
        }
        EnumChildWindows(
            content_host_,
            [](const HWND child, const LPARAM parameter) -> BOOL {
                auto* state =
                    reinterpret_cast<PendingPaintValidation*>(parameter);
                if (state == nullptr || IsWindowVisible(child) == FALSE ||
                    child == state->basic_tab ||
                    child == state->advanced_tab ||
                    child == state->about_tab) {
                    return TRUE;
                }
                RECT update{};
                if (GetUpdateRect(child, &update, FALSE) != FALSE) {
                    ++state->pending;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&validation));

        if (validation.pending != 0U) {
            RedrawWindow(content_host_,
                         nullptr,
                         nullptr,
                         RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                             RDW_UPDATENOW);
        }
    }

    for (const HWND tab :
         {basic_tab_button_, advanced_tab_button_, about_tab_button_}) {
        RedrawWindow(tab,
                     nullptr,
                     nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    }

    if (transition_overlay_active) {
        (void)DwmFlush();
        HideResizeOverlay();
        // The visible transition is complete once the new live hierarchy has
        // been composed beneath the retained popup and that popup is hidden.
        // Do not block the tab click on another compositor round trip solely
        // for movement-cache maintenance. Mark the retained frame stale so
        // the queued refresh renders Vector Click's own HWND hierarchy instead
        // of sampling the composed desktop while the popup may still be
        // retiring. A later ordinary movement boundary can replace it with an
        // exact composed frame through the existing pending-refresh path.
        MarkMoveCoverPresentationDirty(false);
        QueueMoveCoverRefresh();
    } else {
        QueueMoveCoverRefresh();
    }
}

bool MainWindow::BeginResizeOverlay(const RECT* proposed_outer,
                                    const WPARAM sizing_edge,
                                    const bool allow_focus_only_patch) {
    // A page click or sizing gesture can move focus immediately before the
    // retained surface is requested. If the cached bitmap represents a
    // different focused control, rebuild from Vector Click's hierarchy. A
    // desktop BitBlt in this same message turn can still contain the prior
    // compositor frame and would reproduce the stale focus flash.
    const bool move_cover_focus_changed =
        move_cover_bitmap_.get() != nullptr &&
        move_cover_focus_ != GetFocus();

    // Tab clicks commonly invalidate an otherwise current retained frame only
    // because keyboard focus moved to another tab. Repair that narrow case by
    // cloning the retained bitmap and repainting the affected hierarchy regions.
    // Every other cache mismatch keeps the established complete capture path.
    if (allow_focus_only_patch && !HasReusableMoveCover()) {
        (void)TryPatchMoveCoverForFocusOnlyTabSwitch();
    }

    if (!HasReusableMoveCover()) {
        if (move_cover_focus_changed ||
            move_cover_pending_exact_screen_refresh_ ||
            IsCaptureExclusionRequested()) {
            (void)CaptureMoveCoverFromWindow();
        } else if (CanCaptureMoveCoverFromScreen()) {
            (void)CaptureMoveCoverFromScreen();
        } else if (CaptureMoveCoverFromWindow()) {
            move_cover_pending_exact_screen_refresh_ = true;
        }
    }
    if (!HasReusableMoveCover()) {
        return false;
    }

    ClearResizePreviewState();
    if (interactive_resize_ &&
        CurrentLayoutPage() == layout::Page::Advanced) {
        // Capture the complete Advanced scroll content once at the start of a
        // manual resize. Later committed WM_SIZE updates can compose a taller
        // or shorter viewport from cached pixels without repeatedly laying out
        // or printing the live Advanced HWND hierarchy.
        (void)PrepareAdvancedResizePreview();
    }

    if (resize_overlay_ == nullptr ||
        IsWindow(resize_overlay_) == FALSE) {
        // During a genuine manual edge / corner resize, keep the retained
        // surface inside the main window's child tree. Windows then moves it
        // with the parent and clips it to the committed client area, so an old
        // retained frame cannot remain visible at an independent desktop
        // rectangle while the next frame is being composed. Programmatic
        // transitions and capture-exclusion-sensitive sessions retain the
        // accepted top-level popup path.
        const int advanced_maximum_scroll = AdvancedMaximumScrollLogical();
        const bool advanced_bottom_anchored_top_resize =
            interactive_resize_ &&
            CurrentLayoutPage() == layout::Page::Advanced &&
            resize_advanced_preview_bitmap_.get() != nullptr &&
            advanced_maximum_scroll > 0 &&
            advanced_scroll_offset_logical_ == advanced_maximum_scroll &&
            (sizing_edge == WMSZ_TOP || sizing_edge == WMSZ_TOPLEFT ||
             sizing_edge == WMSZ_TOPRIGHT);

        resize_advanced_physical_bottom_anchor_active_ = false;
        resize_advanced_physical_bottom_anchor_screen_y_ = 0;
        if (advanced_bottom_anchored_top_resize) {
            RECT anchor_client{};
            if (GetClientRect(window_, &anchor_client) != FALSE) {
                (void)MapWindowPoints(
                    window_,
                    HWND_DESKTOP,
                    reinterpret_cast<POINT*>(&anchor_client),
                    2);
                const int anchor_width =
                    anchor_client.right - anchor_client.left;
                const int anchor_height =
                    anchor_client.bottom - anchor_client.top;
                const int anchor_viewport =
                    layout::CalculateAdvancedViewportHeight(
                        anchor_height, dpi_, status_height_logical_);
                const layout::Rect anchor_host =
                    layout::CalculateCenteredContentHost(
                        anchor_width,
                        anchor_height,
                        dpi_,
                        status_height_logical_,
                        layout::Page::Advanced,
                        anchor_viewport);
                const layout::LayoutMetrics metrics{};
                const layout::MainLayout anchor_layout =
                    layout::CalculateMainLayout(
                        metrics,
                        status_height_logical_,
                        layout::Page::Advanced,
                        anchor_viewport);
                const int anchor_page_y = Scale(
                    anchor_layout[layout::Control::AdvancedPageHost].y,
                    dpi_);
                resize_advanced_physical_bottom_anchor_screen_y_ =
                    static_cast<int>(anchor_client.top) + anchor_host.y +
                    anchor_page_y -
                    Scale(advanced_scroll_offset_logical_, dpi_);
                resize_advanced_physical_bottom_anchor_active_ = true;
            }
        }

        // A child layered surface follows the parent window immediately. That
        // is ideal for ordinary manual resizing, but a bottom-scrolled
        // Advanced preview must change its source crop by the opposite amount
        // when the top edge moves so the bottom content remains visually
        // anchored. Publishing that new crop separately from the inherited
        // child-window movement can expose a one-frame counter-motion. For
        // this exact case, use the existing top-level layered route so screen
        // position, size, and pixels are committed together by
        // UpdateLayeredWindow. All other manual resize paths remain unchanged.
        resize_overlay_child_clipped_ =
            interactive_resize_ && !IsCaptureExclusionRequested() &&
            !advanced_bottom_anchored_top_resize;
        const DWORD overlay_ex_style =
            resize_overlay_child_clipped_
                ? (WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_NOPARENTNOTIFY)
                : (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED);
        const DWORD overlay_style =
            resize_overlay_child_clipped_
                ? (WS_CHILD | WS_CLIPSIBLINGS)
                : WS_POPUP;
        const HWND overlay = CreateWindowExW(
            overlay_ex_style,
            ResizeOverlayClassName,
            L"",
            overlay_style,
            0,
            0,
            1,
            1,
            window_,
            nullptr,
            instance_,
            this);
        if (overlay == nullptr) {
            resize_overlay_ = nullptr;
            resize_overlay_child_clipped_ = false;
            return false;
        }
        resize_overlay_ = overlay;
        resize_overlay_geometry_valid_ = false;

        if (!resize_overlay_child_clipped_) {
            // The retained top-level surface owns only its per-pixel
            // bottom-corner alpha. Prevent DWM from adding a second independent
            // popup corner treatment. Child overlays are already bounded by
            // the parent client and do not need a top-level corner preference.
            constexpr DWORD RoundCornerPreferenceAttribute = 33;
            constexpr DWORD DoNotRoundCornerPreference = 1;
            (void)DwmSetWindowAttribute(overlay,
                                        RoundCornerPreferenceAttribute,
                                        &DoNotRoundCornerPreference,
                                        sizeof(DoNotRoundCornerPreference));

            if (IsCaptureExclusionRequested() &&
                ApplyRequestedCaptureExclusion(overlay).result !=
                    CaptureExclusionResult::Applied) {
                DestroyWindow(overlay);
                resize_overlay_ = nullptr;
                resize_overlay_child_clipped_ = false;
                return false;
            }
        }
    }

    const bool positioned = proposed_outer != nullptr
                                ? UpdateResizeOverlayForOuterRect(*proposed_outer,
                                                                  sizing_edge)
                                : UpdateResizeOverlayForCurrentClient();
    if (!positioned) {
        HideResizeOverlay();
        return false;
    }

    if (resize_overlay_child_clipped_) {
        // The guard fills newly exposed client pixels beneath the retained
        // child overlay during outward growth. If it cannot be created, keep
        // the parent-clipped retained overlay as the safe fallback.
        (void)EnsureResizeOverlayGuard();
    }

    // Commit the independent client surface before native resizing can expose
    // the stationary live hierarchy beneath it. This wait occurs once at the
    // transition boundary, never for each resize step.
    (void)DwmFlush();
    return true;
}

bool MainWindow::UpdateResizeOverlayForOuterRect(
    const RECT& outer,
    const WPARAM sizing_edge) noexcept {
    if (resize_overlay_ == nullptr ||
        IsWindow(resize_overlay_) == FALSE || window_ == nullptr ||
        IsWindow(window_) == FALSE) {
        return false;
    }

    RECT current_outer{};
    RECT current_client{};
    if (GetWindowRect(window_, &current_outer) == FALSE ||
        GetClientRect(window_, &current_client) == FALSE) {
        return false;
    }
    (void)MapWindowPoints(window_,
                          HWND_DESKTOP,
                          reinterpret_cast<POINT*>(&current_client),
                          2);

    RECT effective_outer = outer;
    const int minimum_width = Scale(layout::MinimumWindowWidth, dpi_);
    const int minimum_height =
        Scale(layout::MinimumOuterHeight(status_height_logical_, CurrentLayoutPage()), dpi_);
    if (effective_outer.right - effective_outer.left < minimum_width) {
        const bool moving_left = sizing_edge == WMSZ_LEFT ||
                                 sizing_edge == WMSZ_TOPLEFT ||
                                 sizing_edge == WMSZ_BOTTOMLEFT;
        if (moving_left) {
            effective_outer.left = effective_outer.right - minimum_width;
        } else {
            effective_outer.right = effective_outer.left + minimum_width;
        }
    }
    if (effective_outer.bottom - effective_outer.top < minimum_height) {
        const bool moving_top = sizing_edge == WMSZ_TOP ||
                                sizing_edge == WMSZ_TOPLEFT ||
                                sizing_edge == WMSZ_TOPRIGHT;
        if (moving_top) {
            effective_outer.top = effective_outer.bottom - minimum_height;
        } else {
            effective_outer.bottom = effective_outer.top + minimum_height;
        }
    }

    const int left_inset = current_client.left - current_outer.left;
    const int top_inset = current_client.top - current_outer.top;
    const int right_inset = current_outer.right - current_client.right;
    const int bottom_inset = current_outer.bottom - current_client.bottom;
    RECT proposed_client{
        effective_outer.left + left_inset,
        effective_outer.top + top_inset,
        effective_outer.right - right_inset,
        effective_outer.bottom - bottom_inset,
    };
    const int width = proposed_client.right - proposed_client.left;
    const int height = proposed_client.bottom - proposed_client.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    const int proposed_advanced_viewport =
        CurrentLayoutPage() == layout::Page::Advanced
            ? layout::CalculateAdvancedViewportHeight(
                  height, dpi_, status_height_logical_)
            : layout::BasicPageHeight;
    resize_overlay_advanced_viewport_logical_ = proposed_advanced_viewport;
    const layout::Rect host = layout::CalculateCenteredContentHost(
        width,
        height,
        dpi_,
        status_height_logical_,
        CurrentLayoutPage(),
        proposed_advanced_viewport);
    const bool unchanged = resize_overlay_geometry_valid_ &&
                           EqualRect(&resize_overlay_screen_rect_,
                                     &proposed_client) != FALSE &&
                           resize_overlay_content_x_ == host.x &&
                           resize_overlay_content_y_ == host.y;
    if (unchanged) {
        return true;
    }

    // Publish the replacement resize frame and its destination geometry in one
    // layered-window transaction. Moving the popup first would temporarily
    // expose the previous retained pixels at the new rectangle, which can look
    // like inner jitter or a trailing shadow during fast resizing.
    const int previous_content_x = resize_overlay_content_x_;
    const int previous_content_y = resize_overlay_content_y_;
    resize_overlay_content_x_ = host.x;
    resize_overlay_content_y_ = host.y;
    if (!PresentResizeOverlay(effective_outer, proposed_client)) {
        resize_overlay_content_x_ = previous_content_x;
        resize_overlay_content_y_ = previous_content_y;
        resize_overlay_geometry_valid_ = false;
        return false;
    }
    resize_overlay_screen_rect_ = proposed_client;
    resize_overlay_geometry_valid_ = true;
    return true;
}

bool MainWindow::UpdateResizeOverlayForCurrentClient() noexcept {
    if (resize_overlay_ == nullptr ||
        IsWindow(resize_overlay_) == FALSE || window_ == nullptr ||
        IsWindow(window_) == FALSE) {
        return false;
    }

    RECT client{};
    if (GetClientRect(window_, &client) == FALSE ||
        IsRectEmpty(&client)) {
        return false;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int proposed_advanced_viewport =
        CurrentLayoutPage() == layout::Page::Advanced
            ? layout::CalculateAdvancedViewportHeight(
                  height, dpi_, status_height_logical_)
            : layout::BasicPageHeight;
    resize_overlay_advanced_viewport_logical_ = proposed_advanced_viewport;
    const layout::Rect host = layout::CalculateCenteredContentHost(
        width,
        height,
        dpi_,
        status_height_logical_,
        CurrentLayoutPage(),
        proposed_advanced_viewport);

    (void)MapWindowPoints(window_,
                          HWND_DESKTOP,
                          reinterpret_cast<POINT*>(&client),
                          2);
    const bool unchanged = resize_overlay_geometry_valid_ &&
                           EqualRect(&resize_overlay_screen_rect_, &client) !=
                               FALSE &&
                           resize_overlay_content_x_ == host.x &&
                           resize_overlay_content_y_ == host.y;
    if (unchanged) {
        return true;
    }

    RECT outer{};
    if (GetWindowRect(window_, &outer) == FALSE) {
        return false;
    }

    // Keep the previous layered surface untouched until the replacement bitmap
    // is ready, then let UpdateLayeredWindow commit position, size, and pixels
    // together. This avoids a compositor-visible old-frame-at-new-geometry
    // interval between separate SetWindowPos and pixel publication calls.
    const int previous_content_x = resize_overlay_content_x_;
    const int previous_content_y = resize_overlay_content_y_;
    resize_overlay_content_x_ = host.x;
    resize_overlay_content_y_ = host.y;
    if (!PresentResizeOverlay(outer, client)) {
        resize_overlay_content_x_ = previous_content_x;
        resize_overlay_content_y_ = previous_content_y;
        resize_overlay_geometry_valid_ = false;
        return false;
    }
    resize_overlay_screen_rect_ = client;
    resize_overlay_geometry_valid_ = true;
    return true;
}

bool MainWindow::EnsureResizeOverlayGuard() noexcept {
    if (!resize_overlay_child_clipped_ || window_ == nullptr ||
        IsWindow(window_) == FALSE || resize_overlay_ == nullptr ||
        IsWindow(resize_overlay_) == FALSE) {
        return !resize_overlay_child_clipped_;
    }

    if (resize_overlay_guard_ != nullptr &&
        IsWindow(resize_overlay_guard_) != FALSE) {
        return true;
    }

    RECT current_client{};
    if (GetClientRect(window_, &current_client) == FALSE) {
        return false;
    }
    const int current_width = current_client.right - current_client.left;
    const int current_height = current_client.bottom - current_client.top;
    if (current_width <= 0 || current_height <= 0) {
        return false;
    }

    // Give the underlay enough parent-relative extent to cover any ordinary
    // manual growth across the current virtual desktop. It is an ordinary
    // child window, not a backing bitmap, so the large logical extent does not
    // add per-frame layered-surface allocation or publication cost. Windows
    // clips it to the actual parent client.
    const int virtual_width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int virtual_height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    const int guard_width = std::max(current_width, virtual_width);
    const int guard_height = std::max(current_height, virtual_height);

    const HWND guard = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
        ResizeOverlayClassName,
        L"",
        WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS,
        0,
        0,
        guard_width,
        guard_height,
        window_,
        nullptr,
        instance_,
        this);
    if (guard == nullptr) {
        return false;
    }

    resize_overlay_guard_ = guard;

    // The retained layered child is already visible at this point. Show the
    // guard directly beneath it so creating the guard cannot flash a blank
    // surface over the retained UI. No per-frame guard repositioning follows.
    if (SetWindowPos(guard,
                     resize_overlay_,
                     0,
                     0,
                     guard_width,
                     guard_height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW) ==
        FALSE) {
        DestroyWindow(guard);
        resize_overlay_guard_ = nullptr;
        return false;
    }

    RedrawWindow(guard,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    return true;
}

void MainWindow::HideResizeOverlayGuard() noexcept {
    if (resize_overlay_guard_ != nullptr &&
        IsWindow(resize_overlay_guard_) != FALSE) {
        SetWindowPos(resize_overlay_guard_,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOREDRAW);
        DestroyWindow(resize_overlay_guard_);
    }
    resize_overlay_guard_ = nullptr;
}

bool MainWindow::PresentResizeOverlay(const RECT& outer,
                                      const RECT& client) noexcept {
    if (resize_overlay_ == nullptr || IsWindow(resize_overlay_) == FALSE) {
        return false;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    RECT current_outer{};
    RECT visible_outer{};
    RECT effective_visible = outer;
    if (GetWindowRect(window_, &current_outer) != FALSE &&
        SUCCEEDED(DwmGetWindowAttribute(window_,
                                        DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &visible_outer,
                                        sizeof(visible_outer)))) {
        effective_visible.left += visible_outer.left - current_outer.left;
        effective_visible.top += visible_outer.top - current_outer.top;
        effective_visible.right += visible_outer.right - current_outer.right;
        effective_visible.bottom += visible_outer.bottom - current_outer.bottom;
    }

    int left_radius = 0;
    int right_radius = 0;
    if (IsZoomed(window_) == FALSE) {
        const int outer_radius = std::max(
            1, Scale(ResizeOverlayCornerRadiusLogical, dpi_));
        const int left_inset = std::max(
            0, static_cast<int>(client.left - effective_visible.left));
        const int right_inset = std::max(
            0, static_cast<int>(effective_visible.right - client.right));
        const int bottom_inset = std::max(
            0, static_cast<int>(effective_visible.bottom - client.bottom));
        left_radius =
            std::max(0, outer_radius - std::min(left_inset, bottom_inset));
        right_radius =
            std::max(0, outer_radius - std::min(right_inset, bottom_inset));
    }

    const HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        return false;
    }
    const HDC surface_dc = CreateCompatibleDC(screen_dc);
    if (surface_dc == nullptr) {
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw_pixels = nullptr;
    const HBITMAP surface = CreateDIBSection(surface_dc,
                                             &info,
                                             DIB_RGB_COLORS,
                                             &raw_pixels,
                                             nullptr,
                                             0);
    if (surface == nullptr || raw_pixels == nullptr) {
        if (surface != nullptr) {
            DeleteObject(surface);
        }
        DeleteDC(surface_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }
    const HGDIOBJ previous_surface = SelectObject(surface_dc, surface);
    if (previous_surface == nullptr || previous_surface == HGDI_ERROR) {
        DeleteObject(surface);
        DeleteDC(surface_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    const RECT local{0, 0, width, height};
    ui::Fill(surface_dc, local, ui::Window);

    const bool advanced_preview_painted =
        PaintAdvancedResizePreview(
            surface_dc, width, height, static_cast<int>(client.top));
    if (!advanced_preview_painted && move_cover_bitmap_.get() != nullptr &&
        move_cover_width_ > 0 && move_cover_height_ > 0) {
        const HDC source = CreateCompatibleDC(surface_dc);
        if (source != nullptr) {
            const HGDIOBJ previous =
                SelectObject(source, move_cover_bitmap_.get());
            if (previous != nullptr && previous != HGDI_ERROR) {
                BitBlt(surface_dc,
                       resize_overlay_content_x_,
                       resize_overlay_content_y_,
                       move_cover_width_,
                       move_cover_height_,
                       source,
                       0,
                       0,
                       SRCCOPY);
                SelectObject(source, previous);
            }
            DeleteDC(source);
        }
    }

    ApplyBottomRoundedOverlayAlpha(static_cast<std::uint32_t*>(raw_pixels),
                                   width,
                                   height,
                                   left_radius,
                                   right_radius);

    // UpdateLayeredWindow ultimately positions a WS_CHILD layered window in
    // the parent-client coordinate space on the supported Windows path. The
    // retained geometry cache remains screen-based so it can still be compared
    // with the committed client rectangle, but the child publication itself
    // must start at the parent client origin. Passing the screen-space client
    // origin here would add the parent origin a second time and displace the
    // retained surface down and right inside the clipped parent.
    POINT destination = resize_overlay_child_clipped_
                            ? POINT{0, 0}
                            : POINT{client.left, client.top};
    SIZE size{width, height};
    POINT source_origin{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL updated = UpdateLayeredWindow(resize_overlay_,
                                             screen_dc,
                                             &destination,
                                             &size,
                                             surface_dc,
                                             &source_origin,
                                             0,
                                             &blend,
                                             ULW_ALPHA);

    SelectObject(surface_dc, previous_surface);
    DeleteObject(surface);
    DeleteDC(surface_dc);
    ReleaseDC(nullptr, screen_dc);

    BOOL shown = FALSE;
    if (updated != FALSE) {
        shown = SetWindowPos(resize_overlay_,
                             HWND_TOP,
                             0,
                             0,
                             0,
                             0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                                 SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }

    return updated != FALSE && shown != FALSE;
}

bool MainWindow::PrepareAdvancedResizePreview() {
    resize_advanced_preview_bitmap_.reset();
    resize_advanced_preview_width_ = 0;
    resize_advanced_preview_height_ = 0;
    resize_overlay_source_advanced_viewport_logical_ =
        advanced_viewport_height_logical_;

    if (CurrentLayoutPage() != layout::Page::Advanced ||
        advanced_scroll_content_host_ == nullptr ||
        IsWindow(advanced_scroll_content_host_) == FALSE ||
        IsWindowVisible(advanced_scroll_content_host_) == FALSE) {
        return false;
    }

    UniqueGdiObject snapshot;
    int snapshot_width = 0;
    int snapshot_height = 0;
    if (!CapturePrintedClient(advanced_scroll_content_host_,
                              snapshot,
                              snapshot_width,
                              snapshot_height) ||
        snapshot_width <= 0 || snapshot_height <= 0) {
        return false;
    }

    resize_advanced_preview_bitmap_ = std::move(snapshot);
    resize_advanced_preview_width_ = snapshot_width;
    resize_advanced_preview_height_ = snapshot_height;
    return true;
}

bool MainWindow::PaintAdvancedResizePreview(const HDC target,
                                            const int overlay_width,
                                            const int overlay_height,
                                            const int client_screen_top) noexcept {
    if (target == nullptr || overlay_width <= 0 || overlay_height <= 0 ||
        CurrentLayoutPage() != layout::Page::Advanced ||
        resize_advanced_preview_bitmap_.get() == nullptr ||
        resize_advanced_preview_width_ <= 0 ||
        resize_advanced_preview_height_ <= 0 ||
        move_cover_bitmap_.get() == nullptr || move_cover_width_ <= 0 ||
        move_cover_height_ <= 0) {
        return false;
    }

    const int source_viewport = layout::ClampAdvancedViewportHeight(
        resize_overlay_source_advanced_viewport_logical_);
    const int destination_viewport = layout::ClampAdvancedViewportHeight(
        resize_overlay_advanced_viewport_logical_);
    const layout::LayoutMetrics metrics{};
    const layout::MainLayout source_layout = layout::CalculateMainLayout(
        metrics,
        status_height_logical_,
        layout::Page::Advanced,
        source_viewport);
    const layout::MainLayout destination_layout = layout::CalculateMainLayout(
        metrics,
        status_height_logical_,
        layout::Page::Advanced,
        destination_viewport);

    const int host_width = Scale(layout::FixedContentWidth, dpi_);
    const int host_height = Scale(
        layout::ContentHeightForViewport(status_height_logical_,
                                         destination_viewport),
        dpi_);
    if (host_width <= 0 || host_height <= 0 ||
        resize_overlay_content_x_ < 0 || resize_overlay_content_y_ < 0 ||
        resize_overlay_content_x_ + host_width > overlay_width ||
        resize_overlay_content_y_ + host_height > overlay_height) {
        return false;
    }

    const HDC move_source = CreateCompatibleDC(target);
    const HDC advanced_source = CreateCompatibleDC(target);
    if (move_source == nullptr || advanced_source == nullptr) {
        if (advanced_source != nullptr) {
            DeleteDC(advanced_source);
        }
        if (move_source != nullptr) {
            DeleteDC(move_source);
        }
        return false;
    }
    const HGDIOBJ previous_move =
        SelectObject(move_source, move_cover_bitmap_.get());
    const HGDIOBJ previous_advanced =
        SelectObject(advanced_source, resize_advanced_preview_bitmap_.get());
    if (previous_move == nullptr || previous_move == HGDI_ERROR ||
        previous_advanced == nullptr || previous_advanced == HGDI_ERROR) {
        if (previous_advanced != nullptr && previous_advanced != HGDI_ERROR) {
            SelectObject(advanced_source, previous_advanced);
        }
        if (previous_move != nullptr && previous_move != HGDI_ERROR) {
            SelectObject(move_source, previous_move);
        }
        DeleteDC(advanced_source);
        DeleteDC(move_source);
        return false;
    }

    const int saved = SaveDC(target);
    if (saved == 0) {
        SelectObject(advanced_source, previous_advanced);
        SelectObject(move_source, previous_move);
        DeleteDC(advanced_source);
        DeleteDC(move_source);
        return false;
    }
    SetViewportOrgEx(target,
                     resize_overlay_content_x_,
                     resize_overlay_content_y_,
                     nullptr);
    IntersectClipRect(target, 0, 0, host_width, host_height);

    const RECT host_rect{0, 0, host_width, host_height};
    ui::Fill(target, host_rect, ui::Window);

    const auto& destination_page =
        destination_layout[layout::Control::AdvancedPageHost];
    const auto& source_page = source_layout[layout::Control::AdvancedPageHost];
    const int page_x = Scale(destination_page.x, dpi_);
    const int page_y = Scale(destination_page.y, dpi_);
    const int page_width = Scale(destination_page.width, dpi_);
    const int page_height = Scale(destination_page.height, dpi_);
    const int destination_footer_y = Scale(
        destination_page.y + destination_page.height, dpi_);

    // Most of the Advanced shell is overwritten later by the retained top
    // strip, cached page, scrollbar, or translated footer. Keep the exact
    // accepted GDI+ renderer, but clip it to only the narrow side gutters
    // that survive into the published resize frame. The host was just filled
    // with ui::Window, so no separate outside-corner fill is needed here.
    RECT shell = host_rect;
    shell.top += Scale(layout::TabDockTop, dpi_);
    const int shell_radius = Scale(14, dpi_);
    const auto& basic_tab = destination_layout[layout::Control::BasicTab];
    const auto& about_tab = destination_layout[layout::Control::AboutTab];
    const int dock_left = Scale(basic_tab.x, dpi_);
    const int dock_right = Scale(about_tab.x + about_tab.width, dpi_);

    const int shell_clip_saved = SaveDC(target);
    if (shell_clip_saved != 0) {
        // Preserve only the left / right shell gutters. The center page, top
        // dock strip, and footer are replaced later from retained sources.
        (void)ExcludeClipRect(target,
                              page_x,
                              page_y,
                              page_x + page_width,
                              destination_footer_y);
        (void)ExcludeClipRect(target, 0, 0, host_width, page_y);
        (void)ExcludeClipRect(target,
                              0,
                              destination_footer_y,
                              host_width,
                              host_height);
        ui::DrawDockedPageShell(target,
                                shell,
                                ui::WindowAlt,
                                ui::BorderSoft,
                                shell_radius,
                                dock_left,
                                dock_right);
        RestoreDC(target, shell_clip_saved);
    } else {
        // Defensive fallback to the accepted full shell draw.
        ui::DrawDockedPageShell(target,
                                shell,
                                ui::WindowAlt,
                                ui::BorderSoft,
                                shell_radius,
                                dock_left,
                                dock_right);
    }

    // Tabs and the dock junction never change during a vertical Advanced
    // resize. Copy that verified top strip from the stable retained content
    // frame instead of asking the live hierarchy to repaint.
    const int top_copy_height = std::min(
        {page_y, move_cover_height_, host_height});
    const int top_copy_width = std::min(host_width, move_cover_width_);
    if (top_copy_width > 0 && top_copy_height > 0) {
        BitBlt(target,
               0,
               0,
               top_copy_width,
               top_copy_height,
               move_source,
               0,
               0,
               SRCCOPY);
    }

    const int preview_maximum_scroll = std::max(
        0, layout::AdvancedPageHeight - destination_viewport);
    const int preview_scroll_offset = std::clamp(
        advanced_scroll_offset_logical_, 0, preview_maximum_scroll);
    int advanced_source_y = std::clamp(
        Scale(preview_scroll_offset, dpi_),
        0,
        resize_advanced_preview_height_);

    // At non-integral DPI scales, repeatedly converting the shrinking logical
    // bottom-scroll maximum back to physical pixels can alternate by one
    // physical pixel around the exact top-edge resize position. The retained
    // page then appears to tremble even though the window edge itself moves
    // smoothly. For the exact bottom-anchored top-resize route selected when
    // the overlay was created, preserve the starting content screen anchor and
    // derive the crop directly in physical pixels while the viewport is still
    // growing through the original bottom position. Ordinary scrolling, other
    // resize directions, and non-bottom Advanced resizes keep the established
    // logical-scroll calculation above.
    if (resize_advanced_physical_bottom_anchor_active_ &&
        preview_maximum_scroll > 0 &&
        preview_maximum_scroll <= advanced_scroll_offset_logical_) {
        const int anchored_source_y =
            client_screen_top + resize_overlay_content_y_ + page_y -
            resize_advanced_physical_bottom_anchor_screen_y_;
        advanced_source_y = std::clamp(
            anchored_source_y, 0, resize_advanced_preview_height_);
    }
    const int advanced_copy_width = std::min(
        page_width, resize_advanced_preview_width_);
    const int advanced_copy_height = std::min(
        page_height,
        resize_advanced_preview_height_ - advanced_source_y);
    if (advanced_copy_width > 0 && advanced_copy_height > 0) {
        BitBlt(target,
               page_x,
               page_y,
               advanced_copy_width,
               advanced_copy_height,
               advanced_source,
               0,
               advanced_source_y,
               SRCCOPY);
    }

    // Recreate the narrow Advanced scrollbar from the proposed viewport. This
    // is GDI-only geometry and never touches the live child hierarchy while
    // the user is holding the resize edge.
    if (preview_maximum_scroll > 0) {
        const int scrollbar_width = Scale(layout::AdvancedScrollbarWidth, dpi_);
        const int scrollbar_x = page_x + page_width +
            Scale(layout::AdvancedScrollbarRightInset, dpi_);
        const int scrollbar_y = page_y +
            Scale(layout::AdvancedScrollbarTopInset, dpi_);
        const int scrollbar_height = std::max(
            1,
            page_height - Scale(layout::AdvancedScrollbarTopInset, dpi_) -
                Scale(layout::AdvancedScrollbarBottomInset, dpi_));
        const RECT scrollbar{
            scrollbar_x,
            scrollbar_y,
            scrollbar_x + scrollbar_width,
            scrollbar_y + scrollbar_height};
        ui::Fill(target, scrollbar, ui::WindowAlt);

        RECT track = scrollbar;
        const int horizontal_inset = std::max(1, Scale(2, dpi_));
        const int vertical_inset = std::max(1, Scale(1, dpi_));
        track.left += horizontal_inset;
        track.right = std::max(track.left + 1, track.right - horizontal_inset);
        track.top += vertical_inset;
        track.bottom = std::max(track.top + 1, track.bottom - vertical_inset);
        ui::DrawRoundedPanel(
            target,
            track,
            ui::ScrollbarTrack,
            ui::ScrollbarTrack,
            std::max(1, static_cast<int>((track.right - track.left) / 2)));

        const int track_height = track.bottom - track.top;
        const int minimum_thumb = std::max(1, Scale(36, dpi_));
        const int proportional_thumb = static_cast<int>(
            (static_cast<long long>(track_height) * destination_viewport) /
            layout::AdvancedPageHeight);
        const int thumb_height = std::clamp(
            proportional_thumb,
            std::min(minimum_thumb, track_height),
            track_height);
        const int travel = track_height - thumb_height;
        const int thumb_top =
            preview_maximum_scroll > 0 && travel > 0
                ? track.top + static_cast<int>(
                      (static_cast<long long>(travel) * preview_scroll_offset +
                       (preview_maximum_scroll / 2)) /
                      preview_maximum_scroll)
                : track.top;
        const int available_width = std::max(
            1, static_cast<int>(scrollbar.right - scrollbar.left));
        const int thumb_width = std::min(
            std::max(1,
                     Scale(ui::ScrollbarThumbLogicalWidth(
                               ui::ScrollbarVisualState::Idle),
                           dpi_)),
            available_width);
        const int thumb_left =
            scrollbar.left + (available_width - thumb_width) / 2;
        const RECT thumb{thumb_left,
                         thumb_top,
                         thumb_left + thumb_width,
                         thumb_top + thumb_height};
        ui::DrawRoundedPanel(
            target,
            thumb,
            ui::ScrollbarIdle,
            ui::ScrollbarIdle,
            std::max(1, static_cast<int>((thumb.right - thumb.left) / 2)));
    }

    // Everything below the Advanced viewport is translated by the viewport
    // height delta. Copy the stable footer as one unit so status, diagnostics,
    // buttons, and the bottom shell move in real time without repainting or
    // remeasuring those controls during the drag.
    const int source_footer_y = Scale(
        source_page.y + source_page.height, dpi_);
    const int footer_copy_width = std::min(host_width, move_cover_width_);
    const int footer_copy_height = std::min(
        move_cover_height_ - source_footer_y,
        host_height - destination_footer_y);
    if (footer_copy_width > 0 && footer_copy_height > 0) {
        BitBlt(target,
               0,
               destination_footer_y,
               footer_copy_width,
               footer_copy_height,
               move_source,
               0,
               source_footer_y,
               SRCCOPY);
    }

    RestoreDC(target, saved);
    SelectObject(advanced_source, previous_advanced);
    SelectObject(move_source, previous_move);
    DeleteDC(advanced_source);
    DeleteDC(move_source);
    return true;
}

void MainWindow::ClearResizePreviewState() noexcept {
    resize_advanced_preview_bitmap_.reset();
    resize_advanced_preview_width_ = 0;
    resize_advanced_preview_height_ = 0;
    resize_overlay_advanced_viewport_logical_ = layout::BasicPageHeight;
    resize_overlay_source_advanced_viewport_logical_ = layout::BasicPageHeight;
    resize_advanced_physical_bottom_anchor_active_ = false;
    resize_advanced_physical_bottom_anchor_screen_y_ = 0;
}

void MainWindow::FinishResizeOverlay() noexcept {
    if (resize_overlay_ == nullptr ||
        IsWindow(resize_overlay_) == FALSE) {
        HideResizeOverlayGuard();
        resize_overlay_ = nullptr;
        programmatic_resize_overlay_ = false;
        resize_overlay_finish_posted_ = false;
        return;
    }

    // The retained surface covers the complete client area, so the real
    // fixed-size child hierarchy can move and repaint once underneath it
    // without exposing clipped controls or a progressive redraw.
    PositionContentHost(false);

    if (move_cover_ != nullptr && IsWindow(move_cover_) != FALSE) {
        SetWindowPos(move_cover_,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOREDRAW);
        DestroyWindow(move_cover_);
    }
    move_cover_ = nullptr;

    // The ordinary guard child participates in sibling clipping. Remove it
    // while the retained layered child still covers the complete client,
    // then repaint the real hierarchy underneath the retained surface. In
    // Repainting after the guard is removed prevents the guard-colored client
    // from remaining visible when both temporary resize children are retired.
    HideResizeOverlayGuard();
    (void)RedrawWindow(
        window_,
        nullptr,
        nullptr,
        RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    (void)DwmFlush();
    HideResizeOverlay();
    // Wait until the independent surface is no longer part of the composed
    // desktop before refreshing the retained movement frame. This prevents a
    // page or resize transition from becoming embedded in its own cache.
    (void)DwmFlush();
    QueueMoveCoverRefresh();
}

void MainWindow::HideResizeOverlay() noexcept {
    // Remove the static underlay while the retained layered child still covers
    // the client, then retire the retained child itself. This prevents a
    // release-time flash of the guard over the freshly repainted live UI.
    HideResizeOverlayGuard();

    if (resize_overlay_ != nullptr &&
        IsWindow(resize_overlay_) != FALSE) {
        SetWindowPos(resize_overlay_,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOREDRAW);
        DestroyWindow(resize_overlay_);
    }
    resize_overlay_ = nullptr;
    resize_overlay_child_clipped_ = false;
    resize_overlay_content_x_ = 0;
    resize_overlay_content_y_ = 0;
    resize_overlay_screen_rect_ = RECT{};
    resize_overlay_geometry_valid_ = false;
    ClearResizePreviewState();
    programmatic_resize_overlay_ = false;
    resize_overlay_finish_posted_ = false;
}

void MainWindow::PositionContentHost(const bool clean_repaint) {
    if (content_host_ == nullptr || !IsWindow(content_host_)) {
        return;
    }

    RECT client{};
    if (GetClientRect(window_, &client) == FALSE) {
        return;
    }

    const int previous_viewport = advanced_viewport_height_logical_;
    advanced_viewport_height_logical_ = CurrentAdvancedViewportHeightLogical();
    advanced_scroll_offset_logical_ = std::clamp(
        advanced_scroll_offset_logical_, 0, AdvancedMaximumScrollLogical());
    const bool viewport_changed =
        previous_viewport != advanced_viewport_height_logical_;

    const layout::Rect host = layout::CalculateCenteredContentHost(
        static_cast<int>(client.right - client.left),
        static_cast<int>(client.bottom - client.top),
        dpi_,
        status_height_logical_,
        CurrentLayoutPage(),
        advanced_viewport_height_logical_);

    RECT old_rect{};
    GetWindowRect(content_host_, &old_rect);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&old_rect), 2);

    const bool unchanged = old_rect.left == host.x &&
                           old_rect.top == host.y &&
                           (old_rect.right - old_rect.left) == host.width &&
                           (old_rect.bottom - old_rect.top) == host.height;
    if (!unchanged) {
        const UINT flags =
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_NOCOPYBITS |
            ((!clean_repaint || !IsWindowVisible(content_host_))
                 ? 0U
                 : SWP_NOREDRAW);
        SetWindowPos(content_host_,
                     nullptr,
                     host.x,
                     host.y,
                     host.width,
                     host.height,
                     flags);
        if (clean_repaint && IsWindowVisible(content_host_)) {
            InvalidateRect(window_, &old_rect, FALSE);
            InvalidateRect(content_host_, nullptr, FALSE);
        }
    }

    if (viewport_changed) {
        LayoutControls(false);
    } else {
        UpdateAdvancedScrollbar(false);
    }
}

int MainWindow::CurrentAdvancedViewportHeightLogical() const noexcept {
    if (CurrentLayoutPage() != layout::Page::Advanced || window_ == nullptr ||
        IsWindow(window_) == FALSE) {
        return layout::BasicPageHeight;
    }

    RECT client{};
    if (GetClientRect(window_, &client) == FALSE) {
        return layout::BasicPageHeight;
    }
    return layout::CalculateAdvancedViewportHeight(
        client.bottom - client.top,
        dpi_,
        status_height_logical_);
}

int MainWindow::AdvancedMaximumScrollLogical() const noexcept {
    return std::max(
        0,
        layout::AdvancedPageHeight - advanced_viewport_height_logical_);
}

RECT MainWindow::AdvancedScrollbarTrackRect() const noexcept {
    RECT client{};
    if (advanced_scrollbar_ == nullptr ||
        IsWindow(advanced_scrollbar_) == FALSE ||
        GetClientRect(advanced_scrollbar_, &client) == FALSE) {
        return {};
    }

    const int horizontal_inset = std::max(1, Scale(2, dpi_));
    const int vertical_inset = std::max(1, Scale(1, dpi_));
    client.left += horizontal_inset;
    client.right = std::max(client.left + 1, client.right - horizontal_inset);
    client.top += vertical_inset;
    client.bottom = std::max(client.top + 1, client.bottom - vertical_inset);
    return client;
}

RECT MainWindow::AdvancedScrollbarThumbRect(
    const bool stable_snapshot) const noexcept {
    const RECT track = AdvancedScrollbarTrackRect();
    const int track_height = track.bottom - track.top;
    if (track_height <= 0) {
        return track;
    }

    const int minimum_thumb = std::max(1, Scale(36, dpi_));
    const int proportional_thumb = static_cast<int>(
        (static_cast<long long>(track_height) *
         advanced_viewport_height_logical_) /
        layout::AdvancedPageHeight);
    const int thumb_height = std::clamp(
        proportional_thumb,
        std::min(minimum_thumb, track_height),
        track_height);
    const int travel = track_height - thumb_height;
    const int maximum = AdvancedMaximumScrollLogical();
    const int thumb_top =
        maximum > 0 && travel > 0
            ? track.top + static_cast<int>(
                              (static_cast<long long>(travel) *
                                   advanced_scroll_offset_logical_ +
                               (maximum / 2)) /
                              maximum)
            : track.top;
    RECT client{};
    if (advanced_scrollbar_ == nullptr ||
        IsWindow(advanced_scrollbar_) == FALSE ||
        GetClientRect(advanced_scrollbar_, &client) == FALSE) {
        return {track.left, thumb_top, track.right, thumb_top + thumb_height};
    }
    const ui::ScrollbarVisualState scrollbar_state =
        stable_snapshot
            ? ui::ScrollbarVisualState::Idle
            : ui::ScrollbarState(advanced_scroll_hovered_,
                                 advanced_scroll_dragging_);
    const int desired_width = std::max(
        1,
        Scale(ui::ScrollbarThumbLogicalWidth(scrollbar_state), dpi_));
    const int available_width = std::max(
        1,
        static_cast<int>(client.right - client.left));
    const int thumb_width = std::min(desired_width, available_width);
    const int thumb_left = client.left + (available_width - thumb_width) / 2;
    return {thumb_left,
            thumb_top,
            thumb_left + thumb_width,
            thumb_top + thumb_height};
}

bool MainWindow::BeginAdvancedScrollSnapshot() {
    if (advanced_scroll_snapshot_active_ &&
        advanced_scroll_snapshot_bitmap_.get() != nullptr &&
        advanced_scroll_snapshot_ != nullptr &&
        IsWindow(advanced_scroll_snapshot_) != FALSE) {
        return true;
    }

    HideAdvancedScrollSnapshot();
    if (CurrentLayoutPage() != layout::Page::Advanced ||
        advanced_page_host_ == nullptr ||
        IsWindow(advanced_page_host_) == FALSE ||
        IsWindowVisible(advanced_page_host_) == FALSE ||
        advanced_scroll_content_host_ == nullptr ||
        IsWindow(advanced_scroll_content_host_) == FALSE ||
        IsWindowVisible(advanced_scroll_content_host_) == FALSE) {
        return false;
    }

    UniqueGdiObject snapshot;
    int snapshot_width = 0;
    int snapshot_height = 0;
    if (!CapturePrintedClient(advanced_scroll_content_host_,
                              snapshot,
                              snapshot_width,
                              snapshot_height) ||
        snapshot_width <= 0 || snapshot_height <= 0) {
        return false;
    }

    if (advanced_scroll_snapshot_ == nullptr ||
        IsWindow(advanced_scroll_snapshot_) == FALSE) {
        advanced_scroll_snapshot_ = CreateWindowExW(
            WS_EX_NOPARENTNOTIFY,
            AdvancedScrollSnapshotClassName,
            L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            0,
            0,
            1,
            1,
            advanced_page_host_,
            nullptr,
            instance_,
            this);
    }
    if (advanced_scroll_snapshot_ == nullptr ||
        IsWindow(advanced_scroll_snapshot_) == FALSE) {
        advanced_scroll_snapshot_ = nullptr;
        return false;
    }

    advanced_scroll_snapshot_bitmap_ = std::move(snapshot);
    advanced_scroll_snapshot_width_ = snapshot_width;
    advanced_scroll_snapshot_height_ = snapshot_height;
    advanced_scroll_snapshot_active_ = true;
    UpdateAdvancedScrollSnapshot();
    return true;
}

void MainWindow::UpdateAdvancedScrollSnapshot() noexcept {
    if (!advanced_scroll_snapshot_active_ ||
        advanced_scroll_snapshot_ == nullptr ||
        IsWindow(advanced_scroll_snapshot_) == FALSE ||
        advanced_page_host_ == nullptr ||
        IsWindow(advanced_page_host_) == FALSE) {
        return;
    }

    RECT client{};
    if (GetClientRect(advanced_page_host_, &client) == FALSE) {
        return;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    SetWindowPos(advanced_scroll_snapshot_,
                 HWND_TOP,
                 0,
                 0,
                 width,
                 height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    RedrawWindow(advanced_scroll_snapshot_,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
}

void MainWindow::FinishAdvancedScrollSnapshot(
    const bool refresh_move_cover) noexcept {
    StopGeneratedTimer(advanced_scroll_settle_timer_id_);
    if (!advanced_scroll_snapshot_active_) {
        return;
    }

    // The live content host has already moved to the same offset shown by the
    // retained frame. Repaint its newly visible range synchronously while the
    // snapshot still conceals the independent child-window paint sequence.
    RECT visible_content{};
    if (advanced_scroll_content_host_ != nullptr &&
        IsWindow(advanced_scroll_content_host_) != FALSE &&
        GetClientRect(advanced_scroll_content_host_, &visible_content) != FALSE) {
        const int visible_top = Scale(advanced_scroll_offset_logical_, dpi_);
        const int visible_bottom = Scale(
            advanced_scroll_offset_logical_ +
                advanced_viewport_height_logical_,
            dpi_);
        visible_content.top = std::max(
            static_cast<LONG>(0),
            static_cast<LONG>(visible_top - 1));
        visible_content.bottom = std::min(
            visible_content.bottom,
            static_cast<LONG>(visible_bottom + 1));
        RedrawWindow(advanced_scroll_content_host_,
                     &visible_content,
                     nullptr,
                     RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE |
                         RDW_UPDATENOW);
    }
    if (advanced_page_host_ != nullptr &&
        IsWindow(advanced_page_host_) != FALSE) {
        RedrawWindow(advanced_page_host_,
                     nullptr,
                     nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    }
    GdiFlush();
    (void)DwmFlush();

    HideAdvancedScrollSnapshot();
    if (refresh_move_cover) {
        QueueMoveCoverRefresh();
    }
}

void MainWindow::HideAdvancedScrollSnapshot() noexcept {
    if (advanced_scroll_snapshot_ != nullptr &&
        IsWindow(advanced_scroll_snapshot_) != FALSE) {
        SetWindowPos(advanced_scroll_snapshot_,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_NOOWNERZORDER | SWP_NOREDRAW | SWP_NOZORDER |
                         SWP_HIDEWINDOW);
    }
    advanced_scroll_snapshot_active_ = false;
    advanced_scroll_snapshot_bitmap_.reset();
    advanced_scroll_snapshot_width_ = 0;
    advanced_scroll_snapshot_height_ = 0;
}

bool MainWindow::SetAdvancedScrollOffset(const int offset_logical,
                                         const bool refresh_move_cover) {
    const int clamped = std::clamp(
        offset_logical, 0, AdvancedMaximumScrollLogical());
    if (clamped == advanced_scroll_offset_logical_) {
        return false;
    }

    // Page-wheel scrolling dismisses an application-owned combo popup.
    // Clear the combo's keyboard focus synchronously before the retained
    // Advanced-scroll frame is captured. CBN_CLOSEUP intentionally posts the
    // ordinary transient-focus clear, but that message cannot run until this
    // wheel turn returns; capturing first would therefore preserve the closed
    // combo's blue focused border in the snapshot until scrolling settles.
    const HWND dismissed_combo = combo_popup_owner_;
    if (dismissed_combo != nullptr && IsWindow(dismissed_combo) != FALSE &&
        GetFocus() == dismissed_combo && window_ != nullptr &&
        IsWindow(window_) != FALSE) {
        SetFocus(window_);
    }
    CloseComboPopup(false);
    (void)tooltips_.DismissAll();
    const bool retained = BeginAdvancedScrollSnapshot();
    advanced_scroll_offset_logical_ = clamped;
    if (advanced_scroll_content_host_ != nullptr &&
        IsWindow(advanced_scroll_content_host_) != FALSE) {
        SetWindowPos(advanced_scroll_content_host_,
                     nullptr,
                     0,
                     -Scale(advanced_scroll_offset_logical_, dpi_),
                     0,
                     0,
                     SWP_NOSIZE | SWP_NOACTIVATE |
                         (retained ? SWP_NOCOPYBITS | SWP_NOREDRAW : 0U) |
                         SWP_NOOWNERZORDER | SWP_NOZORDER);
    }

    if (retained) {
        // Scrolling publishes only one retained bitmap. The live descendant
        // hierarchy moves to matching hit-test coordinates underneath it and
        // is repainted once when wheel input settles or thumb dragging ends.
        UpdateAdvancedScrollSnapshot();
        if (!advanced_scroll_dragging_ && window_ != nullptr &&
            IsWindow(window_) != FALSE) {
            if (StartGeneratedTimer(advanced_scroll_settle_timer_id_,
                                    AdvancedScrollSettleMilliseconds) == 0) {
                FinishAdvancedScrollSnapshot(refresh_move_cover);
            }
        }
    } else {
        // Preserve the established repaint path as a fail-safe when an
        // application-owned full-page snapshot cannot be created.
        RECT visible_content{};
        if (advanced_scroll_content_host_ != nullptr &&
            GetClientRect(advanced_scroll_content_host_, &visible_content) != FALSE) {
            const int visible_top =
                Scale(advanced_scroll_offset_logical_, dpi_);
            const int visible_bottom = Scale(
                advanced_scroll_offset_logical_ +
                    advanced_viewport_height_logical_,
                dpi_);
            visible_content.top = std::max(
                static_cast<LONG>(0),
                static_cast<LONG>(visible_top - 1));
            visible_content.bottom = std::min(
                visible_content.bottom,
                static_cast<LONG>(visible_bottom + 1));
            RedrawWindow(advanced_scroll_content_host_,
                         &visible_content,
                         nullptr,
                         RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE);
        }
        if (refresh_move_cover) {
            QueueMoveCoverRefresh();
        }
    }

    RedrawWindow(advanced_scrollbar_,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    move_cover_advanced_scroll_offset_ = -1;
    return true;
}

void MainWindow::ScrollAdvancedBy(const int delta_logical) {
    (void)SetAdvancedScrollOffset(
        advanced_scroll_offset_logical_ + delta_logical);
}

bool MainWindow::ScrollAdvancedWheel(const WPARAM w_param,
                                     const LPARAM l_param) {
    if (CurrentLayoutPage() != layout::Page::Advanced ||
        AdvancedMaximumScrollLogical() <= 0 ||
        advanced_page_host_ == nullptr ||
        IsWindowVisible(advanced_page_host_) == FALSE) {
        return false;
    }

    POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    RECT page_bounds{};
    if (GetWindowRect(advanced_page_host_, &page_bounds) == FALSE ||
        PtInRect(&page_bounds, point) == FALSE) {
        return false;
    }

    advanced_scroll_wheel_remainder_ += GET_WHEEL_DELTA_WPARAM(w_param);
    const int notches = advanced_scroll_wheel_remainder_ / WHEEL_DELTA;
    advanced_scroll_wheel_remainder_ %= WHEEL_DELTA;
    if (notches == 0) {
        return true;
    }

    UINT lines = 3;
    (void)SystemParametersInfoW(
        SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const int delta =
        lines == WHEEL_PAGESCROLL
            ? std::max(1, (advanced_viewport_height_logical_ * 4) / 5)
            : std::max(1, static_cast<int>(lines) * 16);
    ScrollAdvancedBy(-notches * delta);
    return true;
}

void MainWindow::EnsureAdvancedControlVisible(const HWND control) {
    if (CurrentLayoutPage() != layout::Page::Advanced || control == nullptr ||
        advanced_page_host_ == nullptr ||
        IsChild(advanced_page_host_, control) == FALSE ||
        AdvancedMaximumScrollLogical() <= 0) {
        return;
    }

    RECT bounds{};
    RECT viewport{};
    if (GetWindowRect(control, &bounds) == FALSE ||
        GetClientRect(advanced_page_host_, &viewport) == FALSE) {
        return;
    }
    MapWindowPoints(HWND_DESKTOP,
                    advanced_page_host_,
                    reinterpret_cast<POINT*>(&bounds),
                    2);
    const int margin = Scale(8, dpi_);
    int delta_pixels = 0;
    if (bounds.top < viewport.top + margin) {
        delta_pixels = bounds.top - (viewport.top + margin);
    } else if (bounds.bottom > viewport.bottom - margin) {
        delta_pixels = bounds.bottom - (viewport.bottom - margin);
    }
    if (delta_pixels == 0) {
        return;
    }

    // The child / viewport rectangles are in physical pixels while the
    // Advanced scroll offset is stored in 96-DPI logical units. Round away
    // from zero so a focused edge is not left clipped by scaling. Keyboard
    // focus navigation must also publish the new viewport immediately; a
    // retained scroll snapshot would otherwise conceal newly focused controls
    // until its settle timer expires.
    const long long magnitude =
        delta_pixels < 0 ? -static_cast<long long>(delta_pixels)
                         : static_cast<long long>(delta_pixels);
    const int logical_magnitude = static_cast<int>(
        (magnitude * 96LL + static_cast<long long>(dpi_) - 1LL) /
        static_cast<long long>(dpi_));
    const int delta_logical =
        delta_pixels < 0 ? -logical_magnitude : logical_magnitude;
    if (SetAdvancedScrollOffset(advanced_scroll_offset_logical_ + delta_logical)) {
        FinishAdvancedScrollSnapshot(true);
    }
}

void MainWindow::UpdateAdvancedScrollbar(const bool redraw) noexcept {
    if (advanced_scrollbar_ == nullptr ||
        IsWindow(advanced_scrollbar_) == FALSE ||
        advanced_page_host_ == nullptr ||
        IsWindow(advanced_page_host_) == FALSE || content_host_ == nullptr ||
        IsWindow(content_host_) == FALSE) {
        return;
    }

    RECT page_bounds{};
    if (GetWindowRect(advanced_page_host_, &page_bounds) == FALSE) {
        return;
    }
    MapWindowPoints(HWND_DESKTOP,
                    content_host_,
                    reinterpret_cast<POINT*>(&page_bounds),
                    2);
    const int width = Scale(layout::AdvancedScrollbarWidth, dpi_);
    const int right_inset = Scale(layout::AdvancedScrollbarRightInset, dpi_);
    const int top_inset = Scale(layout::AdvancedScrollbarTopInset, dpi_);
    const int bottom_inset = Scale(layout::AdvancedScrollbarBottomInset, dpi_);
    const int height = std::max(
        1,
        static_cast<int>(page_bounds.bottom - page_bounds.top) - top_inset -
            bottom_inset);
    const bool visible = CurrentLayoutPage() == layout::Page::Advanced &&
                         AdvancedMaximumScrollLogical() > 0;

    if (!visible && advanced_scroll_dragging_) {
        advanced_scroll_dragging_ = false;
        if (GetCapture() == advanced_scrollbar_) {
            ReleaseCapture();
        }
    }
    if (!visible) {
        advanced_scroll_hovered_ = false;
    }

    SetWindowPos(advanced_scrollbar_,
                 HWND_TOP,
                 static_cast<int>(page_bounds.right) + right_inset,
                 static_cast<int>(page_bounds.top) + top_inset,
                 width,
                 height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     (redraw ? 0U : SWP_NOREDRAW) |
                     (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (redraw && visible) {
        InvalidateRect(advanced_scrollbar_, nullptr, FALSE);
    }
}

void MainWindow::Relayout(const bool redraw) {
    LayoutControls(redraw);
}

layout::Page MainWindow::CurrentLayoutPage() const noexcept {
    if (selected_page_index_ == 1) {
        return layout::Page::Advanced;
    }
    if (selected_page_index_ == 2) {
        return layout::Page::About;
    }
    return layout::Page::Basic;
}

int MainWindow::CurrentContentHeightLogical() const noexcept {
    return layout::ContentHeightForViewport(
        status_height_logical_,
        layout::VisiblePageHeight(
            CurrentLayoutPage(), advanced_viewport_height_logical_));
}

int MainWindow::MeasureStatusHeightLogical() const {
    if (status_text_ == nullptr || diagnostics_text_ == nullptr ||
        IsWindow(status_text_) == FALSE || IsWindow(diagnostics_text_) == FALSE) {
        return layout::MinimumStatusHeight;
    }

    const bool diagnostics_visible = diagnostics_check_ != nullptr &&
                                     IsChecked(diagnostics_check_);
    const HWND text_control = status_text_;
    std::wstring text =
        diagnostics_visible
            ? (!last_diagnostics_text_.empty()
                   ? last_diagnostics_text_
                   : GetControlText(diagnostics_text_))
            : GetControlText(status_text_);
    if (!diagnostics_visible) {
        constexpr std::wstring_view prefix = L"Status: ";
        if (text.starts_with(prefix)) {
            text.erase(0, prefix.size());
        }
    }
    if (text.empty()) {
        return layout::MinimumStatusHeight;
    }

    const HDC dc = GetDC(text_control);
    if (dc == nullptr) {
        return layout::MinimumStatusHeight;
    }

    const HFONT text_font = reinterpret_cast<HFONT>(
        font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));
    const HGDIOBJ old_font = SelectObject(dc, text_font);

    constexpr int status_panel_width = layout::StatusPanelWidth;
    const int available_width_logical = diagnostics_visible
        ? status_panel_width - 36 - 8
        : status_panel_width - 42 - 74 - 8 - 16;
    RECT measured{
        0,
        0,
        Scale(std::max(1, available_width_logical), dpi_),
        0,
    };
    DrawTextW(dc,
              text.c_str(),
              -1,
              &measured,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, old_font);
    ReleaseDC(text_control, dc);

    const int vertical_padding = diagnostics_visible
                                     ? DiagnosticsTextVerticalPadding
                                     : StatusTextVerticalPadding;
    const int desired_pixels =
        std::max(Scale(layout::MinimumStatusHeight, dpi_),
                 static_cast<int>(measured.bottom - measured.top) + Scale(vertical_padding, dpi_));
    int desired_logical = std::max(
        layout::MinimumStatusHeight,
        (desired_pixels * 96 + static_cast<int>(dpi_) - 1) /
            static_cast<int>(dpi_));

    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
        const int work_height = monitor_info.rcWork.bottom - monitor_info.rcWork.top;
        const int fixed_outer_without_status =
            Scale(layout::BaseOuterHeight(CurrentLayoutPage()) - layout::MinimumStatusHeight, dpi_);
        const int maximum_status_pixels =
            std::max(Scale(layout::MinimumStatusHeight, dpi_),
                     work_height - fixed_outer_without_status);
        const int maximum_status_logical = std::max(
            layout::MinimumStatusHeight,
            (maximum_status_pixels * 96) / static_cast<int>(dpi_));
        desired_logical = std::min(desired_logical, maximum_status_logical);
    }

    return desired_logical;
}

void MainWindow::UpdateStatusAreaLayout(const bool resize_window) {
    if (status_layout_update_in_progress_ || content_host_ == nullptr ||
        IsWindow(content_host_) == FALSE) {
        return;
    }

    const int desired_height = MeasureStatusHeightLogical();
    if (desired_height == status_height_logical_) {
        return;
    }

    const auto window_rect_in = [](const HWND source,
                                   const HWND target,
                                   RECT& rectangle) noexcept {
        if (source == nullptr || IsWindow(source) == FALSE ||
            target == nullptr || IsWindow(target) == FALSE ||
            GetWindowRect(source, &rectangle) == FALSE) {
            rectangle = {};
            return false;
        }
        MapWindowPoints(nullptr,
                        target,
                        reinterpret_cast<POINT*>(&rectangle),
                        2);
        return true;
    };

    RECT old_host_rect{};
    const bool old_host_valid =
        GetWindowRect(content_host_, &old_host_rect) != FALSE;
    if (old_host_valid) {
        MapWindowPoints(nullptr,
                        window_,
                        reinterpret_cast<POINT*>(&old_host_rect),
                        2);
    }

    // Capture the old status and action-button footprints before any
    // SWP_NOREDRAW geometry change. In a manually enlarged Advanced window,
    // the Start button can remain geometrically unchanged while a broad footer
    // repair still intersects it. Use these footprints to repaint action
    // buttons only when their geometry actually changes.
    RECT old_status_panel_rect{};
    RECT old_start_button_rect{};
    RECT old_stop_button_rect{};
    RECT old_emergency_button_rect{};
    const bool old_status_panel_valid =
        window_rect_in(status_text_, window_, old_status_panel_rect);
    const bool old_start_button_valid =
        window_rect_in(start_button_, window_, old_start_button_rect);
    const bool old_stop_button_valid =
        window_rect_in(stop_button_, window_, old_stop_button_rect);
    const bool old_emergency_button_valid =
        window_rect_in(emergency_button_, window_, old_emergency_button_rect);

    // A status-driven outer-window growth can also move the top edge upward
    // to remain inside the monitor work area. Because this transaction uses
    // SWP_NOREDRAW, those newly exposed client pixels are not guaranteed to
    // receive WM_PAINT. Capture the old composed client footprint now and
    // repaint only the new-minus-old screen area after the child hierarchy has
    // reached its final geometry.
    RECT old_window_screen{};
    RECT new_window_screen{};
    RECT old_client_screen{};
    RECT new_client_screen{};
    bool old_window_valid = false;
    bool new_window_valid = false;
    bool old_client_screen_valid = false;
    bool new_client_screen_valid = false;
    bool outer_geometry_changed = false;
    bool outer_window_moved = false;
    bool outer_set_window_pos_succeeded = false;
    bool old_window_vertically_inside_work_area = false;
    bool outer_exposure_region_failed = false;
    HRGN outer_exposed_client_region = nullptr;

    status_layout_update_in_progress_ = true;
    const int old_height = status_height_logical_;
    status_height_logical_ = desired_height;

    if (resize_window && window_ != nullptr && IsWindow(window_) != FALSE &&
        IsIconic(window_) == FALSE && IsZoomed(window_) == FALSE) {
        RECT window_rect{};
        if (GetWindowRect(window_, &window_rect) != FALSE) {
            old_window_screen = window_rect;
            old_window_valid = true;
            old_client_screen_valid =
                ClientRectInScreen(window_, old_client_screen);

            const int current_height = window_rect.bottom - window_rect.top;
            const int required_height =
                Scale(layout::MinimumOuterHeight(status_height_logical_, CurrentLayoutPage()), dpi_);
            int new_height = current_height;

            if (current_height < required_height) {
                const int growth_pixels = required_height - current_height;
                new_height = required_height;
                const int growth_logical =
                    (growth_pixels * 96 + static_cast<int>(dpi_) - 1) /
                    static_cast<int>(dpi_);
                automatic_status_window_growth_logical_ += growth_logical;
            } else if (desired_height < old_height &&
                       automatic_status_window_growth_logical_ > 0) {
                const int shrink_logical = std::min(
                    old_height - desired_height,
                    automatic_status_window_growth_logical_);
                new_height = std::max(
                    required_height,
                    current_height - Scale(shrink_logical, dpi_));
                automatic_status_window_growth_logical_ -= shrink_logical;
            }

            if (new_height != current_height) {
                int new_top = window_rect.top;
                MONITORINFO monitor_info{};
                monitor_info.cbSize = sizeof(monitor_info);
                const HMONITOR monitor = MonitorFromRect(
                    &window_rect, MONITOR_DEFAULTTONEAREST);
                if (monitor != nullptr &&
                    GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
                    const int work_top =
                        static_cast<int>(monitor_info.rcWork.top);
                    const int work_bottom =
                        static_cast<int>(monitor_info.rcWork.bottom);
                    const int work_height = work_bottom - work_top;
                    old_window_vertically_inside_work_area =
                        window_rect.top >= work_top &&
                        window_rect.bottom <= work_bottom;
                    new_height = std::min(new_height, work_height);

                    if (old_window_vertically_inside_work_area) {
                        // If Vector Click was already fully inside the work
                        // area, move it only by the amount required to keep
                        // the status-driven size change inside that area.
                        // This retains the established visibility safeguard
                        // for ordinary on-screen use.
                        if (new_top + new_height > work_bottom) {
                            new_top = work_bottom - new_height;
                        }
                        new_top = std::max(new_top, work_top);
                    } else {
                        // A restored window can be intentionally parked partly
                        // outside the monitor work area. A small status-only
                        // height change should not reinterpret that existing
                        // placement as a request to snap the complete window
                        // back on-screen. Preserve the user's top position; the
                        // moved-host repair remains available if another
                        // constraint actually changes the position.
                    }
                }

                outer_set_window_pos_succeeded =
                    SetWindowPos(window_,
                                 nullptr,
                                 window_rect.left,
                                 new_top,
                                 window_rect.right - window_rect.left,
                                 new_height,
                                 SWP_NOZORDER | SWP_NOACTIVATE |
                                     SWP_NOOWNERZORDER | SWP_NOREDRAW) != FALSE;

                if (outer_set_window_pos_succeeded &&
                    GetWindowRect(window_, &new_window_screen) != FALSE) {
                    new_window_valid = true;
                    outer_geometry_changed =
                        !old_window_valid ||
                        EqualRect(&old_window_screen,
                                  &new_window_screen) == FALSE;
                    outer_window_moved =
                        old_window_valid &&
                        (old_window_screen.left != new_window_screen.left ||
                         old_window_screen.top != new_window_screen.top);
                    new_client_screen_valid =
                        ClientRectInScreen(window_, new_client_screen);

                    if (old_client_screen_valid &&
                        new_client_screen_valid) {
                        const HRGN old_client_region =
                            CreateRectRgnIndirect(&old_client_screen);
                        const HRGN new_client_region =
                            CreateRectRgnIndirect(&new_client_screen);
                        outer_exposed_client_region =
                            CreateRectRgn(0, 0, 0, 0);
                        if (old_client_region == nullptr ||
                            new_client_region == nullptr ||
                            outer_exposed_client_region == nullptr ||
                            CombineRgn(outer_exposed_client_region,
                                       new_client_region,
                                       old_client_region,
                                       RGN_DIFF) == ERROR) {
                            outer_exposure_region_failed = true;
                            if (outer_exposed_client_region != nullptr) {
                                DeleteObject(outer_exposed_client_region);
                                outer_exposed_client_region = nullptr;
                            }
                        } else {
                            // Convert the new-minus-old screen region into the
                            // current main-window client coordinate space.
                            if (OffsetRgn(outer_exposed_client_region,
                                          -new_client_screen.left,
                                          -new_client_screen.top) == ERROR) {
                                outer_exposure_region_failed = true;
                                DeleteObject(outer_exposed_client_region);
                                outer_exposed_client_region = nullptr;
                            }
                        }
                        if (new_client_region != nullptr) {
                            DeleteObject(new_client_region);
                        }
                        if (old_client_region != nullptr) {
                            DeleteObject(old_client_region);
                        }
                    }
                }

                // If the no-redraw outer transaction succeeded but its final
                // client footprint cannot be measured, do not silently trust
                // the narrow repair. The complete synchronous fallback below
                // is the correctness boundary for this rare measurement
                // failure.
                if (outer_set_window_pos_succeeded &&
                    (!new_window_valid ||
                     (outer_geometry_changed &&
                      (!old_client_screen_valid ||
                       !new_client_screen_valid)))) {
                    outer_exposure_region_failed = true;
                }
            }
        }
    }

    RECT client{};
    if (GetClientRect(window_, &client) == FALSE) {
        if (outer_exposed_client_region != nullptr) {
            DeleteObject(outer_exposed_client_region);
        }
        status_layout_update_in_progress_ = false;
        return;
    }
    const int client_width = static_cast<int>(client.right - client.left);
    const int client_height = static_cast<int>(client.bottom - client.top);
    const int previous_advanced_viewport = advanced_viewport_height_logical_;
    advanced_viewport_height_logical_ = CurrentAdvancedViewportHeightLogical();
    advanced_scroll_offset_logical_ = std::clamp(
        advanced_scroll_offset_logical_, 0, AdvancedMaximumScrollLogical());
    const bool advanced_viewport_changed =
        previous_advanced_viewport != advanced_viewport_height_logical_;
    const layout::Rect host = old_host_valid &&
                                      old_host_rect.right > old_host_rect.left &&
                                      old_host_rect.bottom > old_host_rect.top
                                  ? layout::CalculateClampedContentHost(
                                        client_width,
                                        client_height,
                                        dpi_,
                                        status_height_logical_,
                                        CurrentLayoutPage(),
                                        old_host_rect.left,
                                        old_host_rect.top,
                                        advanced_viewport_height_logical_)
                                  : layout::CalculateCenteredContentHost(
                                        client_width,
                                        client_height,
                                        dpi_,
                                        status_height_logical_,
                                        CurrentLayoutPage(),
                                        advanced_viewport_height_logical_);

    SetWindowPos(content_host_,
                 nullptr,
                 host.x,
                 host.y,
                 host.width,
                 host.height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER |
                     SWP_NOCOPYBITS | SWP_NOREDRAW);

    const layout::MainLayout main_layout = layout::CalculateMainLayout(
        BuildLayoutMetrics(),
        status_height_logical_,
        CurrentLayoutPage(),
        advanced_viewport_height_logical_);
    struct FooterBinding final {
        HWND window{};
        layout::Control control{};
    };
    const std::array<FooterBinding, 5> footer_bindings{{
        {status_text_, layout::Control::StatusText},
        {diagnostics_text_, layout::Control::DiagnosticsText},
        {start_button_, layout::Control::StartButton},
        {stop_button_, layout::Control::StopButton},
        {emergency_button_, layout::Control::EmergencyButton},
    }};

    constexpr UINT footer_flags =
        SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOOWNERZORDER | SWP_NOZORDER |
        SWP_NOREDRAW;
    for (const FooterBinding& binding : footer_bindings) {
        if (binding.window == nullptr || IsWindow(binding.window) == FALSE) {
            continue;
        }
        const layout::Rect& logical = main_layout[binding.control];

        // Implicit copy preservation across translated owner-drawn child
        // windows can leave a movement-height strip clipped or stale. Use the
        // deterministic discard / no-redraw move for every footer child. Pure
        // action-button translations stay out of the parent repair below and
        // are drawn once, synchronously and fully buffered, after the parent
        // surface has been repaired.
        SetWindowPos(binding.window,
                     nullptr,
                     Scale(logical.x, dpi_),
                     Scale(logical.y, dpi_),
                     Scale(logical.width, dpi_),
                     Scale(logical.height, dpi_),
                     footer_flags);
    }

    RECT new_status_panel_rect{};
    RECT new_start_button_rect{};
    RECT new_stop_button_rect{};
    RECT new_emergency_button_rect{};
    const bool new_status_panel_valid =
        window_rect_in(status_text_, window_, new_status_panel_rect);
    const bool new_start_button_valid =
        window_rect_in(start_button_, window_, new_start_button_rect);
    const bool new_stop_button_valid =
        window_rect_in(stop_button_, window_, new_stop_button_rect);
    const bool new_emergency_button_valid =
        window_rect_in(emergency_button_, window_, new_emergency_button_rect);

    const auto geometry_changed = [](const bool old_valid,
                                     const RECT& old_rect,
                                     const bool new_valid,
                                     const RECT& new_rect) noexcept {
        return old_valid != new_valid ||
               (old_valid && new_valid &&
                EqualRect(&old_rect, &new_rect) == FALSE);
    };
    const bool start_button_geometry_changed =
        geometry_changed(old_start_button_valid,
                         old_start_button_rect,
                         new_start_button_valid,
                         new_start_button_rect);
    const bool stop_button_geometry_changed =
        geometry_changed(old_stop_button_valid,
                         old_stop_button_rect,
                         new_stop_button_valid,
                         new_stop_button_rect);
    const bool emergency_button_geometry_changed =
        geometry_changed(old_emergency_button_valid,
                         old_emergency_button_rect,
                         new_emergency_button_valid,
                         new_emergency_button_rect);
    const auto pure_translation = [](const bool old_valid,
                                     const RECT& old_rect,
                                     const bool new_valid,
                                     const RECT& new_rect) noexcept {
        if (!old_valid || !new_valid ||
            EqualRect(&old_rect, &new_rect) != FALSE) {
            return false;
        }
        const LONG old_width = old_rect.right - old_rect.left;
        const LONG old_height = old_rect.bottom - old_rect.top;
        const LONG new_width = new_rect.right - new_rect.left;
        const LONG new_height = new_rect.bottom - new_rect.top;
        return old_width == new_width && old_height == new_height;
    };
    const bool start_button_final_redraw_eligible =
        !outer_window_moved &&
        pure_translation(old_start_button_valid,
                         old_start_button_rect,
                         new_start_button_valid,
                         new_start_button_rect);
    const bool stop_button_final_redraw_eligible =
        !outer_window_moved &&
        pure_translation(old_stop_button_valid,
                         old_stop_button_rect,
                         new_stop_button_valid,
                         new_stop_button_rect);
    const bool emergency_button_final_redraw_eligible =
        !outer_window_moved &&
        pure_translation(old_emergency_button_valid,
                         old_emergency_button_rect,
                         new_emergency_button_valid,
                         new_emergency_button_rect);

    if (advanced_viewport_changed) {
        // A status-height change only changes the visible Advanced viewport;
        // the fixed full-page coordinates of the controls inside the scroll
        // content do not depend on that viewport height. Re-running the full
        // no-redraw layout here used to reposition every Advanced child and
        // then repaint only the footer, which could leave portions of those
        // children visually blank until some later paint happened. Resize only
        // the viewport host and move the already-laid-out scroll surface if its
        // clamped offset changed. Existing child geometry and pixels remain
        // untouched.
        if (advanced_page_host_ != nullptr &&
            IsWindow(advanced_page_host_) != FALSE) {
            const layout::Rect& page_rect =
                main_layout[layout::Control::AdvancedPageHost];
            SetWindowPos(advanced_page_host_,
                         nullptr,
                         Scale(page_rect.x, dpi_),
                         Scale(page_rect.y, dpi_),
                         Scale(page_rect.width, dpi_),
                         Scale(page_rect.height, dpi_),
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
        }

        if (advanced_scroll_content_host_ != nullptr &&
            IsWindow(advanced_scroll_content_host_) != FALSE) {
            SetWindowPos(advanced_scroll_content_host_,
                         nullptr,
                         0,
                         -Scale(advanced_scroll_offset_logical_, dpi_),
                         0,
                         0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                             SWP_NOZORDER);
        }
    }
    UpdateAdvancedScrollbar(false);

    const bool advanced_exposure_repair_needed =
        selected_page_index_ == 1 && advanced_viewport_changed &&
        advanced_viewport_height_logical_ > previous_advanced_viewport;

    // A rectangle-to-client-bottom footer repair can redraw the Start button
    // even when all three action buttons are geometrically unchanged. It can
    // also paint through the strip that becomes Advanced content when
    // diagnostics is disabled in an enlarged window. Build the ordinary
    // status repair from the current status footprint, restore only pixels
    // vacated by the old status footprint, then add old / new action-button
    // footprints only for buttons that actually moved or resized. If the
    // Advanced viewport grows, the hierarchy-specific ordered repair below
    // runs after this parent cleanup and owns the final presentation of newly
    // visible child content.
    bool footer_region_failed = false;
    HRGN footer_region = CreateRectRgn(0, 0, 0, 0);
    HRGN vacated_action_region = CreateRectRgn(0, 0, 0, 0);
    HRGN deferred_action_current_region = CreateRectRgn(0, 0, 0, 0);
    bool action_deferred_region_failed =
        vacated_action_region == nullptr ||
        deferred_action_current_region == nullptr;
    if (footer_region == nullptr || !new_status_panel_valid ||
        action_deferred_region_failed) {
        footer_region_failed = true;
    }

    const auto add_rect_to_footer =
        [&footer_region, &footer_region_failed](const RECT& rectangle) noexcept {
            if (footer_region_failed || footer_region == nullptr ||
                IsRectEmpty(&rectangle) != FALSE) {
                return;
            }
            const HRGN rectangle_region = CreateRectRgnIndirect(&rectangle);
            if (rectangle_region == nullptr ||
                CombineRgn(footer_region,
                           footer_region,
                           rectangle_region,
                           RGN_OR) == ERROR) {
                footer_region_failed = true;
            }
            if (rectangle_region != nullptr) {
                DeleteObject(rectangle_region);
            }
        };

    if (!footer_region_failed) {
        RECT current_status_repair = new_status_panel_rect;
        const int status_overlap = std::max(1, Scale(1, dpi_));
        InflateRect(&current_status_repair, status_overlap, status_overlap);
        add_rect_to_footer(current_status_repair);

        const bool status_geometry_changed =
            geometry_changed(old_status_panel_valid,
                             old_status_panel_rect,
                             new_status_panel_valid,
                             new_status_panel_rect);

        // Presenting the newly exposed Advanced child hierarchy is not enough
        // by itself: omitting the old status footprint can leave the rounded
        // status-card frame cached in the parent surface. Restore only the
        // part of the old status footprint that is no longer owned by the
        // current status window. The ordered Advanced repair below still runs
        // afterward, so any overlapping parent cleanup is repainted
        // back-to-front by the actual child owners before the transaction
        // finishes.
        if (status_geometry_changed && old_status_panel_valid &&
            new_status_panel_valid) {
            RECT old_status_repair = old_status_panel_rect;
            RECT new_status_repair = new_status_panel_rect;
            InflateRect(&old_status_repair, status_overlap, status_overlap);
            InflateRect(&new_status_repair, status_overlap, status_overlap);

            const HRGN old_status_region =
                CreateRectRgnIndirect(&old_status_repair);
            const HRGN new_status_region =
                CreateRectRgnIndirect(&new_status_repair);
            const HRGN vacated_status_region = CreateRectRgn(0, 0, 0, 0);
            if (old_status_region == nullptr || new_status_region == nullptr ||
                vacated_status_region == nullptr ||
                CombineRgn(vacated_status_region,
                           old_status_region,
                           new_status_region,
                           RGN_DIFF) == ERROR) {
                footer_region_failed = true;
            } else {
                if (CombineRgn(footer_region,
                               footer_region,
                               vacated_status_region,
                               RGN_OR) == ERROR) {
                    footer_region_failed = true;
                }
            }
            if (vacated_status_region != nullptr) {
                DeleteObject(vacated_status_region);
            }
            if (new_status_region != nullptr) {
                DeleteObject(new_status_region);
            }
            if (old_status_region != nullptr) {
                DeleteObject(old_status_region);
            }
        } else if (status_geometry_changed && old_status_panel_valid) {
            // If the current footprint cannot be measured, retain the prior
            // safety behavior and include the complete old footprint. Region
            // failure still promotes to the full synchronous fallback below.
            RECT old_status_repair = old_status_panel_rect;
            InflateRect(&old_status_repair, status_overlap, status_overlap);
            add_rect_to_footer(old_status_repair);
        }

        const auto add_changed_button =
            [&add_rect_to_footer,
             &vacated_action_region,
             &deferred_action_current_region,
             &action_deferred_region_failed](const bool changed,
                                                  const bool final_redraw_eligible,
                                                  const bool old_valid,
                                                  const RECT& old_rect,
                                                  const bool new_valid,
                                                  const RECT& new_rect) noexcept {
                if (!changed) {
                    return;
                }

                if (final_redraw_eligible && old_valid && new_valid &&
                    !action_deferred_region_failed) {
                    const HRGN old_region = CreateRectRgnIndirect(&old_rect);
                    const HRGN new_region = CreateRectRgnIndirect(&new_rect);
                    const HRGN vacated_region = CreateRectRgn(0, 0, 0, 0);
                    if (old_region == nullptr || new_region == nullptr ||
                        vacated_region == nullptr ||
                        CombineRgn(vacated_region,
                                   old_region,
                                   new_region,
                                   RGN_DIFF) == ERROR ||
                        CombineRgn(vacated_action_region,
                                   vacated_action_region,
                                   vacated_region,
                                   RGN_OR) == ERROR ||
                        CombineRgn(deferred_action_current_region,
                                   deferred_action_current_region,
                                   new_region,
                                   RGN_OR) == ERROR) {
                        action_deferred_region_failed = true;
                    }
                    if (vacated_region != nullptr) {
                        DeleteObject(vacated_region);
                    }
                    if (new_region != nullptr) {
                        DeleteObject(new_region);
                    }
                    if (old_region != nullptr) {
                        DeleteObject(old_region);
                    }
                    return;
                }

                if (old_valid) {
                    add_rect_to_footer(old_rect);
                }
                if (new_valid) {
                    add_rect_to_footer(new_rect);
                }
            };
        add_changed_button(start_button_geometry_changed,
                           start_button_final_redraw_eligible,
                           old_start_button_valid,
                           old_start_button_rect,
                           new_start_button_valid,
                           new_start_button_rect);
        add_changed_button(stop_button_geometry_changed,
                           stop_button_final_redraw_eligible,
                           old_stop_button_valid,
                           old_stop_button_rect,
                           new_stop_button_valid,
                           new_stop_button_rect);
        add_changed_button(emergency_button_geometry_changed,
                           emergency_button_final_redraw_eligible,
                           old_emergency_button_valid,
                           old_emergency_button_rect,
                           new_emergency_button_valid,
                           new_emergency_button_rect);

        if (action_deferred_region_failed) {
            footer_region_failed = true;
        } else {
            if (CombineRgn(footer_region,
                           footer_region,
                           vacated_action_region,
                           RGN_OR) == ERROR) {
                footer_region_failed = true;
            }
        }

        const HRGN client_region = CreateRectRgnIndirect(&client);
        if (client_region == nullptr ||
            CombineRgn(footer_region,
                       footer_region,
                       client_region,
                       RGN_AND) == ERROR) {
            footer_region_failed = true;
        }
        if (client_region != nullptr) {
            DeleteObject(client_region);
        }
    }

    // Keep both the exposed-client repair and the moved-host safety repair.
    // When status growth moves the top-level window under SWP_NOREDRAW, pixels
    // in the old / new screen-space overlap can remain visually anchored at
    // their previous screen position. Repainting only new-minus-old client
    // pixels can therefore leave stale copies of content-host controls in the
    // overlapping portion of the client.
    //
    // Repair the current content-host footprint only when the outer window
    // position actually changed. Ordinary status-height changes that do not
    // move the top-level window keep the narrower footer + exposure + host-delta
    // path. This remains one synchronous region-clipped hierarchy repaint, with
    // the complete-client fallback retained for any region failure.
    const RECT new_host_rect{
        host.x,
        host.y,
        host.x + host.width,
        host.y + host.height,
    };
    bool moved_host_region_failed = false;
    HRGN moved_host_region = nullptr;
    if (outer_window_moved) {
        const HRGN current_host_region = CreateRectRgnIndirect(&new_host_rect);
        const HRGN client_region = CreateRectRgnIndirect(&client);
        moved_host_region = CreateRectRgn(0, 0, 0, 0);
        if (current_host_region == nullptr || client_region == nullptr ||
            moved_host_region == nullptr ||
            CombineRgn(moved_host_region,
                       current_host_region,
                       client_region,
                       RGN_AND) == ERROR) {
            moved_host_region_failed = true;
            if (moved_host_region != nullptr) {
                DeleteObject(moved_host_region);
                moved_host_region = nullptr;
            }
        }
        if (client_region != nullptr) {
            DeleteObject(client_region);
        }
        if (current_host_region != nullptr) {
            DeleteObject(current_host_region);
        }
    }
    bool host_delta_region_failed = false;
    HRGN host_delta_region = nullptr;
    if (old_host_valid) {
        const HRGN old_host_region = CreateRectRgnIndirect(&old_host_rect);
        const HRGN new_host_region = CreateRectRgnIndirect(&new_host_rect);
        const HRGN client_region = CreateRectRgnIndirect(&client);
        host_delta_region = CreateRectRgn(0, 0, 0, 0);
        if (old_host_region == nullptr || new_host_region == nullptr ||
            client_region == nullptr || host_delta_region == nullptr ||
            CombineRgn(host_delta_region,
                       old_host_region,
                       new_host_region,
                       RGN_XOR) == ERROR ||
            CombineRgn(host_delta_region,
                       host_delta_region,
                       client_region,
                       RGN_AND) == ERROR) {
            host_delta_region_failed = true;
            if (host_delta_region != nullptr) {
                DeleteObject(host_delta_region);
                host_delta_region = nullptr;
            }
        }
        if (client_region != nullptr) {
            DeleteObject(client_region);
        }
        if (new_host_region != nullptr) {
            DeleteObject(new_host_region);
        }
        if (old_host_region != nullptr) {
            DeleteObject(old_host_region);
        }
    }

    // Resizing content_host_ with SWP_NOCOPYBITS | SWP_NOREDRAW has a subtle
    // overlap case: the old / new XOR contains only pixels outside one of the
    // two host rectangles. During growth, the old rounded bottom shell remains
    // inside the new host, so its antialiased border / corners can survive as a
    // duplicate horizontal edge. During a small shrink, the converse can leave
    // the current bottom shell partly unpresented. Repair only the rounded-shell
    // perimeter depth at the old and current bottoms. The shell painter uses
    // radius+2 pixels for its high-quality perimeter fill, so matching that
    // depth is sufficient without repainting the complete content host. If the
    // top-level window moved, the retained moved-host path already repaints the
    // complete current host and the XOR still covers old-exclusive pixels.
    bool host_chrome_region_failed = false;
    HRGN host_chrome_region = nullptr;
    int host_chrome_depth = 0;
    const bool host_geometry_changed =
        old_host_valid && EqualRect(&old_host_rect, &new_host_rect) == FALSE;
    if (host_geometry_changed && !outer_window_moved) {
        const int shell_radius = Scale(14, dpi_);
        host_chrome_depth = shell_radius + std::max(2, Scale(2, dpi_));
        host_chrome_region = CreateRectRgn(0, 0, 0, 0);
        if (host_chrome_region == nullptr) {
            host_chrome_region_failed = true;
        } else {
            const auto add_bottom_shell_band =
                [&host_chrome_region,
                 &host_chrome_region_failed,
                 host_chrome_depth](const RECT& host_rect) noexcept {
                    if (host_chrome_region_failed ||
                        host_rect.right <= host_rect.left ||
                        host_rect.bottom <= host_rect.top) {
                        return;
                    }
                    RECT band = host_rect;
                    band.top = std::max(
                        band.top,
                        static_cast<LONG>(band.bottom - host_chrome_depth));
                    const HRGN band_region = CreateRectRgnIndirect(&band);
                    if (band_region == nullptr ||
                        CombineRgn(host_chrome_region,
                                   host_chrome_region,
                                   band_region,
                                   RGN_OR) == ERROR) {
                        host_chrome_region_failed = true;
                    }
                    if (band_region != nullptr) {
                        DeleteObject(band_region);
                    }
                };
            add_bottom_shell_band(old_host_rect);
            add_bottom_shell_band(new_host_rect);

            const HRGN client_region = CreateRectRgnIndirect(&client);
            if (client_region == nullptr ||
                CombineRgn(host_chrome_region,
                           host_chrome_region,
                           client_region,
                           RGN_AND) == ERROR) {
                host_chrome_region_failed = true;
            }
            if (client_region != nullptr) {
                DeleteObject(client_region);
            }
            if (host_chrome_region_failed) {
                DeleteObject(host_chrome_region);
                host_chrome_region = nullptr;
            }
        }
    }

    HRGN repair_region = CreateRectRgn(0, 0, 0, 0);
    bool repair_region_ready =
        !outer_exposure_region_failed && !host_delta_region_failed &&
        !host_chrome_region_failed && !moved_host_region_failed &&
        !footer_region_failed &&
        !action_deferred_region_failed &&
        footer_region != nullptr && repair_region != nullptr &&
        CombineRgn(repair_region,
                   footer_region,
                   footer_region,
                   RGN_COPY) != ERROR;
    if (repair_region_ready && outer_exposed_client_region != nullptr) {
        repair_region_ready =
            CombineRgn(repair_region,
                       repair_region,
                       outer_exposed_client_region,
                       RGN_OR) != ERROR;
    }
    if (repair_region_ready && host_delta_region != nullptr) {
        repair_region_ready =
            CombineRgn(repair_region,
                       repair_region,
                       host_delta_region,
                       RGN_OR) != ERROR;
    }
    if (repair_region_ready && host_chrome_region != nullptr) {
        repair_region_ready =
            CombineRgn(repair_region,
                       repair_region,
                       host_chrome_region,
                       RGN_OR) != ERROR;
    }
    if (repair_region_ready && moved_host_region != nullptr) {
        repair_region_ready =
            CombineRgn(repair_region,
                       repair_region,
                       moved_host_region,
                       RGN_OR) != ERROR;
    }

    // A pure action-button translation deliberately uses SWP_NOCOPYBITS
    // because implicit copy preservation can leave a movement-height clipped
    // / stale strip. The current translated action footprints stay excluded
    // from the parent repair so RDW_ALLCHILDREN cannot redraw them
    // mid-transaction. After the parent repair completes, each eligible
    // buffered owner-drawn button is synchronously redrawn once at its final
    // geometry. Outer-window movement retains the moved-host safety repair.
    if (repair_region_ready && !outer_window_moved &&
        deferred_action_current_region != nullptr) {
        RECT preserved_bounds{};
        const int preserved_kind =
            GetRgnBox(deferred_action_current_region, &preserved_bounds);
        if (preserved_kind == ERROR ||
            (preserved_kind != NULLREGION &&
             CombineRgn(repair_region,
                        repair_region,
                        deferred_action_current_region,
                        RGN_DIFF) == ERROR)) {
            repair_region_ready = false;
        }
    }

    BOOL redraw_succeeded = FALSE;
    bool full_repaint_fallback_used = false;
    if (repair_region_ready) {
        redraw_succeeded =
            RedrawWindow(window_,
                         nullptr,
                         repair_region,
                         RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                             RDW_UPDATENOW);
    }

    if (!repair_region_ready || redraw_succeeded == FALSE) {
        // Region construction / painting is an optimization boundary, not a
        // correctness boundary. If GDI cannot build or synchronously present
        // the narrow repair, repaint the complete client once rather than
        // risking the same stale-pixel condition this path is meant to prevent.
        full_repaint_fallback_used = true;
        redraw_succeeded =
            RedrawWindow(window_,
                         nullptr,
                         nullptr,
                         RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                             RDW_UPDATENOW);
    }

    // Relying on child copy preservation leaves a movement-height strip
    // clipped during growth and stale after shrink. Keep the current translated
    // action footprint out of the parent RDW_ALLCHILDREN repair, then redraw
    // the complete translated owner-drawn controls exactly once after parent
    // cleanup. DrawPushButtonControl already buffers each action surface, so
    // this is a single completed child presentation rather than an interleaved
    // parent / child repaint. Any direct child redraw failure crosses the same
    // complete synchronous client fallback boundary.
    BOOL start_action_final_redraw_succeeded = TRUE;
    BOOL stop_action_final_redraw_succeeded = TRUE;
    BOOL emergency_action_final_redraw_succeeded = TRUE;
    bool action_final_redraw_failed = false;
    const auto redraw_final_action =
        [&action_final_redraw_failed](const HWND child,
                                      const bool needed,
                                      BOOL& result) noexcept {
            if (!needed) {
                result = TRUE;
                return;
            }
            if (child == nullptr || IsWindow(child) == FALSE) {
                result = FALSE;
                action_final_redraw_failed = true;
                return;
            }
            result = RedrawWindow(child,
                                  nullptr,
                                  nullptr,
                                  RDW_INVALIDATE | RDW_NOERASE |
                                      RDW_UPDATENOW);
            if (result == FALSE) {
                action_final_redraw_failed = true;
            }
        };

    if (!full_repaint_fallback_used) {
        redraw_final_action(start_button_,
                            start_button_final_redraw_eligible,
                            start_action_final_redraw_succeeded);
        redraw_final_action(stop_button_,
                            stop_button_final_redraw_eligible,
                            stop_action_final_redraw_succeeded);
        redraw_final_action(emergency_button_,
                            emergency_button_final_redraw_eligible,
                            emergency_action_final_redraw_succeeded);
    }

    if (action_final_redraw_failed) {
        full_repaint_fallback_used = true;
        redraw_succeeded =
            RedrawWindow(window_,
                         nullptr,
                         nullptr,
                         RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                             RDW_UPDATENOW);
    }

    // Redrawing only the Advanced page host is not sufficient for a newly
    // exposed strip because its owner-drawn group cards and direct controls
    // belong to advanced_scroll_content_host_. Repair the exposed strip in
    // explicit back-to-front ownership order:
    //   1. scroll-content background gaps,
    //   2. intersecting group cards,
    //   3. intersecting direct child controls.
    // This remains narrowly clipped to the newly visible strip. Any geometry
    // or synchronous redraw failure crosses the same full-client / full-
    // Advanced correctness fallback.
    bool advanced_exposure_region_failed = false;
    BOOL advanced_exposure_host_redraw_succeeded = TRUE;
    RECT advanced_exposure_content_rect{};
    RECT advanced_exposure_page_rect{};

    const auto group_mask = [this](const HWND child) noexcept {
        if (child == advanced_timing_group_) {
            return 1U << 0U;
        }
        if (child == target_group_) {
            return 1U << 1U;
        }
        if (child == performance_group_) {
            return 1U << 2U;
        }
        if (child == notifications_group_) {
            return 1U << 3U;
        }
        if (child == options_group_) {
            return 1U << 4U;
        }
        return 0U;
    };

    const auto redraw_exposure_child =
        [this,
         &window_rect_in,
         &advanced_exposure_content_rect,
         &advanced_exposure_region_failed](const HWND child) noexcept {
            if (advanced_exposure_region_failed || child == nullptr ||
                IsWindow(child) == FALSE || IsWindowVisible(child) == FALSE ||
                GetParent(child) != advanced_scroll_content_host_) {
                return child == nullptr || IsWindow(child) == FALSE ||
                       IsWindowVisible(child) == FALSE ||
                       GetParent(child) != advanced_scroll_content_host_;
            }

            RECT child_in_scroll{};
            if (!window_rect_in(child,
                                advanced_scroll_content_host_,
                                child_in_scroll)) {
                advanced_exposure_region_failed = true;
                return false;
            }

            RECT intersection{};
            if (IntersectRect(&intersection,
                              &advanced_exposure_content_rect,
                              &child_in_scroll) == FALSE) {
                return true;
            }

            MapWindowPoints(advanced_scroll_content_host_,
                            child,
                            reinterpret_cast<POINT*>(&intersection),
                            2);
            RECT child_client{};
            RECT clipped{};
            if (GetClientRect(child, &child_client) == FALSE ||
                IntersectRect(&clipped,
                              &intersection,
                              &child_client) == FALSE) {
                advanced_exposure_region_failed = true;
                return false;
            }

            return RedrawWindow(child,
                                &clipped,
                                nullptr,
                                RDW_INVALIDATE | RDW_NOERASE |
                                    RDW_UPDATENOW) != FALSE;
        };

    if (advanced_exposure_repair_needed) {
        RECT scroll_content_client{};
        RECT page_client{};
        if (advanced_scroll_content_host_ == nullptr ||
            IsWindow(advanced_scroll_content_host_) == FALSE ||
            advanced_page_host_ == nullptr ||
            IsWindow(advanced_page_host_) == FALSE ||
            GetClientRect(advanced_scroll_content_host_,
                          &scroll_content_client) == FALSE ||
            GetClientRect(advanced_page_host_, &page_client) == FALSE) {
            advanced_exposure_region_failed = true;
        } else {
            const int content_top = Scale(
                advanced_scroll_offset_logical_ + previous_advanced_viewport,
                dpi_);
            const int content_bottom = Scale(
                advanced_scroll_offset_logical_ +
                    advanced_viewport_height_logical_,
                dpi_);
            advanced_exposure_content_rect = RECT{
                scroll_content_client.left,
                std::max(scroll_content_client.top,
                         static_cast<LONG>(content_top - 1)),
                scroll_content_client.right,
                std::min(scroll_content_client.bottom,
                         static_cast<LONG>(content_bottom + 1)),
            };

            const int page_top = Scale(previous_advanced_viewport, dpi_);
            const int page_bottom =
                Scale(advanced_viewport_height_logical_, dpi_);
            advanced_exposure_page_rect = RECT{
                page_client.left,
                std::max(page_client.top,
                         static_cast<LONG>(page_top - 1)),
                page_client.right,
                std::min(page_client.bottom,
                         static_cast<LONG>(page_bottom + 1)),
            };

            if (advanced_exposure_content_rect.bottom <=
                    advanced_exposure_content_rect.top ||
                advanced_exposure_page_rect.bottom <=
                    advanced_exposure_page_rect.top) {
                advanced_exposure_region_failed = true;
            } else {
                // Paint only background gaps first. WS_CLIPCHILDREN on the
                // scroll-content host prevents this pass from painting over
                // child windows which are redrawn explicitly below.
                advanced_exposure_host_redraw_succeeded =
                    RedrawWindow(advanced_scroll_content_host_,
                                 &advanced_exposure_content_rect,
                                 nullptr,
                                 RDW_INVALIDATE | RDW_NOERASE |
                                     RDW_UPDATENOW);
                if (advanced_exposure_host_redraw_succeeded == FALSE) {
                    advanced_exposure_region_failed = true;
                }

                // Group cards are visual backgrounds for their sibling
                // controls, so repaint intersecting groups before controls.
                const std::array<HWND, 5> advanced_groups{
                    advanced_timing_group_, target_group_, performance_group_,
                    notifications_group_, options_group_};
                for (const HWND group : advanced_groups) {
                    if (advanced_exposure_region_failed || group == nullptr ||
                        IsWindow(group) == FALSE ||
                        IsWindowVisible(group) == FALSE) {
                        continue;
                    }
                    RECT group_in_scroll{};
                    RECT group_intersection{};
                    if (!window_rect_in(group,
                                        advanced_scroll_content_host_,
                                        group_in_scroll)) {
                        advanced_exposure_region_failed = true;
                        break;
                    }
                    if (IntersectRect(&group_intersection,
                                      &advanced_exposure_content_rect,
                                      &group_in_scroll) == FALSE) {
                        continue;
                    }
                    if (!redraw_exposure_child(group)) {
                        advanced_exposure_region_failed = true;
                        break;
                    }
                }

                // Redraw intersecting non-group direct children last. This
                // includes the custom option choices that can straddle the
                // newly visible boundary. Enumerating immediate children keeps
                // the repair tied to the actual live hierarchy rather than a
                // hand-maintained control subset.
                for (HWND child = GetWindow(advanced_scroll_content_host_,
                                            GW_CHILD);
                     !advanced_exposure_region_failed && child != nullptr;
                     child = GetWindow(child, GW_HWNDNEXT)) {
                    if (group_mask(child) != 0U ||
                        IsWindowVisible(child) == FALSE) {
                        continue;
                    }
                    RECT child_in_scroll{};
                    RECT child_intersection{};
                    if (!window_rect_in(child,
                                        advanced_scroll_content_host_,
                                        child_in_scroll)) {
                        advanced_exposure_region_failed = true;
                        break;
                    }
                    if (IntersectRect(&child_intersection,
                                      &advanced_exposure_content_rect,
                                      &child_in_scroll) == FALSE) {
                        continue;
                    }
                    if (!redraw_exposure_child(child)) {
                        advanced_exposure_region_failed = true;
                        break;
                    }
                }
            }
        }
    }

    if (advanced_exposure_repair_needed &&
        (advanced_exposure_region_failed ||
         advanced_exposure_host_redraw_succeeded == FALSE)) {
        // The narrow descendant repair is an optimization boundary. Preserve
        // the existing complete synchronous client fallback, then explicitly
        // refresh the visible Advanced hierarchy as the descendant-specific
        // correctness backstop for this newly identified exposure case.
        full_repaint_fallback_used = true;
        const BOOL fallback_window_redraw =
            RedrawWindow(window_,
                         nullptr,
                         nullptr,
                         RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                             RDW_UPDATENOW);
        const BOOL fallback_advanced_redraw =
            advanced_page_host_ != nullptr &&
                    IsWindow(advanced_page_host_) != FALSE
                ? RedrawWindow(advanced_page_host_,
                               nullptr,
                               nullptr,
                               RDW_ALLCHILDREN | RDW_ERASE |
                                   RDW_INVALIDATE | RDW_UPDATENOW)
                : FALSE;
        redraw_succeeded =
            fallback_window_redraw != FALSE &&
                    fallback_advanced_redraw != FALSE
                ? TRUE
                : FALSE;
    }

    if (repair_region != nullptr) {
        DeleteObject(repair_region);
    }
    if (footer_region != nullptr) {
        DeleteObject(footer_region);
    }
    if (deferred_action_current_region != nullptr) {
        DeleteObject(deferred_action_current_region);
    }
    if (vacated_action_region != nullptr) {
        DeleteObject(vacated_action_region);
    }
    if (host_delta_region != nullptr) {
        DeleteObject(host_delta_region);
    }
    if (host_chrome_region != nullptr) {
        DeleteObject(host_chrome_region);
    }
    if (moved_host_region != nullptr) {
        DeleteObject(moved_host_region);
    }
    if (outer_exposed_client_region != nullptr) {
        DeleteObject(outer_exposed_client_region);
    }

    status_layout_update_in_progress_ = false;
}

UINT_PTR MainWindow::StartGeneratedTimer(UINT_PTR& active_timer_id,
                                         const UINT milliseconds) noexcept {
    StopGeneratedTimer(active_timer_id);
    if (window_ == nullptr || IsWindow(window_) == FALSE) {
        return 0;
    }

    if (next_generated_timer_id_ < GeneratedTimerIdFirst) {
        next_generated_timer_id_ = GeneratedTimerIdFirst;
    }
    const UINT_PTR requested_id = next_generated_timer_id_++;
    const UINT_PTR timer_id = SetTimer(window_, requested_id, milliseconds, nullptr);
    if (timer_id != 0) {
        active_timer_id = timer_id;
    }
    return timer_id;
}

void MainWindow::StopGeneratedTimer(UINT_PTR& active_timer_id) noexcept {
    if (active_timer_id != 0 && window_ != nullptr && IsWindow(window_) != FALSE) {
        KillTimer(window_, active_timer_id);
    }
    active_timer_id = 0;
}

void MainWindow::QueueStatusAreaLayout() noexcept {
    if (window_ == nullptr || IsWindow(window_) == FALSE) {
        return;
    }
    if (StartGeneratedTimer(status_layout_timer_id_,
                            StatusLayoutDebounceMilliseconds) == 0) {
        UpdateStatusAreaLayout();
    }
}

void MainWindow::SetStatusPresentation(const StatusPresentation& presentation,
                                       const bool force_full_repaint) {
    if (status_text_ == nullptr || IsWindow(status_text_) == FALSE) {
        return;
    }

    StatusCategory effective_category = presentation.category;
    const EngineState engine_state = controller_ ? controller_->State() : EngineState::Ready;
    if ((engine_state == EngineState::Ready || engine_state == EngineState::Disarmed) &&
        start_availability_initialized_ && !start_available_ &&
        !start_attention_required_) {
        // Preserve the established readiness override without inferring state
        // from displayed words. A persistent cleanup warning retains its
        // dedicated Attention category instead of being dimmed as ordinary
        // Not ready guidance.
        effective_category = StatusCategory::NotReady;
    }

    const bool text_changed = GetControlText(status_text_) != presentation.text;
    const bool category_changed = status_category_ != effective_category;
    status_category_ = effective_category;

    if (text_changed) {
        // Suppress the owner-drawn static's native WM_SETTEXT invalidation so
        // a small status-value change cannot expose an erased intermediate
        // frame. The application requests the precise non-erasing repaint
        // below after the new text is stored.
        SendMessageW(status_text_, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(status_text_, presentation.text.c_str());
        SendMessageW(status_text_, WM_SETREDRAW, TRUE, 0);

        // Most status updates, including Action pattern unit changes, remain
        // on the existing line. Queue the footer relayout only when the newly
        // measured text actually requires a different status-panel height.
        if (MeasureStatusHeightLogical() != status_height_logical_) {
            QueueStatusAreaLayout();
        }
    }
    const bool ordinary_status_visible =
        IsWindowVisible(status_text_) != FALSE &&
        (diagnostics_check_ == nullptr || !IsChecked(diagnostics_check_));
    const bool transition_text_changed =
        text_changed && effective_category == StatusCategory::Transition;
    if (ordinary_status_visible &&
        (force_full_repaint || category_changed || transition_text_changed)) {
        // A category change can alter the indicator dot, so repaint the full
        // owner-drawn status panel. Stopping and Emergency-stop-complete both
        // intentionally use the Transition category; repaint the full panel
        // when that same-category text changes as well. Otherwise a rapid
        // Running -> Stopping -> Disarmed sequence can update the text before
        // the earlier full repaint reaches the screen and leave the old blue
        // Running indicator beside the completed Emergency Stop text.
        // When live diagnostics owns this area, keep the hidden status control
        // completely out of the paint transaction.
        InvalidateRect(status_text_, nullptr, FALSE);
    } else if (ordinary_status_visible && text_changed) {
        // The dot, panel, and STATUS caption are unchanged. Repaint only the
        // value region instead of invalidating the complete status surface.
        RECT value_bounds{};
        if (GetClientRect(status_text_, &value_bounds) != FALSE) {
            const UINT status_dpi =
                std::max<UINT>(96, GetDpiForWindow(status_text_));
            value_bounds.left += Scale(42 + 74 + 8, status_dpi);
            value_bounds.right -= Scale(16, status_dpi);
            InvalidateRect(status_text_, &value_bounds, FALSE);
        } else {
            InvalidateRect(status_text_, nullptr, FALSE);
        }
    }
    const std::wstring next_ordinary_tooltip =
        presentation.tooltip.empty()
            ? std::wstring(OrdinaryStatusDefaultTooltip)
            : presentation.tooltip;
    if (ordinary_status_tooltip_ != next_ordinary_tooltip) {
        ordinary_status_tooltip_ = next_ordinary_tooltip;
        if (diagnostics_check_ == nullptr || !IsChecked(diagnostics_check_)) {
            UpdateVisibleStatusTooltip();
        }
    }
    if (text_changed || category_changed) {
        MarkMoveCoverPresentationDirty();
    }
}

void MainWindow::UpdateVisibleStatusTooltip() {
    if (status_text_ == nullptr || IsWindow(status_text_) == FALSE) {
        return;
    }

    const bool diagnostics_visible =
        diagnostics_check_ != nullptr && IsChecked(diagnostics_check_);
    if (ordinary_status_tooltip_.empty()) {
        ordinary_status_tooltip_ = OrdinaryStatusDefaultTooltip;
    }
    tooltips_.SetText(
        status_text_,
        diagnostics_visible ? std::wstring(DiagnosticsStatusTooltip)
                            : ordinary_status_tooltip_);
    // SetText can repaint an already-visible tooltip, but it deliberately
    // preserves the old window dimensions. Status and diagnostics help can
    // differ substantially in length, so recompute the live tooltip geometry
    // when ownership or status-specific help changes.
    tooltips_.RefreshVisible(status_text_);
}

void MainWindow::ShowInputMethodTargetRoutingStatus() {
    if (target_window_.window == nullptr || backend_combo_ == nullptr ||
        IsWindow(backend_combo_) == FALSE ||
        (start_availability_initialized_ && !start_available_)) {
        return;
    }

    // A target-aware control change first refreshes start availability and can
    // therefore change the status category from NotReady back to Ready. This
    // function then replaces the generic Ready text with the more specific
    // target-routing message in the same UI turn. That second text update uses
    // the optimized value-only repaint path, which can otherwise supersede the
    // still-pending full category repaint and leave the old dark status lamp on
    // screen even though the stored category is already Ready. Target-routing
    // messages change only on explicit user or target events, so finish each
    // one with a full non-erasing status repaint while keeping the high-rate
    // ordinary status path optimized.
    const auto present_routing_status = [this](StatusPresentation presentation) {
        SetStatusPresentation(presentation, true);
    };

    const LRESULT backend_selection =
        SendMessageW(backend_combo_, CB_GETCURSEL, 0, 0);
    if (backend_selection == 1 || backend_selection == 4) {
        const wchar_t* method = backend_selection == 1
                                    ? L"Standard input"
                                    : L"Unicode text input";
        present_routing_status({
            StatusCategory::Ready,
            L"Status: This input method sends to the currently active window, not the selected target.",
            std::wstring(method) + L" follows the currently active window. Selected target: " +
                target_window_.title + L". Choose a target-aware input method when the selected target must receive the input.",
        });
        return;
    }

    if (backend_selection == 2) {
        present_routing_status({
            StatusCategory::Ready,
            L"Status: Target selected for foreground system input: " +
                target_window_.title,
            {},
        });
        return;
    }
    if (backend_selection == 3) {
        present_routing_status({
            StatusCategory::Ready,
            L"Status: Target selected for targeted window messages: " +
                target_window_.title,
            {},
        });
        return;
    }
    if (backend_selection == 5) {
        present_routing_status({
            StatusCategory::Ready,
            L"Status: Target selected for Targeted Unicode text: " +
                target_window_.title,
            L"Targeted Unicode text sends printable text to a compatible text recipient inside the selected target. Allow background input controls whether that target may remain in the background.",
        });
        return;
    }

    if (backend_selection == 0) {
        if (IsChecked(background_input_check_)) {
            present_routing_status({
                StatusCategory::Ready,
                L"Status: Target selected. Automatic will use targeted window messages: " +
                    target_window_.title,
                {},
            });
        } else {
            present_routing_status({
                StatusCategory::Ready,
                L"Status: Target selected. Automatic will use foreground target input: " +
                    target_window_.title,
                {},
            });
        }
    }
}

void MainWindow::SetDiagnosticsText(const std::wstring& text) {
    if (last_diagnostics_text_ != text) {
        last_diagnostics_text_ = text;
    }

    if (diagnostics_check_ != nullptr && IsChecked(diagnostics_check_) &&
        status_text_ != nullptr && IsWindow(status_text_) != FALSE) {
        InvalidateRect(status_text_, nullptr, FALSE);
        if (MeasureStatusHeightLogical() != status_height_logical_) {
            QueueStatusAreaLayout();
        }
    }
    MarkMoveCoverPresentationDirty();
}

bool MainWindow::IsCompactTextControl(const HWND control) const {
    if (control == nullptr) {
        return false;
    }
    if (control == rate_text_ || control == status_text_ || control == target_status_text_) {
        return true;
    }
    return std::ranges::find(labels_, control) != labels_.end();
}

bool MainWindow::IsGroupControl(const HWND control) const {
    return control == input_group_ || control == position_group_ ||
           control == repeat_group_ || control == hotkeys_group_ ||
           control == advanced_timing_group_ || control == target_group_ ||
           control == performance_group_ || control == notifications_group_ ||
           control == options_group_ ||
           control == about_identity_group_ ||
           control == about_links_group_;
}

bool MainWindow::IsCheckControl(const HWND control) const {
    return control == background_input_check_ ||
           control == random_interval_check_ ||
           control == click_position_indicator_check_ ||
           control == diagnostics_check_ || control == safety_shield_check_ ||
           control == force_exit_on_emergency_stop_check_ ||
           control == capture_exclusion_check_ ||
           control == keep_on_top_check_ ||
           control == remember_settings_check_ ||
           control == running_indicator_check_;
}

bool MainWindow::IsRadioControl(const HWND control) const {
    return control == current_cursor_radio_ || control == fixed_position_radio_ ||
           control == unlimited_radio_ || control == limited_radio_;
}

bool MainWindow::IsPushButtonControl(const HWND control) const {
    return control == capture_position_button_ || control == select_target_button_ ||
           control == clear_target_button_ || control == admin_button_ ||
           control == import_settings_button_ ||
           control == manage_profiles_button_ ||
           control == start_button_ || control == stop_button_ ||
           control == emergency_button_ ||
           control == official_downloads_button_ ||
           control == source_code_button_ || control == report_bug_button_ ||
           control == copy_support_email_button_ || control == view_license_button_ ||
           control == copy_diagnostic_report_button_;
}

void MainWindow::InvalidateControl(const HWND control) const noexcept {
    if (control != nullptr && IsWindow(control) != FALSE) {
        InvalidateRect(control, nullptr, FALSE);
    }
}

ui::Glyph MainWindow::GlyphForControl(const HWND control) const noexcept {
    if (control == nullptr) {
        return ui::Glyph::None;
    }

    const bool keyboard_mode =
        action_type_combo_ != nullptr && IsWindow(action_type_combo_) != FALSE &&
        SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;

    if (control == input_group_ || control == action_type_label_) {
        return keyboard_mode ? ui::Glyph::Keyboard : ui::Glyph::Mouse;
    }
    if (control == mouse_button_label_) {
        return ui::Glyph::Mouse;
    }
    if (control == position_group_) {
        return ui::Glyph::Crosshair;
    }
    if (control == repeat_group_) {
        return ui::Glyph::Repeat;
    }
    if (control == hotkeys_group_ || control == generated_key_label_ ||
        control == start_hotkey_label_ || control == emergency_hotkey_label_ ||
        control == backend_label_) {
        return ui::Glyph::Keyboard;
    }
    if (control == advanced_timing_group_ || control == interval_label_ ||
        control == button_down_label_ || control == minimum_interval_label_ ||
        control == maximum_interval_label_) {
        return ui::Glyph::Clock;
    }
    if (control == action_pattern_label_) {
        return ui::Glyph::Action;
    }
    if (control == action_spacing_label_) {
        return ui::Glyph::Spacing;
    }
    if (control == rate_text_) {
        return keyboard_mode ? ui::Glyph::KeyPress : ui::Glyph::MouseClick;
    }
    if (control == burst_count_label_) {
        return ui::Glyph::List;
    }
    if (control == target_group_) {
        return ui::Glyph::Crosshair;
    }
    if (control == target_label_ || control == select_target_button_) {
        return ui::Glyph::WindowTarget;
    }
    if (control == background_input_check_) {
        return ui::Glyph::Shield;
    }
    if (control == performance_group_) {
        return ui::Glyph::Gauge;
    }
    if (control == notifications_group_) {
        return ui::Glyph::NotificationIndicator;
    }
    if (control == options_group_) {
        return ui::Glyph::Gear;
    }
    if (control == capture_position_button_) {
        return ui::Glyph::Capture;
    }
    if (control == admin_button_) {
        return ui::Glyph::Administrator;
    }
    if (control == manage_profiles_button_ || control == profile_label_) {
        return ui::Glyph::Gear;
    }
    if (control == import_settings_button_) {
        return ui::Glyph::Import;
    }
    if (control == start_button_) {
        return ui::Glyph::Play;
    }
    if (control == stop_button_) {
        return CleanupRequired() ? ui::Glyph::Repeat : ui::Glyph::Stop;
    }
    if (control == emergency_button_) {
        return ui::Glyph::Emergency;
    }
    return ui::Glyph::None;
}

void MainWindow::DrawCompactText(const DRAWITEMSTRUCT& draw) const {
    RECT bounds = draw.rcItem;
    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(draw.hwndItem));
    const bool enabled = IsWindowEnabled(draw.hwndItem) != FALSE;
    const HFONT normal_font = reinterpret_cast<HFONT>(
        font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));
    const HFONT strong_font = reinterpret_cast<HFONT>(
        bold_font_.get() != nullptr ? bold_font_.get() : normal_font);

    if (draw.hwndItem == status_text_) {
        const bool diagnostics_mode =
            diagnostics_check_ != nullptr && IsChecked(diagnostics_check_);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        HDC buffer = width > 0 && height > 0 ? CreateCompatibleDC(draw.hDC) : nullptr;
        HBITMAP bitmap = buffer != nullptr
                             ? CreateCompatibleBitmap(draw.hDC, width, height)
                             : nullptr;
        HGDIOBJ old_bitmap = nullptr;
        HDC target = draw.hDC;
        RECT target_bounds = bounds;
        if (buffer != nullptr && bitmap != nullptr) {
            old_bitmap = SelectObject(buffer, bitmap);
            target = buffer;
            target_bounds = {0, 0, width, height};
        } else {
            if (bitmap != nullptr) {
                DeleteObject(bitmap);
            }
            if (buffer != nullptr) {
                DeleteDC(buffer);
            }
            buffer = nullptr;
        }

        // One buffered child owns the complete footer in both modes. Keeping
        // ordinary status and live diagnostics inside this same HWND removes
        // the overlapping-window paint order that could expose two messages
        // during rapid Emergency Stop and position-capture transitions.
        // The status card is one visual component in both ordinary and live
        // diagnostics modes. Keep its chrome identical across the text-mode
        // switch so diagnostics does not make the border look heavier or the
        // corners appear square. In particular, the outside fill must remain
        // WindowAlt; filling it with Surface makes the rounded corner cutouts
        // indistinguishable from the panel interior.
        ui::Fill(target, target_bounds, ui::WindowAlt);
        ui::DrawRoundedPanel(target,
                             target_bounds,
                             ui::Surface,
                             ui::BorderSoft,
                             Scale(10, control_dpi));

        std::wstring text =
            diagnostics_mode
                ? (!last_diagnostics_text_.empty()
                       ? last_diagnostics_text_
                       : GetControlText(diagnostics_text_))
                : GetControlText(draw.hwndItem);
        const StatusCategory category =
            diagnostics_mode ? diagnostics_category_ : status_category_;
        const COLORREF dot_color = ui::StatusIndicatorColor(category);

        // The status lamp uses one shared geometry path for ordinary status,
        // live diagnostics, and the hidden diagnostics fallback. Its diameter
        // is five logical pixels larger than the original accepted 9-pixel lamp,
        // while the prior center point is preserved across DPI scaling.
        ui::DrawStatusIndicator(target,
                                target_bounds,
                                control_dpi,
                                dot_color);

        RECT value_bounds = target_bounds;
        if (diagnostics_mode) {
            value_bounds.left += Scale(36, control_dpi);
            value_bounds.right -= Scale(8, control_dpi);
        } else {
            constexpr std::wstring_view prefix = L"Status: ";
            if (text.starts_with(prefix)) {
                text.erase(0, prefix.size());
            }

            RECT label_bounds = target_bounds;
            label_bounds.left += Scale(42, control_dpi);
            label_bounds.right = label_bounds.left + Scale(74, control_dpi);
            ui::DrawTextLine(target,
                             L"STATUS",
                             label_bounds,
                             strong_font,
                             ui::Muted,
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            value_bounds.left = label_bounds.right + Scale(8, control_dpi);
            value_bounds.right -= Scale(16, control_dpi);
        }

        SetBkMode(target, TRANSPARENT);
        SetTextColor(target, enabled ? ui::Text : ui::Disabled);
        const HGDIOBJ old_font = SelectObject(target, normal_font);
        RECT measured = value_bounds;
        DrawTextW(target,
                  text.c_str(),
                  -1,
                  &measured,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        const int measured_height = measured.bottom - measured.top;
        if (measured_height > 0 &&
            measured_height < (value_bounds.bottom - value_bounds.top)) {
            value_bounds.top +=
                ((value_bounds.bottom - value_bounds.top) - measured_height) / 2;
        }
        DrawTextW(target,
                  text.c_str(),
                  -1,
                  &value_bounds,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(target, old_font);

        if (buffer != nullptr) {
            BitBlt(draw.hDC,
                   bounds.left,
                   bounds.top,
                   width,
                   height,
                   buffer,
                   0,
                   0,
                   SRCCOPY);
            SelectObject(buffer, old_bitmap);
            DeleteObject(bitmap);
            DeleteDC(buffer);
        }
        return;
    }

    if (draw.hwndItem == target_status_text_) {
        ui::Fill(draw.hDC, bounds, ui::Surface);
        ui::DrawRoundedPanel(draw.hDC,
                             bounds,
                             ui::SurfaceAlt,
                             enabled ? ui::Border : ui::BorderSoft,
                             Scale(8, control_dpi));
        RECT text_bounds = bounds;
        text_bounds.left += Scale(12, control_dpi);
        text_bounds.right -= Scale(12, control_dpi);
        ui::DrawTextLine(draw.hDC,
                         GetControlText(draw.hwndItem),
                         text_bounds,
                         normal_font,
                         enabled ? ui::Muted : ui::Disabled,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    if (draw.hwndItem == rate_text_) {
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        HDC buffer = width > 0 && height > 0 ? CreateCompatibleDC(draw.hDC) : nullptr;
        HBITMAP bitmap = buffer != nullptr
                             ? CreateCompatibleBitmap(draw.hDC, width, height)
                             : nullptr;
        HGDIOBJ old_bitmap = nullptr;
        HDC target = draw.hDC;
        RECT target_bounds = bounds;
        if (buffer != nullptr && bitmap != nullptr) {
            old_bitmap = SelectObject(buffer, bitmap);
            target = buffer;
            target_bounds = {0, 0, width, height};
        } else {
            if (bitmap != nullptr) {
                DeleteObject(bitmap);
            }
            if (buffer != nullptr) {
                DeleteDC(buffer);
            }
            buffer = nullptr;
        }

        ui::Fill(target, target_bounds, ui::Surface);
        RECT glyph_bounds = target_bounds;
        glyph_bounds.left += Scale(3, control_dpi);
        glyph_bounds.right = glyph_bounds.left + Scale(24, control_dpi);
        const int glyph_height = Scale(24, control_dpi);
        const int bounds_height =
            static_cast<int>(target_bounds.bottom - target_bounds.top);
        glyph_bounds.top = target_bounds.top +
                           std::max(0, (bounds_height - glyph_height) / 2);
        glyph_bounds.bottom = glyph_bounds.top + glyph_height;
        ui::DrawGlyph(target,
                      GlyphForControl(draw.hwndItem),
                      glyph_bounds,
                      enabled ? ui::AccentHover : ui::Disabled,
                      std::max(1, Scale(1, control_dpi)));

        RECT text_bounds = target_bounds;
        text_bounds.left = glyph_bounds.right + Scale(8, control_dpi);
        text_bounds.right -= Scale(2, control_dpi);
        const std::wstring text = GetControlText(draw.hwndItem);
        RECT measured = text_bounds;
        const HGDIOBJ measured_font =
            normal_font != nullptr ? normal_font : GetStockObject(DEFAULT_GUI_FONT);
        const HGDIOBJ previous_font = SelectObject(target, measured_font);
        DrawTextW(target,
                  text.c_str(),
                  static_cast<int>(text.size()),
                  &measured,
                  DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(target, previous_font);
        const int available_height =
            static_cast<int>(text_bounds.bottom - text_bounds.top);
        const int measured_height =
            static_cast<int>(measured.bottom - measured.top);
        text_bounds.top += std::max(0, (available_height - measured_height) / 2);
        ui::DrawTextLine(target,
                         text,
                         text_bounds,
                         normal_font,
                         enabled ? ui::AccentHover : ui::Disabled,
                         DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);

        if (buffer != nullptr) {
            BitBlt(draw.hDC,
                   bounds.left,
                   bounds.top,
                   width,
                   height,
                   buffer,
                   0,
                   0,
                   SRCCOPY);
            SelectObject(buffer, old_bitmap);
            DeleteObject(bitmap);
            DeleteDC(buffer);
        }
        return;
    }

    // Ordinary labels sit directly on the card surface rather than appearing
    // as separate rounded boxes. A small line icon gives each major row the
    // same visual hierarchy as the concept while remaining lightweight GDI.
    ui::Fill(draw.hDC, bounds, ui::Surface);
    RECT text_bounds = bounds;
    const ui::Glyph glyph = GlyphForControl(draw.hwndItem);
    if (glyph != ui::Glyph::None) {
        RECT glyph_bounds = bounds;
        const int available_height = bounds.bottom - bounds.top;
        const int preferred_glyph_size =
            glyph == ui::Glyph::WindowTarget ? 24 : 22;
        const int glyph_size = std::min(
            Scale(preferred_glyph_size, control_dpi),
            std::max(Scale(16, control_dpi), available_height - Scale(2, control_dpi)));
        glyph_bounds.left += Scale(2, control_dpi);
        glyph_bounds.right = glyph_bounds.left + glyph_size;
        glyph_bounds.top = bounds.top + ((available_height - glyph_size) / 2);
        glyph_bounds.bottom = glyph_bounds.top + glyph_size;
        ui::DrawGlyph(draw.hDC,
                      glyph,
                      glyph_bounds,
                      enabled ? ui::Icon : ui::Disabled,
                      std::max(1, Scale(1, control_dpi)));
        text_bounds.left = glyph_bounds.right + Scale(10, control_dpi);
    } else {
        text_bounds.left += Scale(2, control_dpi);
    }
    // The Repeat unit label already ends at the card's normal right inset.
    // Do not spend another four logical pixels on generic label padding: the
    // longer keyboard unit ("key presses") needs that width at high DPI.
    // Other labels retain the established inner breathing room.
    if (draw.hwndItem != repeat_unit_label_) {
        text_bounds.right -= Scale(4, control_dpi);
    }

    UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    if (draw.hwndItem == basic_interval_minutes_header_ ||
        draw.hwndItem == basic_interval_seconds_header_ ||
        draw.hwndItem == basic_interval_milliseconds_header_ ||
        draw.hwndItem == advanced_minutes_header_ ||
        draw.hwndItem == advanced_seconds_header_ ||
        draw.hwndItem == advanced_milliseconds_header_) {
        format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    } else if (draw.hwndItem == position_unavailable_text_ ||
        draw.hwndItem == performance_guidance_text_ ||
        draw.hwndItem == about_description_text_ ||
        draw.hwndItem == about_details_text_ ||
        draw.hwndItem == about_links_description_text_) {
        format = DT_LEFT | DT_VCENTER | DT_WORDBREAK;
    }
    const HFONT text_font =
        draw.hwndItem == about_name_text_ && action_font_.get() != nullptr
            ? reinterpret_cast<HFONT>(action_font_.get())
            : normal_font;
    if (draw.hwndItem == about_support_text_) {
        constexpr std::wstring_view support_prefix = L"Support: ";
        const std::wstring support_text = GetControlText(draw.hwndItem);
        ui::DrawTextLine(draw.hDC,
                         support_prefix,
                         text_bounds,
                         text_font,
                         enabled ? ui::Muted : ui::Disabled,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SIZE prefix_size{};
        const HGDIOBJ previous_font = SelectObject(draw.hDC, text_font);
        GetTextExtentPoint32W(draw.hDC,
                              support_prefix.data(),
                              static_cast<int>(support_prefix.size()),
                              &prefix_size);
        SelectObject(draw.hDC, previous_font);

        RECT address_bounds = text_bounds;
        address_bounds.left += prefix_size.cx;
        const std::wstring_view address =
            support_text.starts_with(support_prefix)
                ? std::wstring_view(support_text).substr(support_prefix.size())
                : std::wstring_view(support_text);
        ui::DrawTextLine(draw.hDC,
                         address,
                         address_bounds,
                         text_font,
                         enabled ? ui::Text : ui::Disabled,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    const COLORREF text_color =
        draw.hwndItem == repeat_unit_label_ ||
                draw.hwndItem == basic_interval_minutes_header_ ||
                draw.hwndItem == basic_interval_seconds_header_ ||
                draw.hwndItem == basic_interval_milliseconds_header_ ||
                draw.hwndItem == run_time_hours_header_ ||
                draw.hwndItem == run_time_minutes_header_ ||
                draw.hwndItem == run_time_seconds_header_ ||
                draw.hwndItem == advanced_minutes_header_ ||
                draw.hwndItem == advanced_seconds_header_ ||
                draw.hwndItem == advanced_milliseconds_header_ ||
                draw.hwndItem == performance_guidance_text_ ||
                draw.hwndItem == about_version_text_
            ? ui::Muted
            : enabled ? ui::Text : ui::Disabled;

    if (draw.hwndItem == performance_guidance_text_) {
        // Measure the wrapped recommendation before drawing it. The original
        // compact card gave this STATIC only 32 logical pixels, which could
        // clip the second line at some DPI / font combinations. Keep a 40-pixel
        // control for safe measurement, but bottom-align the measured text in
        // that control so the card can retain the same tight lower padding as
        // the established Advanced cards instead of relying on extra blank
        // card height to hide clipping.
        const std::wstring guidance = GetControlText(draw.hwndItem);
        RECT measured{text_bounds};
        const HGDIOBJ previous_font = SelectObject(draw.hDC, text_font);
        DrawTextW(draw.hDC,
                  guidance.c_str(),
                  static_cast<int>(guidance.size()),
                  &measured,
                  DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(draw.hDC, previous_font);

        const int available_height = text_bounds.bottom - text_bounds.top;
        const int measured_height = measured.bottom - measured.top;
        if (measured_height > 0 && measured_height < available_height) {
            text_bounds.bottom -= Scale(1, control_dpi);
            text_bounds.top = std::max(text_bounds.top,
                                       text_bounds.bottom - measured_height);
        }
        ui::DrawTextLine(draw.hDC,
                         guidance,
                         text_bounds,
                         text_font,
                         text_color,
                         DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);
        return;
    }

    ui::DrawTextLine(draw.hDC,
                     GetControlText(draw.hwndItem),
                     text_bounds,
                     text_font,
                     text_color,
                     format);
}

void MainWindow::DrawGroupControl(const DRAWITEMSTRUCT& draw) const {
    RECT bounds = draw.rcItem;
    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(draw.hwndItem));
    const int panel_radius = Scale(12, control_dpi);
    const bool buffered_role =
        draw.hwndItem == input_group_ ||
        draw.hwndItem == advanced_timing_group_ ||
        draw.hwndItem == target_group_ ||
        draw.hwndItem == position_group_ ||
        draw.hwndItem == repeat_group_ ||
        draw.hwndItem == hotkeys_group_ ||
        draw.hwndItem == performance_group_ ||
        draw.hwndItem == notifications_group_ ||
        draw.hwndItem == options_group_ ||
        draw.hwndItem == about_identity_group_ ||
        draw.hwndItem == about_links_group_;

    const bool buffered = buffered_role &&
        PaintBufferedRegion(
            draw.hDC,
            bounds,
            ui::WindowAlt,
            [&](const HDC buffered_dc, const RECT& local_bounds) noexcept {
                ui::DrawRoundedPanel(buffered_dc,
                                     local_bounds,
                                     ui::Surface,
                                     ui::Border,
                                     panel_radius);
            });
    if (!buffered) {
        // Unknown / future groups and allocation / publication failures retain the
        // accepted direct-HDC implementation.
        ui::Fill(draw.hDC, bounds, ui::WindowAlt);
        ui::DrawRoundedPanel(draw.hDC,
                             bounds,
                             ui::Surface,
                             ui::Border,
                             panel_radius);
    }

    const int header_height = Scale(38, control_dpi);
    const HPEN divider_pen = CreatePen(PS_SOLID,
                                       std::max(1, Scale(1, control_dpi)),
                                       ui::Divider);
    if (divider_pen != nullptr) {
        const HGDIOBJ previous_pen = SelectObject(draw.hDC, divider_pen);
        MoveToEx(draw.hDC,
                 bounds.left + Scale(1, control_dpi),
                 bounds.top + header_height,
                 nullptr);
        LineTo(draw.hDC,
               bounds.right - Scale(1, control_dpi),
               bounds.top + header_height);
        SelectObject(draw.hDC, previous_pen);
        DeleteObject(divider_pen);
    }

    std::wstring title;
    if (draw.hwndItem == hotkeys_group_) {
        // Keep the underlying STATIC text stable. Changing an owner-drawn
        // group's window text invalidates its complete card and can briefly
        // cover sibling labels while Windows processes the registration
        // transition. Derive the small header-only state directly instead.
        title = L"Hotkeys";
        // A confirmed pair remains fully active while Windows evaluates a
        // replacement. Keep the visible state stable during that transaction;
        // "Registering" is reserved for startup or recovery when no safe pair
        // is active yet.
        if (safety_hotkeys_confirmed_) {
            title += L" (Active)";
        } else if (hotkey_registration_pending_) {
            title += L" (Registering...)";
        } else {
            title += L" (Unavailable)";
        }
    } else {
        title = GetControlText(draw.hwndItem);
    }
    std::transform(title.begin(), title.end(), title.begin(),
                   [](const wchar_t character) {
                       return static_cast<wchar_t>(std::towupper(character));
                   });

    RECT title_bounds = bounds;
    title_bounds.left += Scale(18, control_dpi);
    title_bounds.top = bounds.top;
    title_bounds.right -= Scale(54, control_dpi);
    title_bounds.bottom = bounds.top + header_height;
    const HFONT selected_font = reinterpret_cast<HFONT>(
        bold_font_.get() != nullptr ? bold_font_.get()
                                    : font_.get() != nullptr ? font_.get()
                                                             : GetStockObject(DEFAULT_GUI_FONT));
    ui::DrawTextLine(draw.hDC,
                     title,
                     title_bounds,
                     selected_font,
                     ui::Text,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const int header_glyph_size =
        Scale(draw.hwndItem == performance_group_
                  ? 34
                  : draw.hwndItem == notifications_group_
                        ? 30
                        : draw.hwndItem == repeat_group_ ? 24 : 22,
              control_dpi);
    RECT glyph_bounds{};
    const int header_glyph_right_inset =
        Scale(draw.hwndItem == performance_group_
                  ? 10
                  : draw.hwndItem == notifications_group_ ? 12 : 16,
              control_dpi);
    glyph_bounds.right = bounds.right - header_glyph_right_inset;
    glyph_bounds.left = glyph_bounds.right - header_glyph_size;
    glyph_bounds.top = bounds.top + ((header_height - header_glyph_size) / 2);
    glyph_bounds.bottom = glyph_bounds.top + header_glyph_size;
    ui::DrawGlyph(draw.hDC,
                  GlyphForControl(draw.hwndItem),
                  glyph_bounds,
                  ui::Icon,
                  std::max(1, Scale(1, control_dpi)));
}

void MainWindow::DrawChoiceControl(const DRAWITEMSTRUCT& draw, const bool radio) const {
    RECT bounds = draw.rcItem;
    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(draw.hwndItem));
    const bool enabled = IsWindowEnabled(draw.hwndItem) != FALSE;
    const bool checked = IsChecked(draw.hwndItem);
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;

    ui::Fill(draw.hDC, bounds, ui::Surface);

    const ui::Glyph row_glyph = GlyphForControl(draw.hwndItem);
    int mark_left = bounds.left + Scale(2, control_dpi);
    if (row_glyph != ui::Glyph::None) {
        RECT glyph_bounds = bounds;
        const int available_height = bounds.bottom - bounds.top;
        const int glyph_size = std::min(
            Scale(22, control_dpi),
            std::max(Scale(16, control_dpi), available_height - Scale(2, control_dpi)));
        glyph_bounds.left += Scale(1, control_dpi);
        glyph_bounds.right = glyph_bounds.left + glyph_size;
        glyph_bounds.top = bounds.top + ((available_height - glyph_size) / 2);
        glyph_bounds.bottom = glyph_bounds.top + glyph_size;
        ui::DrawGlyph(draw.hDC,
                      row_glyph,
                      glyph_bounds,
                      enabled ? ui::Icon : ui::Disabled,
                      std::max(1, Scale(1, control_dpi)));
        mark_left = glyph_bounds.right + Scale(8, control_dpi);
    }

    const int size = Scale(16, control_dpi);
    const int left = mark_left;
    const int top = bounds.top + ((bounds.bottom - bounds.top) - size) / 2;
    RECT mark{left, top, left + size, top + size};
    const COLORREF mark_border = enabled ? (checked ? ui::AccentHover : ui::Border)
                                         : ui::Disabled;
    const COLORREF mark_fill = checked && enabled ? ui::Accent : ui::SurfaceAlt;

    if (radio) {
        ui::DrawSmoothRadioMark(draw.hDC,
                                mark,
                                mark_fill,
                                mark_border,
                                enabled ? ui::Text : ui::Muted,
                                checked,
                                std::max(1, Scale(1, control_dpi)));
    } else {
        const HBRUSH brush = CreateSolidBrush(mark_fill);
        const HPEN pen = CreatePen(
            PS_SOLID, std::max(1, Scale(1, control_dpi)), mark_border);
        if (brush != nullptr && pen != nullptr) {
            const HGDIOBJ old_brush = SelectObject(draw.hDC, brush);
            const HGDIOBJ old_pen = SelectObject(draw.hDC, pen);
            RoundRect(draw.hDC,
                      mark.left,
                      mark.top,
                      mark.right,
                      mark.bottom,
                      Scale(4, control_dpi),
                      Scale(4, control_dpi));
            SelectObject(draw.hDC, old_pen);
            SelectObject(draw.hDC, old_brush);
        }
        if (pen != nullptr) {
            DeleteObject(pen);
        }
        if (brush != nullptr) {
            DeleteObject(brush);
        }

        if (checked) {
            RECT indicator = mark;
            InflateRect(&indicator,
                        -Scale(2, control_dpi),
                        -Scale(2, control_dpi));
            ui::DrawGlyph(draw.hDC,
                          ui::Glyph::Check,
                          indicator,
                          enabled ? ui::Text : ui::Muted,
                          std::max(1, Scale(2, control_dpi)));
        }
    }

    RECT text_bounds = bounds;
    text_bounds.left = mark.right + Scale(8, control_dpi);
    const HFONT selected_font = reinterpret_cast<HFONT>(
        font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));
    ui::DrawTextLine(draw.hDC,
                     GetControlText(draw.hwndItem),
                     text_bounds,
                     selected_font,
                     enabled ? ui::Text : ui::Disabled,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (focused) {
        ui::DrawFocusOutline(draw.hDC, bounds, ui::AccentHover, Scale(1, control_dpi));
    }
}

void MainWindow::DrawPushButtonControl(const DRAWITEMSTRUCT& draw) const {
    const RECT output_bounds = draw.rcItem;
    const int output_width = output_bounds.right - output_bounds.left;
    const int output_height = output_bounds.bottom - output_bounds.top;
    const bool action_button = draw.hwndItem == start_button_ ||
                               draw.hwndItem == stop_button_ ||
                               draw.hwndItem == emergency_button_;
    // Present footer action-state changes as one completed frame. Directly
    // exposing the multi-stage panel, glyph, and text draw can reveal a
    // partial upper / lower transition on some displays.
    const bool buffered = (action_button ||
                           draw.hwndItem == capture_position_button_) &&
                          output_width > 0 && output_height > 0;

    HDC buffer_dc = buffered ? CreateCompatibleDC(draw.hDC) : nullptr;
    HBITMAP buffer_bitmap = buffer_dc != nullptr
                                ? CreateCompatibleBitmap(draw.hDC,
                                                         output_width,
                                                         output_height)
                                : nullptr;
    HGDIOBJ old_bitmap = nullptr;
    HDC target_dc = draw.hDC;
    RECT bounds = output_bounds;
    if (buffer_dc != nullptr && buffer_bitmap != nullptr) {
        old_bitmap = SelectObject(buffer_dc, buffer_bitmap);
        target_dc = buffer_dc;
        bounds = {0, 0, output_width, output_height};
    } else {
        if (buffer_bitmap != nullptr) {
            DeleteObject(buffer_bitmap);
        }
        if (buffer_dc != nullptr) {
            DeleteDC(buffer_dc);
        }
        buffer_dc = nullptr;
        buffer_bitmap = nullptr;
    }
    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(draw.hwndItem));
    const bool enabled = IsWindowEnabled(draw.hwndItem) != FALSE;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;

    COLORREF fill = ui::SurfaceAlt;
    COLORREF border = ui::Border;
    COLORREF text_color = enabled ? ui::Text : ui::Disabled;
    const bool cleanup_retry_button =
        draw.hwndItem == stop_button_ && CleanupRequired();

    if (draw.hwndItem == start_button_) {
        fill = enabled ? (pressed ? ui::AccentPressed : hot ? ui::AccentHover : ui::Accent)
                       : ui::SurfaceAlt;
        border = enabled ? ui::AccentHover : ui::BorderSoft;
        text_color = enabled ? ui::Text : ui::Disabled;
    } else if (cleanup_retry_button) {
        fill = enabled
                   ? (pressed ? ui::CleanupSurfacePressed
                              : hot ? ui::CleanupSurfaceHover
                                    : ui::CleanupSurface)
                   : ui::SurfaceAlt;
        border = enabled
                     ? (hot || focused ? ui::CleanupActionHover
                                       : ui::CleanupAction)
                     : ui::BorderSoft;
        text_color = enabled ? ui::CleanupActionHover : ui::Disabled;
    } else if (draw.hwndItem == emergency_button_) {
        fill = enabled && pressed ? ui::DangerPressed : ui::SurfaceAlt;
        border = enabled ? ui::Danger : ui::BorderSoft;
        text_color = enabled ? ui::Danger : ui::Disabled;
    } else if (draw.hwndItem == select_target_button_ ||
               draw.hwndItem == admin_button_ ||
               draw.hwndItem == capture_position_button_ ||
               draw.hwndItem == import_settings_button_ ||
               draw.hwndItem == manage_profiles_button_) {
        fill = enabled && pressed ? ui::SurfacePressed
                                  : hot && enabled ? ui::SurfaceHover : ui::SurfaceAlt;
        border = enabled ? ui::Accent : ui::BorderSoft;
        text_color = enabled ? ui::AccentHover : ui::Disabled;
    } else {
        fill = enabled && pressed ? ui::SurfacePressed
                                  : hot && enabled ? ui::SurfaceHover : ui::SurfaceAlt;
        border = enabled ? ui::Border : ui::BorderSoft;
    }

    ui::Fill(target_dc, bounds,
             draw.hwndItem == start_button_ || draw.hwndItem == stop_button_ ||
                     draw.hwndItem == emergency_button_
                 ? ui::WindowAlt
                 : ui::Surface);
    if (draw.hwndItem == start_button_ && enabled && !pressed) {
        ui::DrawStartButtonSurface(target_dc,
                                   bounds,
                                   focused && enabled ? ui::AccentHover : border,
                                   Scale(9, control_dpi),
                                   hot,
                                   1);
    } else {
        ui::DrawRoundedPanel(target_dc,
                             bounds,
                             fill,
                             focused && enabled
                                 ? (cleanup_retry_button
                                        ? ui::CleanupActionHover
                                        : ui::AccentHover)
                                 : border,
                             Scale(9, control_dpi));
    }

    const std::wstring text = GetControlText(draw.hwndItem);
    const HFONT selected_font = reinterpret_cast<HFONT>(
        action_button && action_font_.get() != nullptr
            ? action_font_.get()
            : bold_font_.get() != nullptr
                  ? bold_font_.get()
                  : font_.get() != nullptr ? font_.get()
                                           : GetStockObject(DEFAULT_GUI_FONT));
    const ui::Glyph glyph = GlyphForControl(draw.hwndItem);

    SIZE text_size{};
    const HGDIOBJ old_font = SelectObject(target_dc, selected_font);
    (void)GetTextExtentPoint32W(target_dc,
                               text.c_str(),
                               static_cast<int>(text.size()),
                               &text_size);
    SelectObject(target_dc, old_font);

    const int icon_size = glyph == ui::Glyph::None
                              ? 0
                              : Scale(action_button
                                          ? 26
                                          : (glyph == ui::Glyph::WindowTarget ||
                                             glyph == ui::Glyph::Administrator ||
                                             glyph == ui::Glyph::Import)
                                                ? 24
                                                : 22,
                                      control_dpi);
    const int icon_gap = glyph == ui::Glyph::None
                             ? 0
                             : Scale(action_button ? 10 : 9, control_dpi);
    const int group_width = icon_size + icon_gap + text_size.cx;
    const int button_width = static_cast<int>(bounds.right - bounds.left);
    int group_left = static_cast<int>(bounds.left) +
                     std::max(Scale(8, control_dpi),
                              (button_width - group_width) / 2);
    if (pressed) {
        group_left += Scale(1, control_dpi);
    }

    if (glyph != ui::Glyph::None) {
        RECT glyph_bounds{};
        glyph_bounds.left = group_left;
        glyph_bounds.right = group_left + icon_size;
        glyph_bounds.top = bounds.top + ((bounds.bottom - bounds.top) - icon_size) / 2;
        glyph_bounds.bottom = glyph_bounds.top + icon_size;
        ui::DrawGlyph(target_dc,
                      glyph,
                      glyph_bounds,
                      text_color,
                      std::max(1, Scale(1, control_dpi)));
        group_left = glyph_bounds.right + icon_gap;
    }

    RECT text_bounds = bounds;
    text_bounds.left = group_left;
    text_bounds.right = std::min(bounds.right - Scale(8, control_dpi),
                                 group_left + text_size.cx + Scale(2, control_dpi));
    if (pressed) {
        OffsetRect(&text_bounds, 0, Scale(1, control_dpi));
    }
    ui::DrawTextLine(target_dc,
                     text,
                     text_bounds,
                     selected_font,
                     text_color,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (buffer_dc != nullptr) {
        BitBlt(draw.hDC,
               output_bounds.left,
               output_bounds.top,
               output_width,
               output_height,
               buffer_dc,
               0,
               0,
               SRCCOPY);
        SelectObject(buffer_dc, old_bitmap);
        DeleteObject(buffer_bitmap);
        DeleteDC(buffer_dc);
    }

}

void MainWindow::DrawTabControl(const DRAWITEMSTRUCT& draw) const {
    const RECT output_bounds = draw.rcItem;
    const int output_width = output_bounds.right - output_bounds.left;
    const int output_height = output_bounds.bottom - output_bounds.top;
    if (draw.hDC == nullptr || output_width <= 0 || output_height <= 0) {
        return;
    }

    // A selected owner-drawn tab is repainted when keyboard focus leaves it,
    // even though Vector Click intentionally gives selected tabs the same
    // focused and unfocused appearance. Compose that complete repaint off-screen
    // and publish it with one BitBlt so the intermediate Window / panel / text
    // stages cannot become visible during focus transfer (for example, when
    // Emergency Stop is clicked immediately after selecting a page).
    HDC target_dc = draw.hDC;
    HDC buffer_dc = CreateCompatibleDC(draw.hDC);
    HBITMAP buffer_bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    RECT bounds = output_bounds;
    if (buffer_dc != nullptr) {
        buffer_bitmap = CreateCompatibleBitmap(draw.hDC, output_width, output_height);
        if (buffer_bitmap != nullptr) {
            old_bitmap = SelectObject(buffer_dc, buffer_bitmap);
            if (old_bitmap != nullptr && old_bitmap != HGDI_ERROR) {
                target_dc = buffer_dc;
                bounds = {0, 0, output_width, output_height};
            } else {
                DeleteObject(buffer_bitmap);
                buffer_bitmap = nullptr;
            }
        }
        if (buffer_bitmap == nullptr) {
            DeleteDC(buffer_dc);
            buffer_dc = nullptr;
        }
    }

    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(draw.hwndItem));
    const bool selected =
        (draw.hwndItem == basic_tab_button_ && selected_page_index_ == 0) ||
        (draw.hwndItem == advanced_tab_button_ && selected_page_index_ == 1) ||
        (draw.hwndItem == about_tab_button_ && selected_page_index_ == 2);
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;

    // The tab straddles two parent surfaces. Most of its client lies over the
    // main-window tab dock, while its final DPI-rounded rows overlap the page
    // shell. Clear each portion with the surface actually behind it so the
    // antialiased upper corners blend with Window and the lower junctions
    // blend with WindowAlt instead of exposing either rectangular fringe.
    ui::Fill(target_dc, bounds, ui::Window);
    RECT shell_overlap = bounds;
    const LONG shell_top_in_tab = static_cast<LONG>(
        bounds.top + Scale(layout::TabDockTop, control_dpi) -
        Scale(layout::TabTop, control_dpi));
    shell_overlap.top = std::clamp(shell_top_in_tab,
                                   bounds.top,
                                   bounds.bottom);
    if (shell_overlap.top < shell_overlap.bottom) {
        ui::Fill(target_dc, shell_overlap, ui::WindowAlt);
    }
    RECT panel_bounds = bounds;
    InflateRect(&panel_bounds, -Scale(1, control_dpi), -Scale(1, control_dpi));
    ui::DrawDockedTabPanel(target_dc,
                           panel_bounds,
                           selected ? ui::Surface
                                    : pressed ? ui::SurfacePressed
                                              : hot ? ui::SurfaceHover : ui::Window,
                           selected ? ui::Accent : ui::BorderSoft,
                           Scale(layout::TabCornerRadius, control_dpi),
                           Scale(layout::TabDockJunctionRadius, control_dpi));

    if (selected) {
        RECT accent = panel_bounds;
        accent.left += Scale(12, control_dpi);
        accent.right -= Scale(12, control_dpi);
        accent.bottom -= Scale(1, control_dpi);
        accent.top = accent.bottom - std::max(3, Scale(3, control_dpi));
        ui::DrawRoundedPanel(target_dc,
                             accent,
                             ui::AccentHover,
                             ui::AccentHover,
                             Scale(2, control_dpi));
    }

    const HFONT selected_font = reinterpret_cast<HFONT>(
        bold_font_.get() != nullptr ? bold_font_.get()
                                    : font_.get() != nullptr ? font_.get()
                                                             : GetStockObject(DEFAULT_GUI_FONT));
    ui::DrawTextLine(target_dc,
                     GetControlText(draw.hwndItem),
                     bounds,
                     selected_font,
                     selected ? ui::Text : ui::Muted,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (focused && !selected) {
        RECT focus_bounds = panel_bounds;
        InflateRect(&focus_bounds, -Scale(3, control_dpi), -Scale(3, control_dpi));
        ui::DrawRoundedOutline(target_dc,
                               focus_bounds,
                               ui::AccentHover,
                               Scale(7, control_dpi),
                               std::max(1, Scale(1, control_dpi)));
    }

    if (buffer_dc != nullptr && buffer_bitmap != nullptr &&
        old_bitmap != nullptr && old_bitmap != HGDI_ERROR) {
        BitBlt(draw.hDC,
               output_bounds.left,
               output_bounds.top,
               output_width,
               output_height,
               buffer_dc,
               0,
               0,
               SRCCOPY);
        SelectObject(buffer_dc, old_bitmap);
        DeleteObject(buffer_bitmap);
        DeleteDC(buffer_dc);
    }
}

void MainWindow::DrawComboItem(const DRAWITEMSTRUCT& draw) const {
    if (draw.hwndItem == nullptr) {
        return;
    }

    RECT bounds = draw.rcItem;
    const bool enabled = IsWindowEnabled(draw.hwndItem) != FALSE;
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool edit_portion = (draw.itemState & ODS_COMBOBOXEDIT) != 0;
    const COLORREF fill = selected && !edit_portion ? ui::SurfacePressed : ui::SurfaceAlt;
    ui::Fill(draw.hDC, bounds, fill);

    LRESULT item_index = static_cast<LRESULT>(draw.itemID);
    if (item_index < 0) {
        item_index = SendMessageW(draw.hwndItem, CB_GETCURSEL, 0, 0);
    }

    std::wstring text;
    const bool capturing_key = edit_portion && IsKeyCaptureCombo(draw.hwndItem);
    if (capturing_key) {
        text = KeyCapturePrompt;
    } else if (item_index >= 0) {
        const LRESULT length = SendMessageW(draw.hwndItem,
                                            CB_GETLBTEXTLEN,
                                            static_cast<WPARAM>(item_index),
                                            0);
        if (length >= 0) {
            text.resize(static_cast<std::size_t>(length) + 1U);
            if (length > 0) {
                SendMessageW(draw.hwndItem,
                             CB_GETLBTEXT,
                             static_cast<WPARAM>(item_index),
                             reinterpret_cast<LPARAM>(text.data()));
            }
            text.resize(static_cast<std::size_t>(length));
        }
    }

    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(draw.hwndItem));
    RECT text_bounds = bounds;
    if (edit_portion) {
        RECT client{};
        if (GetClientRect(draw.hwndItem, &client) != FALSE &&
            !IsRectEmpty(&client)) {
            const int arrow_width = Scale(28, control_dpi);
            RECT arrow = client;
            arrow.left = std::max(client.left + Scale(20, control_dpi),
                                  client.right - arrow_width);
            text_bounds = client;
            text_bounds.left += Scale(11, control_dpi);
            text_bounds.right = std::max(text_bounds.left,
                                         arrow.left - Scale(8, control_dpi));
        } else {
            text_bounds.left += Scale(11, control_dpi);
            text_bounds.right -= Scale(8, control_dpi);
        }
    } else {
        text_bounds.left += Scale(11, control_dpi);
        text_bounds.right -= Scale(8, control_dpi);
    }
    const HFONT selected_font = reinterpret_cast<HFONT>(
        font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));
    ui::DrawTextLine(draw.hDC,
                     text,
                     text_bounds,
                     selected_font,
                     enabled ? (capturing_key ? ui::AccentHover : ui::Text)
                             : ui::Disabled,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (focused && !edit_portion) {
        ui::DrawFocusOutline(draw.hDC, bounds, ui::AccentHover, Scale(3, control_dpi));
    }
}

void MainWindow::DrawPrintedCombo(const HWND combo,
                                  const HDC destination) const {
    if (combo == nullptr || destination == nullptr ||
        IsWindow(combo) == FALSE) {
        return;
    }

    RECT bounds{};
    if (GetClientRect(combo, &bounds) == FALSE || IsRectEmpty(&bounds)) {
        return;
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;

    const auto paint = [this, combo, bounds](const HDC target) {
        const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(combo));
        const bool enabled = IsWindowEnabled(combo) != FALSE;
        const bool focused = GetFocus() == combo;
        const bool dropped =
            SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
        const bool capturing = IsKeyCaptureCombo(combo);
        const int arrow_width = Scale(28, control_dpi);

        ui::Fill(target, bounds, ui::SurfaceAlt);

        RECT arrow = bounds;
        arrow.left = std::max(bounds.left + Scale(20, control_dpi),
                              bounds.right - arrow_width);
        InflateRect(&arrow, -Scale(1, control_dpi), -Scale(1, control_dpi));

        const std::wstring text = capturing ? std::wstring(KeyCapturePrompt)
                                           : SelectedComboText(combo);
        RECT text_bounds = bounds;
        text_bounds.left += Scale(11, control_dpi);
        text_bounds.right = std::max(text_bounds.left,
                                     arrow.left - Scale(8, control_dpi));
        const HFONT selected_font = reinterpret_cast<HFONT>(
            font_.get() != nullptr ? font_.get()
                                   : GetStockObject(DEFAULT_GUI_FONT));
        ui::DrawTextLine(target,
                         text,
                         text_bounds,
                         selected_font,
                         enabled ? (capturing ? ui::AccentHover : ui::Text)
                                 : ui::Disabled,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                             DT_END_ELLIPSIS);

        ui::Fill(target, arrow, ui::SurfaceAlt);
        const COLORREF border_color =
            enabled && (focused || dropped || capturing)
                ? ui::AccentHover
                : enabled ? ui::Border : ui::BorderSoft;
        const HPEN border_pen = CreatePen(
            PS_SOLID, std::max(1, Scale(1, control_dpi)), border_color);
        if (border_pen != nullptr) {
            const HGDIOBJ old_pen = SelectObject(target, border_pen);
            const HGDIOBJ old_brush =
                SelectObject(target, GetStockObject(NULL_BRUSH));
            Rectangle(target,
                      bounds.left,
                      bounds.top,
                      bounds.right,
                      bounds.bottom);
            MoveToEx(target,
                     arrow.left,
                     bounds.top + Scale(2, control_dpi),
                     nullptr);
            LineTo(target,
                   arrow.left,
                   bounds.bottom - Scale(2, control_dpi));
            SelectObject(target, old_brush);
            SelectObject(target, old_pen);
            DeleteObject(border_pen);
        }

        // The themed native combo contributes a narrow inset highlight on
        // the top and left inside Vector Click's outer frame. Preserve that
        // asymmetric edge in application-rendered movement and transition
        // bitmaps so the field does not appear to lose thickness while the
        // live child hierarchy is covered.
        if (enabled) {
            const int inset = std::max(1, Scale(1, control_dpi));
            const HPEN inset_pen = CreatePen(
                PS_SOLID,
                std::max(1, Scale(1, control_dpi)),
                GetSysColor(COLOR_WINDOWFRAME));
            if (inset_pen != nullptr) {
                const HGDIOBJ old_pen = SelectObject(target, inset_pen);
                MoveToEx(target,
                         bounds.left + inset,
                         bounds.bottom - inset - 1,
                         nullptr);
                LineTo(target,
                       bounds.left + inset,
                       bounds.top + inset);
                LineTo(target,
                       bounds.right - inset,
                       bounds.top + inset);
                SelectObject(target, old_pen);
                DeleteObject(inset_pen);
            }
        }

        RECT chevron = arrow;
        InflateRect(&chevron, -Scale(7, control_dpi), -Scale(7, control_dpi));
        ui::DrawGlyph(target,
                      ui::Glyph::ChevronDown,
                      chevron,
                      enabled ? ui::Icon : ui::Disabled,
                      std::max(1, Scale(2, control_dpi)));
    };

    if (!PaintControlSnapshotLocally(destination, width, height, paint)) {
        paint(destination);
    }
}

void MainWindow::DrawPrintedEdit(const HWND edit,
                                 const HDC destination) const {
    if (edit == nullptr || destination == nullptr || IsWindow(edit) == FALSE) {
        return;
    }

    RECT bounds{};
    if (GetClientRect(edit, &bounds) == FALSE || IsRectEmpty(&bounds)) {
        return;
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;

    const auto paint = [this, edit, bounds](const HDC target) {
        const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(edit));
        ui::Fill(target, bounds, ui::Surface);
        ui::DrawRoundedPanel(target,
                             bounds,
                             ui::SurfaceAlt,
                             ui::SurfaceAlt,
                             Scale(8, control_dpi));

        const int text_length = GetWindowTextLengthW(edit);
        std::wstring text(
            static_cast<std::size_t>(std::max(0, text_length)) + 1U, L'\0');
        if (text_length > 0) {
            GetWindowTextW(edit, text.data(), text_length + 1);
        }
        text.resize(static_cast<std::size_t>(std::max(0, text_length)));

        RECT text_bounds{};
        (void)SendMessageW(edit,
                           EM_GETRECT,
                           0,
                           reinterpret_cast<LPARAM>(&text_bounds));
        if (IsRectEmpty(&text_bounds)) {
            text_bounds = bounds;
            text_bounds.left += Scale(8, control_dpi);
            text_bounds.right -= Scale(34, control_dpi);
        }

        const HFONT selected_font = reinterpret_cast<HFONT>(
            SendMessageW(edit, WM_GETFONT, 0, 0));
        const HFONT draw_font =
            selected_font != nullptr
                ? selected_font
                : reinterpret_cast<HFONT>(
                      font_.get() != nullptr
                          ? font_.get()
                          : GetStockObject(DEFAULT_GUI_FONT));
        ui::DrawTextLine(target,
                         text,
                         text_bounds,
                         draw_font,
                         IsWindowEnabled(edit) != FALSE ? ui::Text
                                                        : ui::Disabled,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                             DT_END_ELLIPSIS);

        numeric_field::VisualState state{};
        state.hot_edit = numeric_hot_edit_;
        state.hot_part = numeric_hot_part_;
        state.pressed_edit = numeric_pressed_edit_;
        state.pressed_part = numeric_pressed_part_;
        numeric_field::DrawChrome(edit, target, state);
    };

    if (!PaintControlSnapshotLocally(destination, width, height, paint)) {
        paint(destination);
    }
}

std::wstring MainWindow::SelectedComboText(const HWND combo) const {
    if (combo == nullptr || IsWindow(combo) == FALSE) {
        return {};
    }

    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
        return {};
    }
    const LRESULT length = SendMessageW(
        combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(selection), 0);
    if (length < 0) {
        return {};
    }

    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    if (SendMessageW(combo,
                     CB_GETLBTEXT,
                     static_cast<WPARAM>(selection),
                     reinterpret_cast<LPARAM>(text.data())) == CB_ERR) {
        return {};
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

bool MainWindow::IsComboSelectionClipped(const HWND combo,
                                         const std::wstring_view text) const {
    if (combo == nullptr || IsWindow(combo) == FALSE || text.empty() ||
        IsKeyCaptureCombo(combo)) {
        return false;
    }

    RECT bounds{};
    if (GetClientRect(combo, &bounds) == FALSE) {
        return false;
    }
    const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(combo));
    const int text_width = std::max(
        0,
        static_cast<int>(bounds.right - bounds.left) -
            Scale(28, control_dpi) - Scale(11, control_dpi) -
            Scale(8, control_dpi));
    if (text_width <= 0) {
        return true;
    }

    HDC dc = GetDC(combo);
    if (dc == nullptr) {
        return false;
    }
    const HGDIOBJ selected_font = font_.get() != nullptr
                                      ? font_.get()
                                      : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ old_font = SelectObject(dc, selected_font);
    SIZE extent{};
    const BOOL measured = GetTextExtentPoint32W(
        dc, text.data(), static_cast<int>(text.size()), &extent);
    if (old_font != nullptr && old_font != HGDI_ERROR) {
        SelectObject(dc, old_font);
    }
    ReleaseDC(combo, dc);
    return measured != FALSE && extent.cx > text_width;
}

void MainWindow::UpdateKeySelectorTooltip(const HWND combo) {
    const wchar_t* description = nullptr;
    if (combo == generated_key_combo_) {
        description = GeneratedKeyTooltip;
    } else if (combo == start_hotkey_combo_) {
        description = StartHotkeyTooltip;
    } else if (combo == emergency_hotkey_combo_) {
        description = EmergencyHotkeyTooltip;
    }
    if (description == nullptr) {
        return;
    }

    std::wstring tooltip(description);
    const std::wstring selection = SelectedComboText(combo);
    if (IsComboSelectionClipped(combo, selection)) {
        tooltip = selection + L"\n\n" + description;
    }
    tooltips_.SetText(combo, std::move(tooltip));
    tooltips_.RefreshVisible(combo);
}

void MainWindow::UpdateKeySelectorTooltips() {
    UpdateKeySelectorTooltip(generated_key_combo_);
    UpdateKeySelectorTooltip(start_hotkey_combo_);
    UpdateKeySelectorTooltip(emergency_hotkey_combo_);
}

int MainWindow::PreferredControlWidth(const HWND control,
                                      const int extra_logical_width,
                                      const int minimum_logical_width) const {
    if (control == nullptr || !IsWindow(control)) {
        return minimum_logical_width;
    }

    const std::wstring text = GetControlText(control);
    HDC dc = GetDC(control);
    if (dc == nullptr) {
        return minimum_logical_width;
    }

    const HGDIOBJ selected_font = font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ old_font = SelectObject(dc, selected_font);
    SIZE text_size{};
    const BOOL measured = GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &text_size);
    SelectObject(dc, old_font);
    ReleaseDC(control, dc);

    if (measured == FALSE) {
        return minimum_logical_width;
    }

    const int logical_text_width = MulDiv(text_size.cx, 96, static_cast<int>(dpi_));
    return std::max(minimum_logical_width, logical_text_width + extra_logical_width);
}


void MainWindow::ApplyFont(const HWND control) const {
    if (control != nullptr && font_.get() != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_.get()), TRUE);
    }
}

void MainWindow::RecreateFontForDpi() {
    UniqueGdiObject new_font(CreateFontW(
        -MulDiv(10, static_cast<int>(dpi_), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
    UniqueGdiObject new_bold_font(CreateFontW(
        -MulDiv(10, static_cast<int>(dpi_), 72),
        0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
    UniqueGdiObject new_action_font(CreateFontW(
        -MulDiv(11, static_cast<int>(dpi_), 72),
        0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
    if (new_font.get() == nullptr) {
        return;
    }

    const auto font_handle = reinterpret_cast<WPARAM>(new_font.get());
    EnumChildWindows(
        window_,
        [](const HWND child, const LPARAM parameter) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(parameter), TRUE);
            return TRUE;
        },
        static_cast<LPARAM>(font_handle));
    font_ = std::move(new_font);
    if (new_bold_font.get() != nullptr) {
        bold_font_ = std::move(new_bold_font);
    }
    if (new_action_font.get() != nullptr) {
        action_font_ = std::move(new_action_font);
    }

    for (const HWND edit : {interval_minutes_edit_,
                            interval_seconds_edit_,
                            interval_edit_,
                            fixed_x_edit_,
                            fixed_y_edit_,
                            repeat_count_edit_,
                            run_time_hours_edit_,
                            run_time_minutes_edit_,
                            run_time_seconds_edit_,
                            button_down_minutes_edit_,
                            button_down_seconds_edit_,
                            button_down_edit_,
                            action_spacing_minutes_edit_,
                            action_spacing_seconds_edit_,
                            action_spacing_edit_,
                            minimum_interval_minutes_edit_,
                            minimum_interval_seconds_edit_,
                            minimum_interval_edit_,
                            maximum_interval_minutes_edit_,
                            maximum_interval_seconds_edit_,
                            maximum_interval_edit_,
                            burst_count_edit_}) {
        if (edit != nullptr && IsWindow(edit) != FALSE) {
            UpdateNumericEditFormatting(edit);
            InvalidateRect(edit, nullptr, FALSE);
        }
    }
}

bool MainWindow::ReadSettings(core::RunSettings& settings, std::wstring& error) const {
    settings = settings_cache_;

    const LRESULT action_selection = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0);
    if (action_selection < 0 || action_selection > 1) {
        error = L"Select an input type.";
        return false;
    }
    settings.action_type = action_selection == 1 ? core::ActionType::KeyboardPress : core::ActionType::MouseClick;

    const LRESULT pattern_selection = SendMessageW(action_pattern_combo_, CB_GETCURSEL, 0, 0);
    if (pattern_selection < 0 || pattern_selection > 4) {
        error = L"Select an action pattern.";
        return false;
    }
    settings.action_pattern = static_cast<core::ActionPattern>(pattern_selection);

    const LRESULT backend_selection = SendMessageW(backend_combo_, CB_GETCURSEL, 0, 0);
    if (backend_selection < 0 || backend_selection > 5) {
        error = L"Select an input method.";
        return false;
    }
    switch (backend_selection) {
    case 0:
        settings.backend = core::InputBackend::Automatic;
        break;
    case 1:
        settings.backend = core::InputBackend::StandardInput;
        break;
    case 2:
        settings.backend = core::InputBackend::ForegroundTargetInput;
        break;
    case 3:
        settings.backend = core::InputBackend::TargetedWindowMessages;
        break;
    case 4:
        settings.backend = core::InputBackend::UnicodeTextInput;
        break;
    case 5:
        settings.backend = core::InputBackend::TargetedUnicodeText;
        break;
    default:
        error = L"Select an input method.";
        return false;
    }

    const LRESULT mouse_selection = SendMessageW(mouse_button_combo_, CB_GETCURSEL, 0, 0);
    if (mouse_selection < 0 || mouse_selection > 4) {
        error = L"Select a mouse button.";
        return false;
    }
    settings.mouse_button = static_cast<core::MouseButton>(mouse_selection);
    const core::HotkeyBinding generated_key = SelectedGeneratedKey();
    settings.generated_virtual_key = generated_key.virtual_key;
    settings.generated_key_modifiers = generated_key.modifiers;
    const bool unicode_text_backend =
        settings.backend == core::InputBackend::UnicodeTextInput ||
        settings.backend == core::InputBackend::TargetedUnicodeText;
    if (unicode_text_backend) {
        if (settings.action_type != core::ActionType::KeyboardPress) {
            error = L"Unicode text input methods are available only for Keyboard press.";
            return false;
        }
        if (!UnicodeTextCharacterForKey(
                settings.generated_virtual_key,
                settings.generated_key_modifiers).has_value()) {
            error = L"Unicode text input methods require a printable keyboard character. Choose a letter, number, symbol, Space, or printable numpad key.";
            return false;
        }
    }

    auto try_parse_duration = [&](
        const DurationEditSet& edits,
        core::DurationComponents& components,
        std::uint64_t& output) -> bool {
        return TryReadDurationEdits(edits, components, output);
    };

    auto parse_duration = [&](
        const DurationEditSet& edits,
        const wchar_t* name,
        core::DurationComponents& components,
        std::uint64_t& output) -> bool {
        if (try_parse_duration(edits, components, output)) {
            return true;
        }
        error = std::wstring(name) +
                L" must use valid Minutes, Seconds, and Milliseconds values from 0 through 1,000,000. Milliseconds may use up to three decimal places.";
        return false;
    };

    const bool hold_mode = settings.action_pattern == core::ActionPattern::Hold;
    settings.randomize_interval = IsChecked(random_interval_check_);
    const LRESULT random_style_selection =
        SendMessageW(random_interval_style_combo_, CB_GETCURSEL, 0, 0);
    if (random_style_selection < 0 || random_style_selection > 2) {
        error = L"Select a random interval variation style.";
        return false;
    }
    switch (random_style_selection) {
    case 1:
        settings.random_interval_style = core::RandomIntervalStyle::Drifting;
        break;
    case 2:
        settings.random_interval_style = core::RandomIntervalStyle::Natural;
        break;
    default:
        settings.random_interval_style = core::RandomIntervalStyle::Independent;
        break;
    }
    const LRESULT down_behavior_selection =
        SendMessageW(down_duration_behavior_combo_, CB_GETCURSEL, 0, 0);
    if (down_behavior_selection < 0 || down_behavior_selection > 2) {
        error = L"Select a Down duration behavior.";
        return false;
    }
    switch (down_behavior_selection) {
    case 1:
        settings.down_duration_behavior =
            core::DownDurationBehavior::NaturalConfiguredCenter;
        break;
    case 2:
        settings.down_duration_behavior =
            core::DownDurationBehavior::NaturalAutomaticCenter;
        break;
    default:
        settings.down_duration_behavior = core::DownDurationBehavior::Fixed;
        break;
    }
    if (!hold_mode) {
        if (settings.down_duration_behavior ==
            core::DownDurationBehavior::NaturalAutomaticCenter) {
            (void)try_parse_duration(
                ButtonDownDurationEdits(),
                settings.button_down_components,
                settings.button_down_microseconds);
        } else if (!parse_duration(
                       ButtonDownDurationEdits(),
                       L"Down duration",
                       settings.button_down_components,
                       settings.button_down_microseconds)) {
            return false;
        }
        if (settings.randomize_interval) {
            (void)try_parse_duration(
                IntervalDurationEdits(),
                settings.interval_components,
                settings.interval_microseconds);
            if (!parse_duration(
                    MinimumIntervalDurationEdits(),
                    L"Minimum interval",
                    settings.minimum_interval_components,
                    settings.minimum_interval_microseconds) ||
                !parse_duration(
                    MaximumIntervalDurationEdits(),
                    L"Maximum interval",
                    settings.maximum_interval_components,
                    settings.maximum_interval_microseconds)) {
                return false;
            }
        } else {
            if (!parse_duration(
                    IntervalDurationEdits(),
                    L"Input interval",
                    settings.interval_components,
                    settings.interval_microseconds)) {
                return false;
            }
            (void)try_parse_duration(
                MinimumIntervalDurationEdits(),
                settings.minimum_interval_components,
                settings.minimum_interval_microseconds);
            (void)try_parse_duration(
                MaximumIntervalDurationEdits(),
                settings.maximum_interval_components,
                settings.maximum_interval_microseconds);
        }
    } else {
        // Hold does not use these values, but preserve valid component edits
        // made before switching patterns so remembered presentation does not
        // revert to an older representation.
        (void)try_parse_duration(
            IntervalDurationEdits(),
            settings.interval_components,
            settings.interval_microseconds);
        (void)try_parse_duration(
            ButtonDownDurationEdits(),
            settings.button_down_components,
            settings.button_down_microseconds);
        (void)try_parse_duration(
            MinimumIntervalDurationEdits(),
            settings.minimum_interval_components,
            settings.minimum_interval_microseconds);
        (void)try_parse_duration(
            MaximumIntervalDurationEdits(),
            settings.maximum_interval_components,
            settings.maximum_interval_microseconds);
    }

    const bool multi_input_action =
        settings.action_pattern == core::ActionPattern::Double ||
        settings.action_pattern == core::ActionPattern::Triple ||
        settings.action_pattern == core::ActionPattern::Burst;
    if (multi_input_action) {
        if (!parse_duration(
                ActionSpacingDurationEdits(),
                L"Action spacing",
                settings.action_spacing_components,
                settings.action_spacing_microseconds)) {
            return false;
        }
    } else {
        (void)try_parse_duration(
            ActionSpacingDurationEdits(),
            settings.action_spacing_components,
            settings.action_spacing_microseconds);
    }

    const auto burst_text_storage = GetControlText(burst_count_edit_);
    const std::wstring burst_text(TrimNumericText(burst_text_storage));
    const bool burst_text_is_digits =
        !burst_text.empty() &&
        std::ranges::all_of(burst_text, [](const wchar_t character) { return character >= L'0' && character <= L'9'; });
    wchar_t* burst_end{};
    const unsigned long long parsed_burst_count =
        burst_text_is_digits ? std::wcstoull(burst_text.c_str(), &burst_end, 10) : 0;
    const bool burst_count_valid =
        burst_text_is_digits && burst_end != burst_text.c_str() && *burst_end == L'\0' &&
        parsed_burst_count <= static_cast<unsigned long long>(std::numeric_limits<std::uint32_t>::max());
    if (burst_count_valid) {
        settings.burst_count = static_cast<std::uint32_t>(parsed_burst_count);
    } else if (settings.action_pattern == core::ActionPattern::Burst) {
        error = L"Burst count must be a whole number from 2 through 100.";
        return false;
    }

    settings.position_mode = IsChecked(fixed_position_radio_)
                                 ? core::PositionMode::FixedScreen
                                 : core::PositionMode::CurrentCursor;
    if (!ParseSignedCoordinate(fixed_x_edit_, settings.fixed_x) ||
        !ParseSignedCoordinate(fixed_y_edit_, settings.fixed_y)) {
        error = L"Fixed X and Y must be valid whole-number screen coordinates.";
        return false;
    }
    if (settings.action_type == core::ActionType::MouseClick &&
        settings.position_mode == core::PositionMode::FixedScreen &&
        !IsPointOnVirtualDesktop(settings.fixed_x, settings.fixed_y)) {
        error = L"The fixed mouse position is outside the current virtual desktop.";
        return false;
    }

    settings.repeat_mode = IsChecked(limited_radio_)
                               ? core::RepeatMode::Limited
                               : core::RepeatMode::Unlimited;
    const auto repeat_text_storage = GetControlText(repeat_count_edit_);
    const std::wstring repeat_text(TrimNumericText(repeat_text_storage));
    const bool repeat_text_is_digits =
        !repeat_text.empty() &&
        std::ranges::all_of(repeat_text, [](const wchar_t character) { return character >= L'0' && character <= L'9'; });
    wchar_t* repeat_end{};
    const unsigned long long parsed_repeat =
        repeat_text_is_digits ? std::wcstoull(repeat_text.c_str(), &repeat_end, 10) : 0;
    const bool repeat_count_valid =
        repeat_text_is_digits && repeat_end != repeat_text.c_str() && *repeat_end == L'\0' &&
        parsed_repeat >= 1U &&
        parsed_repeat <= static_cast<unsigned long long>(core::MaximumRepeatCount);
    if (repeat_count_valid) {
        settings.repeat_count = static_cast<std::uint64_t>(parsed_repeat);
    } else if (!hold_mode && settings.repeat_mode == core::RepeatMode::Limited) {
        error = L"Limited repeat count must be a whole number from 1 through 1,000,000.";
        return false;
    }

    if (!TryReadRunTimeLimitEdits(
            settings.run_time_limit_components,
            settings.run_time_limit_microseconds)) {
        error = L"Run time limit must use valid Hours, Minutes, and Seconds values from 0 through 1,000,000.";
        return false;
    }

    settings.start_stop_hotkey = SelectedHotkey(start_hotkey_combo_);
    settings.emergency_hotkey = SelectedHotkey(emergency_hotkey_combo_);

    settings.enable_live_diagnostics = IsChecked(diagnostics_check_);
    settings.show_safety_shield = IsChecked(safety_shield_check_);
    settings.force_exit_on_emergency_stop =
        IsChecked(force_exit_on_emergency_stop_check_);
    settings.hide_from_screen_capture = IsChecked(capture_exclusion_check_);
    settings.keep_window_on_top = IsChecked(keep_on_top_check_);
    settings.remember_settings = IsChecked(remember_settings_check_);
    settings.allow_background_input = IsChecked(background_input_check_);
    settings.show_click_position_indicator = IsChecked(click_position_indicator_check_);
    settings.process_priority_mode = SelectedProcessPriorityMode();
    settings.timing_worker_priority_mode = SelectedTimingWorkerPriorityMode();
    settings.hotkey_control_priority_mode = SelectedHotkeyControlPriorityMode();
    settings.timing_worker_qos_mode = SelectedTimingWorkerQosMode();
    settings.windows_notification_mode =
        SelectedRunFeedbackMode(windows_notification_combo_);
    settings.system_sound_mode = SelectedRunFeedbackMode(system_sound_combo_);
    settings.show_running_indicator = IsChecked(running_indicator_check_);

    const auto issues = core::ValidateRunSettings(settings);
    if (core::HasErrors(issues)) {
        std::wstring message;
        for (const auto& issue : issues) {
            if (!message.empty()) {
                message.push_back(L' ');
            }
            message.append(issue.message.begin(), issue.message.end());
        }
        error = std::move(message);
        return false;
    }
    return true;
}

void MainWindow::ApplySettings(const core::RunSettings& settings) {
    suppress_control_events_ = true;
    SendMessageW(action_type_combo_, CB_SETCURSEL,
                 settings.action_type == core::ActionType::KeyboardPress ? 1 : 0, 0);
    SendMessageW(action_pattern_combo_, CB_SETCURSEL,
                 static_cast<WPARAM>(settings.action_pattern), 0);
    int backend_selection = 0;
    if (settings.backend == core::InputBackend::StandardInput) {
        backend_selection = 1;
    } else if (settings.backend == core::InputBackend::ForegroundTargetInput) {
        backend_selection = 2;
    } else if (settings.backend == core::InputBackend::TargetedWindowMessages) {
        backend_selection = 3;
    } else if (settings.backend == core::InputBackend::UnicodeTextInput) {
        backend_selection = 4;
    } else if (settings.backend == core::InputBackend::TargetedUnicodeText) {
        backend_selection = 5;
    }
    SendMessageW(backend_combo_, CB_SETCURSEL, static_cast<WPARAM>(backend_selection), 0);
    SendMessageW(mouse_button_combo_, CB_SETCURSEL, static_cast<WPARAM>(settings.mouse_button), 0);
    SelectGeneratedKey({settings.generated_virtual_key, settings.generated_key_modifiers});

    auto resolved_components = [](
        const core::DurationComponents& requested,
        const std::uint64_t authoritative) {
        if (core::DurationComponentsMatch(requested, authoritative)) {
            return requested;
        }
        core::DurationComponents fallback{};
        (void)core::DecomposeDurationMicroseconds(
            authoritative, fallback);
        return fallback;
    };
    SetDurationEdits(
        IntervalDurationEdits(),
        resolved_components(
            settings.interval_components, settings.interval_microseconds));
    SetChecked(random_interval_check_, settings.randomize_interval);
    int random_style_selection = 0;
    if (settings.random_interval_style == core::RandomIntervalStyle::Drifting) {
        random_style_selection = 1;
    } else if (settings.random_interval_style == core::RandomIntervalStyle::Natural) {
        random_style_selection = 2;
    }
    SendMessageW(
        random_interval_style_combo_, CB_SETCURSEL,
        static_cast<WPARAM>(random_style_selection), 0);
    int down_behavior_selection = 0;
    if (settings.down_duration_behavior ==
        core::DownDurationBehavior::NaturalConfiguredCenter) {
        down_behavior_selection = 1;
    } else if (settings.down_duration_behavior ==
               core::DownDurationBehavior::NaturalAutomaticCenter) {
        down_behavior_selection = 2;
    }
    SendMessageW(
        down_duration_behavior_combo_, CB_SETCURSEL,
        static_cast<WPARAM>(down_behavior_selection), 0);
    SetDurationEdits(
        MinimumIntervalDurationEdits(),
        resolved_components(settings.minimum_interval_components,
                            settings.minimum_interval_microseconds));
    SetDurationEdits(
        MaximumIntervalDurationEdits(),
        resolved_components(settings.maximum_interval_components,
                            settings.maximum_interval_microseconds));
    SetDurationEdits(
        ButtonDownDurationEdits(),
        resolved_components(settings.button_down_components,
                            settings.button_down_microseconds));
    SetControlText(burst_count_edit_, std::to_wstring(settings.burst_count));
    SetDurationEdits(
        ActionSpacingDurationEdits(),
        resolved_components(settings.action_spacing_components,
                            settings.action_spacing_microseconds));

    SetRadioPair(current_cursor_radio_, fixed_position_radio_,
                 settings.position_mode == core::PositionMode::FixedScreen ? fixed_position_radio_ : current_cursor_radio_);
    SetControlText(fixed_x_edit_, std::to_wstring(settings.fixed_x));
    SetControlText(fixed_y_edit_, std::to_wstring(settings.fixed_y));

    SetRadioPair(unlimited_radio_, limited_radio_,
                 settings.repeat_mode == core::RepeatMode::Limited ? limited_radio_ : unlimited_radio_);
    SetControlText(repeat_count_edit_, std::to_wstring(settings.repeat_count));
    core::RunTimeLimitComponents run_time_limit_components =
        settings.run_time_limit_components;
    if (!core::RunTimeLimitComponentsMatch(
            run_time_limit_components, settings.run_time_limit_microseconds)) {
        (void)core::DecomposeRunTimeLimitMicroseconds(
            settings.run_time_limit_microseconds, run_time_limit_components);
    }
    SetRunTimeLimitEdits(run_time_limit_components);

    SelectHotkey(start_hotkey_combo_, settings.start_stop_hotkey);
    SelectHotkey(emergency_hotkey_combo_, settings.emergency_hotkey);
    SetChecked(diagnostics_check_, settings.enable_live_diagnostics);
    SetChecked(safety_shield_check_, settings.show_safety_shield);
    SetChecked(force_exit_on_emergency_stop_check_,
               settings.force_exit_on_emergency_stop);
    SetChecked(capture_exclusion_check_, settings.hide_from_screen_capture);
    SetChecked(keep_on_top_check_, settings.keep_window_on_top);
    safety_shield_requested_.store(
        settings.show_safety_shield, std::memory_order_release);
    force_exit_on_emergency_stop_requested_.store(
        settings.force_exit_on_emergency_stop, std::memory_order_release);
    SetChecked(remember_settings_check_, settings.remember_settings);
    SetChecked(background_input_check_, settings.allow_background_input);
    SetChecked(click_position_indicator_check_,
               settings.show_click_position_indicator && click_position_indicator_available_);
    SelectProcessPriorityMode(settings.process_priority_mode);
    SelectTimingWorkerPriorityMode(settings.timing_worker_priority_mode);
    SelectHotkeyControlPriorityMode(settings.hotkey_control_priority_mode);
    SelectTimingWorkerQosMode(settings.timing_worker_qos_mode);
    SelectRunFeedbackMode(
        windows_notification_combo_, settings.windows_notification_mode);
    SelectRunFeedbackMode(system_sound_combo_, settings.system_sound_mode);
    SetChecked(running_indicator_check_, settings.show_running_indicator);
    // One buffered footer control presents both ordinary status and live
    // diagnostics. The retained diagnostics text child remains hidden and is
    // used only as an internal text store.
    ShowWindow(status_text_, SW_SHOW);
    ShowWindow(diagnostics_text_, SW_HIDE);
    if (settings.enable_live_diagnostics) {
        SetTimer(window_, DiagnosticsTimerId, DiagnosticsTimerMilliseconds, nullptr);
    } else {
        KillTimer(window_, DiagnosticsTimerId);
    }
    UpdateVisibleStatusTooltip();
    suppress_control_events_ = false;

    if (process_priority_manager_.Mode() != settings.process_priority_mode) {
        std::wstring priority_error;
        if (!process_priority_manager_.SetMode(
                settings.process_priority_mode, priority_error)) {
            suppress_control_events_ = true;
            SelectProcessPriorityMode(process_priority_manager_.Mode());
            suppress_control_events_ = false;
            settings_cache_.process_priority_mode = process_priority_manager_.Mode();
            if (!priority_error.empty()) {
                ShowCenteredMessageBox(
                    window_,
                    priority_error.c_str(),
                    L"Process priority unchanged",
                    MB_OK | MB_ICONWARNING);
            }
        }
    }

    UpdateActionControls();
    UpdateActionPatternControls();
    RefreshEnabledState();
}

core::ProcessPriorityMode MainWindow::SelectedProcessPriorityMode() const noexcept {
    if (process_priority_combo_ == nullptr || IsWindow(process_priority_combo_) == FALSE) {
        return core::ProcessPriorityMode::SystemDefault;
    }
    const LRESULT selection = SendMessageW(process_priority_combo_, CB_GETCURSEL, 0, 0);
    if (selection < 0 || selection > 4) {
        return core::ProcessPriorityMode::SystemDefault;
    }
    return static_cast<core::ProcessPriorityMode>(selection);
}

void MainWindow::SelectProcessPriorityMode(
    const core::ProcessPriorityMode mode) noexcept {
    if (process_priority_combo_ == nullptr || IsWindow(process_priority_combo_) == FALSE) {
        return;
    }
    SendMessageW(process_priority_combo_, CB_SETCURSEL,
                 static_cast<WPARAM>(mode), 0);
}

void MainWindow::HandleProcessPrioritySelection() {
    const core::ProcessPriorityMode requested = SelectedProcessPriorityMode();
    const core::ProcessPriorityMode previous = process_priority_manager_.Mode();
    std::wstring error;
    if (!process_priority_manager_.SetMode(requested, error)) {
        suppress_control_events_ = true;
        SelectProcessPriorityMode(previous);
        suppress_control_events_ = false;
        if (!error.empty()) {
            ShowCenteredMessageBox(
                window_, error.c_str(), L"Process priority unchanged",
                MB_OK | MB_ICONWARNING);
        }
        return;
    }

    settings_cache_.process_priority_mode = requested;
    if (hotkey_thread_) {
        (void)hotkey_thread_->RefreshPriorityPolicy();
    }
    ScheduleSettingsSave();
    RefreshStartAvailability();
    MarkMoveCoverPresentationDirty();
}

core::TimingWorkerPriorityMode MainWindow::SelectedTimingWorkerPriorityMode() const noexcept {
    if (timing_worker_priority_combo_ == nullptr ||
        IsWindow(timing_worker_priority_combo_) == FALSE) {
        return core::TimingWorkerPriorityMode::SystemDefault;
    }
    const LRESULT selection = SendMessageW(
        timing_worker_priority_combo_, CB_GETCURSEL, 0, 0);
    if (selection < 0 || selection > 2) {
        return core::TimingWorkerPriorityMode::SystemDefault;
    }
    return static_cast<core::TimingWorkerPriorityMode>(selection);
}

void MainWindow::SelectTimingWorkerPriorityMode(
    const core::TimingWorkerPriorityMode mode) noexcept {
    if (timing_worker_priority_combo_ == nullptr ||
        IsWindow(timing_worker_priority_combo_) == FALSE) {
        return;
    }
    SendMessageW(timing_worker_priority_combo_, CB_SETCURSEL,
                 static_cast<WPARAM>(mode), 0);
}

void MainWindow::HandleTimingWorkerPrioritySelection() {
    settings_cache_.timing_worker_priority_mode = SelectedTimingWorkerPriorityMode();
    RefreshStartAvailability();
    MarkMoveCoverPresentationDirty();
}

core::HotkeyControlPriorityMode MainWindow::SelectedHotkeyControlPriorityMode() const noexcept {
    const LRESULT selection =
        SendMessageW(hotkey_control_priority_combo_, CB_GETCURSEL, 0, 0);
    return selection == 1 ? core::HotkeyControlPriorityMode::AboveNormal
                          : core::HotkeyControlPriorityMode::SystemDefault;
}

void MainWindow::SelectHotkeyControlPriorityMode(
    const core::HotkeyControlPriorityMode mode) noexcept {
    SendMessageW(hotkey_control_priority_combo_,
                 CB_SETCURSEL,
                 mode == core::HotkeyControlPriorityMode::AboveNormal ? 1 : 0,
                 0);
}

void MainWindow::HandleHotkeyControlPrioritySelection() {
    settings_cache_.hotkey_control_priority_mode = SelectedHotkeyControlPriorityMode();
    if (hotkey_thread_ &&
        !hotkey_thread_->SetPriorityMode(settings_cache_.hotkey_control_priority_mode)) {
        SetStatusPresentation({
            StatusCategory::Attention,
            L"Status: Hotkey / control priority update could not be queued",
            {},
        }, true);
    }
    RefreshStartAvailability();
    MarkMoveCoverPresentationDirty();
}


core::TimingWorkerQosMode MainWindow::SelectedTimingWorkerQosMode() const noexcept {
    if (timing_worker_qos_combo_ == nullptr ||
        IsWindow(timing_worker_qos_combo_) == FALSE) {
        return core::TimingWorkerQosMode::SystemManaged;
    }
    const LRESULT selection =
        SendMessageW(timing_worker_qos_combo_, CB_GETCURSEL, 0, 0);
    if (selection == 1) {
        return core::TimingWorkerQosMode::HighQoS;
    }
    if (selection == 2) {
        return core::TimingWorkerQosMode::EcoQoS;
    }
    return core::TimingWorkerQosMode::SystemManaged;
}

void MainWindow::SelectTimingWorkerQosMode(
    const core::TimingWorkerQosMode mode) noexcept {
    if (timing_worker_qos_combo_ == nullptr ||
        IsWindow(timing_worker_qos_combo_) == FALSE) {
        return;
    }
    int selection = 0;
    if (mode == core::TimingWorkerQosMode::HighQoS) {
        selection = 1;
    } else if (mode == core::TimingWorkerQosMode::EcoQoS) {
        selection = 2;
    }
    SendMessageW(timing_worker_qos_combo_, CB_SETCURSEL,
                 static_cast<WPARAM>(selection), 0);
}

void MainWindow::HandleTimingWorkerQosSelection() {
    settings_cache_.timing_worker_qos_mode = SelectedTimingWorkerQosMode();
    RefreshStartAvailability();
    MarkMoveCoverPresentationDirty();
}

core::RunFeedbackMode MainWindow::SelectedRunFeedbackMode(
    const HWND combo) const noexcept {
    if (combo == nullptr || IsWindow(combo) == FALSE) {
        return core::RunFeedbackMode::Off;
    }
    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selection < 0 || selection > 3) {
        return core::RunFeedbackMode::Off;
    }
    return static_cast<core::RunFeedbackMode>(selection);
}

void MainWindow::SelectRunFeedbackMode(
    const HWND combo,
    const core::RunFeedbackMode mode) noexcept {
    if (combo == nullptr || IsWindow(combo) == FALSE) {
        return;
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(mode), 0);
}

void MainWindow::PresentRunFeedbackNotification(
    const std::wstring_view title,
    const std::wstring_view body) {
    if (!run_feedback_presenter_.ShowNotification(title, body)) {
        return;
    }
    if (StartGeneratedTimer(run_notification_cleanup_timer_id_,
                            RunNotificationCleanupMilliseconds) == 0) {
        run_feedback_presenter_.RemoveNotificationIcon();
    }
}

void MainWindow::BeginRunFeedback() {
    if (run_feedback_active_) {
        return;
    }

    run_feedback_active_ = true;

    const core::RunSettings& settings = settings_cache_;
    if (settings.show_running_indicator) {
        run_feedback_presenter_.SetRunningIndicator(true);
    }
    if (core::RunFeedbackIncludesStarted(settings.windows_notification_mode)) {
        PresentRunFeedbackNotification(
            L"Vector Click is running",
            L"The active run started successfully.");
    }
    if (core::RunFeedbackIncludesStarted(settings.system_sound_mode)) {
        run_feedback_presenter_.PlayStartedSound();
    }
}

void MainWindow::HandleRunFeedbackTerminal(
    const EngineState state,
    const bool emergency_context,
    const bool release_warning) {
    if (!run_feedback_active_) {
        return;
    }

    // A terminal engine state can still leave a tracked release unresolved.
    // Keep the active indicator visible and defer all Stop feedback until the
    // same run has crossed its actual release-cleanup boundary. Retry cleanup
    // will call this method again after the ledger is clear.
    if (CleanupRequired()) {
        return;
    }

    if (shutdown_in_progress_.load(std::memory_order_acquire)) {
        run_feedback_presenter_.SetRunningIndicator(false);
        run_feedback_active_ = false;
        return;
    }

    const core::RunSettings& settings = settings_cache_;
    if (core::RunFeedbackIncludesStopped(settings.windows_notification_mode)) {
        if (emergency_context) {
            if (release_warning) {
                PresentRunFeedbackNotification(
                    L"Emergency Stop completed",
                    L"Cleanup completed with a release warning. Verify the target before starting again.");
            } else {
                PresentRunFeedbackNotification(
                    L"Emergency Stop completed",
                    L"Generated input has been released.");
            }
        } else if (controller_ && controller_->LastRunHadBackendFailure()) {
            PresentRunFeedbackNotification(
                L"Vector Click stopped",
                L"The input target or Windows input path became unavailable.");
        } else if (controller_) {
            switch (controller_->LastRunLimitStopReason()) {
            case RunLimitStopReason::RepeatLimit:
                PresentRunFeedbackNotification(
                    L"Vector Click stopped", L"Repeat limit reached.");
                break;
            case RunLimitStopReason::TimeLimit:
                PresentRunFeedbackNotification(
                    L"Vector Click stopped", L"Time limit reached.");
                break;
            case RunLimitStopReason::None:
                if (state == EngineState::Faulted || release_warning) {
                    PresentRunFeedbackNotification(
                        L"Vector Click stopped",
                        L"The run ended with a cleanup or input warning.");
                } else {
                    PresentRunFeedbackNotification(
                        L"Vector Click stopped", L"Stopped manually.");
                }
                break;
            }
        } else {
            PresentRunFeedbackNotification(
                L"Vector Click stopped", L"Stopped manually.");
        }
    }
    if (core::RunFeedbackIncludesStopped(settings.system_sound_mode)) {
        run_feedback_presenter_.PlayStoppedSound();
    }

    run_feedback_presenter_.SetRunningIndicator(false);
    run_feedback_active_ = false;
}

bool MainWindow::BeginProcessPriorityActive() {
    std::wstring error;
    if (process_priority_manager_.BeginActive(error)) {
        if (hotkey_thread_) {
            (void)hotkey_thread_->RefreshPriorityPolicy();
        }
        return true;
    }
    if (!error.empty()) {
        ShowCenteredMessageBox(
            window_,
            error.c_str(),
            L"Cannot start with selected process priority",
            MB_OK | MB_ICONWARNING);
    }
    return false;
}

void MainWindow::EndProcessPriorityActive(const bool show_error) {
    if (!process_priority_manager_.IsActive()) {
        return;
    }
    std::wstring error;
    const bool restored = process_priority_manager_.EndActive(error);
    if (hotkey_thread_) {
        (void)hotkey_thread_->RefreshPriorityPolicy();
    }
    if (restored || error.empty()) {
        return;
    }
    if (show_error) {
        ShowCenteredMessageBox(
            window_,
            error.c_str(),
            L"Process priority restore warning",
            MB_OK | MB_ICONWARNING);
    } else {
        SetStatusPresentation({
            StatusCategory::Attention,
            L"Status: Run ended, but Windows did not restore the earlier process priority",
            error,
        }, true);
    }
}

void MainWindow::DisableActiveRunHotkeyGuard() noexcept {
    if (!active_run_hotkey_guard_active_) {
        return;
    }
    if (hotkey_thread_ == nullptr) {
        return;
    }

    std::wstring ignored_error;
    if (hotkey_thread_->SetActiveRunModifierGuard(false, ignored_error)) {
        active_run_hotkey_guard_active_ = false;
    }
}

bool MainWindow::IsNumericEditControl(const HWND control) const noexcept {
    if (control == nullptr) {
        return false;
    }
    for (const HWND edit : {
             interval_minutes_edit_, interval_seconds_edit_, interval_edit_,
             button_down_minutes_edit_, button_down_seconds_edit_, button_down_edit_,
             action_spacing_minutes_edit_, action_spacing_seconds_edit_, action_spacing_edit_,
             minimum_interval_minutes_edit_, minimum_interval_seconds_edit_, minimum_interval_edit_,
             maximum_interval_minutes_edit_, maximum_interval_seconds_edit_, maximum_interval_edit_,
             fixed_x_edit_, fixed_y_edit_, repeat_count_edit_,
             run_time_hours_edit_, run_time_minutes_edit_, run_time_seconds_edit_,
             burst_count_edit_}) {
        if (control == edit) {
            return true;
        }
    }
    return false;
}

void MainWindow::BeginNumericEditHistorySession(const HWND control) {
    if (!IsNumericEditControl(control) || IsWindow(control) == FALSE) {
        return;
    }
    numeric_edit_history_control_ = control;
    numeric_edit_history_.Begin(GetControlText(control));
}

void MainWindow::EndNumericEditHistorySession(const HWND control) noexcept {
    if (numeric_edit_history_control_ != control) {
        return;
    }
    numeric_edit_history_.End();
    numeric_edit_history_control_ = nullptr;
}

void MainWindow::NoteNumericEditHistoryTextChanged(const HWND control) {
    if (numeric_edit_history_update_in_progress_ ||
        numeric_edit_history_control_ != control ||
        !numeric_edit_history_.IsActive()) {
        return;
    }
    numeric_edit_history_.NoteUserEdit();
}

void MainWindow::SynchronizeNumericEditHistorySession() {
    if (!numeric_edit_history_.IsActive() ||
        numeric_edit_history_control_ == nullptr ||
        IsWindow(numeric_edit_history_control_) == FALSE) {
        return;
    }
    numeric_edit_history_.Synchronize(
        GetControlText(numeric_edit_history_control_));
}

void MainWindow::ApplyNumericEditHistoryText(
    const HWND control,
    const std::wstring& text) {
    if (control == nullptr || IsWindow(control) == FALSE ||
        !IsNumericEditControl(control)) {
        return;
    }

    const bool previous_update_state = numeric_edit_history_update_in_progress_;
    numeric_edit_history_update_in_progress_ = true;
    SetControlText(control, text);
    numeric_edit_history_update_in_progress_ = previous_update_state;

    // Numeric settings are edited as complete values. Keep the restored value
    // selected so the user can immediately replace it again if desired.
    SendMessageW(control, EM_SETSEL, 0, -1);
    FlushNumericPresentationRefresh();
}

bool MainWindow::HandleNumericEditHistoryShortcut(const HWND control,
                                                   const bool redo) {
    if (!CanUseSettingsHistory() || !IsNumericEditControl(control) ||
        IsWindow(control) == FALSE) {
        return false;
    }

    if (numeric_edit_history_control_ != control ||
        !numeric_edit_history_.IsActive()) {
        BeginNumericEditHistorySession(control);
    }
    if (numeric_edit_history_control_ != control ||
        !numeric_edit_history_.IsActive()) {
        return false;
    }

    const std::wstring current = GetControlText(control);
    if (!redo) {
        const std::optional<std::wstring> target =
            numeric_edit_history_.UndoTarget(current);
        if (!target.has_value()) {
            return false;
        }
        ApplyNumericEditHistoryText(control, *target);
        return true;
    }

    const std::optional<std::wstring> target =
        numeric_edit_history_.RedoTarget(current);
    if (target.has_value()) {
        ApplyNumericEditHistoryText(control, *target);
        return true;
    }

    // Do not let application-level Redo jump past a currently edited value.
    // The in-progress numeric transaction must first be undone to its baseline
    // or committed by leaving the field.
    return numeric_edit_history_.HasUncommittedChange(current);
}

core::RunSettings MainWindow::NormalizeSettingsHistoryState(
    core::RunSettings settings) const noexcept {
    // These settings perform persistence, privacy, or operating-system
    // transactions outside ordinary configuration state and are intentionally
    // excluded from session Undo / Redo. Target-window identity is not part of RunSettings
    // at all and therefore cannot enter this history.
    settings.hide_from_screen_capture = false;
    settings.remember_settings = false;
    settings.process_priority_mode = core::ProcessPriorityMode::SystemDefault;
    settings.timing_worker_priority_mode = core::TimingWorkerPriorityMode::SystemDefault;
    settings.hotkey_control_priority_mode = core::HotkeyControlPriorityMode::SystemDefault;
    settings.timing_worker_qos_mode = core::TimingWorkerQosMode::SystemManaged;
    return settings;
}

bool MainWindow::CaptureSettingsHistoryState(
    core::RunSettings& settings) const {
    std::wstring error;
    if (!ReadSettings(settings, error) || !SafetyHotkeysReady()) {
        return false;
    }
    // Controls are restored to the confirmed pair while Windows evaluates a
    // replacement, but make the runtime authority explicit in every snapshot.
    settings.start_stop_hotkey = confirmed_start_hotkey_;
    settings.emergency_hotkey = confirmed_emergency_hotkey_;
    settings = NormalizeSettingsHistoryState(std::move(settings));
    return true;
}

void MainWindow::CommitSettingsHistoryFromControls() {
    if (settings_history_transaction_pending_ || hotkey_registration_pending_) {
        return;
    }
    core::RunSettings current{};
    if (!CaptureSettingsHistoryState(current)) {
        return;
    }
    if (!settings_history_.IsInitialized()) {
        settings_history_.Reset(current);
        return;
    }
    (void)settings_history_.Record(current);
}

void MainWindow::SynchronizeSettingsHistoryFromControls() {
    if (settings_history_transaction_pending_ || hotkey_registration_pending_) {
        return;
    }
    core::RunSettings current{};
    if (!CaptureSettingsHistoryState(current)) {
        return;
    }
    settings_history_.ReplaceCurrent(current);
}

bool MainWindow::CanUseSettingsHistory() const noexcept {
    if (!settings_history_.IsInitialized() || settings_import_picker_open_ ||
        settings_import_transaction_pending_ ||
        settings_history_transaction_pending_ || hotkey_registration_pending_ ||
        key_capture_active_.load(std::memory_order_acquire) ||
        shutdown_in_progress_.load(std::memory_order_acquire) ||
        emergency_latched_.load(std::memory_order_acquire) ||
        safety_shield_.IsVisible() || target_picker_open_ ||
        position_capture_seconds_remaining_ != 0 || CleanupRequired()) {
        return false;
    }
    if (combo_popup_ != nullptr && IsWindow(combo_popup_) != FALSE &&
        IsWindowVisible(combo_popup_) != FALSE) {
        return false;
    }
    if (controller_ == nullptr) {
        return false;
    }
    const EngineState state = controller_->State();
    return state == EngineState::Ready || state == EngineState::Disarmed;
}

bool MainWindow::ApplySettingsHistoryState(
    const core::RunSettings& settings) {
    if (!SafetyHotkeysReady() ||
        settings.start_stop_hotkey != confirmed_start_hotkey_ ||
        settings.emergency_hotkey != confirmed_emergency_hotkey_) {
        return false;
    }

    core::RunSettings applied = settings;
    // Preserve settings whose side effects are deliberately outside history.
    applied.hide_from_screen_capture = IsChecked(capture_exclusion_check_);
    applied.keep_window_on_top = IsChecked(keep_on_top_check_);
    applied.remember_settings = IsChecked(remember_settings_check_);
    applied.process_priority_mode = SelectedProcessPriorityMode();
    applied.timing_worker_priority_mode = SelectedTimingWorkerPriorityMode();
    applied.hotkey_control_priority_mode = SelectedHotkeyControlPriorityMode();
    applied.timing_worker_qos_mode = SelectedTimingWorkerQosMode();
    applied.start_stop_hotkey = confirmed_start_hotkey_;
    applied.emergency_hotkey = confirmed_emergency_hotkey_;

    // Undo / Redo can update several independent native child HWNDs in one
    // semantic step. Suspending redraw on their common ancestor does not make
    // those child-window publications atomic or reliably hide the transition.
    // Instead, retain the already-composed Vector Click client frame above the
    // live hierarchy while the complete settings snapshot is applied beneath
    // it. Once the new hierarchy has synchronously painted and DWM has composed
    // it, retire the retained frame. This hides intermediate control states
    // without changing ordinary control repaint behavior or the tab-switch
    // path.
    bool retained_frame_active = false;
    if (startup_presentation_complete_ && window_ != nullptr &&
        IsWindow(window_) != FALSE && IsWindowVisible(window_) != FALSE &&
        !interactive_resize_ && !programmatic_resize_overlay_) {
        retained_frame_active = BeginResizeOverlay(nullptr, 0, false);
    }

    ApplySettings(applied);
    SynchronizeNumericEditHistorySession();
    settings_cache_ = applied;
    if (!applied.show_click_position_indicator) {
        ResetClickPositionIndicator();
    }
    UpdateKeySelectorTooltips();
    ScheduleSettingsSave();

    if (retained_frame_active) {
        // The retained frame still covers the client, so a complete erasing
        // repaint is safe here and guarantees that controls hidden, shown, or
        // disabled by the snapshot leave no stale live pixels underneath it.
        RedrawWindow(window_,
                     nullptr,
                     nullptr,
                     RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                         RDW_UPDATENOW);
        (void)DwmFlush();
        HideResizeOverlay();
        MarkMoveCoverPresentationDirty(false);
        QueueMoveCoverRefresh();
    } else {
        MarkMoveCoverPresentationDirty();
    }
    return true;
}

bool MainWindow::BeginSettingsHistoryHotkeyTransaction(
    const core::RunSettings& target,
    const SettingsHistoryOperation operation) {
    if (operation == SettingsHistoryOperation::None || !hotkey_thread_ ||
        hotkey_registration_pending_ || settings_history_transaction_pending_ ||
        !IsSupportedSafetyHotkeyPair(target.start_stop_hotkey,
                                     target.emergency_hotkey)) {
        return false;
    }

    RestoreConfirmedHotkeyControls();
    settings_history_operation_ = operation;
    pending_settings_history_target_ = target;
    settings_history_transaction_pending_ = true;
    hotkey_registration_pending_ = true;
    UpdateSafetyHotkeyIndicator();
    RefreshEnabledState();

    const std::uint64_t request_id = hotkey_thread_->Reconfigure(
        target.start_stop_hotkey, target.emergency_hotkey);
    if (request_id == 0) {
        hotkey_registration_pending_ = false;
        settings_history_transaction_pending_ = false;
        settings_history_operation_ = SettingsHistoryOperation::None;
        pending_settings_history_target_.reset();
        UpdateSafetyHotkeyIndicator();
        RefreshEnabledState();
        ShowCenteredMessageBox(
            window_,
            L"Vector Click could not queue the safety-hotkey change required by this settings history step. The current settings were left unchanged.",
            L"Settings history unchanged",
            MB_OK | MB_ICONWARNING);
        return false;
    }
    pending_hotkey_request_id_ = request_id;
    SetStatusPresentation({
        StatusCategory::Transition,
        operation == SettingsHistoryOperation::Undo
            ? L"Status: Applying settings undo..."
            : L"Status: Applying settings redo...",
        L"Vector Click is waiting for Windows to confirm the safety-hotkey pair required by this settings history step.",
    }, true);
    return true;
}

bool MainWindow::RestoreCurrentSettingsHistoryState() {
    if (!CanUseSettingsHistory()) {
        return false;
    }
    const core::RunSettings* current = settings_history_.Current();
    if (current == nullptr) {
        return false;
    }

    // Invalid control text was never committed into semantic history. Restore
    // the current snapshot without moving the history cursor so one Ctrl+Z
    // cannot accidentally skip the last valid settings state.
    if (!ApplySettingsHistoryState(*current)) {
        return false;
    }
    SynchronizeSettingsHistoryFromControls();
    UpdateProfileSelectorPresentation();
    return true;
}

bool MainWindow::HandleSettingsHistoryShortcut(const bool redo) {
    if (!CanUseSettingsHistory()) {
        return false;
    }

    const core::RunSettings* target_ptr =
        redo ? settings_history_.RedoTarget() : settings_history_.UndoTarget();
    if (target_ptr == nullptr) {
        return false;
    }
    const core::RunSettings target = *target_ptr;
    const SettingsHistoryOperation operation =
        redo ? SettingsHistoryOperation::Redo : SettingsHistoryOperation::Undo;

    if (target.start_stop_hotkey != confirmed_start_hotkey_ ||
        target.emergency_hotkey != confirmed_emergency_hotkey_) {
        return BeginSettingsHistoryHotkeyTransaction(target, operation);
    }

    if (!ApplySettingsHistoryState(target)) {
        return false;
    }
    const bool moved = redo ? settings_history_.CommitRedo()
                            : settings_history_.CommitUndo();
    if (!moved) {
        return false;
    }
    // External session state can make a historical dependent option
    // unavailable (for example, background input after its Target was
    // deliberately cleared). Keep the new cursor state aligned with what the
    // current session can actually represent without inventing another step.
    SynchronizeSettingsHistoryFromControls();
    UpdateProfileSelectorPresentation();
    return true;
}

void MainWindow::UpdateActionControls(const bool redraw) {
    const bool keyboard = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;

    // Fixed-position capture belongs exclusively to mouse input. End the
    // countdown before hiding the Position controls so its timer can never
    // update a control that no longer belongs to the active input layout.
    if (keyboard && position_capture_seconds_remaining_ != 0) {
        CancelPositionCapture();
    }

    const bool top_visible = basic_top_host_ != nullptr &&
                             IsWindowVisible(basic_top_host_) != FALSE;

    if (redraw && basic_top_host_ != nullptr) {
        SendMessageW(basic_top_host_, WM_SETREDRAW, FALSE, 0);
    }

    const auto set_visible_without_redraw = [](const HWND control, const bool visible) {
        if (control == nullptr || IsWindow(control) == FALSE) {
            return;
        }
        SetWindowPos(control,
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_NOOWNERZORDER | SWP_NOREDRAW |
                         (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    };

    set_visible_without_redraw(mouse_button_label_, !keyboard);
    set_visible_without_redraw(mouse_button_combo_, !keyboard);
    set_visible_without_redraw(generated_key_label_, keyboard);
    set_visible_without_redraw(generated_key_combo_, keyboard);

    for (const HWND control : {current_cursor_radio_, fixed_position_radio_,
                               fixed_x_label_, fixed_x_edit_, fixed_y_label_,
                               fixed_y_edit_, capture_position_button_}) {
        set_visible_without_redraw(control, !keyboard);
    }
    set_visible_without_redraw(position_unavailable_text_, keyboard);

    // The click-position indicator remains visible on the Advanced page in
    // keyboard mode. RefreshEnabledState() disables and clears the dependent
    // option there, matching other unavailable checkboxes instead of hiding it.

    // Tooltip ownership must follow the same input-type visibility transaction.
    // The Position group remains visible in keyboard mode, so leaving its
    // mouse-only tooltip active would create invisible tooltip hit areas over
    // the former Current cursor, Fixed, coordinate, and Capture rows.
    for (const HWND control : {mouse_button_label_, mouse_button_combo_,
                               position_group_, current_cursor_radio_,
                               fixed_position_radio_, fixed_x_label_,
                               fixed_x_edit_, fixed_y_label_, fixed_y_edit_,
                               capture_position_button_}) {
        tooltips_.SetActive(control, !keyboard);
    }
    for (const HWND control : {generated_key_label_, generated_key_combo_,
                               position_unavailable_text_}) {
        tooltips_.SetActive(control, keyboard);
    }

    SetControlText(position_group_, L"Position");
    SetControlText(position_unavailable_text_,
                   L"Position controls are not used for keyboard input.");
    SetControlText(repeat_unit_label_, ActionUnitText(SelectedActionPattern(), keyboard));
    UpdateRateLabel();

    // All upper-card controls use fixed coordinates within basic_top_host_, so
    // no relayout is required when the input type changes.
    if (redraw && basic_top_host_ != nullptr) {
        SendMessageW(basic_top_host_, WM_SETREDRAW, TRUE, 0);
        if (top_visible) {
            RedrawBasicTopHost();
        }
    }

    if (controller_) {
        UpdateStatus(controller_->State(), controller_->CompletedActions());
    }
}

void MainWindow::RedrawBasicTopHost() {
    if (basic_top_host_ == nullptr || IsWindow(basic_top_host_) == FALSE ||
        IsWindowVisible(basic_top_host_) == FALSE) {
        return;
    }

    RedrawWindow(basic_top_host_,
                 nullptr,
                 nullptr,
                 RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
}

void MainWindow::QueueInputTypeUpdate() noexcept {
    if (!input_type_update_pending_ || input_type_update_posted_ ||
        window_ == nullptr || IsWindow(window_) == FALSE) {
        return;
    }

    input_type_update_posted_ = true;
    if (PostMessageW(window_, WM_APP_APPLY_INPUT_TYPE, 0, 0) == FALSE) {
        input_type_update_posted_ = false;
    }
}

bool MainWindow::CaptureBasicTopSnapshot() {
    HideBasicTopSnapshot();

    if (IsCaptureExclusionRequested()) {
        // Prefer a fast crop from the retained complete protected frame. The
        // crop represents the last stable UI state and avoids recursively
        // printing the upper hierarchy in the selection input message.
        if (CopyMoveCoverRegion(basic_top_host_,
                                basic_top_snapshot_bitmap_)) {
            return true;
        }

        int width = 0;
        int height = 0;
        return CapturePrintedClient(basic_top_host_,
                                    basic_top_snapshot_bitmap_,
                                    width,
                                    height);
    }

    if (basic_top_host_ == nullptr || IsWindow(basic_top_host_) == FALSE ||
        IsWindowVisible(basic_top_host_) == FALSE || window_ == nullptr) {
        return false;
    }

    RECT client{};
    if (GetClientRect(basic_top_host_, &client) == FALSE) {
        return false;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    POINT screen_origin{0, 0};
    if (ClientToScreen(basic_top_host_, &screen_origin) == FALSE) {
        return false;
    }

    const HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        return false;
    }
    const HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (memory_dc == nullptr) {
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    const HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, width, height);
    if (bitmap == nullptr) {
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    const HGDIOBJ previous_bitmap = SelectObject(memory_dc, bitmap);
    const BOOL copied = BitBlt(memory_dc,
                               0,
                               0,
                               width,
                               height,
                               screen_dc,
                               screen_origin.x,
                               screen_origin.y,
                               SRCCOPY | CAPTUREBLT);
    SelectObject(memory_dc, previous_bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);

    if (copied == FALSE) {
        DeleteObject(bitmap);
        return false;
    }

    basic_top_snapshot_bitmap_.reset(bitmap);
    return true;
}

bool MainWindow::ShowBasicTopSnapshot() {
    if (basic_top_snapshot_ != nullptr &&
        IsWindow(basic_top_snapshot_) != FALSE) {
        return true;
    }

    if (basic_top_snapshot_bitmap_.get() == nullptr &&
        !CaptureBasicTopSnapshot()) {
        return false;
    }

    if (basic_top_host_ == nullptr || IsWindow(basic_top_host_) == FALSE ||
        IsWindowVisible(basic_top_host_) == FALSE || window_ == nullptr) {
        return false;
    }

    BITMAP bitmap_info{};
    if (GetObjectW(basic_top_snapshot_bitmap_.get(),
                   sizeof(bitmap_info),
                   &bitmap_info) == 0 ||
        bitmap_info.bmWidth <= 0 || bitmap_info.bmHeight <= 0) {
        return false;
    }

    POINT screen_origin{0, 0};
    if (ClientToScreen(basic_top_host_, &screen_origin) == FALSE) {
        return false;
    }

    const int width = bitmap_info.bmWidth;
    const int height = bitmap_info.bmHeight;

    const HWND snapshot = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        L"STATIC",
        L"",
        WS_POPUP | WS_DISABLED | SS_BITMAP,
        screen_origin.x,
        screen_origin.y,
        width,
        height,
        window_,
        nullptr,
        instance_,
        nullptr);
    if (snapshot == nullptr) {
        return false;
    }
    if (IsCaptureExclusionRequested() &&
        ApplyRequestedCaptureExclusion(snapshot).result !=
            CaptureExclusionResult::Applied) {
        DestroyWindow(snapshot);
        return false;
    }

    basic_top_snapshot_ = snapshot;
    SendMessageW(snapshot,
                 STM_SETIMAGE,
                 IMAGE_BITMAP,
                 reinterpret_cast<LPARAM>(basic_top_snapshot_bitmap_.get()));
    SetWindowPos(snapshot,
                 HWND_TOP,
                 screen_origin.x,
                 screen_origin.y,
                 width,
                 height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    UpdateWindow(snapshot);
    (void)DwmFlush();
    return true;
}

void MainWindow::HideBasicTopSnapshot() noexcept {
    if (basic_top_snapshot_ != nullptr && IsWindow(basic_top_snapshot_) != FALSE) {
        ShowWindow(basic_top_snapshot_, SW_HIDE);
        DestroyWindow(basic_top_snapshot_);
    }
    basic_top_snapshot_ = nullptr;
    basic_top_snapshot_bitmap_.reset();
}


bool MainWindow::CanCaptureMoveCoverFromScreen() const noexcept {
    // A maximized title-bar drag crosses a restore boundary where the composed
    // desktop can temporarily contain the previous maximized frame or pixels
    // from windows underneath. Keep every capture route application-owned
    // until the native movement loop has finished.
    if (maximized_caption_drag_pending_ || restored_caption_drag_active_) {
        return false;
    }

    // The retained movement cover is sourced from the composed desktop. Do not
    // read that surface while the main window is excluded from capture, because
    // supported Windows capture paths can return an omitted or blank region.
    if (IsCaptureExclusionRequested()) {
        return false;
    }

    // The custom tooltip is an independent top-level popup. A composed-desktop
    // capture can otherwise bake it into the retained movement bitmap even
    // though the popup is later hidden. Use the application-rendered fallback
    // whenever a tooltip is visible or waiting on its initial-delay timer.
    if (tooltips_.HasPendingOrVisible()) {
        return false;
    }

    if (!startup_presentation_complete_ || window_ == nullptr ||
        IsWindow(window_) == FALSE || content_host_ == nullptr ||
        IsWindow(content_host_) == FALSE ||
        IsWindowVisible(content_host_) == FALSE ||
        GetForegroundWindow() != window_) {
        return false;
    }

    RECT content{};
    if (GetWindowRect(content_host_, &content) == FALSE ||
        content.right <= content.left || content.bottom <= content.top) {
        return false;
    }

    const HMONITOR monitor = MonitorFromRect(&content, MONITOR_DEFAULTTONULL);
    if (monitor == nullptr) {
        return false;
    }
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (GetMonitorInfoW(monitor, &information) == FALSE ||
        content.left < information.rcWork.left ||
        content.top < information.rcWork.top ||
        content.right > information.rcWork.right ||
        content.bottom > information.rcWork.bottom) {
        return false;
    }

    const int inset_x = std::min(2, std::max(0, static_cast<int>(content.right - content.left) / 4));
    const int inset_y = std::min(2, std::max(0, static_cast<int>(content.bottom - content.top) / 4));
    const std::array<POINT, 5> samples{{
        POINT{content.left + inset_x, content.top + inset_y},
        POINT{content.right - 1 - inset_x, content.top + inset_y},
        POINT{content.left + inset_x, content.bottom - 1 - inset_y},
        POINT{content.right - 1 - inset_x, content.bottom - 1 - inset_y},
        POINT{content.left + ((content.right - content.left) / 2),
              content.top + ((content.bottom - content.top) / 2)},
    }};
    for (const POINT point : samples) {
        const HWND hit = WindowFromPoint(point);
        if (hit == nullptr || GetAncestor(hit, GA_ROOT) != window_) {
            return false;
        }
    }
    return true;
}

HWND MainWindow::CurrentMoveCoverPointerControl() const noexcept {
    if (content_host_ == nullptr || IsWindow(content_host_) == FALSE) {
        return nullptr;
    }

    POINT point{};
    if (GetCursorPos(&point) == FALSE) {
        return nullptr;
    }

    HWND current = WindowFromPoint(point);
    while (current != nullptr && current != content_host_) {
        if (!IsWindowWithinHost(content_host_, current)) {
            return nullptr;
        }
        const int control_id = GetDlgCtrlID(current);
        if (control_id >= BasicPageTab && control_id <= ViewLicenseButton) {
            return current;
        }
        current = GetParent(current);
    }
    return nullptr;
}

bool MainWindow::TryPatchMoveCoverForFocusOnlyTabSwitch() noexcept {
    if (move_cover_bitmap_.get() == nullptr || move_cover_width_ <= 0 ||
        move_cover_height_ <= 0 || content_host_ == nullptr ||
        IsWindow(content_host_) == FALSE ||
        IsCaptureExclusionRequested()) {
        return false;
    }

    const HWND new_focus = GetFocus();
    const bool new_focus_is_tab =
        new_focus == basic_tab_button_ ||
        new_focus == advanced_tab_button_ ||
        new_focus == about_tab_button_;
    if (!new_focus_is_tab || move_cover_focus_ == new_focus) {
        return false;
    }

    const HWND current_pointer = CurrentMoveCoverPointerControl();
    if (current_pointer != new_focus) {
        return false;
    }

    // The repair is allowed only when focus is the sole retained-frame
    // mismatch. Any presentation, page, action, scroll, or size change keeps
    // the established complete-capture path authoritative.
    const int current_action_type =
        action_type_combo_ != nullptr && IsWindow(action_type_combo_) != FALSE
            ? static_cast<int>(SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0))
            : -1;
    const int current_advanced_scroll_offset =
        selected_page_index_ == 1 ? advanced_scroll_offset_logical_ : 0;
    RECT client{};
    if (move_cover_bitmap_revision_ != move_cover_presentation_revision_ ||
        move_cover_page_index_ != selected_page_index_ ||
        move_cover_action_type_index_ != current_action_type ||
        move_cover_advanced_scroll_offset_ != current_advanced_scroll_offset ||
        GetClientRect(content_host_, &client) == FALSE ||
        client.right - client.left != move_cover_width_ ||
        client.bottom - client.top != move_cover_height_) {
        return false;
    }

    if ((move_cover_focus_ != nullptr &&
         !IsWindowWithinHost(content_host_, move_cover_focus_)) ||
        (move_cover_pointer_control_ != nullptr &&
         !IsWindowWithinHost(content_host_, move_cover_pointer_control_))) {
        return false;
    }

    // Do not patch through transient interaction states whose visible owner can
    // extend beyond the ordinary focus / pointer controls.
    if (GetCapture() != nullptr || advanced_scroll_dragging_ ||
        numeric_pressed_edit_ != nullptr ||
        (combo_popup_ != nullptr && IsWindow(combo_popup_) != FALSE &&
         IsWindowVisible(combo_popup_) != FALSE)) {
        return false;
    }

    RetainedFocusPatch patch{};
    const auto append_focus_chain =
        [&](const HWND focus) noexcept -> bool {
        if (focus == nullptr) {
            return true;
        }
        if (!AppendRetainedPatchControl(patch, content_host_, focus)) {
            return false;
        }

        // A system combo can move keyboard focus to an internal child while
        // the visible focus frame belongs to the application-owned combo.
        HWND current = GetParent(focus);
        while (current != nullptr && current != content_host_) {
            const int control_id = GetDlgCtrlID(current);
            if (control_id >= BasicPageTab &&
                control_id <= ViewLicenseButton) {
                return AppendRetainedPatchControl(
                    patch, content_host_, current);
            }
            current = GetParent(current);
        }
        return true;
    };

    if (!append_focus_chain(move_cover_focus_) ||
        !append_focus_chain(new_focus) ||
        !AppendRetainedPatchControl(
            patch, content_host_, move_cover_pointer_control_) ||
        !AppendRetainedPatchControl(
            patch, content_host_, current_pointer) ||
        !AppendRetainedPatchControl(
            patch, content_host_, basic_tab_button_) ||
        !AppendRetainedPatchControl(
            patch, content_host_, advanced_tab_button_) ||
        !AppendRetainedPatchControl(
            patch, content_host_, about_tab_button_) ||
        patch.count == 0U) {
        return false;
    }

    // Build the allowlisted direct three-tab repair on a retained-frame clone
    // and publish it only after every direct control render succeeds. If any
    // direct render fails, discard the partial clone and rebuild from a fresh
    // clone through the clipped content-host WM_PRINT fallback.
    bool direct_rendered =
        CloneRetainedBitmap(content_host_,
                            move_cover_bitmap_.get(),
                            move_cover_width_,
                            move_cover_height_,
                            patch.bitmap);
    if (direct_rendered) {
        direct_rendered =
            RenderRetainedFocusPatchDirectControls(content_host_, patch);
    }

    if (!direct_rendered) {
        patch.bitmap.reset();
        if (!CloneRetainedBitmap(content_host_,
                                 move_cover_bitmap_.get(),
                                 move_cover_width_,
                                 move_cover_height_,
                                 patch.bitmap) ||
            !RenderRetainedFocusPatch(content_host_, patch)) {
            return false;
        }
    }

    HideMoveCover();
    move_cover_bitmap_ = std::move(patch.bitmap);
    move_cover_focus_ = new_focus;
    move_cover_pointer_control_ = current_pointer;
    return true;
}

bool MainWindow::CaptureMoveCoverFromScreen() {
    HideMoveCover();
    if (!CanCaptureMoveCoverFromScreen()) {
        return false;
    }

    RECT content{};
    if (GetWindowRect(content_host_, &content) == FALSE) {
        return false;
    }
    const int width = content.right - content.left;
    const int height = content.bottom - content.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    const HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        return false;
    }
    const HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (memory_dc == nullptr) {
        ReleaseDC(nullptr, screen_dc);
        return false;
    }
    const HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, width, height);
    if (bitmap == nullptr) {
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    const HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    const BOOL copied = BitBlt(memory_dc,
                               0,
                               0,
                               width,
                               height,
                               screen_dc,
                               content.left,
                               content.top,
                               SRCCOPY);
    SelectObject(memory_dc, old_bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);

    if (copied == FALSE) {
        DeleteObject(bitmap);
        return false;
    }

    move_cover_bitmap_.reset(bitmap);
    move_cover_width_ = width;
    move_cover_height_ = height;
    move_cover_page_index_ = selected_page_index_;
    move_cover_action_type_index_ =
        action_type_combo_ != nullptr && IsWindow(action_type_combo_) != FALSE
            ? static_cast<int>(SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0))
            : -1;
    move_cover_advanced_scroll_offset_ =
        selected_page_index_ == 1 ? advanced_scroll_offset_logical_ : 0;
    move_cover_focus_ = GetFocus();
    move_cover_pointer_control_ = CurrentMoveCoverPointerControl();
    move_cover_bitmap_revision_ = move_cover_presentation_revision_;
    return true;
}

bool MainWindow::CaptureMoveCoverFromWindow() {
    HideMoveCover();
    if (content_host_ == nullptr || IsWindow(content_host_) == FALSE ||
        IsWindowVisible(content_host_) == FALSE) {
        return false;
    }

    const bool captured = CapturePrintedClient(content_host_,
                                               move_cover_bitmap_,
                                               move_cover_width_,
                                               move_cover_height_);
    if (captured) {
        move_cover_page_index_ = selected_page_index_;
        move_cover_action_type_index_ =
            action_type_combo_ != nullptr && IsWindow(action_type_combo_) != FALSE
                ? static_cast<int>(SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0))
                : -1;
        move_cover_advanced_scroll_offset_ =
            selected_page_index_ == 1 ? advanced_scroll_offset_logical_ : 0;
        move_cover_focus_ = GetFocus();
        move_cover_pointer_control_ = CurrentMoveCoverPointerControl();
        move_cover_bitmap_revision_ = move_cover_presentation_revision_;
    }
    return captured;
}

bool MainWindow::RefreshMoveCoverForRestoredCaptionDrag() {
    // The restored client size can arrive before or after WM_ENTERSIZEMOVE.
    // Synchronize the fixed content host to whichever dimensions are current,
    // then render only Vector Click's hierarchy. CapturePrintedClient resets
    // stale bitmap state on failure, so an old maximized or desktop-derived
    // frame can never be replayed as a fallback.
    PositionContentHost(false);
    if (!CaptureMoveCoverFromWindow()) {
        HideMoveCover();
        return false;
    }

    move_cover_pending_exact_screen_refresh_ = true;
    return ShowMoveCover();
}

bool MainWindow::HasReusableMoveCover() const noexcept {
    if (move_cover_bitmap_.get() == nullptr || move_cover_width_ <= 0 ||
        move_cover_height_ <= 0 || content_host_ == nullptr ||
        IsWindow(content_host_) == FALSE) {
        return false;
    }

    const int current_action_type =
        action_type_combo_ != nullptr && IsWindow(action_type_combo_) != FALSE
            ? static_cast<int>(SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0))
            : -1;

    RECT client{};
    const int current_advanced_scroll_offset =
        selected_page_index_ == 1 ? advanced_scroll_offset_logical_ : 0;
    // Focus is part of the rendered presentation. Checkboxes, combo boxes,
    // numeric fields, tabs, and buttons can all draw a focus-specific frame.
    // Replaying a bitmap captured while a different child owned focus exposes
    // a stale outline during the next page transition or movement cover.
    // Treat a focus change exactly like any other visual cache mismatch so the
    // application-owned capture path republishes the current neutral / focused
    // state before an overlay is shown.
    return move_cover_bitmap_revision_ == move_cover_presentation_revision_ &&
           move_cover_focus_ == GetFocus() &&
           move_cover_page_index_ == selected_page_index_ &&
           move_cover_action_type_index_ == current_action_type &&
           move_cover_advanced_scroll_offset_ == current_advanced_scroll_offset &&
           GetClientRect(content_host_, &client) != FALSE &&
           client.right - client.left == move_cover_width_ &&
           client.bottom - client.top == move_cover_height_;
}

bool MainWindow::CopyMoveCoverRegion(
    const HWND source,
    UniqueGdiObject& destination) const noexcept {
    destination.reset();
    if (!HasReusableMoveCover() || source == nullptr ||
        IsWindow(source) == FALSE || IsWindowVisible(source) == FALSE) {
        return false;
    }

    RECT source_bounds{};
    if (GetWindowRect(source, &source_bounds) == FALSE) {
        return false;
    }
    (void)MapWindowPoints(HWND_DESKTOP,
                          content_host_,
                          reinterpret_cast<POINT*>(&source_bounds),
                          2);

    const int width = source_bounds.right - source_bounds.left;
    const int height = source_bounds.bottom - source_bounds.top;
    if (width <= 0 || height <= 0 || source_bounds.left < 0 ||
        source_bounds.top < 0 || source_bounds.right > move_cover_width_ ||
        source_bounds.bottom > move_cover_height_) {
        return false;
    }

    const HDC reference = GetDC(content_host_);
    if (reference == nullptr) {
        return false;
    }
    const HDC source_dc = CreateCompatibleDC(reference);
    const HDC destination_dc = CreateCompatibleDC(reference);
    if (source_dc == nullptr || destination_dc == nullptr) {
        if (source_dc != nullptr) {
            DeleteDC(source_dc);
        }
        if (destination_dc != nullptr) {
            DeleteDC(destination_dc);
        }
        ReleaseDC(content_host_, reference);
        return false;
    }

    const HBITMAP bitmap = CreateCompatibleBitmap(reference, width, height);
    if (bitmap == nullptr) {
        DeleteDC(destination_dc);
        DeleteDC(source_dc);
        ReleaseDC(content_host_, reference);
        return false;
    }

    const HGDIOBJ previous_source =
        SelectObject(source_dc, move_cover_bitmap_.get());
    const HGDIOBJ previous_destination = SelectObject(destination_dc, bitmap);
    const bool selected = previous_source != nullptr &&
                          previous_source != HGDI_ERROR &&
                          previous_destination != nullptr &&
                          previous_destination != HGDI_ERROR;
    const BOOL copied = selected
                            ? BitBlt(destination_dc,
                                     0,
                                     0,
                                     width,
                                     height,
                                     source_dc,
                                     source_bounds.left,
                                     source_bounds.top,
                                     SRCCOPY)
                            : FALSE;

    if (previous_destination != nullptr &&
        previous_destination != HGDI_ERROR) {
        SelectObject(destination_dc, previous_destination);
    }
    if (previous_source != nullptr && previous_source != HGDI_ERROR) {
        SelectObject(source_dc, previous_source);
    }
    DeleteDC(destination_dc);
    DeleteDC(source_dc);
    ReleaseDC(content_host_, reference);

    if (copied == FALSE) {
        DeleteObject(bitmap);
        return false;
    }

    destination.reset(bitmap);
    return true;
}

void MainWindow::MarkMoveCoverPresentationDirty(
    const bool queue_refresh) noexcept {
    ++move_cover_presentation_revision_;
    if (move_cover_presentation_revision_ == 0U) {
        move_cover_presentation_revision_ = 1U;
        move_cover_bitmap_revision_ = 0U;
    }

    // The existing bitmap can still own the right page and dimensions while
    // containing pre-edit values or pre-Start enabled states. The revision
    // mismatch makes it unusable immediately, including when the user starts
    // a drag before the queued refresh message runs.
    if (!IsCaptureExclusionRequested()) {
        move_cover_pending_exact_screen_refresh_ = true;
    }
    if (queue_refresh) {
        QueueMoveCoverRefresh();
    }
}

void MainWindow::QueueMoveCoverRefresh() noexcept {
    if (move_cover_refresh_posted_ || !startup_presentation_complete_ ||
        window_ == nullptr || IsWindow(window_) == FALSE ||
        IsIconic(window_) != FALSE) {
        return;
    }

    move_cover_refresh_posted_ = true;
    if (PostMessageW(window_,
                     WM_APP_REFRESH_MOVE_COVER,
                     0,
                     0) == FALSE) {
        move_cover_refresh_posted_ = false;
    }
}

void MainWindow::RefreshMoveCoverCache() {
    if (restored_down_exact_move_pending_ && surface_repair_posted_) {
        // A Restore Down replacement must represent the published restored
        // hierarchy, not the non-erasing repair transaction still waiting in
        // the queue. Move this refresh behind that repair.
        QueueMoveCoverRefresh();
        return;
    }

    if (interactive_resize_ || move_cover_ != nullptr ||
        input_type_update_pending_ || input_type_update_posted_ ||
        window_ == nullptr || IsWindow(window_) == FALSE ||
        IsIconic(window_) != FALSE || content_host_ == nullptr ||
        IsWindowVisible(content_host_) == FALSE) {
        return;
    }

    bool refreshed = false;
    bool exact_screen_frame = false;

    if (restored_down_exact_move_pending_) {
        // Do not use WM_PRINT to create the first post-restore movement frame.
        // That route can print the restored hierarchy at the former maximized
        // scale while retaining the same fixed content dimensions. An exact
        // guarded desktop frame is authoritative here.
        (void)DwmFlush();
        if (CanCaptureMoveCoverFromScreen()) {
            refreshed = CaptureMoveCoverFromScreen();
            exact_screen_frame = refreshed;
        }
        if (!refreshed) {
            // Keep the one-drag live-hierarchy fallback armed. A protected or
            // partly off-screen restored window cannot be sampled safely.
            return;
        }
        // Keep the post-restore boundary armed until the first caption drag.
        // Any intervening UI mutation must continue using exact composed
        // capture rather than returning to the affected WM_PRINT route.
    } else {
        const bool presentation_stale =
            move_cover_bitmap_revision_ != move_cover_presentation_revision_;
        if (presentation_stale || IsCaptureExclusionRequested() ||
            move_cover_exact_refresh_deferred_) {
        // UI mutations can post this refresh before DWM has composed every
        // child repaint. A post-Snap deferral has the same authority rule: any
        // queued refresh before the first later movement boundary must remain
        // application-owned so a still-retiring Snap compositor surface cannot
        // enter the cache through an older posted refresh message.
        refreshed = CaptureMoveCoverFromWindow();
    } else if (CanCaptureMoveCoverFromScreen()) {
        refreshed = CaptureMoveCoverFromScreen();
        exact_screen_frame = refreshed;
    } else {
        // A stable UI change can occur while part of the window is outside the
        // monitor work area. Keep the retained frame synchronized with the
        // selected page instead of preserving an older, dimensionally valid
        // bitmap that would appear to switch tabs during the next drag.
            refreshed = CaptureMoveCoverFromWindow();
            if (refreshed) {
                move_cover_pending_exact_screen_refresh_ = true;
            }
        }
    }

    if (refreshed && IsCaptureExclusionRequested()) {
        // Protected rendering is authoritative while exclusion is active.
        move_cover_pending_exact_screen_refresh_ = false;
        move_cover_exact_refresh_deferred_ = false;
    } else if (refreshed && exact_screen_frame) {
        // A successful guarded desktop capture satisfies both the ordinary
        // exact-replacement request and any completed Snap deferral.
        move_cover_pending_exact_screen_refresh_ = false;
        move_cover_exact_refresh_deferred_ = false;
    }
}

bool MainWindow::ShowMoveCover() {
    if (!HasReusableMoveCover()) {
        return false;
    }
    if (move_cover_ != nullptr && IsWindow(move_cover_) != FALSE) {
        SetWindowPos(move_cover_,
                     HWND_TOP,
                     0,
                     0,
                     move_cover_width_,
                     move_cover_height_,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }
    if (move_cover_bitmap_.get() == nullptr || move_cover_width_ <= 0 ||
        move_cover_height_ <= 0 || content_host_ == nullptr ||
        IsWindow(content_host_) == FALSE) {
        return false;
    }

    RECT client{};
    if (GetClientRect(content_host_, &client) == FALSE ||
        client.right - client.left != move_cover_width_ ||
        client.bottom - client.top != move_cover_height_) {
        return false;
    }

    const HWND cover = CreateWindowExW(
        WS_EX_NOPARENTNOTIFY,
        L"STATIC",
        L"",
        WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS | SS_BITMAP,
        0,
        0,
        move_cover_width_,
        move_cover_height_,
        content_host_,
        nullptr,
        instance_,
        nullptr);
    if (cover == nullptr) {
        return false;
    }

    move_cover_ = cover;
    SendMessageW(cover,
                 STM_SETIMAGE,
                 IMAGE_BITMAP,
                 reinterpret_cast<LPARAM>(move_cover_bitmap_.get()));
    SetWindowPos(cover,
                 HWND_TOP,
                 0,
                 0,
                 move_cover_width_,
                 move_cover_height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(cover);
    return true;
}


void MainWindow::RetireMoveCoverAfterResize() noexcept {
    if (move_cover_ == nullptr || IsWindow(move_cover_) == FALSE) {
        move_cover_ = nullptr;
        PositionContentHost(true);
        RedrawWindow(window_,
                     nullptr,
                     nullptr,
                     RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE |
                         RDW_UPDATENOW);
        return;
    }

    HWND release_overlay = nullptr;
    RECT content_screen{};
    if (move_cover_bitmap_.get() != nullptr && content_host_ != nullptr &&
        IsWindow(content_host_) != FALSE &&
        GetWindowRect(content_host_, &content_screen) != FALSE &&
        content_screen.right - content_screen.left == move_cover_width_ &&
        content_screen.bottom - content_screen.top == move_cover_height_) {
        // Snap restore can change the client size inside a caption move. Keep
        // the already verified movement frame in an independent top-level
        // surface while the real hierarchy performs its final release repair.
        // This is intentionally separate from the resize overlay and exists
        // only after the mouse button has been released.
        release_overlay = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            L"STATIC",
            L"",
            WS_POPUP | WS_DISABLED | SS_BITMAP,
            content_screen.left,
            content_screen.top,
            move_cover_width_,
            move_cover_height_,
            window_,
            nullptr,
            instance_,
            nullptr);
        if (release_overlay != nullptr) {
            if ((IsCaptureExclusionRequested() &&
                 ApplyRequestedCaptureExclusion(release_overlay).result !=
                     CaptureExclusionResult::Applied) ||
                SetLayeredWindowAttributes(release_overlay,
                                           0,
                                           255,
                                           LWA_ALPHA) == FALSE) {
                DestroyWindow(release_overlay);
                release_overlay = nullptr;
            } else {
                SendMessageW(
                    release_overlay,
                    STM_SETIMAGE,
                    IMAGE_BITMAP,
                    reinterpret_cast<LPARAM>(move_cover_bitmap_.get()));
                SetWindowPos(release_overlay,
                             HWND_TOP,
                             content_screen.left,
                             content_screen.top,
                             move_cover_width_,
                             move_cover_height_,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW |
                                 SWP_NOOWNERZORDER);
                UpdateWindow(release_overlay);

                // Commit the identical retained frame before removing the
                // child cover. This synchronization occurs only on release,
                // never in the active caption-movement loop.
                (void)DwmFlush();
            }
        }
    }

    if (release_overlay == nullptr) {
        // If the exact-size bridge cannot be created, preserve the previous
        // direct release repair rather than stretching or replaying stale
        // retained pixels at a different geometry.
        HideMoveCover();
        PositionContentHost(true);
        RedrawWindow(window_,
                     nullptr,
                     nullptr,
                     RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE |
                         RDW_UPDATENOW);
        return;
    }

    // The independent popup now owns the visible frame. Remove the child
    // movement cover without exposing or erasing the content host beneath it.
    SetWindowPos(move_cover_,
                 nullptr,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_HIDEWINDOW | SWP_NOREDRAW);
    DestroyWindow(move_cover_);
    move_cover_ = nullptr;

    // Complete the final Snap-restore hierarchy repair under the release
    // bridge, then wait for that live frame to reach the compositor before
    // exposing it.
    PositionContentHost(true);
    RedrawWindow(window_,
                 nullptr,
                 nullptr,
                 RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE |
                     RDW_UPDATENOW);
    (void)DwmFlush();

    SetWindowPos(release_overlay,
                 nullptr,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_HIDEWINDOW | SWP_NOREDRAW);
    DestroyWindow(release_overlay);
}

void MainWindow::RetireMoveCoverAfterMove() noexcept {
    if (move_cover_ == nullptr || IsWindow(move_cover_) == FALSE) {
        move_cover_ = nullptr;
        return;
    }

    HWND release_overlay = nullptr;
    RECT content_screen{};
    if (move_cover_bitmap_.get() != nullptr && content_host_ != nullptr &&
        IsWindow(content_host_) != FALSE &&
        GetWindowRect(content_host_, &content_screen) != FALSE &&
        content_screen.right - content_screen.left == move_cover_width_ &&
        content_screen.bottom - content_screen.top == move_cover_height_) {
        // A fully opaque layered popup is composited independently from the
        // child HWND tree. It therefore masks the release-time hierarchy
        // repair without clipping or changing how the real controls paint.
        release_overlay = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            L"STATIC",
            L"",
            WS_POPUP | WS_DISABLED | SS_BITMAP,
            content_screen.left,
            content_screen.top,
            move_cover_width_,
            move_cover_height_,
            window_,
            nullptr,
            instance_,
            nullptr);
        if (release_overlay != nullptr) {
            if ((IsCaptureExclusionRequested() &&
                 ApplyRequestedCaptureExclusion(release_overlay).result !=
                     CaptureExclusionResult::Applied) ||
                SetLayeredWindowAttributes(release_overlay,
                                           0,
                                           255,
                                           LWA_ALPHA) == FALSE) {
                DestroyWindow(release_overlay);
                release_overlay = nullptr;
            } else {
                SendMessageW(
                    release_overlay,
                    STM_SETIMAGE,
                    IMAGE_BITMAP,
                    reinterpret_cast<LPARAM>(move_cover_bitmap_.get()));
                SetWindowPos(release_overlay,
                             HWND_TOP,
                             content_screen.left,
                             content_screen.top,
                             move_cover_width_,
                             move_cover_height_,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW |
                                 SWP_NOOWNERZORDER);
                UpdateWindow(release_overlay);

                // Commit the identical popup frame before removing the child
                // cover. This wait happens only after the mouse is released,
                // never inside the active movement loop.
                (void)DwmFlush();
            }
        }
    }

    if (release_overlay == nullptr) {
        // Preserve the proven fallback if the transition surface could
        // not be created for any reason.
        HideMoveCover();
        RedrawWindow(window_,
                     nullptr,
                     nullptr,
                     RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE |
                         RDW_UPDATENOW);
        return;
    }

    // Remove the child cover without asking the content host to expose or
    // erase its background. The independent popup remains visually identical
    // and continues to conceal the live hierarchy.
    SetWindowPos(move_cover_,
                 nullptr,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_HIDEWINDOW | SWP_NOREDRAW);
    DestroyWindow(move_cover_);
    move_cover_ = nullptr;

    // Repaint the real window only after it is safely covered by the separate
    // layered surface. No background erase is requested.
    RedrawWindow(window_,
                 nullptr,
                 nullptr,
                 RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE |
                     RDW_UPDATENOW);
    (void)DwmFlush();

    // The completed live hierarchy is now ready underneath the identical
    // transition surface. Hide and destroy the popup without invalidating the
    // owner, exposing the final frame in one compositor transition.
    SetWindowPos(release_overlay,
                 nullptr,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_HIDEWINDOW | SWP_NOREDRAW);
    DestroyWindow(release_overlay);
}

void MainWindow::HideMoveCover() noexcept {
    if (move_cover_ != nullptr && IsWindow(move_cover_) != FALSE) {
        ShowWindow(move_cover_, SW_HIDE);
        DestroyWindow(move_cover_);
    }
    move_cover_ = nullptr;
}

void MainWindow::ApplyInputTypeUpdate() {
    if (!input_type_update_pending_) {
        return;
    }
    input_type_update_pending_ = false;

    const bool visible = basic_top_host_ != nullptr &&
                         IsWindowVisible(basic_top_host_) != FALSE;
    if (visible) {
        (void)ShowBasicTopSnapshot();
    }

    if (basic_top_host_ != nullptr) {
        SendMessageW(basic_top_host_, WM_SETREDRAW, FALSE, 0);
    }

    UpdateActionControls(false);
    if (SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1) {
        ResetClickPositionIndicator();
    }
    ValidateGeneratedKeySelection(true);
    RefreshEnabledState();

    if (basic_top_host_ != nullptr) {
        SendMessageW(basic_top_host_, WM_SETREDRAW, TRUE, 0);
    }
    if (visible) {
        RedrawBasicTopHost();
        (void)DwmFlush();
    }

    HideBasicTopSnapshot();
    QueueMoveCoverRefresh();

    ScheduleSettingsSave();
    CommitSettingsHistoryFromControls();
}

void MainWindow::UpdateActionPatternControls() {
    const bool keyboard = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;

    // Action-pattern changes do not alter control geometry. The repeat-unit
    // field already reserves enough width for clicks, actions, holds, and key
    // presses, while the rate display has a fixed card-wide rectangle. Avoid
    // relayout and whole-page repaint here so only the text and controls whose
    // enabled state actually changes are redrawn.
    SetControlText(repeat_unit_label_, ActionUnitText(SelectedActionPattern(), keyboard));
    RefreshPresentation(true, false, true, false);
}

RatePresentationInput MainWindow::CaptureRatePresentationInput() const {
    RatePresentationInput input;
    input.action_pattern = SelectedActionPattern();
    input.keyboard = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;
    core::DurationComponents components{};
    input.interval_valid = TryReadDurationEdits(
        IntervalDurationEdits(), components, input.interval_microseconds);
    input.randomize_interval = IsChecked(random_interval_check_);
    input.minimum_interval_valid = TryReadDurationEdits(
        MinimumIntervalDurationEdits(),
        components,
        input.minimum_interval_microseconds);
    input.maximum_interval_valid = TryReadDurationEdits(
        MaximumIntervalDurationEdits(),
        components,
        input.maximum_interval_microseconds);
    input.down_duration_valid = TryReadDurationEdits(
        ButtonDownDurationEdits(),
        components,
        input.down_duration_microseconds);
    input.action_spacing_valid = TryReadDurationEdits(
        ActionSpacingDurationEdits(),
        components,
        input.action_spacing_microseconds);

    std::uint64_t burst_count = 0;
    input.burst_count_valid = TryParseUnsignedIntegerText(
        GetControlText(burst_count_edit_), burst_count) &&
        burst_count <= std::numeric_limits<std::uint32_t>::max();
    if (input.burst_count_valid) {
        input.burst_count = static_cast<std::uint32_t>(burst_count);
    }
    core::RunTimeLimitComponents run_time_limit_components{};
    input.run_time_limit_valid = TryReadRunTimeLimitEdits(
        run_time_limit_components, input.run_time_limit_microseconds);
    return input;
}

PresentationSnapshot MainWindow::BuildPresentationSnapshot(const bool evaluate_availability) const {
    const EngineState engine_state = controller_ ? controller_->State() : EngineState::Ready;
    const PresentationEngineState presentation_state =
        ToPresentationEngineState(engine_state);
    const bool running = engine_state == EngineState::Running ||
                         engine_state == EngineState::Stopping;
    const bool shutdown_active =
        shutdown_in_progress_.load(std::memory_order_acquire);
    const bool emergency_active = emergency_latched_.load(std::memory_order_acquire) ||
                                  safety_shield_.IsVisible();
    const bool cleanup_required = CleanupRequired();
    const bool enable_settings = !running && !emergency_active && !shutdown_active &&
                                 !settings_history_transaction_pending_ &&
                                 !settings_import_transaction_pending_;

    StartAvailabilityPresentation availability;
    if (evaluate_availability) {
        core::RunSettings candidate = core::DefaultRunSettings();
        std::wstring validation_error;
        const bool settings_valid = enable_settings && ReadSettings(candidate, validation_error);
        const core::InputBackend effective_backend = settings_valid
            ? InputBackendDispatcher::EffectiveBackend(
                  candidate.backend,
                  target_window_,
                  candidate.allow_background_input)
            : core::InputBackend::StandardInput;
        const bool target_required =
            effective_backend == core::InputBackend::ForegroundTargetInput ||
            effective_backend == core::InputBackend::TargetedWindowMessages ||
            effective_backend == core::InputBackend::TargetedUnicodeText;
        const bool target_ready = !settings_valid ||
                                  !target_required ||
                                  IsTargetWindowValid(target_window_);

        availability = EvaluateStartAvailability({
            presentation_state,
            emergency_active || shutdown_active,
            SafetyHotkeysReady(),
            SafetyHotkeyUnavailableReason(),
            settings_valid,
            validation_error,
            target_ready,
            start_availability_initialized_,
            start_available_,
            start_unavailable_reason_,
            cleanup_required,
        });
    } else {
        // Rate-only, status-only, and periodic diagnostics refreshes reuse the
        // last evaluated readiness result. Control-change paths that can alter
        // readiness explicitly request a fresh evaluation and apply the full
        // presentation snapshot together.
        const bool start_enabled = start_availability_initialized_ &&
                                   start_available_ && SafetyHotkeysReady() &&
                                   !running && !shutdown_active &&
                                   presentation_state != PresentationEngineState::Faulted &&
                                   !emergency_active && !cleanup_required;
        availability = {
            start_enabled,
            false,
            start_availability_initialized_,
            start_available_,
            start_unavailable_reason_,
            false,
            cleanup_required || start_attention_required_,
        };
    }

    const bool keyboard = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;
    const core::ActionPattern action_pattern = SelectedActionPattern();
    const std::uint64_t completed_actions =
        controller_ ? controller_->CompletedActions() : 0U;
    const DiagnosticsSnapshot diagnostics =
        controller_ ? controller_->Diagnostics() : DiagnosticsSnapshot{};

    std::wstring completion_note;
    if (presentation_state == PresentationEngineState::Ready && controller_) {
        switch (controller_->LastRunLimitStopReason()) {
        case RunLimitStopReason::RepeatLimit:
            completion_note = L"Repeat limit reached";
            break;
        case RunLimitStopReason::TimeLimit:
            completion_note = L"Time limit reached";
            break;
        case RunLimitStopReason::None:
            break;
        }
    }

    PresentationSnapshot snapshot;
    snapshot.diagnostics_visible = IsChecked(diagnostics_check_);
    snapshot.rate = BuildRatePresentation(CaptureRatePresentationInput());
    snapshot.availability = availability;
    snapshot.status = BuildStatusPresentation({
        presentation_state,
        availability,
        snapshot.diagnostics_visible,
        keyboard,
        action_pattern,
        completed_actions,
        completion_note,
    });
    snapshot.diagnostics = BuildDiagnosticsPresentation({
        presentation_state,
        availability,
        keyboard,
        action_pattern,
        diagnostics.completed_actions,
        diagnostics.generated_inputs,
        diagnostics.elapsed_seconds,
        diagnostics.actual_actions_per_second,
        diagnostics.actual_inputs_per_second,
        completion_note,
    });
    return snapshot;
}

void MainWindow::ApplyPresentationSnapshot(const PresentationSnapshot& snapshot,
                                           const bool update_rate,
                                           const bool update_availability,
                                           const bool force_status,
                                           const bool update_diagnostics) {
    if (!last_presentation_snapshot_) {
        last_presentation_snapshot_.emplace();
    }
    PresentationSnapshot& applied = *last_presentation_snapshot_;

    if (update_rate && applied.rate != snapshot.rate) {
        if (rate_text_ != nullptr && IsWindow(rate_text_) != FALSE &&
            GetControlText(rate_text_) != snapshot.rate.text) {
            // Prevent the owner-drawn static from exposing an erase or native
            // intermediate frame while its text changes. The following
            // invalidation produces one buffered application-owned repaint.
            SendMessageW(rate_text_, WM_SETREDRAW, FALSE, 0);
            SetWindowTextW(rate_text_, snapshot.rate.text.c_str());
            SendMessageW(rate_text_, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(rate_text_, nullptr, FALSE);
        }
        tooltips_.SetText(rate_text_, snapshot.rate.tooltip);
        applied.rate = snapshot.rate;
        MarkMoveCoverPresentationDirty();
    }

    bool availability_changed = false;
    if (update_availability) {
        SetEnabledIfChanged(start_button_, snapshot.availability.start_enabled);
        if (snapshot.availability.update_cached_availability) {
            availability_changed = snapshot.availability.availability_changed;
            start_availability_initialized_ = snapshot.availability.initialized;
            start_available_ = snapshot.availability.available;
            start_attention_required_ = snapshot.availability.attention_required;
            start_unavailable_reason_ = snapshot.availability.unavailable_reason;
        }
        applied.availability = snapshot.availability;
    }

    if (force_status || availability_changed) {
        if (availability_changed) {
            last_diagnostics_text_.clear();
            if (snapshot.diagnostics_visible && status_text_ != nullptr &&
                IsWindow(status_text_) != FALSE) {
                InvalidateRect(status_text_, nullptr, FALSE);
            }
        }
        if (force_status || applied.status != snapshot.status ||
            availability_changed) {
            SetStatusPresentation(snapshot.status);
        }
        applied.status = snapshot.status;
    }

    const bool diagnostics_mode_changed =
        applied.diagnostics_visible != snapshot.diagnostics_visible;
    applied.diagnostics_visible = snapshot.diagnostics_visible;
    if (update_diagnostics && snapshot.diagnostics_visible) {
        const bool category_changed =
            diagnostics_category_ != snapshot.diagnostics.category;
        diagnostics_category_ = snapshot.diagnostics.category;
        // Keep the hidden diagnostics child as one coherent presentation
        // snapshot. Text and category are committed together so no print,
        // accessibility, or retained-frame request can observe current text
        // paired with an older status category. The visible footer continues
        // to use last_diagnostics_text_ and diagnostics_category_ directly.
        DiagnosticsDisplay::SetPresentation(diagnostics_text_,
                                            snapshot.diagnostics.text,
                                            diagnostics_category_);

        const bool text_changed =
            snapshot.diagnostics.text != last_diagnostics_text_;
        if (text_changed) {
            SetDiagnosticsText(snapshot.diagnostics.text);
        } else if ((category_changed || diagnostics_mode_changed) &&
                   status_text_ != nullptr &&
                   IsWindow(status_text_) != FALSE) {
            InvalidateRect(status_text_, nullptr, FALSE);
        }
        applied.diagnostics = snapshot.diagnostics;
    } else if (diagnostics_mode_changed && status_text_ != nullptr &&
               IsWindow(status_text_) != FALSE) {
        InvalidateRect(status_text_, nullptr, FALSE);
    }
}

void MainWindow::RefreshPresentation(const bool update_rate,
                                     const bool update_availability,
                                     const bool force_status,
                                     const bool update_diagnostics) {
    ApplyPresentationSnapshot(BuildPresentationSnapshot(update_availability),
                              update_rate,
                              update_availability,
                              force_status,
                              update_diagnostics);
}

void MainWindow::UpdateRateLabel() {
    RefreshPresentation(true, false, false, false);
}

void MainWindow::RefreshEnabledState() {
    UpdateEnabledState(controller_ ? controller_->State() : EngineState::Ready);
}

void MainWindow::RefreshStartAvailability() {
    RefreshPresentation(false, true, false, false);
}

void MainWindow::UpdateEnabledState(const EngineState state) {
    const bool running = state == EngineState::Running || state == EngineState::Stopping;
    const bool shutdown_active =
        shutdown_in_progress_.load(std::memory_order_acquire);
    const bool emergency_active = emergency_latched_.load(std::memory_order_acquire) ||
                                  safety_shield_.IsVisible();
    // A hotkey transaction only needs to lock the two hotkey selectors.
    // Disabling every setting caused a broad enable / disable repaint and made
    // the Basic-page card visibly flicker. Start remains independently gated
    // by SafetyHotkeysReady(), so unrelated settings can stay responsive.
    const bool enable_settings = !running && !emergency_active && !shutdown_active &&
                                 !settings_history_transaction_pending_;
    const bool keyboard = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;
    const bool fixed = IsChecked(fixed_position_radio_);
    const core::ActionPattern action_pattern = SelectedActionPattern();
    const bool hold_action = action_pattern == core::ActionPattern::Hold;
    const bool multi_input_action = action_pattern == core::ActionPattern::Double ||
                                    action_pattern == core::ActionPattern::Triple ||
                                    action_pattern == core::ActionPattern::Burst;

    for (HWND control : {action_type_combo_, action_pattern_combo_, burst_count_edit_, action_spacing_minutes_edit_, action_spacing_seconds_edit_, action_spacing_edit_, backend_combo_, random_interval_check_, random_interval_style_combo_, down_duration_behavior_combo_, minimum_interval_minutes_edit_, minimum_interval_seconds_edit_, minimum_interval_edit_, maximum_interval_minutes_edit_, maximum_interval_seconds_edit_, maximum_interval_edit_, mouse_button_combo_, generated_key_combo_, interval_minutes_edit_, interval_seconds_edit_, interval_edit_, button_down_minutes_edit_, button_down_seconds_edit_, button_down_edit_,
                         select_target_button_, background_input_check_, current_cursor_radio_, fixed_position_radio_, fixed_x_edit_, fixed_y_edit_, capture_position_button_,
                         click_position_indicator_check_, unlimited_radio_, limited_radio_, repeat_count_edit_,
                         start_hotkey_combo_, emergency_hotkey_combo_,
                         diagnostics_check_, safety_shield_check_, force_exit_on_emergency_stop_check_, capture_exclusion_check_, keep_on_top_check_, remember_settings_check_, process_priority_combo_, timing_worker_priority_combo_, timing_worker_qos_combo_, hotkey_control_priority_combo_, windows_notification_combo_, system_sound_combo_, running_indicator_check_, admin_button_}) {
        SetEnabledIfChanged(control, enable_settings);
    }

    if (keyboard) {
        SetEnabledIfChanged(mouse_button_combo_, false);
        SetEnabledIfChanged(current_cursor_radio_, false);
        SetEnabledIfChanged(fixed_position_radio_, false);
        SetEnabledIfChanged(fixed_x_edit_, false);
        SetEnabledIfChanged(fixed_y_edit_, false);
        SetEnabledIfChanged(capture_position_button_, false);
    } else {
        SetEnabledIfChanged(generated_key_combo_, false);
        SetEnabledIfChanged(fixed_x_edit_, enable_settings && fixed);
        SetEnabledIfChanged(fixed_y_edit_, enable_settings && fixed);
        SetEnabledIfChanged(capture_position_button_, enable_settings && position_capture_seconds_remaining_ == 0);
    }

    const bool click_position_indicator_option_available =
        !keyboard && click_position_indicator_available_;
    // Match the established dependent-checkbox behavior used by Allow
    // background input: an unavailable option stays visible but must not retain
    // an active value. Clearing it also closes any indicator surface that may
    // still exist from the previous mouse-input configuration.
    if (enable_settings && !click_position_indicator_option_available &&
        IsChecked(click_position_indicator_check_)) {
        SetChecked(click_position_indicator_check_, false);
        ResetClickPositionIndicator();
        ScheduleSettingsSave();
    }
    SetEnabledIfChanged(click_position_indicator_check_,
                        enable_settings && click_position_indicator_option_available);

    const bool random_interval = IsChecked(random_interval_check_);
    for (const HWND control : {basic_interval_minutes_header_,
                               basic_interval_seconds_header_,
                               basic_interval_milliseconds_header_,
                               interval_minutes_edit_, interval_seconds_edit_,
                               interval_edit_}) {
        SetEnabledIfChanged(
            control, enable_settings && !hold_action && !random_interval);
    }
    SetEnabledIfChanged(
        random_interval_check_, enable_settings && !hold_action);
    SetEnabledIfChanged(
        random_interval_style_label_, enable_settings && !hold_action && random_interval);
    SetEnabledIfChanged(
        random_interval_style_combo_, enable_settings && !hold_action && random_interval);
    SetEnabledIfChanged(
        minimum_interval_label_, enable_settings && !hold_action && random_interval);
    for (const HWND control : {minimum_interval_minutes_edit_,
                               minimum_interval_seconds_edit_,
                               minimum_interval_edit_}) {
        SetEnabledIfChanged(
            control, enable_settings && !hold_action && random_interval);
    }
    SetEnabledIfChanged(
        maximum_interval_label_, enable_settings && !hold_action && random_interval);
    for (const HWND control : {maximum_interval_minutes_edit_,
                               maximum_interval_seconds_edit_,
                               maximum_interval_edit_}) {
        SetEnabledIfChanged(
            control, enable_settings && !hold_action && random_interval);
    }
    const bool natural_down_available = !hold_action;
    SetEnabledIfChanged(
        down_duration_behavior_label_, enable_settings && natural_down_available);
    SetEnabledIfChanged(
        down_duration_behavior_combo_, enable_settings && natural_down_available);
    const bool automatic_down =
        SendMessageW(down_duration_behavior_combo_, CB_GETCURSEL, 0, 0) == 2;
    for (const HWND control : {button_down_minutes_edit_,
                               button_down_seconds_edit_,
                               button_down_edit_}) {
        SetEnabledIfChanged(
            control, enable_settings && !hold_action && !automatic_down);
    }
    SetEnabledIfChanged(burst_count_edit_, enable_settings && action_pattern == core::ActionPattern::Burst);
    for (const HWND control : {action_spacing_minutes_edit_,
                               action_spacing_seconds_edit_,
                               action_spacing_edit_}) {
        SetEnabledIfChanged(
            control, enable_settings && multi_input_action);
    }
    SetEnabledIfChanged(unlimited_radio_, enable_settings && !hold_action);
    SetEnabledIfChanged(limited_radio_, enable_settings && !hold_action);
    for (const HWND control : {run_time_hours_header_, run_time_minutes_header_,
                               run_time_seconds_header_, run_time_hours_edit_,
                               run_time_minutes_edit_, run_time_seconds_edit_}) {
        SetEnabledIfChanged(control, enable_settings);
    }

    SetEnabledIfChanged(select_target_button_, enable_settings && !target_picker_open_);
    // Clear has target-specific availability. Do not include it in the broad
    // settings pass above, because enabling it there and immediately disabling
    // it again when no target exists exposes one incorrect owner-drawn frame.
    SetEnabledIfChanged(clear_target_button_,
                        enable_settings && !target_picker_open_ &&
                            target_window_.window != nullptr);

    core::InputBackend requested_backend = core::InputBackend::Automatic;
    const LRESULT backend_selection = SendMessageW(backend_combo_, CB_GETCURSEL, 0, 0);
    if (backend_selection == 1) {
        requested_backend = core::InputBackend::StandardInput;
    } else if (backend_selection == 2) {
        requested_backend = core::InputBackend::ForegroundTargetInput;
    } else if (backend_selection == 3) {
        requested_backend = core::InputBackend::TargetedWindowMessages;
    } else if (backend_selection == 4) {
        requested_backend = core::InputBackend::UnicodeTextInput;
    } else if (backend_selection == 5) {
        requested_backend = core::InputBackend::TargetedUnicodeText;
    }
    const bool background_option_available =
        target_window_.window != nullptr &&
        (requested_backend == core::InputBackend::Automatic ||
         requested_backend == core::InputBackend::TargetedWindowMessages ||
         requested_backend == core::InputBackend::TargetedUnicodeText);
    // A disabled dependent checkbox should never retain an active value. The
    // selected target-clear path already enforced this rule; apply the same
    // rule when the user switches to an input method for which background
    // delivery has no meaning (Standard, Foreground target, or Unicode text).
    if (enable_settings && !background_option_available &&
        IsChecked(background_input_check_)) {
        SetChecked(background_input_check_, false);
        ScheduleSettingsSave();
    }
    SetEnabledIfChanged(background_input_check_,
                        enable_settings && background_option_available);
    SetEnabledIfChanged(repeat_count_edit_, enable_settings && !hold_action && IsChecked(limited_radio_));
    SetEnabledIfChanged(import_settings_button_, CanImportSettings());
    const bool profiles_available = CanManageProfiles();
    SetEnabledIfChanged(profile_label_, profiles_available);
    SetEnabledIfChanged(profile_combo_, profiles_available);
    SetEnabledIfChanged(manage_profiles_button_,
                        profiles_available && !profile_manager_open_);
    if (profile_manager_ && profile_manager_->IsOpen()) {
        profile_manager_->RefreshAvailability();
    }

    RefreshStartAvailability();
    const bool cleanup_required = CleanupRequired();
    const std::wstring stop_text = cleanup_required ? L"Retry cleanup" : L"Stop";
    if (GetControlText(stop_button_) != stop_text) {
        SetControlText(stop_button_, stop_text);
        InvalidateRect(stop_button_, nullptr, FALSE);
    }
    tooltips_.SetText(
        stop_button_,
        cleanup_required
            ? L"Resubmit every tracked release without generating a new press. Resume or unblock a suspended or unresponsive target before retrying."
            : L"Stop the current run normally and release any input Vector Click still tracks as held.");
    tooltips_.SetText(
        emergency_button_,
        cleanup_required
            ? L"Open the recovery Safety Shield to access Retry cleanup and Force Stop and Exit. The shield can open for unresolved cleanup even when automatic Safety Shield display is disabled."
            : L"Cancel the current run immediately and release any input Vector Click still tracks as held. Input already accepted by Windows or another application cannot be recalled.");
    SetEnabledIfChanged(
        stop_button_, !shutdown_active && (running || cleanup_required));
    SetEnabledIfChanged(emergency_button_, !shutdown_active);

    // Start / Stop, emergency, and readiness transitions alter many enabled
    // controls without changing the dimensions used by the movement-cache
    // validator. Invalidate the visual revision as one coalesced transaction.
    MarkMoveCoverPresentationDirty();
}

void MainWindow::QueueSurfaceRepair() noexcept {
    if (!startup_presentation_complete_ || surface_repair_posted_ ||
        window_ == nullptr || IsWindow(window_) == FALSE ||
        IsIconic(window_) != FALSE) {
        return;
    }
    surface_repair_posted_ = true;
    if (PostMessageW(window_, WM_APP_REPAIR_SURFACES, 0, 0) == FALSE) {
        surface_repair_posted_ = false;
    }
}

void MainWindow::RepairVisibleSurfaces() {
    if (window_ == nullptr || IsWindow(window_) == FALSE ||
        IsIconic(window_) != FALSE) {
        return;
    }

    if (!input_type_update_pending_ && !input_type_update_posted_) {
        HideBasicTopSnapshot();
    }

    // Queue one non-erasing hierarchy repaint and let Windows coalesce the
    // child WM_PAINT messages into the next presentation. Forcing every child
    // to paint synchronously here made the cards visibly update in sequence
    // while the window was moved or restored.
    RedrawWindow(window_,
                 nullptr,
                 nullptr,
                 RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_NOERASE);
}


void MainWindow::UpdateStatus(const EngineState state,
                              const std::uint64_t completed_actions) {
    PresentationSnapshot snapshot = BuildPresentationSnapshot(false);
    const bool keyboard = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;
    std::wstring completion_note;
    if (state == EngineState::Ready && controller_) {
        switch (controller_->LastRunLimitStopReason()) {
        case RunLimitStopReason::RepeatLimit:
            completion_note = L"Repeat limit reached";
            break;
        case RunLimitStopReason::TimeLimit:
            completion_note = L"Time limit reached";
            break;
        case RunLimitStopReason::None:
            break;
        }
    }
    snapshot.status = BuildStatusPresentation({
        ToPresentationEngineState(state),
        snapshot.availability,
        snapshot.diagnostics_visible,
        keyboard,
        SelectedActionPattern(),
        completed_actions,
        completion_note,
    });
    if (snapshot.diagnostics_visible) {
        const DiagnosticsSnapshot diagnostics = controller_
            ? controller_->Diagnostics()
            : DiagnosticsSnapshot{};
        snapshot.diagnostics = BuildDiagnosticsPresentation({
            ToPresentationEngineState(state),
            snapshot.availability,
            keyboard,
            SelectedActionPattern(),
            diagnostics.completed_actions,
            diagnostics.generated_inputs,
            diagnostics.elapsed_seconds,
            diagnostics.actual_actions_per_second,
            diagnostics.actual_inputs_per_second,
            completion_note,
        });
    }
    // Engine-state transitions are safety- and readiness-significant. When
    // live diagnostics is enabled, update its presentation in the same UI turn
    // rather than waiting up to one diagnostics-timer interval for the text or
    // lamp category to catch up. The 250 ms timer remains responsible for
    // periodic rate / counter refreshes between state changes.
    ApplyPresentationSnapshot(snapshot,
                              false,
                              false,
                              true,
                              snapshot.diagnostics_visible);
}

void MainWindow::ConfigureHotkeysFromControls(const bool show_errors) {
    if (hotkey_registration_pending_) {
        RestoreConfirmedHotkeyControls();
        return;
    }

    core::RunSettings candidate = settings_cache_;
    candidate.start_stop_hotkey = SelectedHotkey(start_hotkey_combo_);
    candidate.emergency_hotkey = SelectedHotkey(emergency_hotkey_combo_);
    candidate.action_type = SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1
                                ? core::ActionType::KeyboardPress
                                : core::ActionType::MouseClick;
    const core::HotkeyBinding generated_key = SelectedGeneratedKey();
    candidate.generated_virtual_key = generated_key.virtual_key;
    candidate.generated_key_modifiers = generated_key.modifiers;

    std::wstring conflict;
    const core::HotkeyBinding candidate_generated_key{
        candidate.generated_virtual_key,
        candidate.generated_key_modifiers,
    };
    if (!IsSupportedSafetyHotkeyBinding(candidate.start_stop_hotkey)) {
        conflict = L"Choose a supported Start / Stop hotkey from the list.";
    } else if (!IsSupportedSafetyHotkeyBinding(candidate.emergency_hotkey)) {
        conflict = L"Choose a supported Emergency Stop hotkey from the list.";
    } else if (core::SamePhysicalKey(candidate.start_stop_hotkey,
                                     candidate.emergency_hotkey)) {
        conflict = L"Start / Stop and Emergency Stop must use different physical keys.";
    } else if (candidate.action_type == core::ActionType::KeyboardPress &&
               core::SamePhysicalKey(candidate_generated_key,
                                     candidate.start_stop_hotkey)) {
        conflict = L"The generated keyboard key cannot share the Start / Stop safety key.";
    } else if (candidate.action_type == core::ActionType::KeyboardPress &&
               core::SamePhysicalKey(candidate_generated_key,
                                     candidate.emergency_hotkey)) {
        conflict = L"The generated keyboard key cannot share the Emergency Stop safety key.";
    }

    if (!conflict.empty()) {
        if (show_errors) {
            conflict += L" The confirmed safety hotkeys remain active.";
            ShowCenteredMessageBox(window_,
                                   conflict.c_str(),
                                   L"Safety hotkey conflict",
                                   MB_OK | MB_ICONWARNING);
        }
        RestoreConfirmedHotkeyControls();
        return;
    }

    if (candidate.start_stop_hotkey == confirmed_start_hotkey_ &&
        candidate.emergency_hotkey == confirmed_emergency_hotkey_ &&
        safety_hotkeys_confirmed_) {
        RestoreConfirmedHotkeyControls();
        return;
    }

    if (!hotkey_thread_) {
        RestoreConfirmedHotkeyControls();
        if (show_errors) {
            ShowCenteredMessageBox(
                window_,
                L"The safety-hotkey service is unavailable. Start remains disabled.",
                L"Safety hotkeys unavailable",
                MB_OK | MB_ICONWARNING);
        }
        safety_hotkeys_confirmed_ = false;
        UpdateSafetyHotkeyIndicator();
        RefreshEnabledState();
        return;
    }

    // Keep the controls synchronized with the pair that Windows has actually
    // registered. The requested values become visible only after the hotkey
    // thread confirms that both registrations succeeded.
    //
    // When a confirmed pair already exists, it remains registered throughout
    // the transaction. Do not disturb Start availability, the status bar, or
    // the Hotkeys-card title for this brief replacement check. Only the two
    // selectors are locked until the result arrives. Startup registration,
    // where no active pair exists yet, still uses the full readiness update.
    const bool had_confirmed_pair = SafetyHotkeysReady();
    RestoreConfirmedHotkeyControls();
    hotkey_registration_pending_ = true;
    UpdateSafetyHotkeyIndicator();
    SetEnabledIfChanged(import_settings_button_, false);
    if (!had_confirmed_pair) {
        RefreshEnabledState();
        SetStatusPresentation({
            StatusCategory::Transition,
            L"Status: Registering safety hotkeys...",
            L"Vector Click is waiting for Windows to confirm both global safety hotkeys. Start remains unavailable until registration completes.",
        }, true);
    }

    const std::uint64_t request_id = hotkey_thread_->Reconfigure(
        candidate.start_stop_hotkey, candidate.emergency_hotkey);
    if (request_id == 0) {
        hotkey_registration_pending_ = false;
        UpdateSafetyHotkeyIndicator();
        SetEnabledIfChanged(import_settings_button_, CanImportSettings());
        if (!had_confirmed_pair) {
            RefreshEnabledState();
        }
        if (show_errors) {
            ShowCenteredMessageBox(
                window_,
                L"The safety-hotkey registration request could not be queued. The confirmed pair remains active.",
                L"Safety hotkeys unchanged",
                MB_OK | MB_ICONWARNING);
        }
        return;
    }
    pending_hotkey_request_id_ = request_id;
}

void MainWindow::HandleHotkeyRegistrationResult(HotkeyRegistrationResult result) {
    if (result.request_id < pending_hotkey_request_id_) {
        return;
    }

    const SettingsHistoryOperation history_operation = settings_history_operation_;
    const std::optional<core::RunSettings> history_target =
        pending_settings_history_target_;
    const bool history_transaction = settings_history_transaction_pending_;
    const std::optional<core::RunSettings> import_target =
        pending_settings_import_target_;
    const bool import_transaction = settings_import_transaction_pending_;
    const bool profile_load_transaction =
        import_transaction && settings_import_is_profile_load_;

    pending_hotkey_request_id_ = result.request_id;
    const bool previously_ready = SafetyHotkeysReady();
    hotkey_registration_pending_ = false;
    SetEnabledIfChanged(import_settings_button_, CanImportSettings());
    const bool has_active_pair =
        result.HasActivePair() &&
        IsSupportedSafetyHotkeyPair(result.active_start_stop,
                                    result.active_emergency);
    safety_hotkeys_confirmed_ = has_active_pair;

    if (has_active_pair) {
        confirmed_start_hotkey_ = result.active_start_stop;
        confirmed_emergency_hotkey_ = result.active_emergency;
        settings_cache_.start_stop_hotkey = confirmed_start_hotkey_;
        settings_cache_.emergency_hotkey = confirmed_emergency_hotkey_;
        active_start_hotkey_key_.store(confirmed_start_hotkey_.virtual_key,
                                       std::memory_order_release);
        active_start_hotkey_modifiers_.store(confirmed_start_hotkey_.modifiers,
                                             std::memory_order_release);
        active_emergency_hotkey_key_.store(confirmed_emergency_hotkey_.virtual_key,
                                           std::memory_order_release);
        active_emergency_hotkey_modifiers_.store(confirmed_emergency_hotkey_.modifiers,
                                                 std::memory_order_release);
    } else {
        confirmed_start_hotkey_ = {};
        confirmed_emergency_hotkey_ = {};
        active_start_hotkey_key_.store(0, std::memory_order_release);
        active_start_hotkey_modifiers_.store(0, std::memory_order_release);
        active_emergency_hotkey_key_.store(0, std::memory_order_release);
        active_emergency_hotkey_modifiers_.store(0, std::memory_order_release);
    }

    RestoreConfirmedHotkeyControls();
    UpdateSafetyHotkeyIndicator();
    UpdateProfileSelectorPresentation();

    const bool now_ready = SafetyHotkeysReady();
    if (previously_ready != now_ready) {
        // Startup completion or a genuine loss of all registered safety keys
        // changes application readiness and therefore requires the normal
        // availability / status refresh.
        RefreshEnabledState();
    }

    if (import_transaction) {
        const bool target_pair_confirmed =
            result.requested_pair_registered && import_target.has_value() &&
            has_active_pair &&
            confirmed_start_hotkey_ == import_target->start_stop_hotkey &&
            confirmed_emergency_hotkey_ == import_target->emergency_hotkey;
        if (target_pair_confirmed) {
            const bool applied = ApplyImportedSettings(*import_target);
            FinishSettingsImport(applied);
            if (!applied) {
                ShowCenteredMessageBox(
                    window_,
                    profile_load_transaction
                        ? L"Windows confirmed the profile's safety hotkeys, but Vector Click could not complete the remaining profile transaction."
                        : L"Windows confirmed the imported safety hotkeys, but Vector Click could not complete the remaining settings transaction.",
                    profile_load_transaction
                        ? L"Profile load incomplete"
                        : L"Settings import incomplete",
                    MB_OK | MB_ICONWARNING);
            }
            return;
        }

        std::wstring notice = profile_load_transaction
            ? L"Windows could not activate the safety-hotkey pair required by the selected profile. No profile settings were loaded."
            : L"Windows could not activate the safety-hotkey pair required by the selected settings file. No settings were imported.";
        if (has_active_pair) {
            notice += L" The previously confirmed safety hotkeys remain active.";
        } else {
            notice += L" No complete safety-hotkey pair is currently active, so Start remains disabled.";
        }
        if (!result.error.empty()) {
            notice += L"\n\n" + result.error;
        }
        // Ending the import transaction changes enabled / readiness state and
        // can legitimately rebuild the canonical status. Complete that refresh
        // first so it cannot immediately replace the more specific failure.
        FinishSettingsImport(false);
        SetStatusPresentation({
            StatusCategory::Attention,
            profile_load_transaction
                ? (has_active_pair
                       ? L"Status: Profile not loaded | Current hotkeys active"
                       : L"Status: Profile not loaded | Safety hotkeys unavailable")
                : (has_active_pair
                       ? L"Status: Settings not imported | Current hotkeys active"
                       : L"Status: Settings not imported | Safety hotkeys unavailable"),
            notice,
        }, true);
        ShowCenteredMessageBox(
            window_, notice.c_str(),
            profile_load_transaction ? L"Profile not loaded" : L"Settings not imported",
            MB_OK | MB_ICONWARNING);
        return;
    }

    if (history_transaction) {
        const bool target_pair_confirmed =
            result.requested_pair_registered && history_target.has_value() &&
            has_active_pair &&
            confirmed_start_hotkey_ == history_target->start_stop_hotkey &&
            confirmed_emergency_hotkey_ == history_target->emergency_hotkey;
        if (target_pair_confirmed) {
            settings_history_transaction_pending_ = false;
            settings_history_operation_ = SettingsHistoryOperation::None;
            pending_settings_history_target_.reset();

            const bool applied = ApplySettingsHistoryState(*history_target);
            const bool moved = applied &&
                (history_operation == SettingsHistoryOperation::Redo
                     ? settings_history_.CommitRedo()
                     : settings_history_.CommitUndo());
            if (moved) {
                SynchronizeSettingsHistoryFromControls();
            }
            UpdateProfileSelectorPresentation();
            RefreshEnabledState();
            if (IsChecked(remember_settings_check_)) {
                ScheduleSettingsSave();
            }
            return;
        }

        // The settings cursor does not move unless Windows confirms the exact
        // safety-hotkey pair required by the target snapshot. Any active pair
        // reported by the registration service remains the runtime authority.
        settings_history_transaction_pending_ = false;
        settings_history_operation_ = SettingsHistoryOperation::None;
        pending_settings_history_target_.reset();
        RefreshEnabledState();
        SynchronizeSettingsHistoryFromControls();
    }

    std::wstring notice;
    std::wstring title;
    UINT flags = MB_OK | MB_ICONWARNING;
    if (!history_transaction && result.requested_pair_registered) {
        // RefreshEnabledState already rebuilt the complete readiness snapshot.
        // Do not replace it with a generic Ready message because another
        // setting or target can still legitimately keep Start unavailable.
        CommitSettingsHistoryFromControls();
        if (IsChecked(remember_settings_check_)) {
            ScheduleSettingsSave();
        }
        return;
    }

    if (history_transaction) {
        title = history_operation == SettingsHistoryOperation::Redo
                    ? L"Settings redo not applied"
                    : L"Settings undo not applied";
        const wchar_t* action =
            history_operation == SettingsHistoryOperation::Redo ? L"redo" : L"undo";
        notice = L"Windows could not activate the safety-hotkey pair required by this settings " +
                 std::wstring(action) +
                 L". The settings history position was left unchanged.";
        if (has_active_pair) {
            notice += L" The currently confirmed safety hotkeys remain active.";
        } else {
            notice += L" No complete safety-hotkey pair is currently active, so Start remains disabled.";
        }
        if (!result.error.empty()) {
            notice += L"\n\n" + result.error;
        }
        SetStatusPresentation({
            StatusCategory::Attention,
            history_operation == SettingsHistoryOperation::Redo
                ? (has_active_pair
                       ? L"Status: Settings redo not applied | Current hotkeys active"
                       : L"Status: Settings redo not applied | Safety hotkeys unavailable")
                : (has_active_pair
                       ? L"Status: Settings undo not applied | Current hotkeys active"
                       : L"Status: Settings undo not applied | Safety hotkeys unavailable"),
            notice,
        }, true);
    } else if (result.fallback_used && has_active_pair) {
        title = L"Default safety hotkeys restored";
        notice = result.error.empty()
                     ? L"The saved safety hotkeys were unavailable. Vector Click activated \"F5\" for Start / Stop and \"F8\" for Emergency Stop instead."
                     : result.error +
                           L" Vector Click activated \"F5\" for Start / Stop and \"F8\" for Emergency Stop instead.";
        SetStatusPresentation({
            StatusCategory::Attention,
            L"Status: Saved safety hotkeys unavailable | F5 / F8 active",
            notice,
        }, true);
        if (IsChecked(remember_settings_check_)) {
            ScheduleSettingsSave();
        }
    } else if (has_active_pair) {
        title = L"Safety hotkeys unchanged";
        notice = result.error.empty()
                     ? L"Windows rejected the requested safety hotkeys. The previously confirmed pair remains active."
                     : result.error +
                           L" The previously confirmed pair remains active.";
        SetStatusPresentation({
            StatusCategory::Attention,
            L"Status: Requested safety hotkeys unavailable | Previous pair active",
            notice,
        }, true);
    } else {
        title = L"Safety hotkeys unavailable";
        notice = result.error.empty()
                     ? L"Vector Click could not register a safe Start / Stop and Emergency Stop pair. Start remains disabled until a pair can be registered."
                     : result.error +
                           L" Start remains disabled until a safety-hotkey pair can be registered.";
        SetStatusPresentation({
            StatusCategory::Attention,
            L"Status: Safety hotkeys unavailable | Start disabled",
            notice,
        }, true);
    }

    if (!notice.empty()) {
        if (startup_presentation_complete_) {
            ShowCenteredMessageBox(window_, notice.c_str(), title.c_str(), flags);
        } else {
            deferred_hotkey_notice_ = title + L"\n" + notice;
        }
    }
}

void MainWindow::RestoreConfirmedHotkeyControls() {
    suppress_control_events_ = true;
    const bool start_changed =
        SelectHotkey(start_hotkey_combo_, confirmed_start_hotkey_);
    const bool emergency_changed =
        SelectHotkey(emergency_hotkey_combo_, confirmed_emergency_hotkey_);
    suppress_control_events_ = false;
    if (start_changed) {
        InvalidateRect(start_hotkey_combo_, nullptr, FALSE);
    }
    if (emergency_changed) {
        InvalidateRect(emergency_hotkey_combo_, nullptr, FALSE);
    }
    UpdateKeySelectorTooltip(start_hotkey_combo_);
    UpdateKeySelectorTooltip(emergency_hotkey_combo_);
}

void MainWindow::UpdateSafetyHotkeyIndicator() {
    if (hotkeys_group_ == nullptr || IsWindow(hotkeys_group_) == FALSE) {
        return;
    }

    const HotkeyIndicatorState new_state = safety_hotkeys_confirmed_
        ? HotkeyIndicatorState::Active
        : hotkey_registration_pending_ ? HotkeyIndicatorState::Registering
                                       : HotkeyIndicatorState::Unavailable;

    // Repaint only when the visible title actually changes. Replacement
    // transactions keep the confirmed pair active, so their visible state
    // remains Active and no header repaint is needed.
    if (hotkey_indicator_state_ != new_state) {
        hotkey_indicator_state_ = new_state;
        RECT header{};
        if (GetClientRect(hotkeys_group_, &header) != FALSE) {
            const UINT control_dpi = std::max<UINT>(96, GetDpiForWindow(hotkeys_group_));
            header.bottom = std::min<LONG>(
                header.bottom, static_cast<LONG>(Scale(39, control_dpi)));
            RedrawWindow(hotkeys_group_,
                         &header,
                         nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
    }

    if (hotkey_registration_pending_ && safety_hotkeys_confirmed_) {
        tooltips_.SetText(
            hotkeys_group_,
            L"The confirmed safety hotkeys remain active while Windows checks the requested replacement pair.");
    } else if (hotkey_registration_pending_) {
        tooltips_.SetText(
            hotkeys_group_,
            L"Vector Click is waiting for Windows to confirm both global safety hotkeys. Start remains unavailable until registration completes.");
    } else if (safety_hotkeys_confirmed_) {
        tooltips_.SetText(
            hotkeys_group_,
            L"Both global safety hotkeys are registered and active. The displayed keys match the pair confirmed by Windows.");
    } else {
        tooltips_.SetText(
            hotkeys_group_,
            L"No complete safety-hotkey pair is active. Start remains unavailable until Windows accepts both keys.");
    }
}

bool MainWindow::CleanupRequired() const noexcept {
    if (controller_ == nullptr) {
        return false;
    }
    return RequiresCleanupRecovery(
        ToPresentationEngineState(controller_->State()),
        controller_->HasTrackedInput());
}

bool MainWindow::SafetyHotkeysReady() const noexcept {
    // A confirmed pair remains usable while a replacement pair is tested.
    // Pending registration blocks Start only when no safe pair is active yet,
    // such as during startup or recovery from total registration failure.
    return safety_hotkeys_confirmed_ &&
           IsSupportedSafetyHotkeyPair(confirmed_start_hotkey_,
                                       confirmed_emergency_hotkey_);
}

std::wstring MainWindow::SafetyHotkeyUnavailableReason() const {
    if (hotkey_registration_pending_ && !safety_hotkeys_confirmed_) {
        return L"Safety hotkeys are being registered";
    }
    return L"Emergency Stop hotkey is unavailable";
}

void MainWindow::ValidateGeneratedKeySelection(const bool show_errors) {
    if (SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) != 1) {
        return;
    }

    const core::HotkeyBinding key = SelectedGeneratedKey();
    const core::HotkeyBinding start_key = SelectedHotkey(start_hotkey_combo_);
    const core::HotkeyBinding emergency_key = SelectedHotkey(emergency_hotkey_combo_);
    if (!core::SamePhysicalKey(key, start_key) &&
        !core::SamePhysicalKey(key, emergency_key)) {
        settings_cache_.generated_virtual_key = key.virtual_key;
        settings_cache_.generated_key_modifiers = key.modifiers;
        return;
    }

    std::optional<core::HotkeyBinding> replacement;
    const core::HotkeyBinding previous_key{
        settings_cache_.generated_virtual_key,
        settings_cache_.generated_key_modifiers,
    };
    if (previous_key.IsAssigned() &&
        !core::SamePhysicalKey(previous_key, start_key) &&
        !core::SamePhysicalKey(previous_key, emergency_key)) {
        replacement = previous_key;
    } else {
        std::vector<core::HotkeyBinding> ordered_keys;
        ordered_keys.reserve(generated_key_choices_.size());
        for (const auto& choice : generated_key_choices_) {
            ordered_keys.push_back(choice.second);
        }
        replacement = core::FindNextAvailableGeneratedKey(
            ordered_keys, key, start_key, emergency_key);
    }

    suppress_control_events_ = true;
    if (replacement.has_value()) {
        settings_cache_.generated_virtual_key = replacement->virtual_key;
        settings_cache_.generated_key_modifiers = replacement->modifiers;
        SelectGeneratedKey(*replacement);
    } else {
        settings_cache_.generated_virtual_key = 0;
        settings_cache_.generated_key_modifiers = 0;
        SendMessageW(generated_key_combo_, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    suppress_control_events_ = false;
    InvalidateRect(generated_key_combo_, nullptr, TRUE);
    UpdateWindow(generated_key_combo_);

    if (show_errors) {
        const wchar_t* safety_name =
            core::SamePhysicalKey(key, emergency_key)
                ? L"Emergency Stop"
                : L"Start / Stop";
        std::wstring message = L"This physical key is assigned to the " +
                               std::wstring(safety_name) +
                               L" hotkey and cannot also be generated, even with a different Shift state.";
        if (replacement.has_value()) {
            const auto replacement_choice = std::ranges::find_if(
                generated_key_choices_, [replacement](const auto& choice) {
                    return choice.second == *replacement;
                });
            const std::wstring replacement_name =
                replacement_choice != generated_key_choices_.end()
                    ? replacement_choice->first
                    : std::to_wstring(replacement->virtual_key);
            message += L" Vector Click selected \"" + replacement_name +
                       L"\" instead. You can choose another non-reserved keyboard key.";
        } else {
            message += L" No available replacement key was found.";
        }
        ShowCenteredMessageBox(window_, message.c_str(), L"Reserved safety key",
                               MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::HandleCaptureExclusionToggle() {
    const bool requested = IsChecked(capture_exclusion_check_);

    // Retire application-owned transient surfaces before changing the main
    // window's DWM capture state. Persistent hidden popups are synchronized at
    // their final position immediately before their next show instead of being
    // updated while parked near the desktop origin.
    CloseComboPopup(false);
    tooltips_.Hide();
    ResetClickPositionIndicator();
    HideBasicTopSnapshot();
    HideMoveCover();

    // Immediately before activation the window is still ordinarily visible,
    // so preserve one exact composed frame while that capture is legitimate.
    // This gives the newly protected route a complete first movement cover
    // without printing the full child hierarchy during the checkbox command
    // or the first WM_ENTERSIZEMOVE.
    if (requested &&
        !move_cover_pending_exact_screen_refresh_) {
        (void)CaptureMoveCoverFromScreen();
    }

    const CaptureExclusionOutcome main_outcome =
        ApplyCaptureExclusion(window_, requested);
    if (main_outcome.result != CaptureExclusionResult::Applied) {
        // Failed activation returns to off. Failed removal remains visibly on
        // because Windows may still retain the exclusion on the main window.
        suppress_control_events_ = true;
        SetChecked(capture_exclusion_check_, !requested);
        suppress_control_events_ = false;
        InvalidateControl(capture_exclusion_check_);

        std::wstring message;
        if (main_outcome.result == CaptureExclusionResult::Unsupported) {
            message =
                L"Complete screen-capture exclusion requires Windows 10 version 2004 "
                L"(build 19041) or newer. The option was left off.";
        } else if (requested) {
            message =
                L"Windows could not enable screen-capture hiding for Vector Click. "
                L"The option was left off.";
        } else {
            message =
                L"Windows could not remove screen-capture hiding from Vector Click. "
                L"The option remains shown as enabled.";
        }

        if (main_outcome.error != ERROR_SUCCESS &&
            main_outcome.result != CaptureExclusionResult::Unsupported) {
            message += L"\n\nWindows error code: " +
                       std::to_wstring(main_outcome.error);
        }

        ShowCenteredMessageBox(window_,
                               message.c_str(),
                               L"Screen-capture hiding",
                               MB_OK | MB_ICONWARNING);
        return;
    }

    SetCaptureExclusionRequested(requested);
    if (profile_manager_ && profile_manager_->IsOpen()) {
        profile_manager_->SynchronizeOwnerPresentation();
    }
    if (requested) {
        // Application-owned rendering is authoritative while exclusion is
        // active. A correct application-owned frame therefore needs no later
        // ordinary desktop replacement, and a failed activation leaves the marker
        // untouched because this code is reached only after success.
        move_cover_pending_exact_screen_refresh_ = false;
        move_cover_exact_refresh_deferred_ = false;
    }

    // A complete bitmap is safe to retain across the affinity transition: it
    // contains only Vector Click's own last stable frame, and any top-level
    // surface that presents it receives the current requested affinity before
    // being shown. Refresh later only when no usable frame was available.
    if (!HasReusableMoveCover()) {
        QueueMoveCoverRefresh();
    }

    ScheduleSettingsSave();
}

bool MainWindow::SetMainWindowTopmost(const bool topmost) noexcept {
    if (window_ == nullptr || IsWindow(window_) == FALSE) {
        return false;
    }

    const LONG_PTR extended_style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
    const bool currently_topmost = (extended_style & WS_EX_TOPMOST) != 0;
    if (currently_topmost == topmost) {
        return true;
    }

    return SetWindowPos(
               window_,
               topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
               0,
               0,
               0,
               0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
}

void MainWindow::HandleKeepOnTopToggle() {
    const bool requested = IsChecked(keep_on_top_check_);
    if (SetMainWindowTopmost(requested)) {
        if (requested) {
            // A tooltip that was already visible before the main window moved
            // into the topmost band can otherwise be left immediately behind
            // it. Reassert the existing popup without changing focus or hover.
            tooltips_.ReassertTopmost();
        }
        if (profile_manager_ && profile_manager_->IsOpen()) {
            profile_manager_->SynchronizeOwnerPresentation();
        }

        ScheduleSettingsSave();
        return;
    }

    suppress_control_events_ = true;
    SetChecked(keep_on_top_check_, !requested);
    suppress_control_events_ = false;
    InvalidateControl(keep_on_top_check_);

    ShowCenteredMessageBox(
        window_,
        requested
            ? L"Windows could not keep the Vector Click window on top. The option was left off."
            : L"Windows could not return the Vector Click window to normal Z-order. The option remains enabled.",
        L"Keep on top",
        MB_OK | MB_ICONWARNING);
}

void MainWindow::HandleRememberSettingsToggle() {
    const bool checked = IsChecked(remember_settings_check_);
    if (!checked) {
        CancelScheduledSettingsSave();
    }

    if (checked && !settings_store_.Exists()) {
        const std::wstring message =
            L"Remembering settings will create 'VectorClick settings.json' in the same folder as this application. "
            L"The file contains only Vector Click settings. No file will be created if you cancel.";
        const int answer = ShowCenteredMessageBox(window_,
                                                   message.c_str(),
                                                   L"Create portable settings file?",
                                                   MB_OKCANCEL | MB_ICONINFORMATION);
        if (answer != IDOK) {
            suppress_control_events_ = true;
            SetChecked(remember_settings_check_, false);
            suppress_control_events_ = false;
            return;
        }
    }

    if (!checked && settings_store_.Exists()) {
        const int answer = ShowCenteredMessageBox(
            window_,
            L"Choose Yes to delete 'VectorClick settings.json'. Choose No to keep the file but stop loading it automatically. Choose Cancel to keep remembering settings.",
            L"Disable portable settings?",
            MB_YESNOCANCEL | MB_ICONINFORMATION);
        if (answer == IDCANCEL) {
            suppress_control_events_ = true;
            SetChecked(remember_settings_check_, true);
            suppress_control_events_ = false;
            ScheduleSettingsSave();
            return;
        }
        std::wstring error;
        if (answer == IDYES) {
            if (!settings_store_.Remove(error)) {
                ShowCenteredMessageBox(window_, error.c_str(), L"Settings file", MB_OK | MB_ICONERROR);
                SetChecked(remember_settings_check_, true);
                ScheduleSettingsSave();
            } else {
                last_saved_settings_.reset();
                last_saved_profile_id_.reset();
            }
            return;
        }

        core::RunSettings settings;
        if (ReadSettings(settings, error)) {
            settings.remember_settings = false;
            settings.timing_worker_priority_mode =
                core::TimingWorkerPriorityMode::SystemDefault;
            settings.timing_worker_qos_mode =
                core::TimingWorkerQosMode::SystemManaged;
            if (!settings_store_.Save(settings, error)) {
                ShowCenteredMessageBox(window_, error.c_str(), L"Settings file", MB_OK | MB_ICONERROR);
            } else {
                last_saved_settings_ = settings;
                last_saved_profile_id_.reset();
            }
        }
        return;
    }

    if (checked) {
        ScheduleSettingsSave();
    }
}

bool MainWindow::CanManageProfiles() const noexcept {
    if (settings_import_picker_open_ || settings_import_transaction_pending_ ||
        settings_history_transaction_pending_ || hotkey_registration_pending_ ||
        key_capture_active_.load(std::memory_order_acquire) ||
        shutdown_in_progress_.load(std::memory_order_acquire) ||
        emergency_latched_.load(std::memory_order_acquire) ||
        safety_shield_.IsVisible() || target_picker_open_ ||
        position_capture_seconds_remaining_ != 0 || CleanupRequired() ||
        !SafetyHotkeysReady()) {
        return false;
    }
    if (controller_ == nullptr) {
        return false;
    }
    const EngineState state = controller_->State();
    return state == EngineState::Ready || state == EngineState::Disarmed;
}

bool MainWindow::CaptureCurrentProfileSettings(core::RunSettings& settings,
                                               std::wstring& error) const {
    if (!CanManageProfiles() || !ReadSettings(settings, error)) {
        if (error.empty()) {
            error = L"The current Vector Click settings are not available for profile storage right now.";
        }
        return false;
    }
    settings.start_stop_hotkey = confirmed_start_hotkey_;
    settings.emergency_hotkey = confirmed_emergency_hotkey_;
    settings = ProfileStore::NormalizeSettingsForProfile(settings);
    return true;
}

void MainWindow::RefreshProfileSelector() {
    if (profile_combo_ == nullptr || IsWindow(profile_combo_) == FALSE) {
        return;
    }

    std::vector<ProfileInfo> profiles;
    std::wstring error;
    if (!profile_store_.List(profiles, nullptr, error)) {
        profiles.clear();
    }

    if (active_profile_info_.has_value()) {
        auto found = std::find_if(
            profiles.begin(), profiles.end(),
            [this](const ProfileInfo& profile) {
                return CompareStringOrdinal(
                           profile.id.c_str(), -1,
                           active_profile_info_->id.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
        if (found == profiles.end()) {
            active_profile_info_.reset();
            active_profile_saved_settings_.reset();
        } else {
            core::RunSettings saved{};
            ProfileInfo refreshed{};
            std::wstring load_error;
            if (profile_store_.Load(found->id, saved, refreshed, load_error)) {
                active_profile_info_ = refreshed;
                active_profile_saved_settings_ =
                    ProfileStore::NormalizeSettingsForProfile(saved);
            } else {
                active_profile_info_.reset();
                active_profile_saved_settings_.reset();
            }
        }
    }

    profile_selector_profiles_ = std::move(profiles);
    refreshing_profile_selector_ = true;
    SendMessageW(profile_combo_, CB_RESETCONTENT, 0, 0);
    SendMessageW(profile_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"No profile (current settings)"));
    int selected = 0;
    for (std::size_t index = 0; index < profile_selector_profiles_.size(); ++index) {
        const auto& profile = profile_selector_profiles_[index];
        SendMessageW(profile_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(profile.name.c_str()));
        if (active_profile_info_.has_value() &&
            CompareStringOrdinal(profile.id.c_str(), -1,
                                 active_profile_info_->id.c_str(), -1, TRUE) == CSTR_EQUAL) {
            selected = static_cast<int>(index + 1U);
        }
    }
    SendMessageW(profile_combo_, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    refreshing_profile_selector_ = false;
    UpdateProfileSelectorPresentation();
    UpdateProfileSelectorDroppedWidth();
    UpdateProfileSelectorTooltip();
}

void MainWindow::UpdateProfileSelectorPresentation() {
    if (profile_combo_ == nullptr || IsWindow(profile_combo_) == FALSE ||
        refreshing_profile_selector_) {
        return;
    }

    if (!active_profile_info_.has_value() ||
        !active_profile_saved_settings_.has_value()) {
        if (SendMessageW(profile_combo_, CB_GETCURSEL, 0, 0) != 0) {
            refreshing_profile_selector_ = true;
            SendMessageW(profile_combo_, CB_SETCURSEL, 0, 0);
            refreshing_profile_selector_ = false;
        }
        UpdateProfileSelectorTooltip();
        return;
    }

    int active_index = -1;
    for (std::size_t index = 0; index < profile_selector_profiles_.size(); ++index) {
        if (CompareStringOrdinal(
                profile_selector_profiles_[index].id.c_str(), -1,
                active_profile_info_->id.c_str(), -1, TRUE) == CSTR_EQUAL) {
            active_index = static_cast<int>(index + 1U);
            break;
        }
    }
    if (active_index < 1) {
        active_profile_info_.reset();
        active_profile_saved_settings_.reset();
        RefreshProfileSelector();
        return;
    }

    bool modified = true;
    core::RunSettings current{};
    std::wstring current_error;
    if (ReadSettings(current, current_error)) {
        current.start_stop_hotkey = confirmed_start_hotkey_;
        current.emergency_hotkey = confirmed_emergency_hotkey_;
        current = ProfileStore::NormalizeSettingsForProfile(current);
        modified = current != *active_profile_saved_settings_;
    }

    std::wstring display = active_profile_info_->name;
    if (modified) {
        display += L" (Modified)";
    }
    const std::wstring existing = [&]() {
        const LRESULT length = SendMessageW(profile_combo_, CB_GETLBTEXTLEN,
                                            static_cast<WPARAM>(active_index), 0);
        if (length < 0) return std::wstring{};
        std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
        SendMessageW(profile_combo_, CB_GETLBTEXT,
                     static_cast<WPARAM>(active_index),
                     reinterpret_cast<LPARAM>(value.data()));
        value.resize(static_cast<std::size_t>(length));
        return value;
    }();

    if (existing != display) {
        refreshing_profile_selector_ = true;
        SendMessageW(profile_combo_, CB_DELETESTRING,
                     static_cast<WPARAM>(active_index), 0);
        SendMessageW(profile_combo_, CB_INSERTSTRING,
                     static_cast<WPARAM>(active_index),
                     reinterpret_cast<LPARAM>(display.c_str()));
        SendMessageW(profile_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(active_index), 0);
        refreshing_profile_selector_ = false;
        UpdateProfileSelectorDroppedWidth();
    } else if (SendMessageW(profile_combo_, CB_GETCURSEL, 0, 0) != active_index) {
        refreshing_profile_selector_ = true;
        SendMessageW(profile_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(active_index), 0);
        refreshing_profile_selector_ = false;
    }
    UpdateProfileSelectorTooltip();
}

void MainWindow::UpdateProfileSelectorDroppedWidth() {
    if (profile_combo_ == nullptr || IsWindow(profile_combo_) == FALSE) {
        return;
    }
    HDC dc = GetDC(profile_combo_);
    if (dc == nullptr) {
        return;
    }
    const HGDIOBJ font = font_.get() != nullptr
        ? font_.get() : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ previous = SelectObject(dc, font);
    int widest = 0;
    const LRESULT count = SendMessageW(profile_combo_, CB_GETCOUNT, 0, 0);
    for (LRESULT index = 0; index < count; ++index) {
        const LRESULT length = SendMessageW(profile_combo_, CB_GETLBTEXTLEN,
                                            static_cast<WPARAM>(index), 0);
        if (length <= 0) {
            continue;
        }
        std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
        SendMessageW(profile_combo_, CB_GETLBTEXT,
                     static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(value.data()));
        SIZE extent{};
        if (GetTextExtentPoint32W(dc, value.data(), static_cast<int>(length), &extent) != FALSE) {
            widest = std::max(widest, static_cast<int>(extent.cx));
        }
    }
    if (previous != nullptr && previous != HGDI_ERROR) {
        SelectObject(dc, previous);
    }
    ReleaseDC(profile_combo_, dc);

    RECT combo_rect{};
    GetWindowRect(profile_combo_, &combo_rect);
    const int closed_width = combo_rect.right - combo_rect.left;
    const int desired = widest + Scale(48, GetDpiForWindow(profile_combo_));

    HMONITOR monitor = MonitorFromWindow(profile_combo_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    int maximum = Scale(520, GetDpiForWindow(profile_combo_));
    if (GetMonitorInfoW(monitor, &info) != FALSE) {
        const int work_width = static_cast<int>(info.rcWork.right - info.rcWork.left);
        maximum = std::min(maximum,
                           std::max(closed_width,
                                    work_width - Scale(40, GetDpiForWindow(profile_combo_))));
    }
    SendMessageW(profile_combo_, CB_SETDROPPEDWIDTH,
                 static_cast<WPARAM>(std::clamp(desired, closed_width, maximum)), 0);
}

void MainWindow::UpdateProfileSelectorTooltip() {
    if (profile_combo_ == nullptr || IsWindow(profile_combo_) == FALSE) {
        return;
    }
    constexpr wchar_t description[] =
        L"Choose a named local profile. Profiles are separate validated snapshots and are never overwritten merely because you change the current settings.";
    const std::wstring selected = SelectedComboText(profile_combo_);
    std::wstring tooltip(description);
    if (IsComboSelectionClipped(profile_combo_, selected)) {
        tooltip = selected + L"\n\n" + description;
    }
    tooltips_.SetText(profile_combo_, std::move(tooltip));
    tooltips_.RefreshVisible(profile_combo_);
}

void MainWindow::OnProfilesChanged() {
    const std::optional<std::wstring> previous_id =
        active_profile_info_.has_value()
            ? std::optional<std::wstring>(active_profile_info_->id)
            : std::nullopt;
    RefreshProfileSelector();
    const std::optional<std::wstring> current_id =
        active_profile_info_.has_value()
            ? std::optional<std::wstring>(active_profile_info_->id)
            : std::nullopt;
    if (previous_id != current_id) {
        ScheduleSettingsSave();
    }
}

void MainWindow::OpenProfileManager() {
    if (!CanManageProfiles() || profile_manager_open_) {
        return;
    }
    if (!profile_manager_) {
        ProfileManagerWindow::Callbacks callbacks;
        callbacks.capture_current_settings =
            [this](core::RunSettings& settings, std::wstring& error) {
                return CaptureCurrentProfileSettings(settings, error);
            };
        callbacks.operations_allowed = [this] { return CanManageProfiles(); };
        callbacks.profiles_changed = [this] { OnProfilesChanged(); };
        profile_manager_ = std::make_unique<ProfileManagerWindow>(
            instance_, window_, profile_store_, std::move(callbacks));
    }

    // Keep the same explicit modal-transaction boundary used by the target
    // picker. In particular, a queued global Start request must not begin an
    // automation run while the owner is disabled behind the profile manager.
    profile_manager_open_ = true;
    RefreshEnabledState();
    const bool shown = profile_manager_->Show();
    profile_manager_open_ = false;
    RefreshEnabledState();

    if (!shown) {
        ShowCenteredMessageBox(
            window_,
            L"The local profile manager window could not be created.",
            L"Profiles unavailable", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::HandleProfileSelection() {
    if (refreshing_profile_selector_ || profile_combo_ == nullptr) {
        return;
    }
    const LRESULT selection = SendMessageW(profile_combo_, CB_GETCURSEL, 0, 0);
    if (selection <= 0) {
        const bool was_associated = active_profile_info_.has_value();
        active_profile_info_.reset();
        active_profile_saved_settings_.reset();
        UpdateProfileSelectorPresentation();
        if (was_associated) {
            ScheduleSettingsSave();
        }
        return;
    }
    const std::size_t index = static_cast<std::size_t>(selection - 1);
    if (index >= profile_selector_profiles_.size() || !CanManageProfiles()) {
        RefreshProfileSelector();
        return;
    }
    if (!BeginProfileLoad(profile_selector_profiles_[index])) {
        RefreshProfileSelector();
    }
}

bool MainWindow::BeginProfileLoad(const ProfileInfo& requested_profile) {
    if (!CanManageProfiles()) {
        return false;
    }

    core::RunSettings saved_profile{};
    ProfileInfo profile{};
    std::wstring error;
    if (!profile_store_.Load(requested_profile.id, saved_profile, profile, error)) {
        ShowCenteredMessageBox(
            window_, error.c_str(), L"Profile not loaded", MB_OK | MB_ICONWARNING);
        return false;
    }

    core::RunSettings applied = saved_profile;
    applied.remember_settings = IsChecked(remember_settings_check_);
    applied.timing_worker_priority_mode = SelectedTimingWorkerPriorityMode();
    applied.hotkey_control_priority_mode = SelectedHotkeyControlPriorityMode();
    applied.timing_worker_qos_mode = SelectedTimingWorkerQosMode();

    const auto merged_issues = core::ValidateRunSettings(applied);
    if (core::HasErrors(merged_issues)) {
        std::wstring message =
            L"The selected profile conflicts with the current session-only scheduling configuration. No settings were changed.";
        if (!merged_issues.empty() && !merged_issues.front().message.empty()) {
            message += L"\n\n";
            message.append(merged_issues.front().message.begin(),
                           merged_issues.front().message.end());
        }
        ShowCenteredMessageBox(
            window_, message.c_str(), L"Profile not loaded", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (!IsSupportedSafetyHotkeyPair(applied.start_stop_hotkey,
                                     applied.emergency_hotkey)) {
        ShowCenteredMessageBox(
            window_,
            L"The selected profile does not contain a supported Start / Stop and Emergency Stop hotkey pair. No settings were changed.",
            L"Profile not loaded", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (applied.action_type == core::ActionType::MouseClick &&
        applied.position_mode == core::PositionMode::FixedScreen &&
        !IsPointOnVirtualDesktop(applied.fixed_x, applied.fixed_y)) {
        ShowCenteredMessageBox(
            window_,
            L"The profile's fixed mouse position is outside the current virtual desktop. No settings were changed.",
            L"Profile not loaded", MB_OK | MB_ICONWARNING);
        return false;
    }

    settings_import_transaction_pending_ = true;
    settings_import_source_is_canonical_ = false;
    settings_import_is_profile_load_ = true;
    pending_settings_import_target_ = applied;
    pending_profile_load_info_ = profile;
    pending_profile_load_saved_settings_ =
        ProfileStore::NormalizeSettingsForProfile(saved_profile);
    RefreshEnabledState();

    if (applied.start_stop_hotkey != confirmed_start_hotkey_ ||
        applied.emergency_hotkey != confirmed_emergency_hotkey_) {
        if (!BeginSettingsImportHotkeyTransaction(applied)) {
            FinishSettingsImport(false);
            return false;
        }
        return true;
    }

    const bool installed = ApplyImportedSettings(applied);
    FinishSettingsImport(installed);
    if (!installed) {
        ShowCenteredMessageBox(
            window_,
            L"Vector Click could not complete the validated profile load. The current configuration was left as close to its prior state as possible.",
            L"Profile not loaded", MB_OK | MB_ICONWARNING);
    }
    return installed;
}

bool MainWindow::CanImportSettings() const noexcept {
    if (settings_import_picker_open_ || settings_import_transaction_pending_ ||
        settings_history_transaction_pending_ ||
        hotkey_registration_pending_ ||
        key_capture_active_.load(std::memory_order_acquire) ||
        shutdown_in_progress_.load(std::memory_order_acquire) ||
        emergency_latched_.load(std::memory_order_acquire) ||
        safety_shield_.IsVisible() || target_picker_open_ ||
        position_capture_seconds_remaining_ != 0 || CleanupRequired() ||
        !SafetyHotkeysReady()) {
        return false;
    }
    if (combo_popup_ != nullptr && IsWindow(combo_popup_) != FALSE &&
        IsWindowVisible(combo_popup_) != FALSE) {
        return false;
    }
    if (controller_ == nullptr) {
        return false;
    }
    const EngineState state = controller_->State();
    return state == EngineState::Ready || state == EngineState::Disarmed;
}

void MainWindow::ImportSettingsFromFile() {
    if (!CanImportSettings()) {
        return;
    }

    CloseComboPopup(false);
    tooltips_.Hide();
    EndKeyCapture();

    std::vector<wchar_t> path_buffer(32'768U, L'\0');
    constexpr wchar_t filter[] =
        L"Vector Click settings (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = filter;
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = path_buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(path_buffer.size());
    dialog.lpstrTitle = L"Import Vector Click settings";
    dialog.lpstrDefExt = L"json";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                   OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;

    settings_import_picker_open_ = true;
    RefreshEnabledState();
    const BOOL file_selected = GetOpenFileNameW(&dialog);
    const DWORD dialog_error = file_selected == FALSE ? CommDlgExtendedError() : 0;
    settings_import_picker_open_ = false;
    RefreshEnabledState();

    if (file_selected == FALSE) {
        if (dialog_error != 0) {
            const std::wstring message =
                L"Windows could not open the settings file picker. No settings were changed.\n\nWindows common-dialog error code: " +
                std::to_wstring(dialog_error);
            ShowCenteredMessageBox(
                window_, message.c_str(), L"Settings not imported",
                MB_OK | MB_ICONWARNING);
        }
        return;
    }

    // The native picker owns a modal message loop, so global safety hotkeys can
    // still change Vector Click's state while it is open. Revalidate the whole
    // import boundary after the picker closes rather than trusting the state
    // that existed when the button was first pressed.
    if (!CanImportSettings()) {
        ShowCenteredMessageBox(
            window_,
            L"Vector Click's state changed while the settings file picker was open. No settings were imported.",
            L"Settings not imported",
            MB_OK | MB_ICONWARNING);
        return;
    }

    core::RunSettings imported = core::DefaultRunSettings();
    std::wstring error;
    if (!settings_store_.LoadFromPath(path_buffer.data(), imported, error)) {
        if (error.empty()) {
            error = L"The selected file could not be loaded as Vector Click settings.";
        }
        const std::wstring message =
            error + L"\n\nThe selected file was left unchanged and no settings were imported.";
        ShowCenteredMessageBox(
            window_, message.c_str(), L"Settings not imported",
            MB_OK | MB_ICONWARNING);
        return;
    }

    // Import never changes the user's current persistence decision. The three
    // experimental scheduling controls are also session-only and are not part
    // of the portable JSON contract, so a file that cannot express them must
    // not silently reset the current session choices.
    imported.remember_settings = IsChecked(remember_settings_check_);
    imported.timing_worker_priority_mode = SelectedTimingWorkerPriorityMode();
    imported.hotkey_control_priority_mode = SelectedHotkeyControlPriorityMode();
    imported.timing_worker_qos_mode = SelectedTimingWorkerQosMode();

    // The file is validated before this merge, but the experimental scheduling
    // controls are intentionally session-only. Revalidate the combined state
    // so an imported process-priority choice cannot create a newly invalid
    // priority combination with a session-only worker policy.
    const auto merged_issues = core::ValidateRunSettings(imported);
    if (core::HasErrors(merged_issues)) {
        std::wstring message =
            L"The selected settings conflict with the current session-only scheduling configuration. No settings were changed.";
        if (!merged_issues.empty() && !merged_issues.front().message.empty()) {
            message += L"\n\n";
            message.append(merged_issues.front().message.begin(),
                           merged_issues.front().message.end());
        }
        ShowCenteredMessageBox(
            window_, message.c_str(), L"Settings not imported",
            MB_OK | MB_ICONWARNING);
        return;
    }

    if (!IsSupportedSafetyHotkeyPair(imported.start_stop_hotkey,
                                     imported.emergency_hotkey)) {
        ShowCenteredMessageBox(
            window_,
            L"The selected settings file does not contain a supported Start / Stop and Emergency Stop hotkey pair. No settings were changed.",
            L"Settings not imported",
            MB_OK | MB_ICONWARNING);
        return;
    }

    if (imported.action_type == core::ActionType::MouseClick &&
        imported.position_mode == core::PositionMode::FixedScreen &&
        !IsPointOnVirtualDesktop(imported.fixed_x, imported.fixed_y)) {
        ShowCenteredMessageBox(
            window_,
            L"The imported fixed mouse position is outside the current virtual desktop. No settings were changed.",
            L"Settings not imported",
            MB_OK | MB_ICONWARNING);
        return;
    }

    settings_import_transaction_pending_ = true;
    settings_import_source_is_canonical_ =
        settings_store_.IsCanonicalPath(path_buffer.data());
    pending_settings_import_target_ = imported;
    RefreshEnabledState();

    if (imported.start_stop_hotkey != confirmed_start_hotkey_ ||
        imported.emergency_hotkey != confirmed_emergency_hotkey_) {
        if (!BeginSettingsImportHotkeyTransaction(imported)) {
            FinishSettingsImport(false);
        }
        return;
    }

    const bool applied = ApplyImportedSettings(imported);
    FinishSettingsImport(applied);
    if (!applied) {
        ShowCenteredMessageBox(
            window_,
            L"Vector Click could not complete the validated settings import. The current configuration was left as close to its prior state as possible.",
            L"Settings not imported",
            MB_OK | MB_ICONWARNING);
    }
}

bool MainWindow::BeginSettingsImportHotkeyTransaction(
    const core::RunSettings& target) {
    if (!settings_import_transaction_pending_ || !hotkey_thread_ ||
        hotkey_registration_pending_ || settings_history_transaction_pending_ ||
        !IsSupportedSafetyHotkeyPair(target.start_stop_hotkey,
                                     target.emergency_hotkey)) {
        return false;
    }

    RestoreConfirmedHotkeyControls();
    hotkey_registration_pending_ = true;
    UpdateSafetyHotkeyIndicator();
    RefreshEnabledState();

    const std::uint64_t request_id = hotkey_thread_->Reconfigure(
        target.start_stop_hotkey, target.emergency_hotkey);
    if (request_id == 0) {
        hotkey_registration_pending_ = false;
        UpdateSafetyHotkeyIndicator();
        RefreshEnabledState();
        ShowCenteredMessageBox(
            window_,
            settings_import_is_profile_load_
                ? L"Vector Click could not queue the safety-hotkey change required by the selected profile. The current settings were left unchanged."
                : L"Vector Click could not queue the safety-hotkey change required by the selected settings file. The current settings were left unchanged.",
            settings_import_is_profile_load_ ? L"Profile not loaded" : L"Settings not imported",
            MB_OK | MB_ICONWARNING);
        return false;
    }

    pending_hotkey_request_id_ = request_id;
    SetStatusPresentation({
        StatusCategory::Transition,
        settings_import_is_profile_load_
            ? L"Status: Loading profile..."
            : L"Status: Importing settings...",
        settings_import_is_profile_load_
            ? L"Vector Click is waiting for Windows to confirm the safety-hotkey pair required by the selected profile."
            : L"Vector Click is waiting for Windows to confirm the safety-hotkey pair required by the selected settings file.",
    }, true);
    return true;
}

bool MainWindow::ApplyImportedSettings(core::RunSettings settings) {
    if (!settings_import_transaction_pending_ || !SafetyHotkeysReady() ||
        settings.start_stop_hotkey != confirmed_start_hotkey_ ||
        settings.emergency_hotkey != confirmed_emergency_hotkey_) {
        return false;
    }

    // Preserve the current persistence and session-only scheduling choices even
    // if this helper is reached after an asynchronous hotkey transaction.
    settings.remember_settings = IsChecked(remember_settings_check_);
    settings.timing_worker_priority_mode = SelectedTimingWorkerPriorityMode();
    settings.hotkey_control_priority_mode = SelectedHotkeyControlPriorityMode();
    settings.timing_worker_qos_mode = SelectedTimingWorkerQosMode();
    settings.start_stop_hotkey = confirmed_start_hotkey_;
    settings.emergency_hotkey = confirmed_emergency_hotkey_;

    // Privacy and Z-order are immediate Windows transactions rather than plain
    // control state. Use their established paths only when the imported value
    // actually differs. A rejected Windows-dependent option reverts to the
    // actually active state while the rest of the validated import continues.
    const bool previous_capture_exclusion = IsChecked(capture_exclusion_check_);
    if (settings.hide_from_screen_capture != previous_capture_exclusion) {
        suppress_control_events_ = true;
        SetChecked(capture_exclusion_check_, settings.hide_from_screen_capture);
        suppress_control_events_ = false;
        HandleCaptureExclusionToggle();
    }
    settings.hide_from_screen_capture = IsChecked(capture_exclusion_check_);

    const bool previous_topmost = IsChecked(keep_on_top_check_);
    if (settings.keep_window_on_top != previous_topmost) {
        suppress_control_events_ = true;
        SetChecked(keep_on_top_check_, settings.keep_window_on_top);
        suppress_control_events_ = false;
        HandleKeepOnTopToggle();
    }
    settings.keep_window_on_top = IsChecked(keep_on_top_check_);

    // Process priority is another immediate Windows transaction. Resolve it
    // before installing the retained visual frame so an OS rejection dialog is
    // never presented underneath the temporary transition overlay.
    if (process_priority_manager_.Mode() != settings.process_priority_mode) {
        std::wstring priority_error;
        if (!process_priority_manager_.SetMode(
                settings.process_priority_mode, priority_error)) {
            settings.process_priority_mode = process_priority_manager_.Mode();
            if (!priority_error.empty()) {
                ShowCenteredMessageBox(
                    window_,
                    priority_error.c_str(),
                    L"Process priority unchanged",
                    MB_OK | MB_ICONWARNING);
            }
        } else if (hotkey_thread_) {
            (void)hotkey_thread_->RefreshPriorityPolicy();
        }
    }

    bool retained_frame_active = false;
    if (startup_presentation_complete_ && window_ != nullptr &&
        IsWindow(window_) != FALSE && IsWindowVisible(window_) != FALSE &&
        !interactive_resize_ && !programmatic_resize_overlay_) {
        retained_frame_active = BeginResizeOverlay(nullptr, 0, false);
    }

    ApplySettings(settings);

    // RefreshEnabledState can legitimately adapt target-dependent controls to
    // the current session (for example, background input without a usable
    // target). Re-read the installed controls so persistence and the new Undo /
    // Redo baseline describe what is actually active rather than only what the
    // selected file requested.
    core::RunSettings applied_settings = settings;
    std::wstring applied_error;
    if (ReadSettings(applied_settings, applied_error)) {
        applied_settings.start_stop_hotkey = confirmed_start_hotkey_;
        applied_settings.emergency_hotkey = confirmed_emergency_hotkey_;
        applied_settings.remember_settings = IsChecked(remember_settings_check_);
        applied_settings.timing_worker_priority_mode =
            SelectedTimingWorkerPriorityMode();
        applied_settings.hotkey_control_priority_mode =
            SelectedHotkeyControlPriorityMode();
        applied_settings.timing_worker_qos_mode = SelectedTimingWorkerQosMode();
        settings_cache_ = applied_settings;
    } else {
        // The selected file was already validated and environment-dependent
        // fixed coordinates were preflighted. Keep this defensive fallback
        // deterministic if a future control adds a new runtime-only constraint.
        settings.process_priority_mode = process_priority_manager_.Mode();
        settings_cache_ = settings;
    }

    SynchronizeNumericEditHistorySession();
    if (!IsChecked(click_position_indicator_check_)) {
        ResetClickPositionIndicator();
    }
    UpdateKeySelectorTooltips();

    core::RunSettings history_baseline{};
    if (CaptureSettingsHistoryState(history_baseline)) {
        settings_history_.Reset(history_baseline);
    }

    // The control hierarchy was intentionally locked while the import was in
    // flight. Re-enable the fully installed configuration before publishing the
    // retained final frame so the user never sees an intermediate disabled
    // version of the newly imported UI. No input can interleave here because
    // the transaction is still completing on the UI thread.
    settings_import_transaction_pending_ = false;
    RefreshEnabledState();

    if (retained_frame_active) {
        RedrawWindow(window_,
                     nullptr,
                     nullptr,
                     RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE |
                         RDW_UPDATENOW);
        (void)DwmFlush();
        HideResizeOverlay();
        MarkMoveCoverPresentationDirty(false);
        QueueMoveCoverRefresh();
    } else {
        MarkMoveCoverPresentationDirty();
    }
    return true;
}

void MainWindow::FinishSettingsImport(const bool imported) {
    const bool selected_canonical_source = settings_import_source_is_canonical_;
    const bool was_profile_load = settings_import_is_profile_load_;
    const std::optional<ProfileInfo> loaded_profile = pending_profile_load_info_;
    const std::optional<core::RunSettings> loaded_profile_settings =
        pending_profile_load_saved_settings_;

    if (imported) {
        if (was_profile_load && loaded_profile.has_value() &&
            loaded_profile_settings.has_value()) {
            active_profile_info_ = *loaded_profile;
            active_profile_saved_settings_ =
                ProfileStore::NormalizeSettingsForProfile(*loaded_profile_settings);
        } else if (!was_profile_load) {
            // A standalone settings import is not associated with whichever
            // named profile may previously have been active.
            active_profile_info_.reset();
            active_profile_saved_settings_.reset();
        }
    }

    settings_import_transaction_pending_ = false;
    settings_import_source_is_canonical_ = false;
    settings_import_is_profile_load_ = false;
    pending_settings_import_target_.reset();
    pending_profile_load_info_.reset();
    pending_profile_load_saved_settings_.reset();
    RefreshEnabledState();
    SynchronizeSettingsHistoryFromControls();
    RefreshProfileSelector();
    if (profile_manager_ && profile_manager_->IsOpen()) {
        profile_manager_->Refresh();
    }

    if (!imported) {
        return;
    }
    if (IsChecked(remember_settings_check_) && !selected_canonical_source) {
        SaveSettingsIfEnabled(true);
    }
}

void MainWindow::ScheduleSettingsSave() {
    if (!suppress_control_events_) {
        UpdateProfileSelectorPresentation();
    }
    if (suppress_control_events_ || settings_import_transaction_pending_ ||
        shutdown_in_progress_.load(std::memory_order_acquire) ||
        !IsChecked(remember_settings_check_) ||
        window_ == nullptr || IsWindow(window_) == FALSE) {
        return;
    }

    settings_save_pending_ =
        StartGeneratedTimer(settings_save_timer_id_,
                            SettingsSaveDebounceMilliseconds) != 0;
    if (!settings_save_pending_) {
        SaveSettingsIfEnabled();
    }
}

void MainWindow::CancelScheduledSettingsSave() noexcept {
    StopGeneratedTimer(settings_save_timer_id_);
    settings_save_pending_ = false;
}

void MainWindow::SaveSettingsIfEnabled(const bool show_errors) {
    CancelScheduledSettingsSave();
    if (!IsChecked(remember_settings_check_) || !SafetyHotkeysReady()) {
        return;
    }

    core::RunSettings settings;
    std::wstring error;
    if (!ReadSettings(settings, error)) {
        return;
    }
    settings.start_stop_hotkey = confirmed_start_hotkey_;
    settings.emergency_hotkey = confirmed_emergency_hotkey_;
    settings.remember_settings = true;
    // The timing-worker selectors remain experimental and intentionally do not
    // become part of the portable settings contract yet.
    settings.timing_worker_priority_mode =
        core::TimingWorkerPriorityMode::SystemDefault;
    settings.timing_worker_qos_mode =
        core::TimingWorkerQosMode::SystemManaged;
    const std::optional<std::wstring> active_profile_id =
        active_profile_info_.has_value()
            ? std::optional<std::wstring>(active_profile_info_->id)
            : std::nullopt;
    if (last_saved_settings_.has_value() && *last_saved_settings_ == settings &&
        last_saved_profile_id_ == active_profile_id) {
        return;
    }
    const std::wstring_view profile_id_for_save =
        active_profile_id.has_value() ? std::wstring_view(*active_profile_id)
                                      : std::wstring_view{};
    if (!settings_store_.Save(settings, error, profile_id_for_save)) {
        if (show_errors) {
            ShowCenteredMessageBox(
                window_, error.c_str(), L"Settings not saved", MB_OK | MB_ICONERROR);
        }
    } else {
        last_saved_settings_ = settings;
        last_saved_profile_id_ = active_profile_id;
    }
}

std::wstring MainWindow::BuildDiagnosticReport() const {
    core::DiagnosticReportInput input;
    input.application_version = ApplicationVersion;
#ifdef NDEBUG
    input.release_build = true;
#else
    input.release_build = false;
#endif
    input.environment = CaptureDiagnosticEnvironment(window_, running_as_administrator_);

    const EngineState engine_state = controller_ ? controller_->State() : EngineState::Ready;
    input.current.engine_state = ToDiagnosticEngineState(engine_state);
    input.current.safety_hotkeys_ready = SafetyHotkeysReady();
    input.current.cleanup_required = CleanupRequired();
    input.current.tracked_input = controller_ && controller_->HasTrackedInput();

    core::RunSettings current_settings{};
    std::wstring validation_error;
    input.current.settings_available = ReadSettings(current_settings, validation_error);
    if (input.current.settings_available) {
        current_settings.start_stop_hotkey = confirmed_start_hotkey_;
        current_settings.emergency_hotkey = confirmed_emergency_hotkey_;
        input.current.settings = current_settings;
        input.current.effective_backend = InputBackendDispatcher::EffectiveBackend(
            current_settings.backend,
            target_window_,
            current_settings.allow_background_input);
        input.current.target_required =
            DiagnosticBackendRequiresTarget(input.current.effective_backend);
        input.current.named_profile_active =
            active_profile_info_.has_value() && active_profile_saved_settings_.has_value();
        if (input.current.named_profile_active) {
            const core::RunSettings profile_current =
                ProfileStore::NormalizeSettingsForProfile(current_settings);
            input.current.named_profile_modified =
                profile_current != *active_profile_saved_settings_;
        }
    }

    input.current.target_selected = target_window_.window != nullptr;
    input.current.target_available = IsTargetWindowValid(target_window_);
    input.current.target_foreground =
        input.current.target_available && IsTargetWindowForeground(target_window_);
    input.current.target_elevation = input.current.target_selected
        ? ToDiagnosticTargetElevation(target_window_.elevation)
        : core::DiagnosticTargetElevation::NotApplicable;

    const bool shutdown_active = shutdown_in_progress_.load(std::memory_order_acquire);
    const bool emergency_active = emergency_latched_.load(std::memory_order_acquire) ||
                                  safety_shield_.IsVisible();
    const bool foreground_required =
        input.current.settings_available &&
        (input.current.effective_backend == core::InputBackend::ForegroundTargetInput ||
         ((input.current.effective_backend == core::InputBackend::TargetedWindowMessages ||
           input.current.effective_backend == core::InputBackend::TargetedUnicodeText) &&
          !input.current.settings.allow_background_input));
    const bool administrator_confirmation_required =
        input.current.target_required && input.current.target_available &&
        !running_as_administrator_ &&
        target_window_.elevation == TargetElevation::Elevated &&
        (!elevated_target_continue_.has_value() ||
         !TargetWindowMatchesIdentity(target_window_, *elevated_target_continue_));
    const bool minimized_target_mouse =
        input.current.settings_available && input.current.target_required &&
        input.current.target_available &&
        input.current.settings.action_type == core::ActionType::MouseClick &&
        IsIconic(target_window_.window) != FALSE;

    // Report the live prerequisites in the same significant order used by the
    // authoritative Start path. Diagnostics must not claim Ready merely because
    // the broad Start button gate is enabled; foreground, elevation-decision,
    // and minimized-target checks are intentionally evaluated only when Start is
    // attempted so the existing UI can present its actionable explanation.
    if (shutdown_active) {
        input.current.readiness = core::DiagnosticReadiness::ShutdownActive;
    } else if (engine_state == EngineState::Running) {
        input.current.readiness = core::DiagnosticReadiness::Running;
    } else if (engine_state == EngineState::Stopping) {
        input.current.readiness = core::DiagnosticReadiness::Stopping;
    } else if (engine_state == EngineState::Faulted) {
        input.current.readiness = core::DiagnosticReadiness::Faulted;
    } else if (emergency_active) {
        input.current.readiness = core::DiagnosticReadiness::EmergencyActive;
    } else if (input.current.cleanup_required) {
        input.current.readiness = core::DiagnosticReadiness::CleanupRequired;
    } else if (!input.current.safety_hotkeys_ready) {
        input.current.readiness = core::DiagnosticReadiness::HotkeysUnavailable;
    } else if (!input.current.settings_available) {
        input.current.readiness = core::DiagnosticReadiness::SettingsInvalid;
    } else if (input.current.target_required && !input.current.target_available) {
        input.current.readiness = core::DiagnosticReadiness::TargetUnavailable;
    } else if (administrator_confirmation_required) {
        input.current.readiness =
            core::DiagnosticReadiness::AdministratorConfirmationRequired;
    } else if (foreground_required && !input.current.target_foreground) {
        input.current.readiness = core::DiagnosticReadiness::TargetNotForeground;
    } else if (minimized_target_mouse) {
        input.current.readiness = core::DiagnosticReadiness::TargetMinimized;
    } else if (controller_ != nullptr &&
               (engine_state == EngineState::Ready || engine_state == EngineState::Disarmed)) {
        input.current.readiness = core::DiagnosticReadiness::Ready;
    } else {
        input.current.readiness = core::DiagnosticReadiness::Unavailable;
    }

    input.run = last_run_diagnostic_;
    if (input.run.available && controller_) {
        const DiagnosticsSnapshot diagnostics = controller_->Diagnostics();
        input.run.active = engine_state == EngineState::Running || engine_state == EngineState::Stopping;
        input.run.completed_actions = diagnostics.completed_actions;
        input.run.generated_inputs = diagnostics.generated_inputs;
        input.run.elapsed_seconds = diagnostics.elapsed_seconds;
        input.run.actual_actions_per_second = diagnostics.actual_actions_per_second;
        input.run.actual_inputs_per_second = diagnostics.actual_inputs_per_second;
        input.run.tracked_input = controller_->HasTrackedInput();
        input.run.cleanup_required = CleanupRequired();

        if (input.run.active) {
            input.run.outcome = core::DiagnosticRunOutcome::InProgress;
        } else if (controller_->LastRunHadBackendFailure()) {
            input.run.outcome = core::DiagnosticRunOutcome::BackendFailure;
        } else {
            switch (controller_->LastRunLimitStopReason()) {
            case RunLimitStopReason::RepeatLimit:
                input.run.outcome = core::DiagnosticRunOutcome::RepeatLimit;
                break;
            case RunLimitStopReason::TimeLimit:
                input.run.outcome = core::DiagnosticRunOutcome::TimeLimit;
                break;
            case RunLimitStopReason::None: {
                const core::DiagnosticRunOutcome stop_intent =
                    run_stop_intent_.load(std::memory_order_acquire);
                if (stop_intent == core::DiagnosticRunOutcome::NormalStop ||
                    stop_intent == core::DiagnosticRunOutcome::EmergencyStop) {
                    input.run.outcome = stop_intent;
                } else if (input.run.outcome != core::DiagnosticRunOutcome::NormalStop &&
                           input.run.outcome != core::DiagnosticRunOutcome::EmergencyStop) {
                    input.run.outcome = engine_state == EngineState::Faulted
                        ? core::DiagnosticRunOutcome::Faulted
                        : core::DiagnosticRunOutcome::EndedUnclassified;
                }
                break;
            }
            }
        }
    }
    return core::FormatDiagnosticReport(input);
}

void MainWindow::CopyDiagnosticReport() {
    try {
        const std::wstring report = BuildDiagnosticReport();
        if (CopyUnicodeTextToClipboard(window_, report)) {
            const EngineState state =
                controller_ ? controller_->State() : EngineState::Ready;
            SetStatusPresentation({
                StatusCategoryForEngineState(ToPresentationEngineState(state)),
                DiagnosticReportCopiedStatus,
                DiagnosticReportCopiedTooltip,
            }, true);
            if (StartGeneratedTimer(diagnostic_report_copied_timer_id_,
                                    DiagnosticReportCopiedMilliseconds) == 0) {
                const std::uint64_t completed =
                    controller_ ? controller_->CompletedActions() : 0U;
                UpdateStatus(state, completed);
            }
            return;
        }
    } catch (...) {
        // Treat allocation / formatting failure like a clipboard failure. The
        // diagnostic action must never change automation state or terminate the app.
    }

    ShowCenteredMessageBox(
        window_,
        L"Vector Click could not generate or copy the diagnostic report to the Windows clipboard. No report was transmitted.",
        L"Could not copy diagnostic report",
        MB_OK | MB_ICONWARNING);
}

void MainWindow::StartFromControls() {
    if (target_picker_open_ || profile_manager_open_ || settings_import_picker_open_ ||
        settings_history_transaction_pending_ ||
        settings_import_transaction_pending_ || hotkey_registration_pending_ ||
        !controller_ || controller_->IsRunning() ||
        shutdown_in_progress_.load(std::memory_order_acquire) ||
        emergency_latched_.load(std::memory_order_acquire) ||
        safety_shield_.IsVisible()) {
        return;
    }
    if (CleanupRequired()) {
        ShowCenteredMessageBox(
            window_,
            L"Vector Click still tracks an input that did not receive a confirmed release. Resume or unblock the target, then use the green Retry cleanup button. Press Emergency Stop to open the recovery Safety Shield and access Force Stop and Exit if needed.",
            L"Cleanup required",
            MB_OK | MB_ICONWARNING);
        RefreshEnabledState();
        return;
    }
    if (!SafetyHotkeysReady()) {
        const std::wstring reason = SafetyHotkeyUnavailableReason();
        ShowCenteredMessageBox(
            window_,
            (reason + L". Vector Click cannot start without a confirmed Emergency Stop hotkey.").c_str(),
            L"Cannot start",
            MB_OK | MB_ICONWARNING);
        RefreshEnabledState();
        return;
    }
    const EngineState state = controller_->State();
    if (state != EngineState::Ready && state != EngineState::Disarmed) {
        return;
    }
    FlushNumericPresentationRefresh();
    CancelPositionCapture();
    ResetClickPositionIndicator();

    core::RunSettings settings;
    std::wstring error;
    if (!ReadSettings(settings, error)) {
        ShowCenteredMessageBox(window_, error.c_str(), L"Cannot start", MB_OK | MB_ICONWARNING);
        return;
    }

    if (target_window_.window != nullptr &&
        !IsTargetWindowValid(target_window_)) {
        (void)RecoverTargetWindowIfAvailable(true);
    }

    const core::InputBackend effective_backend =
        InputBackendDispatcher::EffectiveBackend(
            settings.backend, target_window_, settings.allow_background_input);
    const bool foreground_target =
        effective_backend == core::InputBackend::ForegroundTargetInput;
    const bool targeted_messages =
        effective_backend == core::InputBackend::TargetedWindowMessages;
    const bool targeted_unicode =
        effective_backend == core::InputBackend::TargetedUnicodeText;
    const bool target_required = foreground_target || targeted_messages || targeted_unicode;
    if (target_required && !IsTargetWindowValid(target_window_)) {
        std::wstring message =
            L"Select an available target window before using Foreground target input, Targeted window messages, or Targeted Unicode text. Automatic requires a target whenever one is selected.";
        if (!last_target_recovery_error_.empty()) {
            message += L"\n\nRecovery status: " + last_target_recovery_error_;
        }
        ShowCenteredMessageBox(
            window_,
            message.c_str(),
            L"Cannot start",
            MB_OK | MB_ICONWARNING);
        return;
    }

    // The privilege question can take foreground focus. Resolve it before the
    // target foreground check so Continue never starts a run using a stale
    // pre-dialog foreground result. Standard and Unicode text input ignore a selected target.
    if (target_required && !ConfirmAdministratorTargetAccess()) {
        return;
    }

    const bool foreground_required =
        foreground_target ||
        ((targeted_messages || targeted_unicode) && !settings.allow_background_input);
    if (foreground_required && !IsTargetWindowForeground(target_window_)) {
        const std::wstring start_hotkey = GetControlText(start_hotkey_combo_);
        std::wstring message =
            L"Bring the selected target to the foreground, then press \"" +
            start_hotkey + L"\", the Start / Stop hotkey";
        if (targeted_messages || targeted_unicode) {
            message += L", or enable Allow background input.";
        } else {
            message += L". Foreground target input never sends to a background application.";
        }
        ShowCenteredMessageBox(window_, message.c_str(), L"Target must be in the foreground", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (target_required &&
        settings.action_type == core::ActionType::MouseClick &&
        IsIconic(target_window_.window) != FALSE) {
        ShowCenteredMessageBox(
            window_,
            L"Restore the selected target before sending mouse clicks. Minimized windows do not have a usable foreground or client position.",
            L"Target window is minimized",
            MB_OK | MB_ICONINFORMATION);
        return;
    }

    settings.start_stop_hotkey = confirmed_start_hotkey_;
    settings.emergency_hotkey = confirmed_emergency_hotkey_;

    // RegisterHotKey matches an exact modifier combination. While a run is
    // active, a user may physically hold Ctrl, Alt, or Shift before pressing a
    // configured safety key. Reserve temporary supersets of the confirmed
    // Start / Stop and Emergency Stop combinations before generated input can
    // begin so those incidental modifiers cannot create a safety blind spot.
    // The aliases are run-scoped; idle hotkeys retain the user's exact binding.
    if (hotkey_thread_ == nullptr) {
        ShowCenteredMessageBox(
            window_,
            L"The safety-hotkey thread is unavailable, so Vector Click cannot safely start generated input.",
            L"Cannot start safely",
            MB_OK | MB_ICONWARNING);
        return;
    }

    std::wstring guard_error;
    if (!hotkey_thread_->SetActiveRunModifierGuard(true, guard_error)) {
        std::wstring message =
            L"Vector Click could not reserve the temporary Ctrl / Alt / Shift-compatible versions of the active Start / Stop and Emergency Stop hotkeys that protect an active run.";
        if (!guard_error.empty()) {
            message += L"\n\n" + guard_error;
        }
        message +=
            L"\n\nChoose different safety hotkeys or close the application using the conflicting shortcut, then try again.";
        ShowCenteredMessageBox(
            window_, message.c_str(), L"Cannot start safely", MB_OK | MB_ICONWARNING);
        return;
    }
    active_run_hotkey_guard_active_ = true;

    // The active-run hotkey reservation is a synchronous safety transaction on
    // the dedicated hotkey thread. Emergency Stop can still arrive while the
    // UI waits for that confirmation. Never continue into generated input if
    // such a request became authoritative during the pre-start transaction.
    if (emergency_latched_.load(std::memory_order_acquire) ||
        shutdown_in_progress_.load(std::memory_order_acquire)) {
        DisableActiveRunHotkeyGuard();
        return;
    }

    // Temporary process-priority modes begin before the controller starts its
    // backend / session work. Keep the elevated interval through all stop and
    // release cleanup so a busy system cannot make the safety-critical tail of
    // the run lower priority than the generated-input work itself.
    if (!BeginProcessPriorityActive()) {
        DisableActiveRunHotkeyGuard();
        return;
    }

    settings_cache_ = settings;
    if (settings.remember_settings) {
        SaveSettingsIfEnabled();
    }
    if (!controller_->Start(settings, target_window_)) {
        EndProcessPriorityActive(true);
        DisableActiveRunHotkeyGuard();
        ShowCenteredMessageBox(window_, L"Vector Click is already running an action or could not start.", L"Vector Click", MB_OK | MB_ICONWARNING);
        return;
    }

    run_stop_intent_.store(
        core::DiagnosticRunOutcome::InProgress,
        std::memory_order_release);
    last_run_diagnostic_ = {};
    last_run_diagnostic_.available = true;
    last_run_diagnostic_.active = true;
    last_run_diagnostic_.settings = settings;
    last_run_diagnostic_.effective_backend = effective_backend;
    last_run_diagnostic_.target_required = target_required;
    last_run_diagnostic_.target_selected = target_window_.window != nullptr;
    last_run_diagnostic_.target_available = IsTargetWindowValid(target_window_);
    last_run_diagnostic_.target_foreground_at_start =
        last_run_diagnostic_.target_available && IsTargetWindowForeground(target_window_);
    last_run_diagnostic_.target_elevation = last_run_diagnostic_.target_selected
        ? ToDiagnosticTargetElevation(target_window_.elevation)
        : core::DiagnosticTargetElevation::NotApplicable;
    last_run_diagnostic_.outcome = core::DiagnosticRunOutcome::InProgress;
}

void MainWindow::StopNormally() {
    ResetClickPositionIndicator();
    if (controller_) {
        if (last_run_diagnostic_.available && controller_->IsRunning()) {
            last_run_diagnostic_.outcome = core::DiagnosticRunOutcome::NormalStop;
            run_stop_intent_.store(
                core::DiagnosticRunOutcome::NormalStop,
                std::memory_order_release);
        }
        (void)controller_->RequestStop();
    }
}

void MainWindow::EmergencyStop() {
    ResetClickPositionIndicator();
    CancelPositionCapture();

    if (force_exit_on_emergency_stop_requested_.load(
            std::memory_order_acquire)) {
        BeginForceStopAndExit();
        return;
    }

    const bool first_request = !emergency_latched_.exchange(
        true, std::memory_order_acq_rel);
    if (first_request) {
        emergency_cleanup_complete_.store(false, std::memory_order_release);

        bool was_running = false;
        if (controller_) {
            was_running = controller_->RequestEmergencyStop();
        }
        if (was_running && last_run_diagnostic_.available) {
            last_run_diagnostic_.outcome = core::DiagnosticRunOutcome::EmergencyStop;
            run_stop_intent_.store(
                core::DiagnosticRunOutcome::EmergencyStop,
                std::memory_order_release);
        }
        ShowSafetyShield();
        DismissActiveMessageBoxForEmergency();
        // This button handler owns the UI thread until release cleanup returns.
        // Publishing a temporary disabled-settings frame here cannot prevent a
        // competing UI action, but it does invalidate nearly every custom
        // control and can disturb the retained Advanced-scroll frame. Let the
        // queued emergency UI publish only the final safe state instead.
        QueueEmergencyUi();

        bool releases_submitted = false;
        if (controller_) {
            releases_submitted = controller_->CompleteEmergencyStop(was_running);
        }
        const bool cleanup_required = CleanupRequired();
        emergency_release_failed_.store(
            !releases_submitted || cleanup_required,
            std::memory_order_release);
        emergency_cleanup_complete_.store(true, std::memory_order_release);
        if (safety_shield_.IsVisible()) {
            if (releases_submitted && !cleanup_required) {
                safety_shield_.MarkComplete();
            } else {
                safety_shield_.MarkFailed();
            }
        }
    } else {
        ShowSafetyShield();
    }
    DismissActiveMessageBoxForEmergency();
    QueueEmergencyUi();
}

void MainWindow::RetryEmergencyCleanup() {
    if (force_exit_in_progress_.load(std::memory_order_acquire) ||
        controller_ == nullptr) {
        return;
    }

    const bool cleanup_succeeded = controller_->RetryCleanup();
    const bool cleanup_required = controller_->HasTrackedInput();
    const bool cleanup_failed = !cleanup_succeeded || cleanup_required;
    emergency_release_failed_.store(cleanup_failed, std::memory_order_release);
    emergency_cleanup_complete_.store(true, std::memory_order_release);

    if (safety_shield_.IsVisible()) {
        if (cleanup_failed) {
            safety_shield_.MarkRetryFailed();
        } else {
            safety_shield_.MarkComplete();
        }
    }
    // A failed retry must preserve the more specific RetryFailed shield
    // presentation. AdvanceEmergencyCompletion() republishes the original
    // generic release warning for an active shield, so only advance the
    // Emergency Stop lifecycle after cleanup actually succeeds.
    if (!cleanup_failed) {
        AdvanceEmergencyCompletion();
    }
    if (!emergency_latched_.load(std::memory_order_acquire)) {
        HandleRunFeedbackTerminal(
            controller_->State(), false, cleanup_failed);
    }
    RefreshEnabledState();

    // Apply the result after refreshing availability so a readiness snapshot
    // cannot replace the actionable retry result with a generic status line.
    if (cleanup_failed) {
        SetStatusPresentation({
            StatusCategory::Attention,
            L"Status: Cleanup retry failed | Resume or unblock the target, then retry",
            L"The target still did not accept every tracked release event. It may be suspended, unresponsive, closed, or blocked by privilege boundaries. Retry cleanup generates no new press.",
        }, true);
    } else {
        SetStatusPresentation({
            StatusCategory::Ready,
            L"Status: Release cleanup complete",
            L"All tracked release entries were cleared. Vector Click can start again after the Emergency Stop state closes.",
        }, true);
    }
}

void MainWindow::BeginForceStopAndExit(const bool watchdog_already_armed) noexcept {
    if (watchdog_already_armed) {
        // The global Emergency Stop hotkey can arm the watchdog before it
        // depends on this UI thread. Ignore a stale / spurious continuation.
        if (!force_exit_in_progress_.load(std::memory_order_acquire)) {
            return;
        }
    } else {
        bool expected = false;
        if (!force_exit_in_progress_.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }

    // A force-exit request must have a hard deadline before any UI-owned or
    // backend cleanup work begins. The global hotkey path can arm the same
    // watchdog before it depends on this UI thread.
    if (!watchdog_already_armed) {
        force_exit::StartWatchdogOrTerminate();
    }

    Controller* const controller = controller_.get();
    bool initial_request_was_running = false;
    if (!watchdog_already_armed && controller != nullptr) {
        // Preserve the ordinary Emergency Stop ordering: invalidate / cancel the
        // active session immediately, then perform potentially blocking release
        // work off the UI thread. The watchdog is already active if this call
        // itself ever stalls behind a worker lifecycle lock.
        initial_request_was_running = controller->RequestEmergencyStop();
        if (initial_request_was_running && last_run_diagnostic_.available) {
            last_run_diagnostic_.outcome =
                core::DiagnosticRunOutcome::EmergencyStop;
            run_stop_intent_.store(
                core::DiagnosticRunOutcome::EmergencyStop,
                std::memory_order_release);
        }
    }

    // Once escalation starts, VectorClick can never return to ordinary use.
    // Keep the main window and shield alive until the bounded watchdog ends the
    // process so the cleanup thread cannot outlive its owner.
    emergency_latched_.store(true, std::memory_order_release);
    emergency_cleanup_complete_.store(false, std::memory_order_release);
    emergency_release_failed_.store(false, std::memory_order_release);
    RemoveQueuedStartRequests();
    CancelPositionCapture();
    ResetClickPositionIndicator();
    CancelScheduledSettingsSave();
    DismissActiveMessageBoxForEmergency();
    const bool shield_requested =
        safety_shield_requested_.load(std::memory_order_acquire);
    if (shield_requested) {
        (void)safety_shield_.Show();
    }
    if (shield_requested || safety_shield_.IsVisible()) {
        safety_shield_.BeginForceExit();
    }
    RefreshEnabledState();

    try {
        std::thread([controller, watchdog_already_armed, initial_request_was_running] {
            std::size_t completed_passes = 0;
            if (!watchdog_already_armed && controller != nullptr) {
                (void)controller->CompleteEmergencyStop(initial_request_was_running);
                completed_passes = 1;
            }

            for (std::size_t pass = completed_passes;
                 pass < force_exit::CleanupPasses;
                 ++pass) {
                if (controller != nullptr) {
                    (void)controller->EmergencyStop();
                }
                if (pass + 1U < force_exit::CleanupPasses) {
                    Sleep(force_exit::InterpassDelayMilliseconds);
                }
            }

            // Do not terminate early when cleanup finishes. The watchdog owns
            // the final hard deadline so the shield remains present for the
            // full bounded escalation interval and the task ends at about two
            // seconds regardless of whether these best-effort passes respond.
        }).detach();
    } catch (...) {
        // The watchdog was already created and remains authoritative. If the
        // cleanup worker cannot be started, leave the shield in its locked
        // force-stopping state until the same hard two-second termination.
    }
}

void MainWindow::QueueEmergencyUi() noexcept {
    if (emergency_ui_presented_.load(std::memory_order_acquire) &&
        emergency_cleanup_complete_.load(std::memory_order_acquire)) {
        return;
    }

    bool expected = false;
    if (!emergency_ui_pending_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    if (window_ == nullptr || IsWindow(window_) == FALSE ||
        PostMessageW(window_, WM_APP_EMERGENCY_UI, 0, 0) == FALSE) {
        emergency_ui_pending_.store(false, std::memory_order_release);
    }
}

void MainWindow::RemoveQueuedStartRequests() noexcept {
    if (window_ == nullptr || IsWindow(window_) == FALSE) {
        return;
    }

    MSG message{};
    while (PeekMessageW(&message,
                        window_,
                        WM_APP_REQUEST_START,
                        WM_APP_REQUEST_START,
                        PM_REMOVE) != FALSE) {
    }
}

void MainWindow::ResetClickPositionIndicator() noexcept {
    click_position_indicator_.Hide();
    if (window_ != nullptr && IsWindow(window_) != FALSE) {
        MSG message{};
        while (PeekMessageW(&message,
                            window_,
                            WM_APP_CLICK_INDICATOR,
                            WM_APP_CLICK_INDICATOR,
                            PM_REMOVE) != FALSE) {
        }
    }
    click_indicator_update_pending_.store(false, std::memory_order_release);
}

void MainWindow::AdvanceEmergencyCompletion() {
    if (!emergency_latched_.load(std::memory_order_acquire) ||
        emergency_ui_pending_.load(std::memory_order_acquire) ||
        !emergency_cleanup_complete_.load(std::memory_order_acquire) ||
        controller_ == nullptr) {
        return;
    }

    const EngineState state = controller_->State();
    if (state != EngineState::Disarmed && state != EngineState::Faulted) {
        return;
    }

    const bool release_warning =
        emergency_release_failed_.load(std::memory_order_acquire) ||
        CleanupRequired() || state == EngineState::Faulted;
    HandleRunFeedbackTerminal(state, true, release_warning);
    if (safety_shield_.IsVisible()) {
        if (release_warning) {
            safety_shield_.MarkFailed();
        } else {
            safety_shield_.MarkComplete();
        }
    }
    TryClearEmergencyLatch();
}

void MainWindow::TryClearEmergencyLatch() {
    if (force_exit_in_progress_.load(std::memory_order_acquire) ||
        shutdown_in_progress_.load(std::memory_order_acquire) ||
        !emergency_latched_.load(std::memory_order_acquire) ||
        emergency_ui_pending_.load(std::memory_order_acquire) ||
        !emergency_cleanup_complete_.load(std::memory_order_acquire) ||
        safety_shield_.IsVisible() || controller_ == nullptr) {
        return;
    }

    const EngineState state = controller_->State();
    if (state != EngineState::Disarmed && state != EngineState::Faulted) {
        return;
    }

    const bool release_warning =
        emergency_release_failed_.load(std::memory_order_acquire) ||
        CleanupRequired() || state == EngineState::Faulted;
    DisableActiveRunHotkeyGuard();
    emergency_latched_.store(false, std::memory_order_release);
    emergency_cleanup_complete_.store(false, std::memory_order_release);
    emergency_release_failed_.store(false, std::memory_order_release);
    emergency_ui_presented_.store(false, std::memory_order_release);
    RemoveQueuedStartRequests();

    // Emergency release cleanup is complete and the latch is now clear. Only
    // now may a temporary process-priority mode restore its inherited baseline.
    EndProcessPriorityActive(false);

    // Clearing the emergency latch changes readiness and enabled controls.
    // Commit that canonical refresh before the terminal message so a release
    // warning cannot be immediately overwritten by the state transition it
    // accompanies.
    RefreshEnabledState();
    if (release_warning) {
        SetStatusPresentation({
            StatusCategory::Attention,
            L"Status: Emergency Stop completed with a release warning",
            L"Vector Click invalidated the session, but could not confirm that every tracked release event was submitted or the input worker entered an error state. Verify the target manually before starting again.",
        }, true);
    } else {
        UpdateStatus(EngineState::Disarmed, controller_->CompletedActions());
    }
}

void MainWindow::ShowSafetyShield() {
    const bool shield_requested =
        safety_shield_requested_.load(std::memory_order_acquire);

    // Force-exit-on-Emergency-Stop is deliberately independent from the
    // optional Safety Shield. When the user leaves the shield preference off,
    // force-exit mode must not make it appear merely because cleanup is still
    // pending; the already-armed watchdog remains the hard fallback.
    if (force_exit_in_progress_.load(std::memory_order_acquire) &&
        !shield_requested) {
        return;
    }

    // During an ordinary Emergency Stop, unresolved cleanup keeps the shield
    // reachable even when automatic presentation was disabled because it owns
    // Retry cleanup and the manual Force Stop and Exit fallback.
    if (!shield_requested &&
        !CleanupRequired() &&
        !emergency_release_failed_.load(std::memory_order_acquire)) {
        return;
    }

    (void)safety_shield_.Show();
}


bool MainWindow::PrepareSafeShutdown(const bool save_settings,
                                     const bool show_settings_errors) {
    if (force_exit_in_progress_.load(std::memory_order_acquire)) {
        return false;
    }

    bool expected = false;
    if (!shutdown_in_progress_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return controller_ == nullptr ||
               ShutdownCleanupSucceeded(
                   !emergency_release_failed_.load(std::memory_order_acquire),
                   controller_->HasTrackedInput());
    }

    // Cancellation is the first externally meaningful shutdown action. It is
    // published before settings I / O, window destruction, or hotkey-thread
    // teardown so no new generated press can begin while a close is waiting.
    emergency_latched_.store(true, std::memory_order_release);
    emergency_cleanup_complete_.store(false, std::memory_order_release);
    emergency_release_failed_.store(false, std::memory_order_release);

    bool was_running = false;
    if (controller_) {
        was_running = controller_->RequestEmergencyStop();
    }

    RemoveQueuedStartRequests();
    ResetClickPositionIndicator();
    CancelPositionCapture();
    EndKeyCapture();
    CancelScheduledSettingsSave();
    RefreshEnabledState();

    bool releases_submitted = true;
    bool has_tracked_input = false;
    if (controller_) {
        releases_submitted = controller_->CompleteEmergencyStop(was_running);
        has_tracked_input = controller_->HasTrackedInput();
    }

    const bool cleanup_succeeded =
        ShutdownCleanupSucceeded(releases_submitted, has_tracked_input);
    emergency_release_failed_.store(
        !cleanup_succeeded, std::memory_order_release);
    emergency_cleanup_complete_.store(true, std::memory_order_release);

    // Saving is allowed only after cancellation has been published. Forced
    // emergency termination never reaches this path, and session-end requests
    // suppress modal save errors so Windows shutdown is not obstructed.
    if (save_settings &&
        !force_exit_in_progress_.load(std::memory_order_acquire)) {
        SaveSettingsIfEnabled(show_settings_errors);
    }

    return cleanup_succeeded;
}

void MainWindow::PresentShutdownRecovery() {
    shutdown_in_progress_.store(false, std::memory_order_release);
    ShowSafetyShield();
    if (safety_shield_.IsVisible()) {
        safety_shield_.MarkFailed();
    }
    QueueEmergencyUi();
    RefreshEnabledState();
}

void MainWindow::CancelPendingSessionEnd() noexcept {
    if (!session_end_query_pending_) {
        return;
    }

    const bool cleanup_succeeded = session_end_cleanup_succeeded_;
    session_end_query_pending_ = false;
    session_end_cleanup_succeeded_ = true;

    if (!cleanup_succeeded || CleanupRequired()) {
        PresentShutdownRecovery();
        return;
    }

    shutdown_in_progress_.store(false, std::memory_order_release);
    AdvanceEmergencyCompletion();
    RefreshEnabledState();
}

bool MainWindow::ConfirmAdministratorTargetAccess() {
    if (running_as_administrator_ || target_window_.elevation != TargetElevation::Elevated ||
        !IsTargetWindowValid(target_window_)) {
        return true;
    }

    const TargetWindowIdentity identity = CaptureTargetWindowIdentity(target_window_);
    if (elevated_target_continue_.has_value() &&
        TargetWindowMatchesIdentity(target_window_, *elevated_target_continue_)) {
        return true;
    }

    const std::wstring message =
        L"The selected target, " + target_window_.title + L" (" +
        target_window_.process_name +
        L"), is running as administrator, but Vector Click is not.\n\n"
        L"Windows may block Foreground target input, Targeted window messages, and Targeted Unicode text across this privilege boundary.\n\n"
        L"Restart Vector Click as administrator and restore this target?";
    const MessageBoxButtonLabels labels{
        L"Restart as administrator",
        L"Continue anyway",
        L"Cancel",
    };
    const int answer = ShowCenteredChoiceMessageBox(
        window_,
        message.c_str(),
        L"Administrator access recommended",
        MB_ICONWARNING,
        labels);
    if (answer == IDYES) {
        RestartElevated();
        return false;
    }
    if (answer == IDNO) {
        elevated_target_continue_ = identity;
        SetStatusPresentation({
            StatusCategory::Transition,
            L"Status: Continuing without administrator access for " +
                target_window_.title,
            {},
        }, true);
        return true;
    }
    return false;
}

void MainWindow::RestoreStartupTarget() {
    if (!startup_target_restore_.has_value()) {
        return;
    }
    if (!running_as_administrator_) {
        startup_target_restore_error_ =
            L"The administrator restart did not produce an elevated Vector Click process.";
        startup_target_restore_.reset();
        return;
    }

    TargetWindowInfo restored{};
    if (RestoreTargetWindow(*startup_target_restore_,
                            GetCurrentProcessId(),
                            restored,
                            startup_target_restore_error_)) {
        target_window_ = std::move(restored);
        startup_target_restored_ = true;
        SetTimer(window_, TargetStatusTimerId, TargetStatusTimerMilliseconds, nullptr);
    }
    startup_target_restore_.reset();
}

void MainWindow::RestartElevated() {
    if (running_as_administrator_) {
        ShowCenteredMessageBox(window_, L"Vector Click is already running as administrator.", L"Administrator status", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (shutdown_in_progress_.load(std::memory_order_acquire)) {
        return;
    }

    // Elevation should behave like a restart of the current session, not like
    // closing and reopening Vector Click. Capture the validated live controls
    // before shutdown so Remember settings remains a persistence preference
    // rather than a prerequisite for one-time state transfer.
    FlushNumericPresentationRefresh();
    core::RunSettings settings_to_restore;
    std::wstring settings_error;
    if (!ReadSettings(settings_to_restore, settings_error) ||
        !SafetyHotkeysReady()) {
        if (settings_error.empty()) {
            settings_error = SafetyHotkeyUnavailableReason();
        }
        ShowCenteredMessageBox(
            window_,
            (L"Vector Click cannot restart as administrator while the current settings cannot be safely transferred.\n\n" +
             settings_error).c_str(),
            L"Cannot restart as administrator",
            MB_OK | MB_ICONWARNING);
        return;
    }
    settings_to_restore.start_stop_hotkey = confirmed_start_hotkey_;
    settings_to_restore.emergency_hotkey = confirmed_emergency_hotkey_;

    const TargetWindowInfo* const target_to_restore =
        IsTargetWindowValid(target_window_) ? &target_window_ : nullptr;
    const int page_to_restore = selected_page_index_;
    const std::wstring_view profile_id_to_restore =
        active_profile_info_.has_value()
            ? std::wstring_view(active_profile_info_->id)
            : std::wstring_view{};
    const std::wstring restart_arguments = BuildElevatedRestartArguments(
        target_to_restore, page_to_restore, settings_to_restore,
        profile_id_to_restore);
    if (restart_arguments.empty()) {
        ShowCenteredMessageBox(
            window_,
            L"Vector Click could not prepare the current settings for the administrator restart. No settings were changed.",
            L"Cannot restart as administrator",
            MB_OK | MB_ICONWARNING);
        return;
    }

    if (!PrepareSafeShutdown(true, true)) {
        PresentShutdownRecovery();
        return;
    }

    if (RestartAsAdministrator(restart_arguments)) {
        DestroyWindow(window_);
        return;
    }

    shutdown_in_progress_.store(false, std::memory_order_release);
    AdvanceEmergencyCompletion();
    RefreshEnabledState();
    ShowCenteredMessageBox(window_, L"Windows did not restart Vector Click as administrator. The UAC request may have been cancelled.",
                L"Administrator restart", MB_OK | MB_ICONWARNING);
}

void MainWindow::SetCapturePositionButtonText(const std::wstring& text) {
    if (capture_position_button_ == nullptr ||
        IsWindow(capture_position_button_) == FALSE ||
        GetControlText(capture_position_button_) == text) {
        return;
    }

    // Owner-drawn buttons otherwise expose an erase or partially redrawn
    // frame while Windows changes the caption. Install the caption with redraw
    // suppressed, then request one non-erasing buffered button repaint.
    SendMessageW(capture_position_button_, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(capture_position_button_, text.c_str());
    SendMessageW(capture_position_button_, WM_SETREDRAW, TRUE, 0);
    // Hidden mode-dependent controls still need their canonical caption so
    // they are correct if shown later, but they do not need a synchronous
    // owner-draw pass while absent from the active layout.
    if (IsWindowVisible(capture_position_button_) != FALSE) {
        RedrawWindow(capture_position_button_,
                     nullptr,
                     nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
    MarkMoveCoverPresentationDirty();
}

void MainWindow::BeginPositionCapture() {
    const bool keyboard =
        SendMessageW(action_type_combo_, CB_GETCURSEL, 0, 0) == 1;
    if (position_capture_seconds_remaining_ != 0 || keyboard ||
        controller_ == nullptr || controller_->IsRunning()) {
        return;
    }
    position_capture_seconds_remaining_ = PositionCaptureSeconds;
    EnableWindow(capture_position_button_, FALSE);
    SetCapturePositionButtonText(
        L"Capturing in " + std::to_wstring(PositionCaptureSeconds) + L"...");
    if (StartGeneratedTimer(position_capture_timer_id_,
                            PositionCaptureTimerMilliseconds) == 0) {
        // Do not leave the capture control disabled with a countdown that can
        // never advance if Windows cannot allocate the timer. Restore the
        // ordinary enabled-state snapshot first, then publish the specific
        // failure as the final status presentation.
        position_capture_seconds_remaining_ = 0;
        SetCapturePositionButtonText(L"Capture in 4 seconds");
        RefreshEnabledState();
        SetStatusPresentation({
            StatusCategory::Attention,
            L"Status: Fixed-position capture countdown could not be started",
            L"Windows could not start the capture countdown timer. No cursor position was captured and no fixed-position setting was changed.",
        }, true);
        return;
    }
    SetStatusPresentation({
        StatusCategory::Transition,
        L"Status: Move the cursor to the desired fixed position...",
        {},
    }, true);
}

void MainWindow::FinishPositionCapture() {
    StopGeneratedTimer(position_capture_timer_id_);
    position_capture_seconds_remaining_ = 0;
    POINT point{};
    const bool captured = GetCursorPos(&point) != FALSE;
    StatusPresentation result{
        StatusCategory::Ready,
        L"Status: The cursor position could not be captured",
        {},
    };
    if (captured) {
        suppress_control_events_ = true;
        SetControlText(fixed_x_edit_, std::to_wstring(point.x));
        SetControlText(fixed_y_edit_, std::to_wstring(point.y));
        SetRadioPair(current_cursor_radio_, fixed_position_radio_, fixed_position_radio_);
        suppress_control_events_ = false;
        result.text =
            L"Status: Fixed position captured at X " + std::to_wstring(point.x) +
            L", Y " + std::to_wstring(point.y);
        ScheduleSettingsSave();
        CommitSettingsHistoryFromControls();
    }

    // Capturing can itself repair an invalid fixed-position configuration.
    // Recompute readiness before presenting the result so the final text and
    // lamp use the post-capture availability rather than the stale pre-capture
    // cache, and so that refresh cannot immediately replace the confirmation.
    RefreshEnabledState();
    SetStatusPresentation(result, true);
    SetCapturePositionButtonText(L"Capture in 4 seconds");
}

void MainWindow::CancelPositionCapture() {
    const bool was_active = position_capture_seconds_remaining_ != 0;
    StopGeneratedTimer(position_capture_timer_id_);
    position_capture_seconds_remaining_ = 0;
    if (capture_position_button_ != nullptr && IsWindow(capture_position_button_)) {
        SetCapturePositionButtonText(L"Capture in 4 seconds");
    }

    // The capture prompt owns the ordinary status line while the countdown is
    // active. If another action cancels that countdown, restore the real engine
    // presentation at the same time as the button caption. Leaving only the
    // caption reset can strand "Move the cursor..." on screen after capture is
    // no longer active.
    if (was_active &&
        !shutdown_in_progress_.load(std::memory_order_acquire) &&
        controller_ != nullptr && status_text_ != nullptr &&
        IsWindow(status_text_) != FALSE) {
        UpdateStatus(controller_->State(), controller_->CompletedActions());
    }
}

void MainWindow::ChooseTargetWindow() {
    if (target_picker_open_ || controller_ == nullptr || controller_->IsRunning()) {
        return;
    }

    CancelPositionCapture();
    target_picker_open_ = true;
    RefreshEnabledState();

    TargetWindowInfo candidate{};
    const bool selected = ShowTargetWindowPicker(
        instance_,
        window_,
        GetCurrentProcessId(),
        target_window_,
        candidate);

    target_picker_open_ = false;
    if (selected) {
        target_window_ = std::move(candidate);
        elevated_target_continue_.reset();
        last_target_recovery_error_.clear();
        SetTimer(window_, TargetStatusTimerId, TargetStatusTimerMilliseconds, nullptr);
        UpdateTargetDisplay();
        ShowInputMethodTargetRoutingStatus();
    }
    RefreshEnabledState();
    ShowInputMethodTargetRoutingStatus();
}

void MainWindow::ClearTargetWindow() {
    KillTimer(window_, TargetStatusTimerId);
    target_window_ = {};
    elevated_target_continue_.reset();
    last_target_recovery_error_.clear();

    // Background delivery has no meaning without a selected target. Reset the
    // dependent option before disabling it so the cleared state is both
    // logically and visually unselected.
    if (IsChecked(background_input_check_)) {
        SetChecked(background_input_check_, false);
        ScheduleSettingsSave();
    }

    UpdateTargetDisplay();
    SetStatusPresentation({StatusCategory::Ready,
                           L"Status: Target window cleared",
                           {}}, true);
    RefreshEnabledState();
    SynchronizeSettingsHistoryFromControls();
}

bool MainWindow::RecoverTargetWindowIfAvailable(const bool present_status) {
    if (target_window_.window == nullptr ||
        IsTargetWindowValid(target_window_) ||
        target_window_.recovery_policy == TargetRecoveryPolicy::ExactWindowOnly ||
        target_picker_open_ || controller_ == nullptr ||
        CleanupRequired() ||
        shutdown_in_progress_.load(std::memory_order_acquire) ||
        emergency_latched_.load(std::memory_order_acquire) ||
        safety_shield_.IsVisible()) {
        return false;
    }

    const EngineState state = controller_->State();
    if (state != EngineState::Ready && state != EngineState::Disarmed) {
        return false;
    }

    TargetWindowInfo candidate = target_window_;
    std::wstring error;
    if (!TryRecoverTargetWindow(candidate, GetCurrentProcessId(), error)) {
        last_target_recovery_error_ = std::move(error);
        return false;
    }

    target_window_ = std::move(candidate);
    elevated_target_continue_.reset();
    last_target_recovery_error_.clear();
    target_validity_initialized_ = false;
    SetTimer(window_, TargetStatusTimerId, TargetStatusTimerMilliseconds, nullptr);

    if (present_status) {
        // Recovery can change Start from unavailable to available. Commit only
        // that readiness first; UpdateTargetDisplay may perform its normal
        // broader validity refresh afterward, but it will no longer replace
        // this final recovery message with an intermediate generic Ready
        // presentation.
        RefreshStartAvailability();
        SetStatusPresentation({
            StatusCategory::Ready,
            L"Status: Target recovered safely: " + target_window_.title,
            L"Vector Click reacquired one unique idle target using " +
                std::wstring(TargetRecoveryPolicyText(target_window_.recovery_policy)) +
                L". No target change occurred during an active run.",
        }, true);
    }
    return true;
}

void MainWindow::UpdateTargetDisplay() {
    if (target_status_text_ == nullptr || IsWindow(target_status_text_) == FALSE) {
        return;
    }

    if (target_window_.window != nullptr &&
        !IsTargetWindowValid(target_window_)) {
        (void)RecoverTargetWindowIfAvailable(true);
    }

    const bool valid = IsTargetWindowValid(target_window_);
    std::wstring summary = TargetWindowSummary(target_window_);
    std::wstring details = TargetWindowDetails(target_window_);
    if (!valid && !last_target_recovery_error_.empty()) {
        details += L" Recovery status: " + last_target_recovery_error_;
    }
    if (valid && target_window_.elevation == TargetElevation::Elevated &&
        !running_as_administrator_) {
        summary += L" | Administrator restart recommended";
        details += L" Vector Click is not running as administrator. Windows may block input across this privilege boundary. Start will offer to restart Vector Click and restore this target.";
    }

    if (summary != last_target_summary_) {
        last_target_summary_ = summary;
        SetControlText(target_status_text_, summary);
        InvalidateRect(target_status_text_, nullptr, TRUE);
    }
    if (details != last_target_details_) {
        last_target_details_ = details;
        tooltips_.SetText(target_status_text_, details);
    }

    const bool validity_changed = !target_validity_initialized_ || valid != last_target_valid_;
    target_validity_initialized_ = true;
    last_target_valid_ = valid;

    if (target_window_.window != nullptr && !valid) {
        if (target_window_.recovery_policy == TargetRecoveryPolicy::ExactWindowOnly) {
            KillTimer(window_, TargetStatusTimerId);
        }
        elevated_target_continue_.reset();
    }
    if (validity_changed) {
        RefreshEnabledState();
    }
}

core::ActionPattern MainWindow::SelectedActionPattern() const noexcept {
    const LRESULT selection = SendMessageW(action_pattern_combo_, CB_GETCURSEL, 0, 0);
    if (selection < 0 || selection > 4) return core::ActionPattern::Single;
    return static_cast<core::ActionPattern>(selection);
}

core::HotkeyBinding MainWindow::SelectedHotkey(const HWND combo) const {
    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selection < 0 ||
        static_cast<std::size_t>(selection) >= hotkey_choices_.size()) {
        return {};
    }
    return hotkey_choices_[static_cast<std::size_t>(selection)].second;
}

bool MainWindow::SelectHotkey(const HWND combo,
                              const core::HotkeyBinding& binding) {
    LRESULT desired = -1;
    const auto iterator = std::ranges::find_if(
        hotkey_choices_, [&binding](const auto& choice) {
            return choice.second == binding;
        });
    if (iterator != hotkey_choices_.end()) {
        desired = static_cast<LRESULT>(
            std::distance(hotkey_choices_.begin(), iterator));
    }

    const LRESULT current = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (current == desired) {
        return false;
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(desired), 0);
    return true;
}

core::HotkeyBinding MainWindow::SelectedGeneratedKey() const {
    const LRESULT selection = SendMessageW(generated_key_combo_, CB_GETCURSEL, 0, 0);
    if (selection < 0 ||
        static_cast<std::size_t>(selection) >= generated_key_choices_.size()) {
        return {};
    }
    return generated_key_choices_[static_cast<std::size_t>(selection)].second;
}

void MainWindow::SelectGeneratedKey(const core::HotkeyBinding& binding) {
    const auto iterator = std::ranges::find_if(
        generated_key_choices_, [&binding](const auto& choice) {
            return choice.second == binding;
        });
    if (iterator == generated_key_choices_.end()) {
        SendMessageW(generated_key_combo_, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        return;
    }
    const auto index = std::distance(generated_key_choices_.begin(), iterator);
    SendMessageW(generated_key_combo_, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

bool MainWindow::ParseSignedCoordinate(const HWND edit, std::int32_t& value) {
    const std::wstring text_storage = GetControlText(edit);
    const std::wstring text(TrimNumericText(text_storage));
    if (text.empty()) {
        return false;
    }
    wchar_t* end{};
    errno = 0;
    const long long parsed = std::wcstoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != L'\0' ||
        parsed < std::numeric_limits<std::int32_t>::min() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    value = static_cast<std::int32_t>(parsed);
    return true;
}

bool MainWindow::IsChecked(const HWND control) noexcept {
    return control != nullptr && SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void MainWindow::SetChecked(const HWND control, const bool checked) noexcept {
    if (control == nullptr || IsWindow(control) == FALSE) {
        return;
    }
    const bool currently_checked = IsChecked(control);
    if (currently_checked == checked) {
        return;
    }
    SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(control, nullptr, FALSE);
    MarkMoveCoverPresentationDirty();
}

void MainWindow::SetRadioPair(const HWND first, const HWND second, const HWND selected) noexcept {
    SetChecked(first, first == selected);
    SetChecked(second, second == selected);
}

bool MainWindow::IsPointOnVirtualDesktop(const std::int32_t x, const std::int32_t y) noexcept {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) {
        return false;
    }
    const std::int64_t right = static_cast<std::int64_t>(left) + width;
    const std::int64_t bottom = static_cast<std::int64_t>(top) + height;
    return x >= left && static_cast<std::int64_t>(x) < right &&
           y >= top && static_cast<std::int64_t>(y) < bottom;
}

std::wstring MainWindow::GetControlText(const HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return text;
}

void MainWindow::SetControlText(const HWND control, const std::wstring& text) {
    if (control == nullptr || IsWindow(control) == FALSE || GetControlText(control) == text) {
        return;
    }
    SetWindowTextW(control, text.c_str());
    MarkMoveCoverPresentationDirty();
}

} // namespace vectorclick::win

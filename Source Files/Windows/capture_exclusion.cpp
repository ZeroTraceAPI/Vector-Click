#include "Windows/capture_exclusion.h"

#include <atomic>
#include <commctrl.h>
#include <dwmapi.h>
#include <new>

namespace vectorclick::win {
namespace {

constexpr DWORD ExcludeFromCaptureAffinity = 0x00000011UL;
constexpr DWORD MinimumSupportedBuild = 19041;

using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);

std::atomic<bool> capture_exclusion_requested{false};

constexpr wchar_t CaptionShieldClassName[] =
    L"VectorClick.CaptureExcludedCaptionShield";
constexpr UINT_PTR CaptionShieldOwnerSubclassId = 0x56434353U; // "VCCS"
constexpr wchar_t CaptionShieldStateProperty[] =
    L"VectorClick.CaptureExcludedCaptionShieldState";
constexpr BYTE CaptionShieldAlpha = 1;
// DWMWA_CAPTION_BUTTON_BOUNDS describes the painted caption-button rectangle,
// but native non-client hit testing on Windows 11 includes the first physical
// pixel immediately below that rectangle. Cover that one raster row as well so
// Windows-owned hover UI cannot escape through the lower boundary.
constexpr int CaptionShieldBottomGuardPixels = 1;

struct CaptionShieldState final {
    HWND owner{};
    HWND shield{};
    LRESULT pressed_hit{HTNOWHERE};
};

[[nodiscard]] bool IsCaptionButtonHit(const LRESULT hit_test) noexcept {
    return hit_test == HTMINBUTTON || hit_test == HTMAXBUTTON ||
           hit_test == HTCLOSE;
}

[[nodiscard]] LPARAM ScreenPointParameter(const POINT point) noexcept {
    return MAKELPARAM(static_cast<WORD>(point.x),
                      static_cast<WORD>(point.y));
}

[[nodiscard]] LRESULT CaptionHitFromBoundsFallback(
    const HWND owner,
    const POINT screen_point) noexcept {
    RECT button_bounds{};
    RECT window_bounds{};
    if (FAILED(DwmGetWindowAttribute(owner,
                                     DWMWA_CAPTION_BUTTON_BOUNDS,
                                     &button_bounds,
                                     sizeof(button_bounds))) ||
        GetWindowRect(owner, &window_bounds) == FALSE) {
        return HTNOWHERE;
    }

    const LONG relative_x = screen_point.x - window_bounds.left;
    const LONG relative_y = screen_point.y - window_bounds.top;
    if (relative_x < button_bounds.left || relative_x >= button_bounds.right ||
        relative_y < button_bounds.top ||
        relative_y >= button_bounds.bottom + CaptionShieldBottomGuardPixels) {
        return HTNOWHERE;
    }

    const LONG_PTR style = GetWindowLongPtrW(owner, GWL_STYLE);
    const bool has_minimize = (style & WS_MINIMIZEBOX) != 0;
    const bool has_maximize = (style & WS_MAXIMIZEBOX) != 0;
    if (!has_minimize && !has_maximize) {
        return HTCLOSE;
    }

    // Vector Click's resizable main window has the conventional three-button
    // group. The other standard-caption windows are close-only. This fallback
    // is used only when both DWM and DefWindowProc decline to identify a point.
    const LONG width = button_bounds.right - button_bounds.left;
    if (has_minimize && has_maximize && width >= 3) {
        const LONG offset = relative_x - button_bounds.left;
        const LONG third = width / 3;
        if (offset < third) {
            return HTMINBUTTON;
        }
        if (offset < third * 2) {
            return HTMAXBUTTON;
        }
        return HTCLOSE;
    }

    return HTCLOSE;
}

[[nodiscard]] LRESULT NativeCaptionHitTest(const HWND owner,
                                           const POINT screen_point) noexcept {
    if (owner == nullptr || IsWindow(owner) == FALSE) {
        return HTNOWHERE;
    }
    LRESULT result = HTNOWHERE;
    const LPARAM point_parameter = ScreenPointParameter(screen_point);
    if (DwmDefWindowProc(owner, WM_NCHITTEST, 0, point_parameter, &result) != FALSE &&
        IsCaptionButtonHit(result)) {
        return result;
    }

    result = DefWindowProcW(owner, WM_NCHITTEST, 0, point_parameter);
    return IsCaptionButtonHit(result)
               ? result
               : CaptionHitFromBoundsFallback(owner, screen_point);
}

[[nodiscard]] UINT CaptionSystemCommand(const HWND owner,
                                        const LRESULT hit_test) noexcept {
    switch (hit_test) {
    case HTMINBUTTON:
        return SC_MINIMIZE;
    case HTMAXBUTTON:
        return IsZoomed(owner) != FALSE ? SC_RESTORE : SC_MAXIMIZE;
    case HTCLOSE:
        return SC_CLOSE;
    default:
        return 0;
    }
}

[[nodiscard]] bool IsStandardCaptionWindow(const HWND window) noexcept {
    if (window == nullptr || IsWindow(window) == FALSE) {
        return false;
    }
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    return (style & WS_CAPTION) == WS_CAPTION &&
           (style & WS_SYSMENU) != 0;
}

void HideCaptionShield(CaptionShieldState& state) noexcept {
    if (state.shield != nullptr && IsWindow(state.shield) != FALSE) {
        ShowWindow(state.shield, SW_HIDE);
    }
}

void UpdateCaptionShield(CaptionShieldState& state) noexcept {
    if (state.owner == nullptr || IsWindow(state.owner) == FALSE ||
        state.shield == nullptr || IsWindow(state.shield) == FALSE ||
        !IsStandardCaptionWindow(state.owner) ||
        IsWindowVisible(state.owner) == FALSE ||
        IsWindowEnabled(state.owner) == FALSE ||
        IsIconic(state.owner) != FALSE) {
        HideCaptionShield(state);
        return;
    }

    RECT button_bounds{};
    if (FAILED(DwmGetWindowAttribute(state.owner,
                                     DWMWA_CAPTION_BUTTON_BOUNDS,
                                     &button_bounds,
                                     sizeof(button_bounds))) ||
        button_bounds.right <= button_bounds.left ||
        button_bounds.bottom <= button_bounds.top) {
        HideCaptionShield(state);
        return;
    }

    RECT window_bounds{};
    if (GetWindowRect(state.owner, &window_bounds) == FALSE) {
        HideCaptionShield(state);
        return;
    }

    const int width = button_bounds.right - button_bounds.left;
    const int height = button_bounds.bottom - button_bounds.top +
                       CaptionShieldBottomGuardPixels;
    const int x = window_bounds.left + button_bounds.left;
    const int y = window_bounds.top + button_bounds.top;
    // Ownership already guarantees that the shield remains above its owner.
    // Preserve that established Z-order instead of promoting an inactive
    // Vector Click window above unrelated applications while merely tracking
    // a move / resize.
    (void)SetWindowPos(state.shield,
                       nullptr,
                       x,
                       y,
                       width,
                       height,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOZORDER |
                           SWP_NOOWNERZORDER);
}

LRESULT CALLBACK CaptionShieldWindowProc(HWND window,
                                         UINT message,
                                         WPARAM w_param,
                                         LPARAM l_param) noexcept {
    auto* state = reinterpret_cast<CaptionShieldState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        state = create != nullptr
                    ? static_cast<CaptionShieldState*>(create->lpCreateParams)
                    : nullptr;
        SetWindowLongPtrW(window,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        // Keep ordinary hover hit testing on the capture-excluded shield so
        // Windows-owned caption hover surfaces are not created outside the
        // protected application window. For the actual left-button press, let
        // Windows continue hit testing to the underlying owner in this same UI
        // thread. That gives Maximize / Restore the real non-client mouse path,
        // including native activation, tracking, drag-away cancellation, and
        // Z-order handling, instead of translating the click into a system
        // command ourselves. If this physical-button-state check is not active
        // for a particular message sequence, the shield's button-down / button-up
        // relay below remains as the fallback.
        if (state != nullptr && state->owner != nullptr &&
            GetWindowThreadProcessId(state->owner, nullptr) == GetCurrentThreadId() &&
            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
            return HTTRANSPARENT;
        }
        return HTCLIENT;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        if (dc != nullptr) {
            RECT client{};
            if (GetClientRect(window, &client) != FALSE) {
                FillRect(dc,
                         &client,
                         reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (state != nullptr && state->owner != nullptr &&
            IsWindow(state->owner) != FALSE) {
            POINT point{};
            if (GetCursorPos(&point) != FALSE) {
                const LRESULT hit = NativeCaptionHitTest(state->owner, point);
                if (IsCaptionButtonHit(hit)) {
                    // The shield is deliberately non-activating so it never
                    // becomes the foreground window itself. A real caption
                    // click activates the owner before executing its system
                    // command, however. Reproduce that owner activation here
                    // or the first shielded click can instead leave Vector
                    // Click behind the previously active window.
                    (void)SetForegroundWindow(state->owner);
                    state->pressed_hit = hit;
                    SetCapture(window);
                }
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (state != nullptr && IsCaptionButtonHit(state->pressed_hit)) {
            const LRESULT pressed_hit = state->pressed_hit;
            state->pressed_hit = HTNOWHERE;
            if (GetCapture() == window) {
                (void)ReleaseCapture();
            }

            POINT point{};
            if (GetCursorPos(&point) != FALSE &&
                state->owner != nullptr && IsWindow(state->owner) != FALSE &&
                NativeCaptionHitTest(state->owner, point) == pressed_hit) {
                const HWND owner = state->owner;
                const UINT command = CaptionSystemCommand(owner, pressed_hit);
                if (command == SC_CLOSE) {
                    // Keep Close asynchronous because handling it synchronously
                    // can destroy the owner and this shield while the shield's
                    // own window procedure is still on the stack.
                    (void)PostMessageW(owner,
                                       WM_SYSCOMMAND,
                                       command,
                                       ScreenPointParameter(point));
                } else if (command != 0) {
                    // Native caption Minimize / Maximize / Restore processing
                    // is synchronous with the completed click. Dispatch these
                    // commands synchronously as well so the owner's existing
                    // WM_SYSCOMMAND and resize-transition logic completes before
                    // the shield begins tracking the resulting geometry.
                    (void)SendMessageW(owner,
                                       WM_SYSCOMMAND,
                                       command,
                                       ScreenPointParameter(point));
                }
            }
        }
        return 0;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (state != nullptr) {
            state->pressed_hit = HTNOWHERE;
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

[[nodiscard]] bool RegisterCaptionShieldClass() noexcept {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = CaptionShieldWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = CaptionShieldClassName;
    if (RegisterClassExW(&window_class) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

LRESULT CALLBACK CaptionShieldOwnerSubclassProc(HWND window,
                                                UINT message,
                                                WPARAM w_param,
                                                LPARAM l_param,
                                                UINT_PTR subclass_id,
                                                DWORD_PTR reference_data) noexcept {
    auto* state = reinterpret_cast<CaptionShieldState*>(reference_data);
    if (message == WM_NCDESTROY) {
        (void)RemovePropW(window, CaptionShieldStateProperty);
        if (state != nullptr) {
            if (state->shield != nullptr && IsWindow(state->shield) != FALSE) {
                DestroyWindow(state->shield);
                state->shield = nullptr;
            }
            delete state;
        }
        (void)RemoveWindowSubclass(window,
                                   CaptionShieldOwnerSubclassProc,
                                   subclass_id);
        return DefSubclassProc(window, message, w_param, l_param);
    }

    const LRESULT result = DefSubclassProc(window, message, w_param, l_param);
    if (state == nullptr) {
        return result;
    }

    switch (message) {
    case WM_ENABLE:
    case WM_SHOWWINDOW:
    case WM_SIZE:
    case WM_WINDOWPOSCHANGED:
    case WM_STYLECHANGED:
    case WM_DPICHANGED:
        UpdateCaptionShield(*state);
        break;
    default:
        break;
    }
    return result;
}

[[nodiscard]] CaptureExclusionOutcome EnsureCaptionShield(
    const HWND owner) noexcept {
    if (!IsStandardCaptionWindow(owner)) {
        return {CaptureExclusionResult::Applied, ERROR_SUCCESS};
    }

    auto* existing = reinterpret_cast<CaptionShieldState*>(
        GetPropW(owner, CaptionShieldStateProperty));
    if (existing != nullptr) {
        UpdateCaptionShield(*existing);
        return {CaptureExclusionResult::Applied, ERROR_SUCCESS};
    }

    if (!RegisterCaptionShieldClass()) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_CANNOT_MAKE;
        }
        return {CaptureExclusionResult::SystemFailure, error};
    }

    auto* state = new (std::nothrow) CaptionShieldState{};
    if (state == nullptr) {
        return {CaptureExclusionResult::SystemFailure, ERROR_NOT_ENOUGH_MEMORY};
    }
    state->owner = owner;

    state->shield = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        CaptionShieldClassName,
        L"",
        WS_POPUP,
        0,
        0,
        1,
        1,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        state);
    if (state->shield == nullptr) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_CANNOT_MAKE;
        }
        delete state;
        return {CaptureExclusionResult::SystemFailure, error};
    }

    if (SetLayeredWindowAttributes(state->shield,
                                   0,
                                   CaptionShieldAlpha,
                                   LWA_ALPHA) == FALSE) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_GEN_FAILURE;
        }
        DestroyWindow(state->shield);
        delete state;
        return {CaptureExclusionResult::SystemFailure, error};
    }

    if (SetWindowSubclass(owner,
                          CaptionShieldOwnerSubclassProc,
                          CaptionShieldOwnerSubclassId,
                          reinterpret_cast<DWORD_PTR>(state)) == FALSE) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_GEN_FAILURE;
        }
        DestroyWindow(state->shield);
        delete state;
        return {CaptureExclusionResult::SystemFailure, error};
    }

    SetLastError(ERROR_SUCCESS);
    if (SetPropW(owner,
                 CaptionShieldStateProperty,
                 reinterpret_cast<HANDLE>(state)) == FALSE) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_GEN_FAILURE;
        }
        (void)RemoveWindowSubclass(owner,
                                   CaptionShieldOwnerSubclassProc,
                                   CaptionShieldOwnerSubclassId);
        DestroyWindow(state->shield);
        delete state;
        return {CaptureExclusionResult::SystemFailure, error};
    }

    const CaptureExclusionOutcome shield_outcome =
        ApplyCaptureExclusion(state->shield, true);
    if (shield_outcome.result != CaptureExclusionResult::Applied) {
        (void)RemovePropW(owner, CaptionShieldStateProperty);
        (void)RemoveWindowSubclass(owner,
                                   CaptionShieldOwnerSubclassProc,
                                   CaptionShieldOwnerSubclassId);
        DestroyWindow(state->shield);
        delete state;
        return shield_outcome;
    }

    UpdateCaptionShield(*state);
    return {CaptureExclusionResult::Applied, ERROR_SUCCESS};
}

void RemoveCaptionShield(const HWND owner) noexcept {
    if (owner == nullptr || IsWindow(owner) == FALSE) {
        return;
    }

    auto* state = reinterpret_cast<CaptionShieldState*>(
        GetPropW(owner, CaptionShieldStateProperty));
    if (state == nullptr) {
        return;
    }

    (void)RemovePropW(owner, CaptionShieldStateProperty);
    (void)RemoveWindowSubclass(owner,
                               CaptionShieldOwnerSubclassProc,
                               CaptionShieldOwnerSubclassId);
    if (state != nullptr) {
        if (state->shield != nullptr && IsWindow(state->shield) != FALSE) {
            DestroyWindow(state->shield);
            state->shield = nullptr;
        }
        delete state;
    }
}

[[nodiscard]] bool IsCurrentProcessTopLevelWindow(const HWND window) noexcept {
    if (window == nullptr || IsWindow(window) == FALSE ||
        GetAncestor(window, GA_ROOT) != window) {
        return false;
    }

    DWORD process_id = 0;
    (void)GetWindowThreadProcessId(window, &process_id);
    return process_id == GetCurrentProcessId();
}


} // namespace

bool IsFullCaptureExclusionSupported() noexcept {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }

    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version == nullptr) {
        return false;
    }

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version(&version) < 0) {
        return false;
    }

    return version.dwMajorVersion > 10 ||
           (version.dwMajorVersion == 10 &&
            version.dwBuildNumber >= MinimumSupportedBuild);
}

bool IsCaptureExclusionRequested() noexcept {
    return capture_exclusion_requested.load(std::memory_order_acquire);
}

void SetCaptureExclusionRequested(const bool enabled) noexcept {
    capture_exclusion_requested.store(enabled, std::memory_order_release);
}

CaptureExclusionOutcome ApplyCaptureExclusion(const HWND window,
                                               const bool enabled) noexcept {
    if (!IsCurrentProcessTopLevelWindow(window)) {
        return {CaptureExclusionResult::InvalidWindow, ERROR_INVALID_WINDOW_HANDLE};
    }
    if (enabled && !IsFullCaptureExclusionSupported()) {
        return {CaptureExclusionResult::Unsupported, ERROR_OLD_WIN_VERSION};
    }

    const DWORD requested_affinity = enabled ? ExcludeFromCaptureAffinity : WDA_NONE;
    SetLastError(ERROR_SUCCESS);
    if (SetWindowDisplayAffinity(window, requested_affinity) == FALSE) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_GEN_FAILURE;
        }
        return {CaptureExclusionResult::SystemFailure, error};
    }

    DWORD observed_affinity = WDA_NONE;
    const bool verified =
        GetWindowDisplayAffinity(window, &observed_affinity) != FALSE;
    if (verified && observed_affinity != requested_affinity) {
        return {
            CaptureExclusionResult::SystemFailure,
            ERROR_GEN_FAILURE,
            true,
            observed_affinity,
        };
    }

    if (enabled && IsStandardCaptionWindow(window)) {
        const CaptureExclusionOutcome shield_outcome =
            EnsureCaptionShield(window);
        if (shield_outcome.result != CaptureExclusionResult::Applied) {
            // The owner itself is protected, but a standard caption can still
            // ask Windows to create separate hover UI. Do not report a complete
            // activation when the application-owned interception surface could
            // not be protected as well.
            (void)SetWindowDisplayAffinity(window, WDA_NONE);
            RemoveCaptionShield(window);
            return shield_outcome;
        }
    } else if (!enabled) {
        RemoveCaptionShield(window);
    }

    return {
        CaptureExclusionResult::Applied,
        ERROR_SUCCESS,
        verified,
        observed_affinity,
    };
}

CaptureExclusionOutcome ApplyRequestedCaptureExclusion(const HWND window) noexcept {
    return ApplyCaptureExclusion(window, IsCaptureExclusionRequested());
}

CaptureExclusionOutcome SynchronizeRequestedCaptureExclusion(
    const HWND window) noexcept {
    if (!IsCurrentProcessTopLevelWindow(window)) {
        return {CaptureExclusionResult::InvalidWindow,
                ERROR_INVALID_WINDOW_HANDLE};
    }

    const bool requested = IsCaptureExclusionRequested();
    if (requested && !IsFullCaptureExclusionSupported()) {
        return {CaptureExclusionResult::Unsupported, ERROR_OLD_WIN_VERSION};
    }

    const DWORD requested_affinity =
        requested ? ExcludeFromCaptureAffinity : WDA_NONE;

    DWORD current_affinity = WDA_NONE;
    if (GetWindowDisplayAffinity(window, &current_affinity) != FALSE &&
        current_affinity == requested_affinity) {
        if (requested && IsStandardCaptionWindow(window)) {
            const CaptureExclusionOutcome shield_outcome =
                EnsureCaptionShield(window);
            if (shield_outcome.result != CaptureExclusionResult::Applied) {
                return shield_outcome;
            }
        } else if (!requested) {
            RemoveCaptionShield(window);
        }

        return {
            CaptureExclusionResult::Applied,
            ERROR_SUCCESS,
            true,
            current_affinity,
        };
    }

    return ApplyCaptureExclusion(window, requested);
}

} // namespace vectorclick::win

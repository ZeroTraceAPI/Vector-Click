#include "Windows/window_message_backend.h"
#include "Windows/keyboard_character.h"
#include "Windows/numpad_keys.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace vectorclick::win {
namespace {

constexpr UINT MessageTimeoutMilliseconds = 25;
constexpr int MaximumChildDepth = 32;

struct ButtonMessages {
    UINT down{};
    UINT up{};
    WORD down_key_state{};
    WORD x_button{};
};

ButtonMessages MessagesFor(const core::MouseButton button) noexcept {
    switch (button) {
    case core::MouseButton::Left:
        return {WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON, 0};
    case core::MouseButton::Right:
        return {WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON, 0};
    case core::MouseButton::Middle:
        return {WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON, 0};
    case core::MouseButton::X1:
        return {WM_XBUTTONDOWN, WM_XBUTTONUP, MK_XBUTTON1, XBUTTON1};
    case core::MouseButton::X2:
        return {WM_XBUTTONDOWN, WM_XBUTTONUP, MK_XBUTTON2, XBUTTON2};
    }
    return {};
}

bool IsRepresentableClientPoint(const POINT point) noexcept {
    return point.x >= std::numeric_limits<std::int16_t>::min() &&
           point.x <= std::numeric_limits<std::int16_t>::max() &&
           point.y >= std::numeric_limits<std::int16_t>::min() &&
           point.y <= std::numeric_limits<std::int16_t>::max();
}

LPARAM PackClientPoint(const POINT point) noexcept {
    return MAKELPARAM(static_cast<WORD>(static_cast<std::int16_t>(point.x)),
                      static_cast<WORD>(static_cast<std::int16_t>(point.y)));
}

bool SendWithTimeout(const HWND recipient,
                     const UINT message,
                     const WPARAM w_param,
                     const LPARAM l_param,
                     DWORD* windows_error = nullptr) noexcept {
    SetLastError(ERROR_SUCCESS);
    DWORD_PTR ignored_result{};
    const bool completed = SendMessageTimeoutW(
                               recipient,
                               message,
                               w_param,
                               l_param,
                               SMTO_ABORTIFHUNG | SMTO_BLOCK,
                               MessageTimeoutMilliseconds,
                               &ignored_result) != 0;
    const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
    if (windows_error != nullptr) {
        *windows_error = error;
    }
    return completed;
}

bool IsExtendedVirtualKey(const std::uint16_t virtual_key) noexcept {
    switch (virtual_key) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_DIVIDE:
        return true;
    default:
        return false;
    }
}

LPARAM MakeKeyDownLParam(const std::uint16_t virtual_key) noexcept {
    UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC_EX);
    if (scan_code == 0) {
        scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    }

    LPARAM value = 1;
    value |= static_cast<LPARAM>(scan_code & 0xFFU) << 16;
    if (IsExtendedVirtualKey(virtual_key) || (scan_code & 0xFF00U) == 0xE000U) {
        value |= static_cast<LPARAM>(1) << 24;
    }
    return value;
}


} // namespace

bool WindowMessageBackend::BeginSession(const core::RunSettings&,
                                        const TargetWindowInfo& target) noexcept {
    // Never erase a prior release ledger merely because another run was
    // requested. The caller must retry cleanup until every tracked press has
    // a confirmed matching release or the process ends.
    if (HasTrackedInput()) {
        return false;
    }

    ClearSession();
    if (!IsTargetWindowValid(target)) {
        return false;
    }

    try {
        target_ = target;
    } catch (...) {
        ClearSession();
        return false;
    }
    return true;
}

bool WindowMessageBackend::Press(const core::RunSettings& settings,
                                 const InputSessionCancellation& cancellation) noexcept {
    if (cancellation.Requested() ||
        !IsTargetWindowValid(target_) ||
        !DeliveryIsAllowed(settings)) {
        return false;
    }

    if (settings.action_type == core::ActionType::KeyboardPress) {
        if (keyboard_pressed_.pressed || keyboard_pressed_.shift_pressed) {
            return !cancellation.Requested();
        }
        if (settings.generated_virtual_key == 0 ||
            (settings.generated_key_modifiers & ~core::SupportedKeyModifiers) != 0) {
            return false;
        }

        const HWND recipient = ResolveKeyboardRecipient();
        if (recipient == nullptr || cancellation.Requested()) {
            return false;
        }

        keyboard_pressed_.recipient = recipient;
        const bool requires_shift =
            (settings.generated_key_modifiers & core::KeyModifierShift) != 0;
        if (requires_shift) {
            keyboard_pressed_.shift_down_lparam = MakeKeyDownLParam(VK_SHIFT);

            // SendMessageTimeout can return zero after the destination window
            // procedure has already started processing a sent message. Pre-arm
            // the release ledger so a late Shift-down remains represented for
            // cleanup. UIPI access denial is documented as definite
            // non-delivery, so that one failure can safely roll the ledger back.
            keyboard_pressed_.shift_pressed = true;
            DWORD windows_error{};
            if (!SendKeyMessage(recipient,
                                VK_SHIFT,
                                true,
                                keyboard_pressed_.shift_down_lparam,
                                &windows_error)) {
                if (windows_error == ERROR_ACCESS_DENIED) {
                    keyboard_pressed_.shift_pressed = false;
                    keyboard_pressed_ = {};
                }
                return false;
            }
            if (cancellation.Requested()) {
                return false;
            }
        }

        std::uint16_t delivered_virtual_key = settings.generated_virtual_key;
        std::uint16_t physical_scan_key = settings.generated_virtual_key;
        if (requires_shift) {
            if (const auto navigation_key =
                    numpad::ShiftedNavigationVirtualKey(
                        settings.generated_virtual_key);
                navigation_key.has_value()) {
                // Targeted messages do not pass through the keyboard driver's
                // Num Lock translation. Deliver the navigation virtual key
                // explicitly while retaining the physical numpad scan code.
                delivered_virtual_key = *navigation_key;
            }
        }

        const LPARAM key_down_lparam = MakeKeyDownLParam(physical_scan_key);

        // Pre-arm before the state-changing send for the same timeout ambiguity
        // handled above. A zero return is not proof that the target did not apply
        // WM_KEYDOWN: its window procedure may already be running and can finish
        // after our timeout. Keep the ledger unless Windows reports documented
        // UIPI access denial, then let the existing cleanup path attempt key-up.
        keyboard_pressed_.pressed = true;
        keyboard_pressed_.virtual_key = delivered_virtual_key;
        keyboard_pressed_.key_down_lparam = key_down_lparam;
        DWORD windows_error{};
        if (!SendKeyMessage(recipient,
                            delivered_virtual_key,
                            true,
                            key_down_lparam,
                            &windows_error)) {
            if (windows_error == ERROR_ACCESS_DENIED) {
                keyboard_pressed_.pressed = false;
            }
            (void)ReleaseKeyboardState();
            return false;
        }

        if (cancellation.Requested()) {
            return false;
        }

        if (!SendCharacterMessage(recipient,
                                  delivered_virtual_key,
                                  settings.generated_key_modifiers,
                                  key_down_lparam)) {
            (void)ReleaseKeyboardState();
            return false;
        }

        return !cancellation.Requested();
    }

    const std::size_t index = MouseIndex(settings.mouse_button);
    auto& state = mouse_pressed_[index];
    if (state.pressed) {
        return !cancellation.Requested();
    }

    HWND recipient{};
    LPARAM client_position{};
    ScreenPoint screen_point{};
    if (!ResolveMouseRecipient(settings, recipient, client_position, screen_point) ||
        cancellation.Requested()) {
        return false;
    }

    // A movement message helps ordinary desktop controls update their internal
    // hover position without moving the user's physical pointer. Never follow it
    // with a down message after Stop or Emergency Stop has invalidated the run.
    if (!SendWithTimeout(recipient, WM_MOUSEMOVE, 0, client_position) ||
        cancellation.Requested()) {
        return false;
    }

    // Pre-arm the release ledger before the state-changing send. A timed-out
    // SendMessageTimeout call can already be executing the target WndProc, which
    // means WM_*BUTTONDOWN may complete after this thread resumes. Keeping the
    // ledger on indeterminate failure ensures ReleaseAll still attempts the
    // matching up. UIPI access denial is documented as non-delivery and is
    // therefore the one failure that can safely roll this tentative entry back.
    state.pressed = true;
    state.recipient = recipient;
    state.client_position = client_position;
    state.screen_point = screen_point;
    DWORD windows_error{};
    if (!SendButtonMessage(recipient,
                           settings.mouse_button,
                           true,
                           client_position,
                           &windows_error)) {
        if (windows_error == ERROR_ACCESS_DENIED) {
            state = {};
        }
        return false;
    }

    last_mouse_press_point_ = screen_point;
    has_last_mouse_press_point_ = true;
    return !cancellation.Requested();
}

bool WindowMessageBackend::Release(const core::RunSettings& settings) noexcept {
    if (settings.action_type == core::ActionType::KeyboardPress) {
        if ((keyboard_pressed_.pressed || keyboard_pressed_.shift_pressed) &&
            (keyboard_pressed_.recipient == nullptr ||
             IsWindow(keyboard_pressed_.recipient) == FALSE)) {
            // Resolve the dead-recipient ledger, but report the normal release as
            // failed so the active run stops instead of waiting for another input.
            keyboard_pressed_ = {};
            return false;
        }
        return ReleaseKeyboardState();
    }

    auto& state = mouse_pressed_[MouseIndex(settings.mouse_button)];
    if (!state.pressed) {
        return true;
    }

    const HWND recipient = state.recipient;
    const LPARAM client_position = state.client_position;
    if (recipient == nullptr || IsWindow(recipient) == FALSE) {
        // Direct window-message input has no global Windows button state to
        // release. Once the exact recipient HWND is gone, retaining this ledger
        // can only block future runs; never redirect the release to a replacement.
        state = {};
        return false;
    }
    if (!IsRecipientWithinTarget(recipient) ||
        !SendButtonMessage(recipient, settings.mouse_button, false, client_position)) {
        return false;
    }
    state = {};
    return true;
}

bool WindowMessageBackend::ReleaseAll() noexcept {
    constexpr std::array buttons{
        core::MouseButton::Left,
        core::MouseButton::Right,
        core::MouseButton::Middle,
        core::MouseButton::X1,
        core::MouseButton::X2,
    };

    bool all_releases_submitted = true;
    for (const auto button : buttons) {
        auto& state = mouse_pressed_[MouseIndex(button)];
        if (!state.pressed) {
            continue;
        }

        const HWND recipient = state.recipient;
        const LPARAM client_position = state.client_position;
        if (recipient == nullptr || IsWindow(recipient) == FALSE) {
            // The recipient was destroyed, so there is no longer a WndProc that
            // can own the direct-message pressed state or accept a matching up.
            // Clear only this proven-dead ledger entry and never retarget it.
            state = {};
            continue;
        }
        if (!IsRecipientWithinTarget(recipient) ||
            !SendButtonMessage(recipient, button, false, client_position)) {
            all_releases_submitted = false;
        } else {
            state = {};
        }
    }

    if ((keyboard_pressed_.pressed || keyboard_pressed_.shift_pressed) &&
        !ReleaseKeyboardState()) {
        all_releases_submitted = false;
    }

    bool has_tracked_input =
        keyboard_pressed_.pressed || keyboard_pressed_.shift_pressed;
    for (const auto& state : mouse_pressed_) {
        has_tracked_input = has_tracked_input || state.pressed;
    }
    if (!has_tracked_input) {
        target_ = {};
    }
    return all_releases_submitted;
}

bool WindowMessageBackend::HasTrackedInput() const noexcept {
    if (keyboard_pressed_.pressed || keyboard_pressed_.shift_pressed) {
        return true;
    }
    return std::any_of(mouse_pressed_.begin(), mouse_pressed_.end(), [](const PressedMouseState& state) {
        return state.pressed;
    });
}

bool WindowMessageBackend::LastMousePressScreenPoint(ScreenPoint& point) const noexcept {
    if (!has_last_mouse_press_point_) {
        return false;
    }
    point = last_mouse_press_point_;
    return true;
}

std::size_t WindowMessageBackend::MouseIndex(const core::MouseButton button) noexcept {
    return static_cast<std::size_t>(button);
}

bool WindowMessageBackend::DeliveryIsAllowed(const core::RunSettings& settings) const noexcept {
    return settings.allow_background_input || IsTargetWindowForeground(target_);
}

bool WindowMessageBackend::ResolveMouseRecipient(const core::RunSettings& settings,
                                                 HWND& recipient,
                                                 LPARAM& client_position,
                                                 ScreenPoint& resolved_screen_point) const noexcept {
    if (IsIconic(target_.window) != FALSE) {
        return false;
    }

    POINT screen_point{};
    if (settings.position_mode == core::PositionMode::FixedScreen) {
        screen_point.x = settings.fixed_x;
        screen_point.y = settings.fixed_y;
    } else if (GetCursorPos(&screen_point) == FALSE) {
        return false;
    }

    resolved_screen_point = {screen_point.x, screen_point.y};

    recipient = target_.window;
    POINT point_in_recipient = screen_point;
    if (ScreenToClient(recipient, &point_in_recipient) == FALSE) {
        return false;
    }

    RECT client_rect{};
    if (GetClientRect(recipient, &client_rect) == FALSE ||
        PtInRect(&client_rect, point_in_recipient) == FALSE) {
        return false;
    }

    for (int depth = 0; depth < MaximumChildDepth; ++depth) {
        const HWND child = ChildWindowFromPointEx(
            recipient,
            point_in_recipient,
            CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (child == nullptr || child == recipient || !IsRecipientWithinTarget(child)) {
            break;
        }

        recipient = child;
        point_in_recipient = screen_point;
        if (ScreenToClient(recipient, &point_in_recipient) == FALSE) {
            return false;
        }
        if (GetClientRect(recipient, &client_rect) == FALSE ||
            PtInRect(&client_rect, point_in_recipient) == FALSE) {
            return false;
        }
    }

    if (!IsRepresentableClientPoint(point_in_recipient)) {
        return false;
    }
    client_position = PackClientPoint(point_in_recipient);
    return true;
}

HWND WindowMessageBackend::ResolveKeyboardRecipient() const noexcept {
    return ResolveTargetKeyboardRecipient(target_);
}

bool WindowMessageBackend::SendButtonMessage(const HWND recipient,
                                             const core::MouseButton button,
                                             const bool pressed,
                                             const LPARAM client_position,
                                             DWORD* const windows_error) noexcept {
    const ButtonMessages messages = MessagesFor(button);
    if (messages.down == 0 || messages.up == 0) {
        return false;
    }

    const UINT message = pressed ? messages.down : messages.up;
    const WORD low_word = pressed ? messages.down_key_state : 0;
    const WPARAM w_param = MAKEWPARAM(low_word, messages.x_button);
    return SendWithTimeout(
        recipient, message, w_param, client_position, windows_error);
}

bool WindowMessageBackend::SendKeyMessage(const HWND recipient,
                                          const std::uint16_t virtual_key,
                                          const bool pressed,
                                          const LPARAM key_down_lparam,
                                          DWORD* const windows_error) noexcept {
    const LPARAM down_lparam = key_down_lparam != 0
                                  ? key_down_lparam
                                  : MakeKeyDownLParam(virtual_key);
    const LPARAM l_param = pressed
                               ? down_lparam
                               : down_lparam |
                                     (static_cast<LPARAM>(1) << 30) |
                                     (static_cast<LPARAM>(1) << 31);
    return SendWithTimeout(recipient,
                           pressed ? WM_KEYDOWN : WM_KEYUP,
                           static_cast<WPARAM>(virtual_key),
                           l_param,
                           windows_error);
}

bool WindowMessageBackend::SendCharacterMessage(
    const HWND recipient,
    const std::uint16_t virtual_key,
    const std::uint16_t modifiers,
    const LPARAM key_down_lparam) noexcept {
    const auto character = CharacterForVirtualKey(virtual_key, modifiers);
    if (!character.has_value()) {
        return true;
    }
    return SendWithTimeout(recipient,
                           WM_CHAR,
                           static_cast<WPARAM>(*character),
                           key_down_lparam);
}

bool WindowMessageBackend::ReleaseKeyboardState() noexcept {
    if (!keyboard_pressed_.pressed && !keyboard_pressed_.shift_pressed) {
        keyboard_pressed_ = {};
        return true;
    }

    if (keyboard_pressed_.recipient == nullptr ||
        IsWindow(keyboard_pressed_.recipient) == FALSE) {
        // Like mouse window messages, these presses exist only in the exact
        // recipient's message processing. A destroyed HWND cannot accept an up
        // and must not be replaced with a newly created window.
        keyboard_pressed_ = {};
        return true;
    }
    if (!IsRecipientWithinTarget(keyboard_pressed_.recipient)) {
        return false;
    }

    bool all_releases_submitted = true;
    if (keyboard_pressed_.pressed) {
        if (SendKeyMessage(keyboard_pressed_.recipient,
                           keyboard_pressed_.virtual_key,
                           false,
                           keyboard_pressed_.key_down_lparam)) {
            keyboard_pressed_.pressed = false;
        } else {
            all_releases_submitted = false;
        }
    }
    if (keyboard_pressed_.shift_pressed) {
        if (SendKeyMessage(keyboard_pressed_.recipient,
                           VK_SHIFT,
                           false,
                           keyboard_pressed_.shift_down_lparam)) {
            keyboard_pressed_.shift_pressed = false;
        } else {
            all_releases_submitted = false;
        }
    }

    if (!keyboard_pressed_.pressed && !keyboard_pressed_.shift_pressed) {
        keyboard_pressed_ = {};
    }
    return all_releases_submitted;
}

bool WindowMessageBackend::IsRecipientWithinTarget(const HWND recipient) const noexcept {
    return IsWindowWithinTarget(target_, recipient);
}

void WindowMessageBackend::ClearSession() noexcept {
    for (auto& state : mouse_pressed_) {
        state = {};
    }
    keyboard_pressed_ = {};
    has_last_mouse_press_point_ = false;
    target_ = {};
}

} // namespace vectorclick::win

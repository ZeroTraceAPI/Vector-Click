#include "Windows/send_input_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace vectorclick::win {
namespace {

constexpr ULONG_PTR VectorClickInputMarker = static_cast<ULONG_PTR>(0);

struct MouseFlags {
    DWORD down{};
    DWORD up{};
    DWORD data{};
};

MouseFlags FlagsFor(const core::MouseButton button) noexcept {
    switch (button) {
    case core::MouseButton::Left:
        return {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0};
    case core::MouseButton::Right:
        return {MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0};
    case core::MouseButton::Middle:
        return {MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0};
    case core::MouseButton::X1:
        return {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1};
    case core::MouseButton::X2:
        return {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2};
    }
    return {};
}

bool IsExtendedKey(const std::uint16_t virtual_key) noexcept {
    switch (virtual_key) {
    case VK_RMENU:
    case VK_RCONTROL:
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
    case VK_NUMLOCK:
    case VK_DIVIDE:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
        return true;
    default:
        return false;
    }
}

INPUT MakeKeyboardInput(const std::uint16_t virtual_key, const bool key_up) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.dwExtraInfo = VectorClickInputMarker;

    const UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    if (scan_code != 0) {
        input.ki.wVk = 0;
        input.ki.wScan = static_cast<WORD>(scan_code);
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
    } else {
        input.ki.wVk = static_cast<WORD>(virtual_key);
    }

    if (IsExtendedKey(virtual_key)) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (key_up) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    return input;
}

bool NormalizeFixedPosition(const std::int32_t x,
                            const std::int32_t y,
                            std::int32_t& normalized_x,
                            std::int32_t& normalized_y) noexcept {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 1 || height <= 1) {
        return false;
    }

    const std::int64_t right = static_cast<std::int64_t>(left) + width - 1;
    const std::int64_t bottom = static_cast<std::int64_t>(top) + height - 1;
    if (x < left || static_cast<std::int64_t>(x) > right ||
        y < top || static_cast<std::int64_t>(y) > bottom) {
        return false;
    }

    const std::int64_t relative_x = static_cast<std::int64_t>(x) - left;
    const std::int64_t relative_y = static_cast<std::int64_t>(y) - top;
    normalized_x = static_cast<std::int32_t>((relative_x * 65'535 + (width - 1) / 2) / (width - 1));
    normalized_y = static_cast<std::int32_t>((relative_y * 65'535 + (height - 1) / 2) / (height - 1));
    return true;
}

bool SubmitMouseEvent(const DWORD flags, const DWORD data) noexcept {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = data;
    input.mi.dwExtraInfo = VectorClickInputMarker;
    return SendInput(1, &input, sizeof(input)) == 1;
}

} // namespace

std::size_t SendInputBackend::MouseIndex(const core::MouseButton button) noexcept {
    return static_cast<std::size_t>(button);
}

bool SendInputBackend::Press(const core::RunSettings& settings,
                             const InputSessionCancellation& cancellation) noexcept {
    if (cancellation.Requested()) {
        return false;
    }
    if (settings.action_type == core::ActionType::KeyboardPress) {
        return PressKeyboard(settings, cancellation);
    }
    return PressMouse(settings);
}

bool SendInputBackend::Release(const core::RunSettings& settings) noexcept {
    if (settings.action_type == core::ActionType::KeyboardPress) {
        return ReleaseKeyboard(settings);
    }
    return ReleaseMouse(settings.mouse_button);
}

bool SendInputBackend::PressMouse(const core::RunSettings& settings) noexcept {
    const auto index = MouseIndex(settings.mouse_button);
    auto& state = mouse_pressed_[index];
    if (state.pressed) {
        has_last_mouse_press_point_ = state.has_screen_point;
        if (state.has_screen_point) {
            last_mouse_press_point_ = state.screen_point;
        }
        return true;
    }

    const auto flags = FlagsFor(settings.mouse_button);
    const bool atomic_pulse =
        settings.action_pattern != core::ActionPattern::Hold &&
        settings.button_down_microseconds == 0;
    PressedMouseState accepted{};
    if (settings.position_mode == core::PositionMode::FixedScreen) {
        if (!NormalizeFixedPosition(settings.fixed_x,
                                    settings.fixed_y,
                                    accepted.normalized_x,
                                    accepted.normalized_y)) {
            return false;
        }

        accepted.fixed_position = true;
        accepted.has_screen_point = true;
        accepted.screen_point = {settings.fixed_x, settings.fixed_y};

        if (atomic_pulse) {
            std::array<INPUT, 3> inputs{};
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dx = accepted.normalized_x;
            inputs[0].mi.dy = accepted.normalized_y;
            inputs[0].mi.dwFlags =
                MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            inputs[0].mi.dwExtraInfo = VectorClickInputMarker;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dwFlags = flags.down;
            inputs[1].mi.mouseData = flags.data;
            inputs[1].mi.dwExtraInfo = VectorClickInputMarker;
            inputs[2].type = INPUT_MOUSE;
            inputs[2].mi.dwFlags = flags.up;
            inputs[2].mi.mouseData = flags.data;
            inputs[2].mi.dwExtraInfo = VectorClickInputMarker;

            const UINT submitted = SendInput(
                static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
            const bool down_accepted = submitted >= 2U;
            const bool up_accepted = submitted >= 3U;
            if (down_accepted) {
                has_last_mouse_press_point_ = true;
                last_mouse_press_point_ = accepted.screen_point;
            }
            if (down_accepted && !up_accepted) {
                accepted.pressed = true;
                state = accepted;
            }
            return submitted == inputs.size();
        }

        std::array<INPUT, 2> inputs{};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = accepted.normalized_x;
        inputs[0].mi.dy = accepted.normalized_y;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inputs[0].mi.dwExtraInfo = VectorClickInputMarker;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = flags.down;
        inputs[1].mi.mouseData = flags.data;
        inputs[1].mi.dwExtraInfo = VectorClickInputMarker;
        if (SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) != inputs.size()) {
            return false;
        }
    } else {
        POINT screen_point{};
        if (settings.show_click_position_indicator &&
            GetCursorPos(&screen_point) != FALSE) {
            accepted.has_screen_point = true;
            accepted.screen_point = {screen_point.x, screen_point.y};
        }
        if (atomic_pulse) {
            std::array<INPUT, 2> inputs{};
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dwFlags = flags.down;
            inputs[0].mi.mouseData = flags.data;
            inputs[0].mi.dwExtraInfo = VectorClickInputMarker;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dwFlags = flags.up;
            inputs[1].mi.mouseData = flags.data;
            inputs[1].mi.dwExtraInfo = VectorClickInputMarker;

            const UINT submitted = SendInput(
                static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
            const bool down_accepted = submitted >= 1U;
            const bool up_accepted = submitted >= 2U;
            if (down_accepted) {
                has_last_mouse_press_point_ = accepted.has_screen_point;
                if (accepted.has_screen_point) {
                    last_mouse_press_point_ = accepted.screen_point;
                }
            }
            if (down_accepted && !up_accepted) {
                accepted.pressed = true;
                state = accepted;
            }
            return submitted == inputs.size();
        }

        if (!SubmitMouseEvent(flags.down, flags.data)) {
            return false;
        }
    }

    accepted.pressed = true;
    state = accepted;
    has_last_mouse_press_point_ = accepted.has_screen_point;
    if (accepted.has_screen_point) {
        last_mouse_press_point_ = accepted.screen_point;
    }
    return true;
}

bool SendInputBackend::ReleaseMouse(const core::MouseButton button) noexcept {
    const auto index = MouseIndex(button);
    auto& state = mouse_pressed_[index];
    if (!state.pressed) {
        return true;
    }

    const auto flags = FlagsFor(button);
    bool released = false;
    if (state.fixed_position) {
        std::array<INPUT, 2> inputs{};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = state.normalized_x;
        inputs[0].mi.dy = state.normalized_y;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inputs[0].mi.dwExtraInfo = VectorClickInputMarker;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = flags.up;
        inputs[1].mi.mouseData = flags.data;
        inputs[1].mi.dwExtraInfo = VectorClickInputMarker;
        released = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
    } else {
        released = SubmitMouseEvent(flags.up, flags.data);
    }

    if (!released) {
        return false;
    }

    state = {};
    return true;
}

bool SendInputBackend::PressKeyboard(
    const core::RunSettings& settings,
    const InputSessionCancellation& cancellation) noexcept {
    if (settings.generated_virtual_key == 0 ||
        (settings.generated_key_modifiers & ~core::SupportedKeyModifiers) != 0) {
        return false;
    }

    generated_shift_injected_ = false;
    const bool requires_shift =
        (settings.generated_key_modifiers & core::KeyModifierShift) != 0;
    const bool atomic_pulse =
        settings.action_pattern != core::ActionPattern::Hold &&
        settings.button_down_microseconds == 0;
    if (!requires_shift) {
        if (atomic_pulse) {
            const std::array<KeyboardTransition, 2> transitions{{
                {settings.generated_virtual_key, false, false},
                {settings.generated_virtual_key, true, false},
            }};
            return SubmitKeyboardSequence(transitions.data(), transitions.size()) &&
                   !cancellation.Requested();
        }
        return PressKey(settings.generated_virtual_key) && !cancellation.Requested();
    }

    // A user's physical Shift remains authoritative. Vector Click claims Shift
    // only when neither Shift key is already down before this generated chord.
    // This preserves the established rule that generated cleanup must never
    // release a modifier the user was already holding.
    const bool physical_shift_is_down =
        (GetAsyncKeyState(VK_SHIFT) & static_cast<SHORT>(0x8000)) != 0;

    if (atomic_pulse) {
        // A zero-duration shifted key is one complete chord. Submitting the
        // whole pulse in one SendInput array prevents another physical or
        // injected keyboard event from being interspersed between Shift and
        // the base key while still leaving no generated key held afterward.
        if (physical_shift_is_down) {
            const std::array<KeyboardTransition, 2> transitions{{
                {settings.generated_virtual_key, false, false},
                {settings.generated_virtual_key, true, false},
            }};
            return SubmitKeyboardSequence(transitions.data(), transitions.size()) &&
                   !cancellation.Requested();
        }

        const std::array<KeyboardTransition, 4> transitions{{
            {VK_SHIFT, false, true},
            {settings.generated_virtual_key, false, false},
            {settings.generated_virtual_key, true, false},
            {VK_SHIFT, true, true},
        }};
        return SubmitKeyboardSequence(transitions.data(), transitions.size()) &&
               !cancellation.Requested();
    }

    if (physical_shift_is_down) {
        return PressKey(settings.generated_virtual_key) && !cancellation.Requested();
    }

    // For a nonzero Down duration, Shift and the base key must remain held
    // across the scheduler wait. Submit the down phase together so nothing can
    // be inserted between the two events. The matching up phase is likewise
    // submitted as one sequence by ReleaseKeyboard().
    const std::array<KeyboardTransition, 2> transitions{{
        {VK_SHIFT, false, true},
        {settings.generated_virtual_key, false, false},
    }};
    if (!SubmitKeyboardSequence(transitions.data(), transitions.size())) {
        return false;
    }

    if (cancellation.Requested()) {
        return false;
    }
    return true;
}

bool SendInputBackend::ReleaseKeyboard(
    const core::RunSettings& settings) noexcept {
    if (!key_pressed_[settings.generated_virtual_key] && !generated_shift_injected_) {
        // Zero-duration shifted chords are already fully released by the
        // atomic sequence submitted from PressKeyboard().
        return true;
    }

    if (!generated_shift_injected_) {
        return ReleaseKey(settings.generated_virtual_key);
    }

    const std::array<KeyboardTransition, 2> transitions{{
        {settings.generated_virtual_key, true, false},
        {VK_SHIFT, true, true},
    }};
    return SubmitKeyboardSequence(transitions.data(), transitions.size());
}

bool SendInputBackend::SubmitKeyboardSequence(
    const KeyboardTransition* transitions,
    const std::size_t count) noexcept {
    if (transitions == nullptr || count == 0 || count > 4) {
        return false;
    }

    std::array<INPUT, 4> inputs{};
    for (std::size_t index = 0; index < count; ++index) {
        const auto virtual_key = transitions[index].virtual_key;
        if (virtual_key == 0 || virtual_key >= key_pressed_.size()) {
            return false;
        }
        inputs[index] = MakeKeyboardInput(virtual_key, transitions[index].key_up);
    }

    const UINT submitted = SendInput(
        static_cast<UINT>(count), inputs.data(), sizeof(INPUT));

    // SendInput reports the number of events accepted from the supplied array.
    // Apply only that accepted prefix to the persistent release ledger. If a
    // partial submission ever occurs, the caller fails safely and ReleaseAll()
    // remains able to release every generated state that Windows accepted.
    const std::size_t accepted = std::min<std::size_t>(submitted, count);
    for (std::size_t index = 0; index < accepted; ++index) {
        const auto& transition = transitions[index];
        key_pressed_[transition.virtual_key] = !transition.key_up;
        if (transition.generated_shift_transition) {
            generated_shift_injected_ = !transition.key_up;
        }
    }

    return accepted == count;
}

bool SendInputBackend::PressKey(const std::uint16_t virtual_key) noexcept {
    if (virtual_key == 0 || virtual_key >= key_pressed_.size()) {
        return false;
    }
    if (key_pressed_[virtual_key]) {
        return true;
    }

    INPUT input = MakeKeyboardInput(virtual_key, false);
    if (SendInput(1, &input, sizeof(input)) != 1) {
        return false;
    }
    key_pressed_[virtual_key] = true;
    return true;
}

bool SendInputBackend::ReleaseKey(const std::uint16_t virtual_key) noexcept {
    if (virtual_key == 0 || virtual_key >= key_pressed_.size()) {
        return false;
    }
    if (!key_pressed_[virtual_key]) {
        return true;
    }

    INPUT input = MakeKeyboardInput(virtual_key, true);
    if (SendInput(1, &input, sizeof(input)) != 1) {
        return false;
    }
    key_pressed_[virtual_key] = false;
    return true;
}

bool SendInputBackend::ReleaseAll() noexcept {
    constexpr std::array buttons{
        core::MouseButton::Left,
        core::MouseButton::Right,
        core::MouseButton::Middle,
        core::MouseButton::X1,
        core::MouseButton::X2,
    };

    bool all_releases_submitted = true;
    for (const auto button : buttons) {
        if (!ReleaseMouse(button)) {
            all_releases_submitted = false;
        }
    }
    // Release generated base keys before generated Shift. Normal shifted-key
    // release already follows this chord order; cleanup paths such as Hold,
    // Emergency Stop, shutdown, and partial-submission recovery must do the same
    // so a held Shift+key is never briefly converted into an unshifted held key.
    for (std::size_t key = 1; key < key_pressed_.size(); ++key) {
        if (key == VK_SHIFT) {
            continue;
        }
        if (key_pressed_[key] &&
            !ReleaseKey(static_cast<std::uint16_t>(key))) {
            all_releases_submitted = false;
        }
    }
    if (key_pressed_[VK_SHIFT] && !ReleaseKey(VK_SHIFT)) {
        all_releases_submitted = false;
    }
    if (!key_pressed_[VK_SHIFT]) {
        generated_shift_injected_ = false;
    }
    return all_releases_submitted;
}

bool SendInputBackend::HasTrackedInput() const noexcept {
    if (generated_shift_injected_) {
        return true;
    }
    for (const auto& state : mouse_pressed_) {
        if (state.pressed) {
            return true;
        }
    }
    return std::any_of(key_pressed_.begin(), key_pressed_.end(), [](const bool pressed) {
        return pressed;
    });
}

bool SendInputBackend::LastMousePressScreenPoint(ScreenPoint& point) const noexcept {
    if (!has_last_mouse_press_point_) {
        return false;
    }
    point = last_mouse_press_point_;
    return true;
}

} // namespace vectorclick::win

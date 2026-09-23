#include "Windows/unicode_input_backend.h"
#include "Windows/keyboard_character.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>

namespace vectorclick::win {
namespace {

constexpr ULONG_PTR VectorClickInputMarker = static_cast<ULONG_PTR>(0);

INPUT MakeUnicodeInput(const wchar_t character, const bool key_up) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = static_cast<WORD>(character);
    input.ki.dwFlags = KEYEVENTF_UNICODE | (key_up ? KEYEVENTF_KEYUP : 0U);
    input.ki.dwExtraInfo = VectorClickInputMarker;
    return input;
}

} // namespace

bool UnicodeInputBackend::Press(
    const core::RunSettings& settings,
    const InputSessionCancellation& cancellation) noexcept {
    if (cancellation.Requested() ||
        settings.action_type != core::ActionType::KeyboardPress ||
        pressed_character_.has_value()) {
        return false;
    }

    const auto character = UnicodeTextCharacterForKey(
        settings.generated_virtual_key, settings.generated_key_modifiers);
    if (!character.has_value()) {
        return false;
    }

    if (settings.button_down_microseconds == 0) {
        return SubmitPulse(*character) && !cancellation.Requested();
    }

    if (!SubmitCharacter(*character, false)) {
        return false;
    }
    pressed_character_ = *character;
    return !cancellation.Requested();
}

bool UnicodeInputBackend::Release(const core::RunSettings&) noexcept {
    return ReleaseAll();
}

bool UnicodeInputBackend::ReleaseAll() noexcept {
    if (!pressed_character_.has_value()) {
        return true;
    }
    if (!SubmitCharacter(*pressed_character_, true)) {
        return false;
    }
    pressed_character_.reset();
    return true;
}

bool UnicodeInputBackend::HasTrackedInput() const noexcept {
    return pressed_character_.has_value();
}

bool UnicodeInputBackend::LastMousePressScreenPoint(ScreenPoint&) const noexcept {
    return false;
}

bool UnicodeInputBackend::SubmitCharacter(
    const wchar_t character,
    const bool key_up) noexcept {
    INPUT input = MakeUnicodeInput(character, key_up);
    return SendInput(1, &input, sizeof(input)) == 1;
}

bool UnicodeInputBackend::SubmitPulse(const wchar_t character) noexcept {
    std::array<INPUT, 2> inputs{
        MakeUnicodeInput(character, false),
        MakeUnicodeInput(character, true),
    };
    const UINT submitted = SendInput(
        static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (submitted == inputs.size()) {
        return true;
    }
    if (submitted == 1U) {
        pressed_character_ = character;
    }
    return false;
}

} // namespace vectorclick::win

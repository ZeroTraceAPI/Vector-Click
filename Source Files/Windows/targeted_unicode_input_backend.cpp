#include "Windows/targeted_unicode_input_backend.h"
#include "Windows/keyboard_character.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win {
namespace {

constexpr UINT MessageTimeoutMilliseconds = 25;

LPARAM MakeCharacterLParam(const std::uint16_t virtual_key) noexcept {
    UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC_EX);
    if (scan_code == 0) {
        scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    }

    LPARAM value = 1; // repeat count
    value |= static_cast<LPARAM>(scan_code & 0xFFU) << 16;
    return value;
}

} // namespace

bool TargetedUnicodeInputBackend::BeginSession(
    const core::RunSettings&,
    const TargetWindowInfo& target) noexcept {
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

bool TargetedUnicodeInputBackend::Press(
    const core::RunSettings& settings,
    const InputSessionCancellation& cancellation) noexcept {
    if (cancellation.Requested() ||
        settings.action_type != core::ActionType::KeyboardPress ||
        !IsTargetWindowValid(target_) ||
        !DeliveryIsAllowed(settings)) {
        return false;
    }

    const auto character = UnicodeTextCharacterForKey(
        settings.generated_virtual_key, settings.generated_key_modifiers);
    if (!character.has_value()) {
        return false;
    }

    const HWND recipient = ResolveTargetKeyboardRecipient(target_);
    if (recipient == nullptr || cancellation.Requested()) {
        return false;
    }

    return SendCharacter(recipient,
                         *character,
                         settings.generated_virtual_key) &&
           !cancellation.Requested();
}

bool TargetedUnicodeInputBackend::Release(const core::RunSettings&) noexcept {
    // WM_CHAR represents a complete text character rather than a held physical
    // key state. The scheduler may still wait for the configured Down duration
    // before calling Release, but there is no matching character-up message.
    return true;
}

bool TargetedUnicodeInputBackend::ReleaseAll() noexcept {
    ClearSession();
    return true;
}

bool TargetedUnicodeInputBackend::HasTrackedInput() const noexcept {
    return false;
}

bool TargetedUnicodeInputBackend::LastMousePressScreenPoint(ScreenPoint&) const noexcept {
    return false;
}

bool TargetedUnicodeInputBackend::DeliveryIsAllowed(
    const core::RunSettings& settings) const noexcept {
    return settings.allow_background_input || IsTargetWindowForeground(target_);
}

bool TargetedUnicodeInputBackend::SendCharacter(
    const HWND recipient,
    const wchar_t character,
    const std::uint16_t virtual_key) noexcept {
    if (!IsWindowWithinTarget(target_, recipient)) {
        return false;
    }

    DWORD_PTR ignored_result{};
    return SendMessageTimeoutW(
               recipient,
               WM_CHAR,
               static_cast<WPARAM>(character),
               MakeCharacterLParam(virtual_key),
               SMTO_ABORTIFHUNG | SMTO_BLOCK,
               MessageTimeoutMilliseconds,
               &ignored_result) != 0;
}

void TargetedUnicodeInputBackend::ClearSession() noexcept {
    target_ = {};
}

} // namespace vectorclick::win

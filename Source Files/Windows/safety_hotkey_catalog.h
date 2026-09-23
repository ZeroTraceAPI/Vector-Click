#pragma once

#include "Core/settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win {

// Keep this predicate synchronized with the choices exposed by the Start / Stop
// and Emergency Stop fields. Registration is allowed only for bindings that
// VectorClick can display, capture, validate, and save without ambiguity.
[[nodiscard]] constexpr bool IsSupportedSafetyHotkeyBinding(
    const core::HotkeyBinding binding) noexcept {
    if (binding.virtual_key == 0 ||
        (binding.modifiers & ~core::SupportedKeyModifiers) != 0) {
        return false;
    }

    const bool shifted = binding.modifiers == core::KeyModifierShift;
    const std::uint16_t key = binding.virtual_key;

    if ((key >= VK_F1 && key <= VK_F12) ||
        (key >= L'A' && key <= L'Z')) {
        return !shifted;
    }

    // Number-row digits have both unshifted and shifted-symbol entries.
    if (key >= L'0' && key <= L'9') {
        return true;
    }

    switch (key) {
    case VK_OEM_1:
    case VK_OEM_PLUS:
    case VK_OEM_COMMA:
    case VK_OEM_MINUS:
    case VK_OEM_PERIOD:
    case VK_OEM_2:
    case VK_OEM_3:
    case VK_OEM_4:
    case VK_OEM_5:
    case VK_OEM_6:
    case VK_OEM_7:
        return true;

    // Numpad digits and Decimal are deliberately excluded because their
    // global virtual-key identity changes with Num Lock. The four operators
    // below remain stable and are exposed by both safety-hotkey fields.
    case VK_MULTIPLY:
    case VK_ADD:
    case VK_SUBTRACT:
    case VK_DIVIDE:
        return !shifted;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool IsSupportedSafetyHotkeyPair(
    const core::HotkeyBinding start_stop,
    const core::HotkeyBinding emergency) noexcept {
    return IsSupportedSafetyHotkeyBinding(start_stop) &&
           IsSupportedSafetyHotkeyBinding(emergency) &&
           start_stop.virtual_key != emergency.virtual_key;
}

static_assert(IsSupportedSafetyHotkeyBinding({VK_F5, 0}));
static_assert(IsSupportedSafetyHotkeyBinding({L'1', core::KeyModifierShift}));
static_assert(IsSupportedSafetyHotkeyBinding({VK_OEM_MINUS, core::KeyModifierShift}));
static_assert(IsSupportedSafetyHotkeyBinding({VK_DIVIDE, 0}));
static_assert(!IsSupportedSafetyHotkeyBinding({VK_NUMPAD1, 0}));
static_assert(!IsSupportedSafetyHotkeyBinding({VK_DECIMAL, 0}));
static_assert(!IsSupportedSafetyHotkeyBinding({VK_F5, core::KeyModifierShift}));

} // namespace vectorclick::win

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Core/settings.h"

#include <cstdint>
#include <optional>

namespace vectorclick::win {

[[nodiscard]] inline std::optional<wchar_t> CharacterForVirtualKey(
    const std::uint16_t virtual_key,
    const std::uint16_t modifiers) noexcept {
    const bool shifted = (modifiers & core::KeyModifierShift) != 0;
    if (virtual_key >= L'A' && virtual_key <= L'Z') {
        return shifted
                   ? static_cast<wchar_t>(virtual_key)
                   : static_cast<wchar_t>(L'a' + (virtual_key - L'A'));
    }
    if (virtual_key >= L'0' && virtual_key <= L'9') {
        constexpr wchar_t ShiftedNumberRow[] = L")!@#$%^&*(";
        return shifted
                   ? ShiftedNumberRow[virtual_key - L'0']
                   : static_cast<wchar_t>(virtual_key);
    }
    if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9) {
        return static_cast<wchar_t>(L'0' + (virtual_key - VK_NUMPAD0));
    }

    switch (virtual_key) {
    case VK_SPACE: return L' ';
    case VK_RETURN: return L'\r';
    case VK_TAB: return L'\t';
    case VK_BACK: return L'\b';
    case VK_OEM_3: return shifted ? L'~' : L'`';
    case VK_OEM_MINUS: return shifted ? L'_' : L'-';
    case VK_OEM_PLUS: return shifted ? L'+' : L'=';
    case VK_OEM_4: return shifted ? L'{' : L'[';
    case VK_OEM_6: return shifted ? L'}' : L']';
    case VK_OEM_5: return shifted ? L'|' : L'\\';
    case VK_OEM_1: return shifted ? L':' : L';';
    case VK_OEM_7: return shifted ? L'"' : L'\'';
    case VK_OEM_COMMA: return shifted ? L'<' : L',';
    case VK_OEM_PERIOD: return shifted ? L'>' : L'.';
    case VK_OEM_2: return shifted ? L'?' : L'/';
    case VK_MULTIPLY: return L'*';
    case VK_ADD: return L'+';
    case VK_SUBTRACT: return L'-';
    case VK_DECIMAL: return L'.';
    case VK_DIVIDE: return L'/';
    default: return std::nullopt;
    }
}

[[nodiscard]] inline bool IsUnicodeTextCharacter(const wchar_t character) noexcept {
    return character >= static_cast<wchar_t>(0x20) &&
           character != static_cast<wchar_t>(0x7F);
}

[[nodiscard]] inline std::optional<wchar_t> UnicodeTextCharacterForKey(
    const std::uint16_t virtual_key,
    const std::uint16_t modifiers) noexcept {
    const auto character = CharacterForVirtualKey(virtual_key, modifiers);
    if (!character.has_value() || !IsUnicodeTextCharacter(*character)) {
        return std::nullopt;
    }
    return character;
}

} // namespace vectorclick::win

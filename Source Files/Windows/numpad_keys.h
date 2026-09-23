#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <optional>

namespace vectorclick::win::numpad {

// Main navigation keys and their numpad counterparts share scan codes. The
// extended-key bit distinguishes the dedicated navigation cluster from the
// physical numpad. Normalize by physical scan code so capture remains stable
// whether Num Lock is on or off.
[[nodiscard]] inline std::uint16_t NormalizeCapturedVirtualKey(
    const UINT reported_virtual_key,
    const LPARAM key_lparam) noexcept {
    const bool extended =
        (static_cast<ULONG_PTR>(key_lparam) & (static_cast<ULONG_PTR>(1) << 24U)) != 0;
    if (extended) {
        return static_cast<std::uint16_t>(reported_virtual_key);
    }

    const UINT scan_code =
        static_cast<UINT>((static_cast<ULONG_PTR>(key_lparam) >> 16U) & 0xFFU);
    switch (scan_code) {
    case 0x52U: return VK_NUMPAD0;
    case 0x4FU: return VK_NUMPAD1;
    case 0x50U: return VK_NUMPAD2;
    case 0x51U: return VK_NUMPAD3;
    case 0x4BU: return VK_NUMPAD4;
    case 0x4CU: return VK_NUMPAD5;
    case 0x4DU: return VK_NUMPAD6;
    case 0x47U: return VK_NUMPAD7;
    case 0x48U: return VK_NUMPAD8;
    case 0x49U: return VK_NUMPAD9;
    case 0x53U: return VK_DECIMAL;
    default: return static_cast<std::uint16_t>(reported_virtual_key);
    }
}

// With Num Lock enabled, Shift changes these physical numpad keys to their
// navigation meanings. Standard input intentionally preserves physical-key
// behavior, while targeted messages use this mapping explicitly.
[[nodiscard]] inline std::optional<std::uint16_t> ShiftedNavigationVirtualKey(
    const std::uint16_t virtual_key) noexcept {
    switch (virtual_key) {
    case VK_NUMPAD0: return VK_INSERT;
    case VK_NUMPAD1: return VK_END;
    case VK_NUMPAD2: return VK_DOWN;
    case VK_NUMPAD3: return VK_NEXT;
    case VK_NUMPAD4: return VK_LEFT;
    case VK_NUMPAD5: return VK_CLEAR;
    case VK_NUMPAD6: return VK_RIGHT;
    case VK_NUMPAD7: return VK_HOME;
    case VK_NUMPAD8: return VK_UP;
    case VK_NUMPAD9: return VK_PRIOR;
    case VK_DECIMAL: return VK_DELETE;
    default: return std::nullopt;
    }
}

} // namespace vectorclick::win::numpad

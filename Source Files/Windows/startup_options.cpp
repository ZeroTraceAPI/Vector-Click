#include "Windows/startup_options.h"

#include "Windows/app_identity.h"
#include "Core/elevated_restart_settings.h"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace vectorclick::win {
namespace {

// Elevated restart state travels through the command line, so every argument
// parser below is deliberately strict and bounded. Invalid restoration data is
// ignored rather than being allowed to alter normal startup behavior.
constexpr wchar_t HexDigits[] = L"0123456789ABCDEF";

std::wstring EncodeWindowClass(const std::wstring_view value) {
    std::wstring encoded;
    encoded.reserve(value.size() * 4U);
    for (const wchar_t character : value) {
        const auto unit = static_cast<std::uint16_t>(character);
        encoded.push_back(HexDigits[(unit >> 12U) & 0x0FU]);
        encoded.push_back(HexDigits[(unit >> 8U) & 0x0FU]);
        encoded.push_back(HexDigits[(unit >> 4U) & 0x0FU]);
        encoded.push_back(HexDigits[unit & 0x0FU]);
    }
    return encoded;
}

int HexValue(const wchar_t character) noexcept {
    if (character >= L'0' && character <= L'9') return character - L'0';
    if (character >= L'A' && character <= L'F') return character - L'A' + 10;
    if (character >= L'a' && character <= L'f') return character - L'a' + 10;
    return -1;
}

bool DecodeWindowClass(const std::wstring_view encoded, std::wstring& value) {
    value.clear();
    if (encoded.empty() || encoded.size() % 4U != 0U || encoded.size() > 1'024U) {
        return false;
    }

    value.reserve(encoded.size() / 4U);
    for (std::size_t offset = 0; offset < encoded.size(); offset += 4U) {
        std::uint16_t unit{};
        for (std::size_t digit = 0; digit < 4U; ++digit) {
            const int parsed = HexValue(encoded[offset + digit]);
            if (parsed < 0) {
                value.clear();
                return false;
            }
            unit = static_cast<std::uint16_t>((unit << 4U) | static_cast<std::uint16_t>(parsed));
        }
        if (unit == 0) {
            value.clear();
            return false;
        }
        value.push_back(static_cast<wchar_t>(unit));
    }
    return !value.empty();
}

template <typename Unsigned>
bool ParseUnsigned(const std::wstring_view text, const int base, Unsigned& value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    if (text.empty() || (base != 10 && base != 16)) {
        return false;
    }
    for (const wchar_t character : text) {
        const bool valid = base == 10
                               ? character >= L'0' && character <= L'9'
                               : HexValue(character) >= 0;
        if (!valid) {
            return false;
        }
    }

    std::wstring copy(text);
    wchar_t* end{};
    errno = 0;
    const unsigned long long parsed = std::wcstoull(copy.c_str(), &end, base);
    if (errno == ERANGE || end == copy.c_str() || *end != L'\0' ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<Unsigned>::max())) {
        return false;
    }
    value = static_cast<Unsigned>(parsed);
    return true;
}

bool ParsePageArgument(const std::wstring_view argument, int& page_index) noexcept {
    if (!argument.starts_with(ElevatedPageArgumentPrefix)) {
        return false;
    }

    const std::wstring_view value =
        argument.substr(std::size(ElevatedPageArgumentPrefix) - 1U);
    if (value == L"basic") {
        page_index = 0;
        return true;
    }
    if (value == L"advanced") {
        page_index = 1;
        return true;
    }
    if (value == L"about") {
        page_index = 2;
        return true;
    }
    return false;
}

bool ParseTargetArgument(const std::wstring_view argument, TargetWindowIdentity& identity) {
    if (!argument.starts_with(ElevatedTargetArgumentPrefix)) {
        return false;
    }

    const std::wstring_view value = argument.substr(std::size(ElevatedTargetArgumentPrefix) - 1U);
    const std::size_t first = value.find(L',');
    const std::size_t second = first == std::wstring_view::npos
                                   ? std::wstring_view::npos
                                   : value.find(L',', first + 1U);
    const std::size_t third = second == std::wstring_view::npos
                                  ? std::wstring_view::npos
                                  : value.find(L',', second + 1U);
    const std::size_t fourth = third == std::wstring_view::npos
                                   ? std::wstring_view::npos
                                   : value.find(L',', third + 1U);
    if (first == std::wstring_view::npos || second == std::wstring_view::npos ||
        third == std::wstring_view::npos ||
        (fourth != std::wstring_view::npos &&
         value.find(L',', fourth + 1U) != std::wstring_view::npos)) {
        return false;
    }

    const std::array<std::wstring_view, 4> parts{
        value.substr(0, first),
        value.substr(first + 1U, second - first - 1U),
        value.substr(second + 1U, third - second - 1U),
        fourth == std::wstring_view::npos
            ? value.substr(third + 1U)
            : value.substr(third + 1U, fourth - third - 1U),
    };

    TargetWindowIdentity parsed{};
    if (!ParseUnsigned(parts[0], 16, parsed.window_value) ||
        !ParseUnsigned(parts[1], 10, parsed.process_id) ||
        !ParseUnsigned(parts[2], 10, parsed.thread_id) ||
        !DecodeWindowClass(parts[3], parsed.window_class) ||
        parsed.window_value == 0 || parsed.process_id == 0 || parsed.thread_id == 0) {
        return false;
    }

    if (fourth != std::wstring_view::npos) {
        std::uint32_t policy{};
        if (!ParseUnsigned(value.substr(fourth + 1U), 10, policy) || policy > 2U) {
            return false;
        }
        parsed.recovery_policy = static_cast<TargetRecoveryPolicy>(policy);
    }

    identity = std::move(parsed);
    return true;
}

bool ParseProfileArgument(const std::wstring_view argument,
                          std::wstring& profile_id) noexcept {
    if (!argument.starts_with(ElevatedProfileArgumentPrefix)) {
        return false;
    }
    const std::wstring_view value =
        argument.substr(std::size(ElevatedProfileArgumentPrefix) - 1U);
    if (value.size() != 8U) {
        return false;
    }
    std::wstring normalized;
    normalized.reserve(8U);
    for (const wchar_t ch : value) {
        if (ch >= L'0' && ch <= L'9') {
            normalized.push_back(ch);
        } else if (ch >= L'A' && ch <= L'F') {
            normalized.push_back(ch);
        } else if (ch >= L'a' && ch <= L'f') {
            normalized.push_back(static_cast<wchar_t>(ch - L'a' + L'A'));
        } else {
            return false;
        }
    }
    profile_id = std::move(normalized);
    return true;
}

bool ParseSettingsArgument(const std::wstring_view argument,
                           std::optional<core::RunSettings>& settings) noexcept {
    if (!argument.starts_with(ElevatedSettingsArgumentPrefix)) {
        return false;
    }
    const std::wstring_view value =
        argument.substr(std::size(ElevatedSettingsArgumentPrefix) - 1U);
    settings = core::DecodeElevatedRestartSettings(value);
    return true;
}

} // namespace

// Only an explicit elevated-restart marker authorizes the private restoration
// arguments. A normal launch never treats those arguments as persistent state.
StartupOptions ParseStartupOptions() {
    StartupOptions options{};

    int count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        return options;
    }

    for (int index = 1; index < count; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (_wcsicmp(arguments[index], ElevatedRestartArgument) == 0) {
            options.elevated_restart = true;
            continue;
        }

        int page_index{};
        if (ParsePageArgument(argument, page_index)) {
            options.page_to_restore = page_index;
            continue;
        }

        std::optional<core::RunSettings> settings;
        if (ParseSettingsArgument(argument, settings)) {
            if (settings.has_value()) {
                options.settings_to_restore = std::move(settings);
            } else {
                options.settings_restore_invalid = true;
            }
            continue;
        }

        std::wstring profile_id;
        if (ParseProfileArgument(argument, profile_id)) {
            options.profile_id_to_restore = std::move(profile_id);
            continue;
        }

        TargetWindowIdentity target{};
        if (ParseTargetArgument(argument, target)) {
            options.target_to_restore = std::move(target);
        }
    }

    LocalFree(arguments);
    if (!options.elevated_restart) {
        options.target_to_restore.reset();
        options.page_to_restore.reset();
        options.settings_to_restore.reset();
        options.profile_id_to_restore.reset();
        options.settings_restore_invalid = false;
    }
    return options;
}

// Serialize only the state required to reconstruct the current session after
// the user accepts an administrator restart. The matching parser above remains
// the authority for validating the reconstructed values.
std::wstring BuildElevatedRestartArguments(
    const TargetWindowInfo* const target,
    const int selected_page_index,
    const core::RunSettings& settings,
    const std::wstring_view active_profile_id) {
    const std::wstring encoded_settings =
        core::EncodeElevatedRestartSettings(settings);
    if (encoded_settings.empty()) {
        return {};
    }

    std::wstring arguments(ElevatedRestartArgument);
    arguments += L" ";
    arguments += ElevatedPageArgumentPrefix;
    arguments += selected_page_index == 2
                     ? L"about"
                     : selected_page_index == 1 ? L"advanced" : L"basic";
    arguments += L" ";
    arguments += ElevatedSettingsArgumentPrefix;
    arguments += encoded_settings;

    if (!active_profile_id.empty()) {
        if (active_profile_id.size() != 8U ||
            std::any_of(active_profile_id.begin(), active_profile_id.end(), [](const wchar_t ch) {
                return !((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F'));
            })) {
            return {};
        }
        arguments += L" ";
        arguments += ElevatedProfileArgumentPrefix;
        arguments.append(active_profile_id);
    }

    if (target == nullptr || !IsTargetWindowValid(*target)) {
        return arguments;
    }

    const TargetWindowIdentity identity = CaptureTargetWindowIdentity(*target);
    arguments += L" ";
    arguments += ElevatedTargetArgumentPrefix;

    wchar_t window_buffer[2U * sizeof(std::uintptr_t) + 1U]{};
    std::swprintf(window_buffer,
                   std::size(window_buffer),
                   L"%llX",
                   static_cast<unsigned long long>(identity.window_value));
    arguments += window_buffer;
    arguments += L",";
    arguments += std::to_wstring(identity.process_id);
    arguments += L",";
    arguments += std::to_wstring(identity.thread_id);
    arguments += L",";
    arguments += EncodeWindowClass(identity.window_class);
    arguments += L",";
    arguments += std::to_wstring(
        static_cast<std::uint32_t>(identity.recovery_policy));
    return arguments;
}

} // namespace vectorclick::win

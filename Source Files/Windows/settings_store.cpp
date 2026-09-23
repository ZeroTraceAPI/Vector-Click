#include "Windows/settings_store.h"

#include "Core/json_object.h"
#include "Core/validation.h"
#include "Windows/admin_utils.h"
#include "Windows/win32_raii.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <charconv>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vectorclick::win {
namespace {

constexpr std::uintmax_t MaxSettingsBytes = 64 * 1024;
constexpr unsigned int SettingsTemporaryCreateAttempts = 32;

void AppendHex(std::wstring& output, std::uint64_t value) {
    constexpr wchar_t Digits[] = L"0123456789abcdef";
    wchar_t buffer[16]{};
    std::size_t count = 0;
    do {
        buffer[count++] = Digits[value & 0x0FU];
        value >>= 4U;
    } while (value != 0 && count < 16);

    while (count != 0) {
        output.push_back(buffer[--count]);
    }
}

std::wstring SettingsTemporaryPrefix(const std::wstring& final_path) {
    LARGE_INTEGER counter{};
    (void)QueryPerformanceCounter(&counter);

    std::wstring prefix = final_path;
    prefix += L".tmp.";
    AppendHex(prefix, static_cast<std::uint64_t>(GetCurrentProcessId()));
    prefix.push_back(L'.');
    AppendHex(prefix, static_cast<std::uint64_t>(GetCurrentThreadId()));
    prefix.push_back(L'.');
    AppendHex(prefix, static_cast<std::uint64_t>(counter.QuadPart));
    prefix.push_back(L'.');
    return prefix;
}

UniqueHandle CreateSettingsTemporaryFile(const std::wstring& final_path) {
    const std::wstring prefix = SettingsTemporaryPrefix(final_path);
    for (unsigned int attempt = 0; attempt < SettingsTemporaryCreateAttempts; ++attempt) {
        std::wstring temporary_path = prefix;
        AppendHex(temporary_path, attempt);

        UniqueHandle file(CreateFileW(temporary_path.c_str(),
                                      GENERIC_WRITE | DELETE,
                                      0,
                                      nullptr,
                                      CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                      nullptr));
        if (file) {
            return file;
        }

        const DWORD create_error = GetLastError();
        if (create_error != ERROR_FILE_EXISTS &&
            create_error != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    return {};
}

void MarkSettingsTemporaryFileForDeletion(HANDLE file) noexcept {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    (void)SetFileInformationByHandle(
        file, FileDispositionInfo, &disposition, sizeof(disposition));
}

bool ReplaceSettingsFileByHandle(
    HANDLE file,
    const std::wstring& final_path) {
    if (final_path.size() >
        static_cast<std::size_t>(MAXDWORD) / sizeof(wchar_t)) {
        return false;
    }

    const std::size_t file_name_bytes =
        final_path.size() * sizeof(wchar_t);
    const std::size_t buffer_bytes =
        offsetof(FILE_RENAME_INFO, FileName) +
        file_name_bytes + sizeof(wchar_t);
    if (buffer_bytes > static_cast<std::size_t>(MAXDWORD)) {
        return false;
    }

    std::vector<std::byte> buffer(buffer_bytes);
    auto* rename_info =
        reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename_info->ReplaceIfExists = TRUE;
    rename_info->RootDirectory = nullptr;
    rename_info->FileNameLength =
        static_cast<DWORD>(file_name_bytes);
    std::memcpy(
        rename_info->FileName,
        final_path.c_str(),
        file_name_bytes + sizeof(wchar_t));

    return SetFileInformationByHandle(
               file,
               FileRenameInfo,
               rename_info,
               static_cast<DWORD>(buffer.size())) != FALSE;
}

template <typename T>
bool AppendNumber(std::string& output, const T value) {
    static_assert(std::is_integral_v<T>);
    char buffer[32]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        return false;
    }
    output.append(buffer, result.ptr);
    return true;
}

bool ReadAll(HANDLE file, std::string& data) noexcept {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t remaining = data.size() - offset;
        const DWORD request = static_cast<DWORD>(remaining);
        DWORD bytes_read = 0;
        if (ReadFile(file, data.data() + offset, request, &bytes_read, nullptr) == FALSE ||
            bytes_read == 0) {
            return false;
        }
        offset += bytes_read;
    }
    return true;
}

bool WriteAll(HANDLE file, const std::string& data) noexcept {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t remaining = data.size() - offset;
        const DWORD request = static_cast<DWORD>(remaining);
        DWORD written = 0;
        if (WriteFile(file, data.data() + offset, request, &written, nullptr) == FALSE ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

const core::JsonObjectMember* FindValue(
    const core::JsonObject& json,
    const std::string_view key) noexcept {
    return json.Find(key);
}

template <typename T>
std::optional<T> ReadUnsigned(
    const core::JsonObject& json,
    const std::string_view key) {
    const auto* value = FindValue(json, key);
    if (value == nullptr || value->kind != core::JsonValueKind::Number) {
        return std::nullopt;
    }
    T parsed{};
    const auto first = value->text.data();
    const auto last = first + value->text.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return parsed;
}

template <typename T>
std::optional<T> ReadSigned(
    const core::JsonObject& json,
    const std::string_view key) {
    const auto* value = FindValue(json, key);
    if (value == nullptr || value->kind != core::JsonValueKind::Number) {
        return std::nullopt;
    }
    T parsed{};
    const auto first = value->text.data();
    const auto last = first + value->text.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<bool> ReadBool(
    const core::JsonObject& json,
    const std::string_view key) {
    const auto* value = FindValue(json, key);
    if (value == nullptr || value->kind != core::JsonValueKind::Boolean) {
        return std::nullopt;
    }
    return value->text == "true";
}

std::optional<std::string> ReadString(
    const core::JsonObject& json,
    const std::string_view key) {
    const auto* value = FindValue(json, key);
    if (value == nullptr || value->kind != core::JsonValueKind::String) {
        return std::nullopt;
    }
    return value->text;
}

template <typename T>
bool PresentButInvalid(
    const core::JsonObject& json,
    const std::string_view key,
    const std::optional<T>& value) noexcept {
    return FindValue(json, key) != nullptr && !value.has_value();
}

bool HasMalformedDurationComponents(
    const core::JsonObject& json,
    const std::string_view prefix) {
    const std::string minutes_key = std::string(prefix) + "_minutes";
    const std::string seconds_key = std::string(prefix) + "_seconds";
    const std::string milliseconds_key =
        std::string(prefix) + "_milliseconds_microseconds";
    const auto minutes = ReadUnsigned<std::uint32_t>(json, minutes_key);
    const auto seconds = ReadUnsigned<std::uint32_t>(json, seconds_key);
    const auto milliseconds =
        ReadUnsigned<std::uint64_t>(json, milliseconds_key);
    return PresentButInvalid(json, minutes_key, minutes) ||
           PresentButInvalid(json, seconds_key, seconds) ||
           PresentButInvalid(json, milliseconds_key, milliseconds);
}

bool HasMalformedRunTimeLimitComponents(const core::JsonObject& json) {
    const auto hours = ReadUnsigned<std::uint32_t>(
        json, "run_time_limit_hours");
    const auto minutes = ReadUnsigned<std::uint32_t>(
        json, "run_time_limit_minutes");
    const auto seconds = ReadUnsigned<std::uint32_t>(
        json, "run_time_limit_seconds");
    return PresentButInvalid(json, "run_time_limit_hours", hours) ||
           PresentButInvalid(json, "run_time_limit_minutes", minutes) ||
           PresentButInvalid(json, "run_time_limit_seconds", seconds);
}

std::string ActionTypeToken(const core::ActionType action_type) {
    return action_type == core::ActionType::KeyboardPress ? "keyboard" : "mouse";
}

std::optional<core::ActionType> ParseActionType(const std::string& token) {
    if (token == "mouse") return core::ActionType::MouseClick;
    if (token == "keyboard") return core::ActionType::KeyboardPress;
    return std::nullopt;
}

std::string RandomIntervalStyleToken(const core::RandomIntervalStyle style) {
    switch (style) {
    case core::RandomIntervalStyle::Independent: return "independent";
    case core::RandomIntervalStyle::Drifting: return "drifting";
    case core::RandomIntervalStyle::Natural: return "natural";
    }
    return "independent";
}

std::optional<core::RandomIntervalStyle> ParseRandomIntervalStyle(
    const std::string& token) {
    if (token == "independent") return core::RandomIntervalStyle::Independent;
    if (token == "drifting") return core::RandomIntervalStyle::Drifting;
    if (token == "natural") return core::RandomIntervalStyle::Natural;
    return std::nullopt;
}

std::string DownDurationBehaviorToken(const core::DownDurationBehavior behavior) {
    switch (behavior) {
    case core::DownDurationBehavior::Fixed: return "fixed";
    case core::DownDurationBehavior::NaturalConfiguredCenter: return "natural_configured";
    case core::DownDurationBehavior::NaturalAutomaticCenter: return "natural_automatic";
    }
    return "fixed";
}

std::optional<core::DownDurationBehavior> ParseDownDurationBehavior(
    const std::string& token) {
    if (token == "fixed") return core::DownDurationBehavior::Fixed;
    if (token == "natural_configured") {
        return core::DownDurationBehavior::NaturalConfiguredCenter;
    }
    if (token == "natural_automatic") {
        return core::DownDurationBehavior::NaturalAutomaticCenter;
    }
    return std::nullopt;
}

std::string ActionPatternToken(const core::ActionPattern pattern) {
    switch (pattern) {
    case core::ActionPattern::Single: return "single";
    case core::ActionPattern::Double: return "double";
    case core::ActionPattern::Triple: return "triple";
    case core::ActionPattern::Burst: return "burst";
    case core::ActionPattern::Hold: return "hold";
    }
    return "single";
}

std::optional<core::ActionPattern> ParseActionPattern(const std::string& token) {
    if (token == "single") return core::ActionPattern::Single;
    if (token == "double") return core::ActionPattern::Double;
    if (token == "triple") return core::ActionPattern::Triple;
    if (token == "burst") return core::ActionPattern::Burst;
    if (token == "hold") return core::ActionPattern::Hold;
    return std::nullopt;
}

std::string InputBackendToken(const core::InputBackend backend) {
    switch (backend) {
    case core::InputBackend::Automatic: return "automatic";
    case core::InputBackend::StandardInput: return "standard_input";
    case core::InputBackend::ForegroundTargetInput: return "foreground_target_input";
    case core::InputBackend::TargetedWindowMessages: return "targeted_window_messages";
    case core::InputBackend::UnicodeTextInput: return "unicode_text_input";
    case core::InputBackend::TargetedUnicodeText: return "targeted_unicode_text";
    }
    return "automatic";
}

std::optional<core::InputBackend> ParseInputBackend(const std::string& token) {
    if (token == "automatic") return core::InputBackend::Automatic;
    if (token == "standard_input") return core::InputBackend::StandardInput;
    if (token == "foreground_target_input") return core::InputBackend::ForegroundTargetInput;
    if (token == "targeted_window_messages") return core::InputBackend::TargetedWindowMessages;
    if (token == "unicode_text_input") return core::InputBackend::UnicodeTextInput;
    if (token == "targeted_unicode_text") return core::InputBackend::TargetedUnicodeText;
    return std::nullopt;
}

std::string PositionModeToken(const core::PositionMode mode) {
    return mode == core::PositionMode::FixedScreen ? "fixed_screen" : "current_cursor";
}

std::optional<core::PositionMode> ParsePositionMode(const std::string& token) {
    if (token == "current_cursor") return core::PositionMode::CurrentCursor;
    if (token == "fixed_screen") return core::PositionMode::FixedScreen;
    return std::nullopt;
}

std::string MouseButtonToken(const core::MouseButton button) {
    switch (button) {
    case core::MouseButton::Left: return "left";
    case core::MouseButton::Right: return "right";
    case core::MouseButton::Middle: return "middle";
    case core::MouseButton::X1: return "x1";
    case core::MouseButton::X2: return "x2";
    }
    return "left";
}

std::optional<core::MouseButton> ParseMouseButton(const std::string& token) {
    if (token == "left") return core::MouseButton::Left;
    if (token == "right") return core::MouseButton::Right;
    if (token == "middle") return core::MouseButton::Middle;
    if (token == "x1") return core::MouseButton::X1;
    if (token == "x2") return core::MouseButton::X2;
    return std::nullopt;
}

std::string ProcessPriorityModeToken(const core::ProcessPriorityMode mode) {
    switch (mode) {
    case core::ProcessPriorityMode::SystemDefault: return "system_default";
    case core::ProcessPriorityMode::AboveNormalWhileActive: return "above_normal_while_active";
    case core::ProcessPriorityMode::AboveNormal: return "above_normal";
    case core::ProcessPriorityMode::HighWhileActive: return "high_while_active";
    case core::ProcessPriorityMode::High: return "high";
    }
    return "system_default";
}

std::optional<core::ProcessPriorityMode> ParseProcessPriorityMode(
    const std::string& token) {
    if (token == "system_default") return core::ProcessPriorityMode::SystemDefault;
    if (token == "above_normal_while_active") return core::ProcessPriorityMode::AboveNormalWhileActive;
    if (token == "above_normal") return core::ProcessPriorityMode::AboveNormal;
    if (token == "high_while_active") return core::ProcessPriorityMode::HighWhileActive;
    if (token == "high") return core::ProcessPriorityMode::High;
    return std::nullopt;
}

std::string RunFeedbackModeToken(const core::RunFeedbackMode mode) {
    switch (mode) {
    case core::RunFeedbackMode::Off: return "off";
    case core::RunFeedbackMode::Started: return "started";
    case core::RunFeedbackMode::Stopped: return "stopped";
    case core::RunFeedbackMode::StartedAndStopped: return "started_and_stopped";
    }
    return "off";
}

std::optional<core::RunFeedbackMode> ParseRunFeedbackMode(
    const std::string& token) {
    if (token == "off") return core::RunFeedbackMode::Off;
    if (token == "started") return core::RunFeedbackMode::Started;
    if (token == "stopped") return core::RunFeedbackMode::Stopped;
    if (token == "started_and_stopped") return core::RunFeedbackMode::StartedAndStopped;
    return std::nullopt;
}

core::DurationComponents ReadDurationComponents(
    const core::JsonObject& json,
    const std::string_view prefix,
    const std::uint64_t authoritative_microseconds) noexcept {
    core::DurationComponents fallback{};
    if (!core::DecomposeDurationMicroseconds(
            authoritative_microseconds, fallback)) {
        return {};
    }

    const std::string minutes_key = std::string(prefix) + "_minutes";
    const std::string seconds_key = std::string(prefix) + "_seconds";
    const std::string milliseconds_key =
        std::string(prefix) + "_milliseconds_microseconds";
    const auto minutes =
        ReadUnsigned<std::uint32_t>(json, minutes_key);
    const auto seconds =
        ReadUnsigned<std::uint32_t>(json, seconds_key);
    const auto milliseconds =
        ReadUnsigned<std::uint64_t>(json, milliseconds_key);
    if (!minutes || !seconds || !milliseconds) {
        return fallback;
    }

    const core::DurationComponents candidate{
        *minutes, *seconds, *milliseconds};
    return core::DurationComponentsMatch(
               candidate, authoritative_microseconds)
               ? candidate
               : fallback;
}

core::DurationComponents DurationComponentsForSave(
    const core::DurationComponents& requested,
    const std::uint64_t authoritative_microseconds) noexcept {
    if (core::DurationComponentsMatch(
            requested, authoritative_microseconds)) {
        return requested;
    }
    core::DurationComponents fallback{};
    (void)core::DecomposeDurationMicroseconds(
        authoritative_microseconds, fallback);
    return fallback;
}

core::RunTimeLimitComponents ReadRunTimeLimitComponents(
    const core::JsonObject& json,
    const std::uint64_t authoritative_microseconds) noexcept {
    core::RunTimeLimitComponents fallback{};
    if (!core::DecomposeRunTimeLimitMicroseconds(
            authoritative_microseconds, fallback)) {
        return {};
    }

    const auto hours = ReadUnsigned<std::uint32_t>(
        json, "run_time_limit_hours");
    const auto minutes = ReadUnsigned<std::uint32_t>(
        json, "run_time_limit_minutes");
    const auto seconds = ReadUnsigned<std::uint32_t>(
        json, "run_time_limit_seconds");
    if (!hours || !minutes || !seconds) {
        return fallback;
    }

    const core::RunTimeLimitComponents candidate{
        *hours, *minutes, *seconds};
    return core::RunTimeLimitComponentsMatch(
               candidate, authoritative_microseconds)
               ? candidate
               : fallback;
}

core::RunTimeLimitComponents RunTimeLimitComponentsForSave(
    const core::RunTimeLimitComponents& requested,
    const std::uint64_t authoritative_microseconds) noexcept {
    if (core::RunTimeLimitComponentsMatch(
            requested, authoritative_microseconds)) {
        return requested;
    }
    core::RunTimeLimitComponents fallback{};
    (void)core::DecomposeRunTimeLimitMicroseconds(
        authoritative_microseconds, fallback);
    return fallback;
}

} // namespace

SettingsStore::SettingsStore(std::wstring product_name) {
    std::wstring directory = ExecutableDirectory();
    if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/') {
        directory.push_back(L'\\');
    }
    path_ = std::move(directory);
    path_ += std::move(product_name);
    path_ += L" settings.json";
}

bool SettingsStore::Exists() const {
    const DWORD attributes = GetFileAttributesW(path_.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool SettingsStore::Load(core::RunSettings& settings, std::wstring& error) const {
    return LoadFromPath(path_, settings, error);
}

bool SettingsStore::IsCanonicalPath(const std::wstring& path) const noexcept {
    if (path.empty() || path_.empty()) {
        return false;
    }
    return CompareStringOrdinal(
               path_.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool SettingsStore::LoadActiveProfileId(
    std::optional<std::wstring>& profile_id) const {
    profile_id.reset();
    UniqueHandle file(CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return false;
    }
    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file.get(), &file_size) == FALSE || file_size.QuadPart < 0 ||
        static_cast<std::uint64_t>(file_size.QuadPart) > MaxSettingsBytes) {
        return false;
    }
    std::string json_text(static_cast<std::size_t>(file_size.QuadPart), '\0');
    if (!json_text.empty() && !ReadAll(file.get(), json_text)) {
        return false;
    }
    core::JsonObject json;
    if (!json.Parse(json_text)) {
        return false;
    }
    const auto encoded = ReadString(json, "active_profile_id");
    if (!encoded.has_value()) {
        return true;
    }
    if (encoded->size() != 8U) {
        return true;
    }
    std::wstring decoded;
    decoded.reserve(8U);
    for (const char ch : *encoded) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool upper = ch >= 'A' && ch <= 'F';
        const bool lower = ch >= 'a' && ch <= 'f';
        if (!digit && !upper && !lower) {
            return true;
        }
        decoded.push_back(static_cast<wchar_t>(
            lower ? (ch - 'a' + 'A') : ch));
    }
    profile_id = std::move(decoded);
    return true;
}

bool SettingsStore::LoadFromPath(const std::wstring& path,
                                 core::RunSettings& settings,
                                 std::wstring& error) const {
    UniqueHandle file(CreateFileW(path.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
    if (!file) {
        error = L"The settings file could not be opened.";
        return false;
    }

    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file.get(), &file_size) == FALSE || file_size.QuadPart < 0 ||
        static_cast<std::uint64_t>(file_size.QuadPart) > MaxSettingsBytes) {
        error = L"The settings file is unavailable or exceeds the 64 KB safety limit.";
        return false;
    }

    std::string json_text(static_cast<std::size_t>(file_size.QuadPart), '\0');
    if (!json_text.empty() && !ReadAll(file.get(), json_text)) {
        error = L"Reading the settings file failed.";
        return false;
    }

    core::JsonObject json;
    if (!json.Parse(json_text)) {
        error = L"The settings file is not a valid structural JSON object.";
        return false;
    }

    // Settings are loaded structurally rather than through a growing
    // version-migration table. Unknown or retired keys are ignored. Established
    // fields remain required, while newly added modifier fields default safely
    // to unshifted values when an older settings file does not contain them.
    const auto action = ReadString(json, "action_type");
    const auto backend = ReadString(json, "input_backend");
    const auto background = ReadBool(json, "allow_background_input");
    const auto button = ReadString(json, "mouse_button");
    const auto generated_key = ReadUnsigned<std::uint16_t>(json, "generated_virtual_key");
    const auto generated_modifiers =
        ReadUnsigned<std::uint16_t>(json, "generated_key_modifiers");
    const auto position_mode = ReadString(json, "position_mode");
    const auto fixed_x = ReadSigned<std::int32_t>(json, "fixed_x");
    const auto fixed_y = ReadSigned<std::int32_t>(json, "fixed_y");
    const auto indicator = ReadBool(json, "show_click_position_indicator");
    const auto interval = ReadUnsigned<std::uint64_t>(json, "interval_microseconds");
    const auto randomize_interval = ReadBool(json, "randomize_interval");
    const auto random_interval_style = ReadString(json, "random_interval_style");
    const auto minimum_interval =
        ReadUnsigned<std::uint64_t>(json, "minimum_interval_microseconds");
    const auto maximum_interval =
        ReadUnsigned<std::uint64_t>(json, "maximum_interval_microseconds");
    const auto down = ReadUnsigned<std::uint64_t>(json, "button_down_microseconds");
    const auto down_duration_behavior = ReadString(json, "down_duration_behavior");
    const auto action_pattern = ReadString(json, "action_pattern");
    const auto burst_count = ReadUnsigned<std::uint32_t>(json, "burst_count");
    const auto action_spacing = ReadUnsigned<std::uint64_t>(json, "action_spacing_microseconds");
    const auto limited = ReadBool(json, "limited_repeat");
    const auto repeat_count = ReadUnsigned<std::uint64_t>(json, "repeat_count");
    const auto run_time_limit =
        ReadUnsigned<std::uint64_t>(json, "run_time_limit_microseconds");
    const auto start_key = ReadUnsigned<std::uint16_t>(json, "start_stop_virtual_key");
    const auto start_modifiers =
        ReadUnsigned<std::uint16_t>(json, "start_stop_modifiers");
    const auto emergency_key = ReadUnsigned<std::uint16_t>(json, "emergency_virtual_key");
    const auto emergency_modifiers =
        ReadUnsigned<std::uint16_t>(json, "emergency_modifiers");
    const auto windows_notification_mode =
        ReadString(json, "windows_notification_mode");
    const auto system_sound_mode = ReadString(json, "system_sound_mode");
    const auto running_indicator = ReadBool(json, "show_running_indicator");
    const auto diagnostics = ReadBool(json, "enable_live_diagnostics");
    const auto shield = ReadBool(json, "show_safety_shield");
    const auto force_exit_on_emergency =
        ReadBool(json, "force_exit_on_emergency_stop");
    const auto capture_exclusion = ReadBool(json, "hide_from_screen_capture");
    const auto keep_on_top = ReadBool(json, "keep_window_on_top");
    const auto remember = ReadBool(json, "remember_settings");
    const auto process_priority = ReadString(json, "process_priority_mode");

    if (!action || !backend || !background || !button || !generated_key ||
        !position_mode || !fixed_x || !fixed_y || !indicator || !interval ||
        !down || !action_pattern || !burst_count || !action_spacing || !limited ||
        !repeat_count || !start_key || !emergency_key || !diagnostics || !shield ||
        !remember) {
        error = L"The settings file is incomplete or contains invalid current settings.";
        return false;
    }

    const auto parsed_action = ParseActionType(*action);
    const auto parsed_backend = ParseInputBackend(*backend);
    const auto parsed_button = ParseMouseButton(*button);
    const auto parsed_position = ParsePositionMode(*position_mode);
    const auto parsed_pattern = ParseActionPattern(*action_pattern);
    const auto parsed_process_priority = process_priority
        ? ParseProcessPriorityMode(*process_priority)
        : std::optional<core::ProcessPriorityMode>(
              core::ProcessPriorityMode::SystemDefault);
    const auto parsed_windows_notification_mode = windows_notification_mode
        ? ParseRunFeedbackMode(*windows_notification_mode)
        : std::optional<core::RunFeedbackMode>(core::RunFeedbackMode::Off);
    const auto parsed_system_sound_mode = system_sound_mode
        ? ParseRunFeedbackMode(*system_sound_mode)
        : std::optional<core::RunFeedbackMode>(core::RunFeedbackMode::Off);
    const auto parsed_random_interval_style = random_interval_style
        ? ParseRandomIntervalStyle(*random_interval_style)
        : std::optional<core::RandomIntervalStyle>(
              core::RandomIntervalStyle::Independent);
    const auto parsed_down_duration_behavior = down_duration_behavior
        ? ParseDownDurationBehavior(*down_duration_behavior)
        : std::optional<core::DownDurationBehavior>(
              core::DownDurationBehavior::Fixed);
    const bool malformed_optional_scalars =
        PresentButInvalid(json, "generated_key_modifiers", generated_modifiers) ||
        PresentButInvalid(json, "randomize_interval", randomize_interval) ||
        PresentButInvalid(json, "minimum_interval_microseconds", minimum_interval) ||
        PresentButInvalid(json, "maximum_interval_microseconds", maximum_interval) ||
        PresentButInvalid(json, "run_time_limit_microseconds", run_time_limit) ||
        PresentButInvalid(json, "start_stop_modifiers", start_modifiers) ||
        PresentButInvalid(json, "emergency_modifiers", emergency_modifiers) ||
        PresentButInvalid(json, "show_running_indicator", running_indicator) ||
        PresentButInvalid(json, "force_exit_on_emergency_stop", force_exit_on_emergency) ||
        PresentButInvalid(json, "hide_from_screen_capture", capture_exclusion) ||
        PresentButInvalid(json, "keep_window_on_top", keep_on_top);
    const bool malformed_optional_tokens =
        (FindValue(json, "random_interval_style") != nullptr &&
         (!random_interval_style || !parsed_random_interval_style)) ||
        (FindValue(json, "down_duration_behavior") != nullptr &&
         (!down_duration_behavior || !parsed_down_duration_behavior)) ||
        (FindValue(json, "windows_notification_mode") != nullptr &&
         (!windows_notification_mode || !parsed_windows_notification_mode)) ||
        (FindValue(json, "system_sound_mode") != nullptr &&
         (!system_sound_mode || !parsed_system_sound_mode)) ||
        (FindValue(json, "process_priority_mode") != nullptr &&
         (!process_priority || !parsed_process_priority));
    const bool malformed_optional_components =
        HasMalformedDurationComponents(json, "interval") ||
        HasMalformedDurationComponents(json, "minimum_interval") ||
        HasMalformedDurationComponents(json, "maximum_interval") ||
        HasMalformedDurationComponents(json, "button_down") ||
        HasMalformedDurationComponents(json, "action_spacing") ||
        HasMalformedRunTimeLimitComponents(json);
    if (!parsed_action || !parsed_backend || !parsed_button || !parsed_position ||
        !parsed_pattern || !parsed_process_priority || !parsed_random_interval_style ||
        !parsed_down_duration_behavior ||
        !parsed_windows_notification_mode || !parsed_system_sound_mode ||
        malformed_optional_scalars || malformed_optional_tokens ||
        malformed_optional_components) {
        error = L"The settings file contains an unknown or malformed current setting value.";
        return false;
    }

    core::RunSettings candidate = core::DefaultRunSettings();
    candidate.action_type = *parsed_action;
    candidate.backend = *parsed_backend;
    candidate.allow_background_input = *background;
    candidate.mouse_button = *parsed_button;
    candidate.generated_virtual_key = *generated_key;
    // Older settings files may omit generated-key modifiers. Treat a missing
    // value as unshifted so compatible portable settings remain valid.
    candidate.generated_key_modifiers = generated_modifiers.value_or(0);
    candidate.position_mode = *parsed_position;
    candidate.fixed_x = *fixed_x;
    candidate.fixed_y = *fixed_y;
    candidate.show_click_position_indicator = *indicator;
    candidate.interval_microseconds = *interval;
    candidate.interval_components = ReadDurationComponents(
        json, "interval", candidate.interval_microseconds);
    // Older settings files may omit random-interval fields. Keep those files
    // valid and preserve the fixed 10 ms default until the user explicitly
    // enables interval randomization.
    candidate.randomize_interval = randomize_interval.value_or(false);
    candidate.random_interval_style = *parsed_random_interval_style;
    candidate.minimum_interval_microseconds = minimum_interval.value_or(10'000);
    candidate.minimum_interval_components = ReadDurationComponents(
        json, "minimum_interval", candidate.minimum_interval_microseconds);
    candidate.maximum_interval_microseconds = maximum_interval.value_or(100'000);
    candidate.maximum_interval_components = ReadDurationComponents(
        json, "maximum_interval", candidate.maximum_interval_microseconds);
    candidate.button_down_microseconds = *down;
    candidate.button_down_components = ReadDurationComponents(
        json, "button_down", candidate.button_down_microseconds);
    candidate.down_duration_behavior = *parsed_down_duration_behavior;
    candidate.action_pattern = *parsed_pattern;
    candidate.burst_count = *burst_count;
    candidate.action_spacing_microseconds = *action_spacing;
    candidate.action_spacing_components = ReadDurationComponents(
        json, "action_spacing", candidate.action_spacing_microseconds);
    candidate.repeat_mode = *limited ? core::RepeatMode::Limited : core::RepeatMode::Unlimited;
    candidate.repeat_count = *repeat_count;
    // Older settings files have no run-duration limit. Treat the missing
    // optional value as off so existing portable settings remain compatible.
    candidate.run_time_limit_microseconds = run_time_limit.value_or(0U);
    candidate.run_time_limit_components = ReadRunTimeLimitComponents(
        json, candidate.run_time_limit_microseconds);
    candidate.start_stop_hotkey.virtual_key = *start_key;
    candidate.start_stop_hotkey.modifiers = start_modifiers.value_or(0);
    candidate.emergency_hotkey.virtual_key = *emergency_key;
    candidate.emergency_hotkey.modifiers = emergency_modifiers.value_or(0);
    // Older settings files may omit run feedback. Keep all three channels
    // off unless the user explicitly enables them.
    candidate.windows_notification_mode = *parsed_windows_notification_mode;
    candidate.system_sound_mode = *parsed_system_sound_mode;
    candidate.show_running_indicator = running_indicator.value_or(false);
    candidate.enable_live_diagnostics = *diagnostics;
    candidate.show_safety_shield = *shield;
    // Older settings files may omit the optional Emergency Stop force-exit
    // escalation. Keep it off unless explicitly set.
    candidate.force_exit_on_emergency_stop =
        force_exit_on_emergency.value_or(false);
    // Older settings files may omit this privacy option. Preserve backward
    // compatibility by treating a missing key as off.
    candidate.hide_from_screen_capture = capture_exclusion.value_or(false);
    // Older settings files may omit this display option. A missing key safely
    // leaves the main window non-topmost.
    candidate.keep_window_on_top = keep_on_top.value_or(false);
    candidate.remember_settings = *remember;
    // Older settings files may omit process priority. Leave those launches
    // fully under the environment's existing priority.
    candidate.process_priority_mode = *parsed_process_priority;

    const auto issues = core::ValidateRunSettings(candidate);
    if (core::HasErrors(issues)) {
        error = L"The settings file contains values that failed safety validation.";
        return false;
    }

    settings = candidate;
    return true;
}

bool SettingsStore::Save(const core::RunSettings& settings,
                         std::wstring& error,
                         const std::wstring_view active_profile_id) const {
    std::string metadata;
    if (!active_profile_id.empty()) {
        if (active_profile_id.size() != 8U) {
            error = L"The active profile identity is invalid and settings were not saved.";
            return false;
        }
        std::string ascii_id;
        ascii_id.reserve(8U);
        for (const wchar_t ch : active_profile_id) {
            if (!((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F'))) {
                error = L"The active profile identity is invalid and settings were not saved.";
                return false;
            }
            ascii_id.push_back(static_cast<char>(ch));
        }
        metadata = "  \"active_profile_id\": \"" + ascii_id + "\",\n";
    }
    return SaveToPath(path_, settings, error, metadata);
}

bool SettingsStore::SaveToPath(const std::wstring& path,
                               const core::RunSettings& settings,
                               std::wstring& error,
                               const std::string_view leading_json_members) const {
    if (path.empty()) {
        error = L"The settings destination path is empty.";
        return false;
    }

    const auto issues = core::ValidateRunSettings(settings);
    if (core::HasErrors(issues)) {
        error = L"Unsafe or invalid settings were not written to disk.";
        return false;
    }

    std::string json;
    json.reserve(1024);
    const auto append_number = [&](const auto value) {
        if (AppendNumber(json, value)) {
            return true;
        }
        error = L"A numeric setting could not be serialized.";
        return false;
    };
    const core::DurationComponents interval_components =
        DurationComponentsForSave(
            settings.interval_components, settings.interval_microseconds);
    const core::DurationComponents minimum_interval_components =
        DurationComponentsForSave(
            settings.minimum_interval_components,
            settings.minimum_interval_microseconds);
    const core::DurationComponents maximum_interval_components =
        DurationComponentsForSave(
            settings.maximum_interval_components,
            settings.maximum_interval_microseconds);
    const core::DurationComponents button_down_components =
        DurationComponentsForSave(
            settings.button_down_components,
            settings.button_down_microseconds);
    const core::DurationComponents action_spacing_components =
        DurationComponentsForSave(
            settings.action_spacing_components,
            settings.action_spacing_microseconds);
    const core::RunTimeLimitComponents run_time_limit_components =
        RunTimeLimitComponentsForSave(
            settings.run_time_limit_components,
            settings.run_time_limit_microseconds);
    json += "{\n";
    if (!leading_json_members.empty()) {
        json.append(leading_json_members);
    }
    json += "  \"action_type\": \"";
    json += ActionTypeToken(settings.action_type);
    json += "\",\n  \"input_backend\": \"";
    json += InputBackendToken(settings.backend);
    json += "\",\n  \"allow_background_input\": ";
    json += settings.allow_background_input ? "true" : "false";
    json += ",\n  \"mouse_button\": \"";
    json += MouseButtonToken(settings.mouse_button);
    json += "\",\n  \"generated_virtual_key\": ";
    if (!append_number(settings.generated_virtual_key)) return false;
    json += ",\n  \"generated_key_modifiers\": ";
    if (!append_number(settings.generated_key_modifiers)) return false;
    json += ",\n  \"position_mode\": \"";
    json += PositionModeToken(settings.position_mode);
    json += "\",\n  \"fixed_x\": ";
    if (!append_number(settings.fixed_x)) return false;
    json += ",\n  \"fixed_y\": ";
    if (!append_number(settings.fixed_y)) return false;
    json += ",\n  \"show_click_position_indicator\": ";
    json += settings.show_click_position_indicator ? "true" : "false";
    json += ",\n  \"interval_microseconds\": ";
    if (!append_number(settings.interval_microseconds)) return false;
    json += ",\n  \"interval_minutes\": ";
    if (!append_number(interval_components.minutes)) return false;
    json += ",\n  \"interval_seconds\": ";
    if (!append_number(interval_components.seconds)) return false;
    json += ",\n  \"interval_milliseconds_microseconds\": ";
    if (!append_number(interval_components.milliseconds_microseconds)) return false;
    json += ",\n  \"randomize_interval\": ";
    json += settings.randomize_interval ? "true" : "false";
    json += ",\n  \"random_interval_style\": \"";
    json += RandomIntervalStyleToken(settings.random_interval_style);
    json += "\",\n  \"minimum_interval_microseconds\": ";
    if (!append_number(settings.minimum_interval_microseconds)) return false;
    json += ",\n  \"minimum_interval_minutes\": ";
    if (!append_number(minimum_interval_components.minutes)) return false;
    json += ",\n  \"minimum_interval_seconds\": ";
    if (!append_number(minimum_interval_components.seconds)) return false;
    json += ",\n  \"minimum_interval_milliseconds_microseconds\": ";
    if (!append_number(minimum_interval_components.milliseconds_microseconds)) return false;
    json += ",\n  \"maximum_interval_microseconds\": ";
    if (!append_number(settings.maximum_interval_microseconds)) return false;
    json += ",\n  \"maximum_interval_minutes\": ";
    if (!append_number(maximum_interval_components.minutes)) return false;
    json += ",\n  \"maximum_interval_seconds\": ";
    if (!append_number(maximum_interval_components.seconds)) return false;
    json += ",\n  \"maximum_interval_milliseconds_microseconds\": ";
    if (!append_number(maximum_interval_components.milliseconds_microseconds)) return false;
    json += ",\n  \"button_down_microseconds\": ";
    if (!append_number(settings.button_down_microseconds)) return false;
    json += ",\n  \"down_duration_behavior\": \"";
    json += DownDurationBehaviorToken(settings.down_duration_behavior);
    json += "\"";
    json += ",\n  \"button_down_minutes\": ";
    if (!append_number(button_down_components.minutes)) return false;
    json += ",\n  \"button_down_seconds\": ";
    if (!append_number(button_down_components.seconds)) return false;
    json += ",\n  \"button_down_milliseconds_microseconds\": ";
    if (!append_number(button_down_components.milliseconds_microseconds)) return false;
    json += ",\n  \"action_pattern\": \"";
    json += ActionPatternToken(settings.action_pattern);
    json += "\",\n  \"burst_count\": ";
    if (!append_number(settings.burst_count)) return false;
    json += ",\n  \"action_spacing_microseconds\": ";
    if (!append_number(settings.action_spacing_microseconds)) return false;
    json += ",\n  \"action_spacing_minutes\": ";
    if (!append_number(action_spacing_components.minutes)) return false;
    json += ",\n  \"action_spacing_seconds\": ";
    if (!append_number(action_spacing_components.seconds)) return false;
    json += ",\n  \"action_spacing_milliseconds_microseconds\": ";
    if (!append_number(action_spacing_components.milliseconds_microseconds)) return false;
    json += ",\n  \"limited_repeat\": ";
    json += settings.repeat_mode == core::RepeatMode::Limited ? "true" : "false";
    json += ",\n  \"repeat_count\": ";
    if (!append_number(settings.repeat_count)) return false;
    json += ",\n  \"run_time_limit_microseconds\": ";
    if (!append_number(settings.run_time_limit_microseconds)) return false;
    json += ",\n  \"run_time_limit_hours\": ";
    if (!append_number(run_time_limit_components.hours)) return false;
    json += ",\n  \"run_time_limit_minutes\": ";
    if (!append_number(run_time_limit_components.minutes)) return false;
    json += ",\n  \"run_time_limit_seconds\": ";
    if (!append_number(run_time_limit_components.seconds)) return false;
    json += ",\n  \"start_stop_virtual_key\": ";
    if (!append_number(settings.start_stop_hotkey.virtual_key)) return false;
    json += ",\n  \"start_stop_modifiers\": ";
    if (!append_number(settings.start_stop_hotkey.modifiers)) return false;
    json += ",\n  \"emergency_virtual_key\": ";
    if (!append_number(settings.emergency_hotkey.virtual_key)) return false;
    json += ",\n  \"emergency_modifiers\": ";
    if (!append_number(settings.emergency_hotkey.modifiers)) return false;
    json += ",\n  \"windows_notification_mode\": \"";
    json += RunFeedbackModeToken(settings.windows_notification_mode);
    json += "\",\n  \"system_sound_mode\": \"";
    json += RunFeedbackModeToken(settings.system_sound_mode);
    json += "\",\n  \"show_running_indicator\": ";
    json += settings.show_running_indicator ? "true" : "false";
    json += ",\n  \"enable_live_diagnostics\": ";
    json += settings.enable_live_diagnostics ? "true" : "false";
    json += ",\n  \"show_safety_shield\": ";
    json += settings.show_safety_shield ? "true" : "false";
    json += ",\n  \"force_exit_on_emergency_stop\": ";
    json += settings.force_exit_on_emergency_stop ? "true" : "false";
    json += ",\n  \"hide_from_screen_capture\": ";
    json += settings.hide_from_screen_capture ? "true" : "false";
    json += ",\n  \"keep_window_on_top\": ";
    json += settings.keep_window_on_top ? "true" : "false";
    json += ",\n  \"remember_settings\": ";
    json += settings.remember_settings ? "true" : "false";
    json += ",\n  \"process_priority_mode\": \"";
    json += ProcessPriorityModeToken(settings.process_priority_mode);
    json += "\"\n}\n";

    UniqueHandle file = CreateSettingsTemporaryFile(path);
    if (!file) {
        error = L"The portable settings file could not be created beside the application.";
        return false;
    }
    if (!WriteAll(file.get(), json) || FlushFileBuffers(file.get()) == FALSE) {
        error = L"Writing the portable settings file failed.";
        MarkSettingsTemporaryFileForDeletion(file.get());
        return false;
    }

    if (!ReplaceSettingsFileByHandle(file.get(), path)) {
        error = L"The portable settings file could not be replaced safely.";
        MarkSettingsTemporaryFileForDeletion(file.get());
        return false;
    }
    return true;
}

bool SettingsStore::Remove(std::wstring& error) const {
    if (!Exists()) {
        return true;
    }
    if (!DeleteFileW(path_.c_str())) {
        error = L"The portable settings file could not be deleted.";
        return false;
    }
    return true;
}

} // namespace vectorclick::win

#include "Core/elevated_restart_settings.h"

#include "Core/validation.h"

#include <bit>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace vectorclick::core {
namespace {

constexpr std::uint8_t PayloadVersion = 3;
constexpr std::uint8_t PreviousPayloadVersion = 2;
constexpr wchar_t HexDigits[] = L"0123456789ABCDEF";

class HexWriter {
public:
    template <typename Unsigned>
    void AppendUnsigned(const Unsigned value) {
        static_assert(std::is_unsigned_v<Unsigned>);
        constexpr std::size_t digits = sizeof(Unsigned) * 2U;
        for (std::size_t index = 0; index < digits; ++index) {
            const std::size_t shift = (digits - index - 1U) * 4U;
            output_.push_back(HexDigits[(value >> shift) & 0x0FU]);
        }
    }

    void AppendBool(const bool value) {
        AppendUnsigned<std::uint8_t>(value ? 1U : 0U);
    }

    [[nodiscard]] std::wstring Take() && { return std::move(output_); }

private:
    std::wstring output_;
};

int HexValue(const wchar_t character) noexcept {
    if (character >= L'0' && character <= L'9') return character - L'0';
    if (character >= L'A' && character <= L'F') return character - L'A' + 10;
    if (character >= L'a' && character <= L'f') return character - L'a' + 10;
    return -1;
}

class HexReader {
public:
    explicit HexReader(const std::wstring_view input) noexcept : input_(input) {}

    template <typename Unsigned>
    bool ReadUnsigned(Unsigned& value) noexcept {
        static_assert(std::is_unsigned_v<Unsigned>);
        constexpr std::size_t digits = sizeof(Unsigned) * 2U;
        if (offset_ > input_.size() || input_.size() - offset_ < digits) {
            return false;
        }
        Unsigned parsed{};
        for (std::size_t index = 0; index < digits; ++index) {
            const int digit = HexValue(input_[offset_ + index]);
            if (digit < 0) {
                return false;
            }
            parsed = static_cast<Unsigned>(
                (parsed << 4U) | static_cast<Unsigned>(digit));
        }
        offset_ += digits;
        value = parsed;
        return true;
    }

    bool ReadBool(bool& value) noexcept {
        std::uint8_t parsed{};
        if (!ReadUnsigned(parsed) || parsed > 1U) {
            return false;
        }
        value = parsed != 0U;
        return true;
    }

    [[nodiscard]] bool AtEnd() const noexcept { return offset_ == input_.size(); }

private:
    std::wstring_view input_;
    std::size_t offset_{};
};

template <typename Enum>
void AppendEnum(HexWriter& writer, const Enum value) {
    writer.AppendUnsigned<std::uint8_t>(static_cast<std::uint8_t>(value));
}

template <typename Enum>
bool ReadEnum(HexReader& reader, Enum& value) noexcept {
    std::uint8_t parsed{};
    if (!reader.ReadUnsigned(parsed)) {
        return false;
    }
    value = static_cast<Enum>(parsed);
    return true;
}

void AppendDuration(HexWriter& writer,
                    const DurationComponents& components) {
    writer.AppendUnsigned<std::uint32_t>(components.minutes);
    writer.AppendUnsigned<std::uint32_t>(components.seconds);
    writer.AppendUnsigned<std::uint64_t>(components.milliseconds_microseconds);
}

bool ReadDuration(HexReader& reader,
                  DurationComponents& components) noexcept {
    return reader.ReadUnsigned(components.minutes) &&
           reader.ReadUnsigned(components.seconds) &&
           reader.ReadUnsigned(components.milliseconds_microseconds);
}

void AppendRunTime(HexWriter& writer,
                   const RunTimeLimitComponents& components) {
    writer.AppendUnsigned<std::uint32_t>(components.hours);
    writer.AppendUnsigned<std::uint32_t>(components.minutes);
    writer.AppendUnsigned<std::uint32_t>(components.seconds);
}

bool ReadRunTime(HexReader& reader,
                 RunTimeLimitComponents& components) noexcept {
    return reader.ReadUnsigned(components.hours) &&
           reader.ReadUnsigned(components.minutes) &&
           reader.ReadUnsigned(components.seconds);
}

void AppendHotkey(HexWriter& writer, const HotkeyBinding binding) {
    writer.AppendUnsigned<std::uint16_t>(binding.virtual_key);
    writer.AppendUnsigned<std::uint16_t>(binding.modifiers);
}

bool ReadHotkey(HexReader& reader, HotkeyBinding& binding) noexcept {
    return reader.ReadUnsigned(binding.virtual_key) &&
           reader.ReadUnsigned(binding.modifiers);
}

bool ComponentsAreConsistent(const RunSettings& settings) noexcept {
    return DurationComponentsMatch(
               settings.interval_components, settings.interval_microseconds) &&
           DurationComponentsMatch(
               settings.minimum_interval_components,
               settings.minimum_interval_microseconds) &&
           DurationComponentsMatch(
               settings.maximum_interval_components,
               settings.maximum_interval_microseconds) &&
           DurationComponentsMatch(
               settings.button_down_components,
               settings.button_down_microseconds) &&
           DurationComponentsMatch(
               settings.action_spacing_components,
               settings.action_spacing_microseconds) &&
           RunTimeLimitComponentsMatch(
               settings.run_time_limit_components,
               settings.run_time_limit_microseconds);
}

} // namespace

std::wstring EncodeElevatedRestartSettings(const RunSettings& settings) {
    if (HasErrors(ValidateRunSettings(settings)) ||
        !ComponentsAreConsistent(settings)) {
        return {};
    }

    HexWriter writer;
    writer.AppendUnsigned<std::uint8_t>(PayloadVersion);
    AppendEnum(writer, settings.action_type);
    AppendEnum(writer, settings.mouse_button);
    writer.AppendUnsigned<std::uint16_t>(settings.generated_virtual_key);
    writer.AppendUnsigned<std::uint16_t>(settings.generated_key_modifiers);
    AppendEnum(writer, settings.backend);
    AppendEnum(writer, settings.position_mode);
    writer.AppendUnsigned<std::uint32_t>(
        std::bit_cast<std::uint32_t>(settings.fixed_x));
    writer.AppendUnsigned<std::uint32_t>(
        std::bit_cast<std::uint32_t>(settings.fixed_y));

    writer.AppendUnsigned<std::uint64_t>(settings.interval_microseconds);
    AppendDuration(writer, settings.interval_components);
    writer.AppendBool(settings.randomize_interval);
    AppendEnum(writer, settings.random_interval_style);
    writer.AppendUnsigned<std::uint64_t>(settings.minimum_interval_microseconds);
    AppendDuration(writer, settings.minimum_interval_components);
    writer.AppendUnsigned<std::uint64_t>(settings.maximum_interval_microseconds);
    AppendDuration(writer, settings.maximum_interval_components);
    writer.AppendUnsigned<std::uint64_t>(settings.button_down_microseconds);
    AppendDuration(writer, settings.button_down_components);
    AppendEnum(writer, settings.down_duration_behavior);

    AppendEnum(writer, settings.action_pattern);
    writer.AppendUnsigned<std::uint32_t>(settings.burst_count);
    writer.AppendUnsigned<std::uint64_t>(settings.action_spacing_microseconds);
    AppendDuration(writer, settings.action_spacing_components);
    AppendEnum(writer, settings.repeat_mode);
    writer.AppendUnsigned<std::uint64_t>(settings.repeat_count);
    writer.AppendUnsigned<std::uint64_t>(settings.run_time_limit_microseconds);
    AppendRunTime(writer, settings.run_time_limit_components);

    AppendHotkey(writer, settings.start_stop_hotkey);
    AppendHotkey(writer, settings.emergency_hotkey);
    AppendEnum(writer, settings.windows_notification_mode);
    AppendEnum(writer, settings.system_sound_mode);
    writer.AppendBool(settings.show_running_indicator);
    writer.AppendBool(settings.enable_live_diagnostics);
    writer.AppendBool(settings.show_safety_shield);
    writer.AppendBool(settings.force_exit_on_emergency_stop);
    writer.AppendBool(settings.hide_from_screen_capture);
    writer.AppendBool(settings.keep_window_on_top);
    writer.AppendBool(settings.remember_settings);
    writer.AppendBool(settings.allow_background_input);
    writer.AppendBool(settings.show_click_position_indicator);
    AppendEnum(writer, settings.process_priority_mode);
    AppendEnum(writer, settings.timing_worker_priority_mode);
    AppendEnum(writer, settings.hotkey_control_priority_mode);
    AppendEnum(writer, settings.timing_worker_qos_mode);
    return std::move(writer).Take();
}

std::optional<RunSettings> DecodeElevatedRestartSettings(
    const std::wstring_view payload) noexcept {
    // The current format is intentionally tiny. A broad upper bound prevents
    // accidental command-line abuse before any field parsing occurs.
    if (payload.empty() || payload.size() > 1'024U ||
        (payload.size() % 2U) != 0U) {
        return std::nullopt;
    }

    HexReader reader(payload);
    std::uint8_t version{};
    if (!reader.ReadUnsigned(version) ||
        (version != PayloadVersion && version != PreviousPayloadVersion)) {
        return std::nullopt;
    }

    RunSettings settings{};
    std::uint32_t fixed_x_bits{};
    std::uint32_t fixed_y_bits{};
    if (!ReadEnum(reader, settings.action_type) ||
        !ReadEnum(reader, settings.mouse_button) ||
        !reader.ReadUnsigned(settings.generated_virtual_key) ||
        !reader.ReadUnsigned(settings.generated_key_modifiers) ||
        !ReadEnum(reader, settings.backend) ||
        !ReadEnum(reader, settings.position_mode) ||
        !reader.ReadUnsigned(fixed_x_bits) ||
        !reader.ReadUnsigned(fixed_y_bits) ||
        !reader.ReadUnsigned(settings.interval_microseconds) ||
        !ReadDuration(reader, settings.interval_components) ||
        !reader.ReadBool(settings.randomize_interval) ||
        !ReadEnum(reader, settings.random_interval_style) ||
        !reader.ReadUnsigned(settings.minimum_interval_microseconds) ||
        !ReadDuration(reader, settings.minimum_interval_components) ||
        !reader.ReadUnsigned(settings.maximum_interval_microseconds) ||
        !ReadDuration(reader, settings.maximum_interval_components) ||
        !reader.ReadUnsigned(settings.button_down_microseconds) ||
        !ReadDuration(reader, settings.button_down_components)) {
        return std::nullopt;
    }
    if (version >= PayloadVersion) {
        if (!ReadEnum(reader, settings.down_duration_behavior)) {
            return std::nullopt;
        }
    } else {
        settings.down_duration_behavior = DownDurationBehavior::Fixed;
    }
    if (!ReadEnum(reader, settings.action_pattern) ||
        !reader.ReadUnsigned(settings.burst_count) ||
        !reader.ReadUnsigned(settings.action_spacing_microseconds) ||
        !ReadDuration(reader, settings.action_spacing_components) ||
        !ReadEnum(reader, settings.repeat_mode) ||
        !reader.ReadUnsigned(settings.repeat_count) ||
        !reader.ReadUnsigned(settings.run_time_limit_microseconds) ||
        !ReadRunTime(reader, settings.run_time_limit_components) ||
        !ReadHotkey(reader, settings.start_stop_hotkey) ||
        !ReadHotkey(reader, settings.emergency_hotkey) ||
        !ReadEnum(reader, settings.windows_notification_mode) ||
        !ReadEnum(reader, settings.system_sound_mode) ||
        !reader.ReadBool(settings.show_running_indicator) ||
        !reader.ReadBool(settings.enable_live_diagnostics) ||
        !reader.ReadBool(settings.show_safety_shield) ||
        !reader.ReadBool(settings.force_exit_on_emergency_stop) ||
        !reader.ReadBool(settings.hide_from_screen_capture) ||
        !reader.ReadBool(settings.keep_window_on_top) ||
        !reader.ReadBool(settings.remember_settings) ||
        !reader.ReadBool(settings.allow_background_input) ||
        !reader.ReadBool(settings.show_click_position_indicator) ||
        !ReadEnum(reader, settings.process_priority_mode) ||
        !ReadEnum(reader, settings.timing_worker_priority_mode) ||
        !ReadEnum(reader, settings.hotkey_control_priority_mode) ||
        !ReadEnum(reader, settings.timing_worker_qos_mode) ||
        !reader.AtEnd()) {
        return std::nullopt;
    }

    settings.fixed_x = std::bit_cast<std::int32_t>(fixed_x_bits);
    settings.fixed_y = std::bit_cast<std::int32_t>(fixed_y_bits);

    if (!ComponentsAreConsistent(settings) ||
        HasErrors(ValidateRunSettings(settings))) {
        return std::nullopt;
    }
    return settings;
}

} // namespace vectorclick::core

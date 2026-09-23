#include "Core/settings.h"

#include <algorithm>
#include <limits>

namespace vectorclick::core {

RunSettings DefaultRunSettings() noexcept {
    RunSettings settings{};
    settings.action_type = ActionType::MouseClick;
    settings.mouse_button = MouseButton::Left;
    settings.generated_virtual_key = 0x41; // A
    settings.generated_key_modifiers = 0;
    settings.backend = InputBackend::Automatic;
    settings.position_mode = PositionMode::CurrentCursor;
    settings.fixed_x = 0;
    settings.fixed_y = 0;
    settings.interval_microseconds = 10'000;
    settings.interval_components = {0, 0, 10'000};
    settings.randomize_interval = false;
    settings.random_interval_style = RandomIntervalStyle::Independent;
    settings.minimum_interval_microseconds = 10'000;
    settings.minimum_interval_components = {0, 0, 10'000};
    settings.maximum_interval_microseconds = 100'000;
    settings.maximum_interval_components = {0, 0, 100'000};
    settings.button_down_microseconds = 1'000;
    settings.button_down_components = {0, 0, 1'000};
    settings.down_duration_behavior = DownDurationBehavior::Fixed;
    settings.action_pattern = ActionPattern::Single;
    settings.burst_count = 4;
    settings.action_spacing_microseconds = 10'000;
    settings.action_spacing_components = {0, 0, 10'000};
    settings.repeat_mode = RepeatMode::Unlimited;
    settings.repeat_count = 100;
    settings.run_time_limit_microseconds = 0;
    settings.run_time_limit_components = {0, 0, 0};
    settings.start_stop_hotkey = {0x74, 0}; // F5
    settings.emergency_hotkey = {0x77, 0};  // F8
    settings.windows_notification_mode = RunFeedbackMode::Off;
    settings.system_sound_mode = RunFeedbackMode::Off;
    settings.show_running_indicator = false;
    settings.enable_live_diagnostics = false;
    settings.show_safety_shield = false;
    settings.force_exit_on_emergency_stop = false;
    settings.hide_from_screen_capture = false;
    settings.keep_window_on_top = false;
    settings.remember_settings = false;
    settings.allow_background_input = false;
    settings.show_click_position_indicator = false;
    return settings;
}

InputBackend ResolveInputBackend(const InputBackend requested,
                                 const bool has_selected_target,
                                 const bool allow_background_input) noexcept {
    if (requested != InputBackend::Automatic) {
        return requested;
    }
    if (!has_selected_target) {
        return InputBackend::StandardInput;
    }
    return allow_background_input
               ? InputBackend::TargetedWindowMessages
               : InputBackend::ForegroundTargetInput;
}

std::string FormatMilliseconds(const std::uint64_t microseconds) {
    const std::uint64_t whole_milliseconds = microseconds / 1'000;
    const std::uint64_t remainder = microseconds % 1'000;
    if (remainder == 0) {
        return std::to_string(whole_milliseconds);
    }

    std::string fractional = std::to_string(remainder + 1'000).substr(1);
    while (!fractional.empty() && fractional.back() == '0') {
        fractional.pop_back();
    }
    return std::to_string(whole_milliseconds) + "." + fractional;
}

bool ComposeDurationMicroseconds(const DurationComponents& components,
                                 std::uint64_t& total_microseconds) noexcept {
    if (components.minutes > MaximumTimeComponent ||
        components.seconds > MaximumTimeComponent ||
        components.milliseconds_microseconds >
            MaximumMillisecondsComponentMicroseconds) {
        return false;
    }

    const std::uint64_t minutes_microseconds =
        static_cast<std::uint64_t>(components.minutes) *
        MicrosecondsPerMinute;
    const std::uint64_t seconds_microseconds =
        static_cast<std::uint64_t>(components.seconds) *
        MicrosecondsPerSecond;
    if (minutes_microseconds >
        std::numeric_limits<std::uint64_t>::max() - seconds_microseconds) {
        return false;
    }
    const std::uint64_t partial =
        minutes_microseconds + seconds_microseconds;
    if (partial > std::numeric_limits<std::uint64_t>::max() -
                      components.milliseconds_microseconds) {
        return false;
    }
    total_microseconds = partial + components.milliseconds_microseconds;
    return true;
}

bool DecomposeDurationMicroseconds(
    const std::uint64_t total_microseconds,
    DurationComponents& components) noexcept {
    if (total_microseconds > MaximumCombinedDurationMicroseconds) {
        return false;
    }

    std::uint64_t remaining = total_microseconds;
    const std::uint64_t minutes = std::min<std::uint64_t>(
        MaximumTimeComponent, remaining / MicrosecondsPerMinute);
    remaining -= minutes * MicrosecondsPerMinute;
    const std::uint64_t seconds = std::min<std::uint64_t>(
        MaximumTimeComponent, remaining / MicrosecondsPerSecond);
    remaining -= seconds * MicrosecondsPerSecond;
    if (remaining > MaximumMillisecondsComponentMicroseconds) {
        return false;
    }

    components.minutes = static_cast<std::uint32_t>(minutes);
    components.seconds = static_cast<std::uint32_t>(seconds);
    components.milliseconds_microseconds = remaining;
    return true;
}

bool DurationComponentsMatch(const DurationComponents& components,
                             const std::uint64_t total_microseconds) noexcept {
    std::uint64_t composed = 0;
    return ComposeDurationMicroseconds(components, composed) &&
           composed == total_microseconds;
}

bool ComposeRunTimeLimitMicroseconds(
    const RunTimeLimitComponents& components,
    std::uint64_t& total_microseconds) noexcept {
    if (components.hours > MaximumTimeComponent ||
        components.minutes > MaximumTimeComponent ||
        components.seconds > MaximumTimeComponent) {
        return false;
    }

    const std::uint64_t hours_microseconds =
        static_cast<std::uint64_t>(components.hours) * MicrosecondsPerHour;
    const std::uint64_t minutes_microseconds =
        static_cast<std::uint64_t>(components.minutes) * MicrosecondsPerMinute;
    const std::uint64_t seconds_microseconds =
        static_cast<std::uint64_t>(components.seconds) * MicrosecondsPerSecond;
    total_microseconds = hours_microseconds + minutes_microseconds +
                         seconds_microseconds;
    return total_microseconds <= MaximumRunTimeLimitMicroseconds;
}

bool DecomposeRunTimeLimitMicroseconds(
    const std::uint64_t total_microseconds,
    RunTimeLimitComponents& components) noexcept {
    if (total_microseconds > MaximumRunTimeLimitMicroseconds) {
        return false;
    }

    std::uint64_t remaining = total_microseconds;
    const std::uint64_t hours = std::min<std::uint64_t>(
        MaximumTimeComponent, remaining / MicrosecondsPerHour);
    remaining -= hours * MicrosecondsPerHour;
    const std::uint64_t minutes = std::min<std::uint64_t>(
        MaximumTimeComponent, remaining / MicrosecondsPerMinute);
    remaining -= minutes * MicrosecondsPerMinute;
    const std::uint64_t seconds = std::min<std::uint64_t>(
        MaximumTimeComponent, remaining / MicrosecondsPerSecond);
    remaining -= seconds * MicrosecondsPerSecond;
    if (remaining != 0U) {
        return false;
    }

    components.hours = static_cast<std::uint32_t>(hours);
    components.minutes = static_cast<std::uint32_t>(minutes);
    components.seconds = static_cast<std::uint32_t>(seconds);
    return true;
}

bool RunTimeLimitComponentsMatch(
    const RunTimeLimitComponents& components,
    const std::uint64_t total_microseconds) noexcept {
    std::uint64_t composed = 0;
    return ComposeRunTimeLimitMicroseconds(components, composed) &&
           composed == total_microseconds;
}

std::uint32_t InputsPerAction(const RunSettings& settings) noexcept {
    switch (settings.action_pattern) {
    case ActionPattern::Single: return 1;
    case ActionPattern::Double: return 2;
    case ActionPattern::Triple: return 3;
    case ActionPattern::Burst: return settings.burst_count;
    case ActionPattern::Hold: return 1;
    }
    return 0;
}

bool SamePhysicalKey(const HotkeyBinding& left,
                     const HotkeyBinding& right) noexcept {
    return left.virtual_key != 0 && left.virtual_key == right.virtual_key;
}

} // namespace vectorclick::core

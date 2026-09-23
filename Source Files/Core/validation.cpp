#include "Core/validation.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace vectorclick::core {

std::vector<ValidationIssue> ValidateRunSettings(const RunSettings& settings) {
    std::vector<ValidationIssue> issues;

    if (settings.action_type != ActionType::MouseClick &&
        settings.action_type != ActionType::KeyboardPress) {
        issues.push_back({"Select a supported action type."});
    }

    if (settings.mouse_button != MouseButton::Left &&
        settings.mouse_button != MouseButton::Right &&
        settings.mouse_button != MouseButton::Middle &&
        settings.mouse_button != MouseButton::X1 &&
        settings.mouse_button != MouseButton::X2) {
        issues.push_back({"Select a supported mouse button."});
    }

    if (settings.position_mode != PositionMode::CurrentCursor &&
        settings.position_mode != PositionMode::FixedScreen) {
        issues.push_back({"Select a supported position mode."});
    }

    if (settings.repeat_mode != RepeatMode::Unlimited &&
        settings.repeat_mode != RepeatMode::Limited) {
        issues.push_back({"Select a supported repeat mode."});
    }

    if (settings.backend != InputBackend::Automatic &&
        settings.backend != InputBackend::StandardInput &&
        settings.backend != InputBackend::ForegroundTargetInput &&
        settings.backend != InputBackend::TargetedWindowMessages &&
        settings.backend != InputBackend::UnicodeTextInput &&
        settings.backend != InputBackend::TargetedUnicodeText) {
        issues.push_back({"Select a supported input method."});
    }

    const bool unicode_text_backend =
        settings.backend == InputBackend::UnicodeTextInput ||
        settings.backend == InputBackend::TargetedUnicodeText;
    if (unicode_text_backend && settings.action_type != ActionType::KeyboardPress) {
        issues.push_back({"Unicode text input methods are available only for keyboard presses."});
    }

    if (unicode_text_backend && settings.action_pattern == ActionPattern::Hold) {
        issues.push_back({"Unicode text input methods do not support Hold because they send text rather than a physical key state."});
    }

    if (settings.action_pattern != ActionPattern::Single &&
        settings.action_pattern != ActionPattern::Double &&
        settings.action_pattern != ActionPattern::Triple &&
        settings.action_pattern != ActionPattern::Burst &&
        settings.action_pattern != ActionPattern::Hold) {
        issues.push_back({"Select a supported action pattern."});
    }

    if (settings.action_pattern == ActionPattern::Burst &&
        (settings.burst_count < MinimumBurstCount ||
         settings.burst_count > MaximumBurstCount)) {
        issues.push_back({"Burst count must be between 2 and 100."});
    }

    if (settings.random_interval_style != RandomIntervalStyle::Independent &&
        settings.random_interval_style != RandomIntervalStyle::Drifting &&
        settings.random_interval_style != RandomIntervalStyle::Natural) {
        issues.push_back({"Select a supported random interval style."});
    }

    if (settings.down_duration_behavior != DownDurationBehavior::Fixed &&
        settings.down_duration_behavior != DownDurationBehavior::NaturalConfiguredCenter &&
        settings.down_duration_behavior != DownDurationBehavior::NaturalAutomaticCenter) {
        issues.push_back({"Select a supported down duration behavior."});
    }

    if (settings.process_priority_mode != ProcessPriorityMode::SystemDefault &&
        settings.process_priority_mode != ProcessPriorityMode::AboveNormalWhileActive &&
        settings.process_priority_mode != ProcessPriorityMode::AboveNormal &&
        settings.process_priority_mode != ProcessPriorityMode::HighWhileActive &&
        settings.process_priority_mode != ProcessPriorityMode::High) {
        issues.push_back({"Select a supported process priority mode."});
    }

    if (settings.timing_worker_priority_mode != TimingWorkerPriorityMode::SystemDefault &&
        settings.timing_worker_priority_mode != TimingWorkerPriorityMode::AboveNormal &&
        settings.timing_worker_priority_mode != TimingWorkerPriorityMode::Highest) {
        issues.push_back({"Select a supported timing worker priority mode."});
    }

    if (settings.hotkey_control_priority_mode != HotkeyControlPriorityMode::SystemDefault &&
        settings.hotkey_control_priority_mode != HotkeyControlPriorityMode::AboveNormal) {
        issues.push_back({"Select a supported hotkey / control priority mode."});
    }

    if (settings.timing_worker_qos_mode != TimingWorkerQosMode::SystemManaged &&
        settings.timing_worker_qos_mode != TimingWorkerQosMode::HighQoS &&
        settings.timing_worker_qos_mode != TimingWorkerQosMode::EcoQoS) {
        issues.push_back({"Select a supported timing worker QoS mode."});
    }

    const auto valid_feedback_mode = [](const RunFeedbackMode mode) noexcept {
        return mode == RunFeedbackMode::Off ||
               mode == RunFeedbackMode::Started ||
               mode == RunFeedbackMode::Stopped ||
               mode == RunFeedbackMode::StartedAndStopped;
    };
    if (!valid_feedback_mode(settings.windows_notification_mode)) {
        issues.push_back({"Select a supported Windows notification mode."});
    }
    if (!valid_feedback_mode(settings.system_sound_mode)) {
        issues.push_back({"Select a supported system sound mode."});
    }

    // Interaction stage safety envelope:
    // - Above Normal process may use worker +1 or +2.
    // - High process may use only worker +1; +2 would raise the worker to a
    //   base priority of 15 and is deliberately excluded.
    // - Hotkey / control +1 may be combined through High process priority, but
    //   Realtime remains unavailable and is also suppressed at runtime if an
    //   external program establishes it.
    const bool high_process_mode =
        settings.process_priority_mode == ProcessPriorityMode::HighWhileActive ||
        settings.process_priority_mode == ProcessPriorityMode::High;
    if (high_process_mode &&
        settings.timing_worker_priority_mode == TimingWorkerPriorityMode::Highest) {
        issues.push_back({
            "High process priority may use at most an Above Normal (+1) timing worker; Highest (+2) is intentionally blocked."});
    }

    for (const auto& timing :
         std::initializer_list<std::pair<std::uint64_t, const char*>>{
             {settings.interval_microseconds, "Input interval"},
             {settings.minimum_interval_microseconds, "Minimum random interval"},
             {settings.maximum_interval_microseconds, "Maximum random interval"},
             {settings.button_down_microseconds, "Down duration"},
             {settings.action_spacing_microseconds, "Action spacing"},
         }) {
        if (timing.first > MaximumCombinedDurationMicroseconds) {
            issues.push_back({std::string(timing.second) +
                              " exceeds the supported Minutes / Seconds / Milliseconds range."});
        }
    }

    const bool hold_mode = settings.action_pattern == ActionPattern::Hold;
    const bool automatic_down =
        settings.down_duration_behavior == DownDurationBehavior::NaturalAutomaticCenter;
    if (!hold_mode &&
        settings.down_duration_behavior == DownDurationBehavior::NaturalConfiguredCenter &&
        settings.button_down_microseconds == 0U) {
        issues.push_back({
            "Natural configured-center down duration requires a value greater than zero."});
    }

    const std::uint64_t validation_down_microseconds =
        automatic_down ? 0U : settings.button_down_microseconds;

    std::uint64_t minimum_action_interval = settings.interval_microseconds;
    if (!hold_mode && settings.randomize_interval) {
        minimum_action_interval = settings.minimum_interval_microseconds;
        if (settings.minimum_interval_microseconds == 0) {
            issues.push_back({"The minimum random interval must be greater than zero."});
        }
        if (settings.maximum_interval_microseconds <
            settings.minimum_interval_microseconds) {
            issues.push_back({
                "The maximum random interval cannot be less than the minimum random interval."});
        }
    } else if (!hold_mode && settings.interval_microseconds == 0) {
        issues.push_back({"The input interval must be greater than zero."});
    }

    if (!hold_mode &&
        validation_down_microseconds > minimum_action_interval) {
        issues.push_back({
            settings.randomize_interval
                ? "The down duration cannot exceed the minimum random interval."
                : "The down duration cannot exceed the complete input interval."});
    }

    const std::uint32_t inputs_per_action = InputsPerAction(settings);
    if (!hold_mode && inputs_per_action > 1) {
        if (settings.action_spacing_microseconds == 0) {
            issues.push_back({"Multi-input actions require spacing greater than zero."});
        } else if (validation_down_microseconds > settings.action_spacing_microseconds) {
            issues.push_back({
                "The down duration cannot exceed the spacing between inputs in one action."});
        }

        if (settings.action_spacing_microseconds > 0) {
            const std::uint64_t additional_inputs =
                static_cast<std::uint64_t>(inputs_per_action - 1U);
            const bool duration_overflows = additional_inputs >
                (std::numeric_limits<std::uint64_t>::max() -
                 validation_down_microseconds) /
                    settings.action_spacing_microseconds;
            if (duration_overflows) {
                issues.push_back({"The requested multi-input action duration is too large."});
            }
        }
    }

    if (!hold_mode &&
        settings.repeat_mode == RepeatMode::Limited &&
        (settings.repeat_count == 0 ||
         settings.repeat_count > MaximumRepeatCount)) {
        issues.push_back({"Limited repeat count must be a whole number from 1 through 1,000,000."});
    }

    RunTimeLimitComponents run_time_limit_components{};
    if (!DecomposeRunTimeLimitMicroseconds(
            settings.run_time_limit_microseconds, run_time_limit_components)) {
        issues.push_back({"Run time limit must use whole seconds within the supported Hours / Minutes / Seconds range."});
    }

    if (!settings.start_stop_hotkey.IsAssigned()) {
        issues.push_back({"A Start / Stop hotkey is required."});
    }

    if (!settings.emergency_hotkey.IsAssigned()) {
        issues.push_back({"An Emergency Stop hotkey is required."});
    }

    if ((settings.start_stop_hotkey.modifiers & ~SupportedKeyModifiers) != 0 ||
        (settings.emergency_hotkey.modifiers & ~SupportedKeyModifiers) != 0) {
        issues.push_back({"Safety hotkeys use an unsupported modifier combination."});
    }

    if (SamePhysicalKey(settings.start_stop_hotkey, settings.emergency_hotkey)) {
        issues.push_back({"Start / Stop and Emergency Stop must use different physical keys."});
    }

    if ((settings.generated_key_modifiers & ~SupportedKeyModifiers) != 0) {
        issues.push_back({"The generated key uses an unsupported modifier combination."});
    }

    if (settings.action_type == ActionType::KeyboardPress) {
        const HotkeyBinding generated_key{
            settings.generated_virtual_key,
            settings.generated_key_modifiers,
        };
        if (!generated_key.IsAssigned()) {
            issues.push_back({"Select a keyboard key to press."});
        } else {
            // Treat the same base key as reserved even when Shift differs. A
            // generated shifted symbol still submits the underlying physical
            // key, so allowing it to share that key with a safety hotkey would
            // make the safety boundary dependent on RegisterHotKey behavior.
            if (SamePhysicalKey(generated_key, settings.start_stop_hotkey)) {
                issues.push_back({
                    "The generated key conflicts with the Start / Stop safety hotkey."});
            }
            if (SamePhysicalKey(generated_key, settings.emergency_hotkey)) {
                issues.push_back({
                    "The generated key conflicts with the Emergency Stop safety hotkey."});
            }
        }
    }

    return issues;
}

std::optional<HotkeyBinding> FindNextAvailableGeneratedKey(
    const std::vector<HotkeyBinding>& ordered_keys,
    const HotkeyBinding& current_key,
    const HotkeyBinding& start_stop_hotkey,
    const HotkeyBinding& emergency_hotkey) noexcept {
    if (ordered_keys.empty()) {
        return std::nullopt;
    }

    const auto current = std::ranges::find(ordered_keys, current_key);
    const std::size_t first_index =
        current == ordered_keys.end()
            ? 0U
            : (static_cast<std::size_t>(std::distance(ordered_keys.begin(), current)) + 1U) %
                  ordered_keys.size();

    for (std::size_t offset = 0; offset < ordered_keys.size(); ++offset) {
        const HotkeyBinding candidate =
            ordered_keys[(first_index + offset) % ordered_keys.size()];
        if (candidate.IsAssigned() &&
            !SamePhysicalKey(candidate, start_stop_hotkey) &&
            !SamePhysicalKey(candidate, emergency_hotkey)) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool HasErrors(const std::vector<ValidationIssue>& issues) noexcept {
    return !issues.empty();
}

} // namespace vectorclick::core

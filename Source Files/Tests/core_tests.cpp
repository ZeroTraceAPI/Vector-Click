#include "Core/diagnostic_report.h"
#include "Core/elevated_restart_settings.h"
#include "Core/json_object.h"
#include "Core/natural_down_duration.h"
#include "Core/numeric_edit_history.h"
#include "Core/settings.h"
#include "Core/settings_history.h"
#include "Core/timing.h"
#include "Core/validation.h"
#include "Windows/input_backend.h"
#include "Windows/shutdown_policy.h"
#include "Windows/ui_layout.h"
#include "Windows/ui_presentation_state.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::wstring ReferenceFixed(const double value, const int precision) {
    char buffer[64]{};
    const auto result = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, precision);
    if (result.ec != std::errc{}) {
        return {};
    }
    return std::wstring(buffer, result.ptr);
}

} // namespace

int main() {
    using namespace vectorclick::core;

    // Diagnostic report formatting, privacy, and bounded-value behavior.
    DiagnosticReportInput diagnostic_input{};
    diagnostic_input.application_version = L"1.0.0.0";
    diagnostic_input.release_build = true;
    diagnostic_input.environment.windows_version_available = true;
    diagnostic_input.environment.windows_major = 10;
    diagnostic_input.environment.windows_minor = 0;
    diagnostic_input.environment.windows_build = 26100;
    diagnostic_input.environment.native_architecture = DiagnosticArchitecture::X64;
    diagnostic_input.environment.application_architecture = DiagnosticArchitecture::X64;
    diagnostic_input.environment.ui_scaling_percent = 150;
    diagnostic_input.current.engine_state = DiagnosticEngineState::Ready;
    diagnostic_input.current.readiness = DiagnosticReadiness::Ready;
    diagnostic_input.current.settings_available = true;
    diagnostic_input.current.settings = DefaultRunSettings();
    diagnostic_input.current.settings.start_stop_hotkey = {0x74, 0};
    diagnostic_input.current.settings.emergency_hotkey = {0x77, 0};
    diagnostic_input.current.settings.force_exit_on_emergency_stop = true;
    diagnostic_input.current.effective_backend = InputBackend::StandardInput;
    diagnostic_input.current.safety_hotkeys_ready = true;
    diagnostic_input.current.named_profile_active = true;
    diagnostic_input.current.named_profile_modified = true;
    diagnostic_input.run.available = true;
    diagnostic_input.run.settings = diagnostic_input.current.settings;
    diagnostic_input.run.effective_backend = InputBackend::StandardInput;
    diagnostic_input.run.completed_actions = 100;
    diagnostic_input.run.generated_inputs = 100;
    diagnostic_input.run.elapsed_seconds = 1.25;
    diagnostic_input.run.actual_actions_per_second = 80.0;
    diagnostic_input.run.actual_inputs_per_second = 80.0;
    diagnostic_input.run.outcome = DiagnosticRunOutcome::RepeatLimit;

    const std::wstring diagnostic_report = FormatDiagnosticReport(diagnostic_input);
    Expect(diagnostic_report.find(L"Vector Click Diagnostic Report") != std::wstring::npos &&
               diagnostic_report.find(L"Report format: 3") != std::wstring::npos &&
               diagnostic_report.find(L"Windows API-reported version: NT 10.0, build 26100") != std::wstring::npos &&
               diagnostic_report.find(L"Start readiness: Ready") != std::wstring::npos &&
               diagnostic_report.find(L"Force exit on Emergency Stop: Yes") != std::wstring::npos &&
               diagnostic_report.find(L"Named local profile active: Yes") != std::wstring::npos &&
               diagnostic_report.find(L"Named local profile modified: Yes") != std::wstring::npos &&
               diagnostic_report.find(L"Outcome: Repeat limit reached") != std::wstring::npos,
           "The diagnostic report should contain the bounded environment, readiness, and run fields");


    DiagnosticReportInput readiness_input = diagnostic_input;
    readiness_input.current.readiness = DiagnosticReadiness::AdministratorConfirmationRequired;
    Expect(FormatDiagnosticReport(readiness_input).find(
               L"Start readiness: Confirmation required, selected target is elevated") != std::wstring::npos,
           "Diagnostics should distinguish an elevation decision from a hard Start block");
    readiness_input.current.readiness = DiagnosticReadiness::TargetNotForeground;
    Expect(FormatDiagnosticReport(readiness_input).find(
               L"Start readiness: Blocked, selected target is not foreground") != std::wstring::npos,
           "Diagnostics should report a foreground-required target that is not currently foreground");
    readiness_input.current.readiness = DiagnosticReadiness::TargetMinimized;
    Expect(FormatDiagnosticReport(readiness_input).find(
               L"Start readiness: Blocked, selected target is minimized for mouse input") != std::wstring::npos,
           "Diagnostics should report the targeted mouse minimized-window Start block");

    diagnostic_input.environment.wine_detected = true;
    diagnostic_input.environment.wine_version = L"11.0";
    diagnostic_input.environment.host_os = DiagnosticHostOs::Linux;
    const std::wstring wine_report = FormatDiagnosticReport(diagnostic_input);
    Expect(wine_report.find(L"Runtime: Windows compatibility layer") != std::wstring::npos &&
               wine_report.find(L"Compatibility layer: Wine") != std::wstring::npos &&
               wine_report.find(L"Host operating system: Linux") != std::wstring::npos &&
               wine_report.find(L"Windows compatibility version: NT 10.0, build 26100") != std::wstring::npos,
           "Wine diagnostics should distinguish the compatibility runtime from the host operating system");

    DiagnosticReportInput privacy_input = diagnostic_input;
    privacy_input.current.target_required = true;
    privacy_input.current.target_selected = true;
    privacy_input.current.target_available = true;
    privacy_input.current.target_foreground = true;
    privacy_input.current.target_elevation = DiagnosticTargetElevation::Elevated;
    privacy_input.run.target_required = true;
    privacy_input.run.target_selected = true;
    privacy_input.run.target_available = true;
    privacy_input.run.target_foreground_at_start = true;
    privacy_input.run.target_elevation = DiagnosticTargetElevation::Elevated;
    const std::wstring privacy_report = FormatDiagnosticReport(privacy_input);
    for (const wchar_t* forbidden : {
             L"C:\\Users\\VC_PRIVACY_TEST_USERNAME_DO_NOT_COPY",
             L"VC_SECRET_WINDOW_TITLE_983274",
             L"VC_PRIVATE_TEXT_PAYLOAD_62519",
             L"VC_SECRET_PROCESS_NAME_17482"}) {
        Expect(privacy_report.find(forbidden) == std::wstring::npos,
               "The allowlisted diagnostic formatter must not manufacture or expose forbidden identity markers");
    }
    Expect(privacy_report.find(L"Target identity: Intentionally omitted") != std::wstring::npos,
           "Target diagnostics should explicitly state that target identity is omitted");

    privacy_input.current.settings.action_type = ActionType::KeyboardPress;
    privacy_input.current.settings.backend = InputBackend::UnicodeTextInput;
    privacy_input.current.effective_backend = InputBackend::UnicodeTextInput;
    privacy_input.run.settings = privacy_input.current.settings;
    privacy_input.run.effective_backend = InputBackend::UnicodeTextInput;
    const std::wstring unicode_report = FormatDiagnosticReport(privacy_input);
    Expect(unicode_report.find(L"Generated character: Content intentionally omitted") != std::wstring::npos,
           "Unicode diagnostics should never serialize the generated character content");

    DiagnosticReportInput boundary_input = diagnostic_input;
    boundary_input.environment.wine_detected = false;
    boundary_input.environment.wine_version.clear();
    boundary_input.environment.host_os = DiagnosticHostOs::NotApplicable;
    boundary_input.run.elapsed_seconds = std::numeric_limits<double>::infinity();
    boundary_input.run.actual_actions_per_second = std::numeric_limits<double>::quiet_NaN();
    boundary_input.run.actual_inputs_per_second = std::numeric_limits<double>::max();
    const std::wstring boundary_report = FormatDiagnosticReport(boundary_input);
    Expect(boundary_report.find(L"Elapsed: Unavailable s") != std::wstring::npos &&
               boundary_report.find(L"Action rate: Unavailable/s") != std::wstring::npos &&
               boundary_report.find(L"Input rate: Unavailable/s") != std::wstring::npos,
           "Non-finite or unrepresentable diagnostic measurements should degrade to Unavailable");
    Expect(boundary_report.size() < 16'384U,
           "The allowlisted diagnostic report should remain comfortably bounded");

    DiagnosticReportInput no_run_input{};
    no_run_input.application_version = L"1.0.0.0";
    no_run_input.current.engine_state = DiagnosticEngineState::Ready;
    no_run_input.current.readiness = DiagnosticReadiness::SettingsInvalid;
    const std::wstring no_run_report = FormatDiagnosticReport(no_run_input);
    Expect(no_run_report.find(L"Start readiness: Blocked, current settings are invalid") != std::wstring::npos &&
               no_run_report.find(L"Settings details: Not included because the current settings are invalid or unavailable") != std::wstring::npos &&
               no_run_report.find(L"No run has occurred since Vector Click started.") != std::wstring::npos,
           "Diagnostics should accurately represent invalid current settings and an absent run");
    Expect(no_run_report.find(L"Privacy:") == std::wstring::npos &&
               no_run_report.find(L"Generated locally on request") == std::wstring::npos,
           "The copied diagnostic report should not repeat privacy guidance already provided by the UI tooltip");

    // Elevated-restart serialization and strict parser compatibility.
    RunSettings elevated_handoff = DefaultRunSettings();
    elevated_handoff.mouse_button = MouseButton::X2;
    elevated_handoff.backend = InputBackend::TargetedWindowMessages;
    elevated_handoff.position_mode = PositionMode::FixedScreen;
    elevated_handoff.fixed_x = -12345;
    elevated_handoff.fixed_y = 67890;
    elevated_handoff.interval_microseconds = 25'500;
    Expect(DecomposeDurationMicroseconds(
               elevated_handoff.interval_microseconds,
               elevated_handoff.interval_components),
           "Elevated-restart test interval should decompose");
    elevated_handoff.randomize_interval = true;
    elevated_handoff.random_interval_style = RandomIntervalStyle::Natural;
    elevated_handoff.minimum_interval_microseconds = 20'000;
    elevated_handoff.maximum_interval_microseconds = 40'000;
    Expect(DecomposeDurationMicroseconds(
               elevated_handoff.minimum_interval_microseconds,
               elevated_handoff.minimum_interval_components) &&
               DecomposeDurationMicroseconds(
                   elevated_handoff.maximum_interval_microseconds,
                   elevated_handoff.maximum_interval_components),
           "Elevated-restart random interval bounds should decompose");
    elevated_handoff.button_down_microseconds = 5'000;
    Expect(DecomposeDurationMicroseconds(
               elevated_handoff.button_down_microseconds,
               elevated_handoff.button_down_components),
           "Elevated-restart down duration should decompose");
    elevated_handoff.action_pattern = ActionPattern::Double;
    elevated_handoff.action_spacing_microseconds = 10'500;
    Expect(DecomposeDurationMicroseconds(
               elevated_handoff.action_spacing_microseconds,
               elevated_handoff.action_spacing_components),
           "Elevated-restart action spacing should decompose");
    elevated_handoff.repeat_mode = RepeatMode::Limited;
    elevated_handoff.repeat_count = 321;
    elevated_handoff.run_time_limit_microseconds = 9 * MicrosecondsPerSecond;
    Expect(DecomposeRunTimeLimitMicroseconds(
               elevated_handoff.run_time_limit_microseconds,
               elevated_handoff.run_time_limit_components),
           "Elevated-restart runtime limit should decompose");
    elevated_handoff.start_stop_hotkey = {0x75, 0}; // F6
    elevated_handoff.emergency_hotkey = {0x78, 0};  // F9
    elevated_handoff.windows_notification_mode = RunFeedbackMode::StartedAndStopped;
    elevated_handoff.system_sound_mode = RunFeedbackMode::Stopped;
    elevated_handoff.show_running_indicator = true;
    elevated_handoff.enable_live_diagnostics = true;
    elevated_handoff.show_safety_shield = true;
    elevated_handoff.force_exit_on_emergency_stop = true;
    elevated_handoff.hide_from_screen_capture = true;
    elevated_handoff.keep_window_on_top = true;
    elevated_handoff.remember_settings = false;
    elevated_handoff.allow_background_input = true;
    elevated_handoff.show_click_position_indicator = true;
    elevated_handoff.down_duration_behavior =
        DownDurationBehavior::NaturalConfiguredCenter;
    elevated_handoff.process_priority_mode = ProcessPriorityMode::AboveNormal;
    elevated_handoff.timing_worker_priority_mode = TimingWorkerPriorityMode::Highest;
    elevated_handoff.hotkey_control_priority_mode = HotkeyControlPriorityMode::AboveNormal;
    elevated_handoff.timing_worker_qos_mode = TimingWorkerQosMode::HighQoS;

    const std::wstring elevated_payload =
        EncodeElevatedRestartSettings(elevated_handoff);
    const auto elevated_roundtrip =
        DecodeElevatedRestartSettings(elevated_payload);
    Expect(!elevated_payload.empty() && elevated_payload.size() < 1'024U &&
               elevated_roundtrip.has_value() &&
               *elevated_roundtrip == elevated_handoff,
           "The elevated restart payload should preserve every validated live setting without persistence");
    for (const wchar_t character : elevated_payload) {
        Expect((character >= L'0' && character <= L'9') ||
                   (character >= L'A' && character <= L'F'),
               "The elevated restart payload should contain only bounded hexadecimal scalar data");
    }

    std::wstring malformed_elevated_payload = elevated_payload;
    malformed_elevated_payload[0] = L'G';
    Expect(!DecodeElevatedRestartSettings(malformed_elevated_payload).has_value(),
           "Malformed elevated restart hex should be rejected");
    std::wstring unknown_elevated_version = elevated_payload;
    unknown_elevated_version[0] = L'0';
    unknown_elevated_version[1] = L'4';
    Expect(!DecodeElevatedRestartSettings(unknown_elevated_version).has_value(),
           "Unknown elevated restart payload versions should be rejected");
    Expect(!DecodeElevatedRestartSettings(elevated_payload + L"00").has_value(),
           "Trailing elevated restart payload data should be rejected");

    // Payload v2 predates DownDurationBehavior. Remove the one-byte v3 field
    // at its fixed position and confirm the supported previous format still
    // decodes with the compatibility default instead of shifting later fields.
    constexpr std::size_t down_behavior_hex_offset = 230U;
    Expect(elevated_payload.size() > down_behavior_hex_offset + 2U,
           "The v3 elevated payload fixture should contain the Down behavior field");
    std::wstring previous_elevated_payload = elevated_payload;
    previous_elevated_payload[0] = L'0';
    previous_elevated_payload[1] = L'2';
    previous_elevated_payload.erase(down_behavior_hex_offset, 2U);
    const auto previous_elevated_roundtrip =
        DecodeElevatedRestartSettings(previous_elevated_payload);
    RunSettings expected_previous_elevated = elevated_handoff;
    expected_previous_elevated.down_duration_behavior =
        DownDurationBehavior::Fixed;
    Expect(previous_elevated_roundtrip.has_value() &&
               *previous_elevated_roundtrip == expected_previous_elevated,
           "Elevated restart payload v2 should remain readable with Fixed Down duration");
    RunSettings invalid_elevated_handoff = elevated_handoff;
    invalid_elevated_handoff.emergency_hotkey = invalid_elevated_handoff.start_stop_hotkey;
    Expect(EncodeElevatedRestartSettings(invalid_elevated_handoff).empty(),
           "Unsafe settings should never be encoded for an elevated restart");

    Expect(FormatMilliseconds(10'000) == "10",
           "10,000 microseconds should display as 10 ms");
    Expect(FormatMilliseconds(1'000) == "1",
           "1,000 microseconds should display as 1 ms");
    Expect(FormatMilliseconds(500) == "0.5",
           "500 microseconds should display as 0.5 ms");
    Expect(FormatMilliseconds(1) == "0.001",
           "1 microsecond should display as 0.001 ms");
    Expect(FormatMilliseconds(10'500) == "10.5",
           "10,500 microseconds should display as 10.5 ms");

    // JSON structural parsing, settings history, and numeric edit history.
    JsonObject json_object;
    Expect(json_object.Parse(
               R"({"action_type":"mouse","unknown":{"action_type":"keyboard"},"items":[1,true,null]})"),
           "The structural JSON reader should accept valid root objects and ignore nested unknown values");
    const JsonObjectMember* action_member = json_object.Find("action_type");
    Expect(action_member != nullptr &&
               action_member->kind == JsonValueKind::String &&
               action_member->text == "mouse",
           "A recognized root JSON member should retain its decoded string value");
    Expect(json_object.Parse(
               R"({"retired":{"action_type":"keyboard"},"other":true})") &&
               json_object.Find("action_type") == nullptr,
           "A recognized key nested inside an unknown object must not masquerade as a root setting");
    Expect(json_object.Parse(
               R"({"action_\u0074ype":"mouse","escaped":"line\nvalue"})") &&
               json_object.Find("action_type") != nullptr,
           "JSON member-name escapes should decode before root-key lookup");
    Expect(!json_object.Parse(R"({"action_type":"mouse"} trailing)"),
           "Trailing non-whitespace text must invalidate a settings JSON object");
    Expect(!json_object.Parse(R"([{"action_type":"mouse"}])"),
           "The settings JSON root must be an object rather than an array");
    Expect(!json_object.Parse(R"({"value":01})"),
           "JSON numbers with forbidden leading zeroes must be rejected structurally");
    Expect(!json_object.Parse(R"({"action_type":"mouse","action_type":"keyboard"})"),
           "Duplicate root setting names must be rejected instead of creating ambiguous precedence");
    Expect(!json_object.Parse("{\"bad\":\"\\uD800\"}"),
           "Unpaired UTF-16 surrogate escapes must be rejected");

    RunSettings invalid_enum_settings = DefaultRunSettings();
    invalid_enum_settings.action_type = static_cast<ActionType>(0xFFU);
    Expect(HasErrors(ValidateRunSettings(invalid_enum_settings)),
           "Central validation must reject an invalid action-type representation");
    invalid_enum_settings = DefaultRunSettings();
    invalid_enum_settings.mouse_button = static_cast<MouseButton>(0xFFU);
    Expect(HasErrors(ValidateRunSettings(invalid_enum_settings)),
           "Central validation must reject an invalid mouse-button representation");
    invalid_enum_settings = DefaultRunSettings();
    invalid_enum_settings.position_mode = static_cast<PositionMode>(0xFFU);
    Expect(HasErrors(ValidateRunSettings(invalid_enum_settings)),
           "Central validation must reject an invalid position-mode representation");
    invalid_enum_settings = DefaultRunSettings();
    invalid_enum_settings.repeat_mode = static_cast<RepeatMode>(0xFFU);
    Expect(HasErrors(ValidateRunSettings(invalid_enum_settings)),
           "Central validation must reject an invalid repeat-mode representation");

    SettingsHistory history;
    RunSettings history_state = DefaultRunSettings();
    history.Reset(history_state);
    Expect(history.IsInitialized() && !history.CanUndo() && !history.CanRedo(),
           "A reset settings history should start with one current state and no navigation");

    history_state.repeat_mode = RepeatMode::Limited;
    history_state.repeat_count = 10;
    Expect(history.Record(history_state) && history.CanUndo() && !history.CanRedo(),
           "Recording one semantic settings change should create one undo step");
    const RunSettings* undo_target = history.UndoTarget();
    Expect(undo_target != nullptr && undo_target->repeat_mode == RepeatMode::Unlimited,
           "Undo should expose the previous complete semantic settings state");
    Expect(history.CommitUndo() && history.CanRedo(),
           "Undo cursor movement should be committed explicitly after application succeeds");
    const RunSettings* redo_target = history.RedoTarget();
    Expect(redo_target != nullptr && redo_target->repeat_count == 10,
           "Redo should expose the state that was just undone");
    Expect(history.CommitRedo() && !history.CanRedo(),
           "Redo cursor movement should return to the newest committed state");

    Expect(history.CommitUndo(),
           "A second undo should return to the original state for branch testing");
    RunSettings branched_state = DefaultRunSettings();
    branched_state.mouse_button = MouseButton::Right;
    Expect(history.Record(branched_state) && !history.CanRedo(),
           "Recording after Undo must discard the old redo branch");

    RunSettings synchronized_state = branched_state;
    synchronized_state.allow_background_input = true;
    history.ReplaceCurrent(synchronized_state);
    Expect(history.Current() != nullptr && history.Current()->allow_background_input &&
               history.UndoDepth() == 1,
           "Replacing current history state should synchronize external dependent changes without adding a step");

    Expect(history.CommitUndo() && history.CanRedo(),
           "External synchronization branch test should expose one redo step");
    RunSettings externally_adjusted_state = *history.Current();
    externally_adjusted_state.show_click_position_indicator = true;
    history.ReplaceCurrent(externally_adjusted_state);
    Expect(history.Current() != nullptr &&
               history.Current()->show_click_position_indicator &&
               !history.CanRedo() && history.UndoDepth() == 0,
           "A changed external synchronization must discard a stale redo branch without adding an undo step");

    SettingsHistory bounded_history;
    RunSettings bounded_state = DefaultRunSettings();
    bounded_history.Reset(bounded_state);
    for (std::size_t index = 1; index <= MaximumSettingsHistoryEntries + 7U; ++index) {
        bounded_state.repeat_mode = RepeatMode::Limited;
        bounded_state.repeat_count = static_cast<std::uint64_t>(index);
        (void)bounded_history.Record(bounded_state);
    }
    Expect(bounded_history.UndoDepth() == MaximumSettingsHistoryEntries,
           "Settings history must retain at most 24 previous committed changes");
    std::size_t bounded_undo_count = 0;
    while (bounded_history.CommitUndo()) {
        ++bounded_undo_count;
    }
    Expect(bounded_undo_count == MaximumSettingsHistoryEntries &&
               bounded_history.RedoDepth() == MaximumSettingsHistoryEntries,
           "The bounded history must provide exactly 24 Undo / Redo steps after overflow");

    NumericEditHistory numeric_history;
    numeric_history.Begin(L"10");
    Expect(numeric_history.IsActive() &&
               !numeric_history.HasUncommittedChange(L"10"),
           "A focused numeric edit should begin at its committed baseline");
    numeric_history.NoteUserEdit();
    const std::optional<std::wstring> numeric_undo =
        numeric_history.UndoTarget(L"4");
    Expect(numeric_undo.has_value() && *numeric_undo == L"10",
           "Numeric Ctrl+Z should restore the complete pre-edit value instead of an intermediate text state");
    const std::optional<std::wstring> numeric_redo =
        numeric_history.RedoTarget(L"10");
    Expect(numeric_redo.has_value() && *numeric_redo == L"4",
           "Numeric Ctrl+Y should reapply the complete in-progress value");
    Expect(!numeric_history.RedoTarget(L"4").has_value() &&
               numeric_history.HasUncommittedChange(L"4"),
           "Application-level Redo must not jump past an uncommitted numeric edit");
    numeric_history.NoteUserEdit();
    Expect(!numeric_history.RedoTarget(L"10").has_value(),
           "A new direct numeric edit after Undo must discard the old local Redo branch");
    numeric_history.Synchronize(L"8");
    Expect(!numeric_history.HasUncommittedChange(L"8") &&
               !numeric_history.RedoTarget(L"8").has_value(),
           "Application-level settings history should synchronize the focused numeric baseline and clear stale local Redo text");
    numeric_history.End();
    Expect(!numeric_history.IsActive(),
           "Numeric edit history should end when the field loses focus");

    IntervalRandomGenerator random_one(0x123456789ABCDEF0ULL);
    IntervalRandomGenerator random_two(0x123456789ABCDEF0ULL);
    bool random_range_varied = false;
    // Random interval generators, including mouse and keyboard Natural
    // calibration.
    std::uint64_t previous_random_value = 0;
    for (int index = 0; index < 128; ++index) {
        const std::uint64_t first = random_one.NextInclusive(10, 100);
        const std::uint64_t second = random_two.NextInclusive(10, 100);
        Expect(first == second,
               "Equal interval-random seeds must produce equal deterministic test sequences");
        Expect(first >= 10 && first <= 100,
               "Random intervals must remain inside the inclusive configured range");
        if (index > 0 && first != previous_random_value) {
            random_range_varied = true;
        }
        previous_random_value = first;
    }
    Expect(random_range_varied,
           "A non-degenerate random interval range should produce more than one value");
    Expect(random_one.NextInclusive(42, 42) == 42,
           "An equal random interval range must preserve the exact configured value");

    IntervalRandomGenerator drifting_one(0x0FEDCBA987654321ULL);
    IntervalRandomGenerator drifting_two(0x0FEDCBA987654321ULL);
    bool drifting_range_varied = false;
    std::uint64_t previous_drifting_value = 0;
    for (int index = 0; index < 128; ++index) {
        const std::uint64_t first = drifting_one.NextDriftingInclusive(10, 100);
        const std::uint64_t second = drifting_two.NextDriftingInclusive(10, 100);
        Expect(first == second,
               "Equal drifting seeds must produce equal deterministic test sequences");
        Expect(first >= 10 && first <= 100,
               "Drifting intervals must remain inside the inclusive configured range");
        if (index > 0 && first != previous_drifting_value) {
            drifting_range_varied = true;
        }
        previous_drifting_value = first;
    }
    Expect(drifting_range_varied,
           "A non-degenerate drifting range should produce more than one value");
    Expect(drifting_one.NextDriftingInclusive(42, 42) == 42,
           "An equal drifting range must preserve the exact configured value");

    IntervalRandomGenerator natural_one(0x1122334455667788ULL);
    IntervalRandomGenerator natural_two(0x1122334455667788ULL);
    bool natural_range_varied = false;
    std::uint64_t previous_natural_value = 0;
    for (int index = 0; index < 512; ++index) {
        const std::uint64_t first =
            natural_one.NextNaturalInclusive(10'000, 100'000);
        const std::uint64_t second =
            natural_two.NextNaturalInclusive(10'000, 100'000);
        Expect(first == second,
               "Equal natural-variation seeds must produce equal deterministic test sequences");
        Expect(first >= 10'000 && first <= 100'000,
               "Natural-variation intervals must remain inside the inclusive configured range");
        if (index > 0 && first != previous_natural_value) {
            natural_range_varied = true;
        }
        previous_natural_value = first;
    }
    Expect(natural_range_varied,
           "A non-degenerate natural-variation range should produce more than one value");
    Expect(natural_one.NextNaturalInclusive(42, 42) == 42,
           "An equal natural-variation range must preserve the exact configured value");

    constexpr std::array<std::uint64_t, 16> accepted_mouse_natural_sequence{{
        531'284U, 562'673U, 476'415U, 462'289U,
        443'884U, 439'850U, 678'819U, 509'127U,
        514'990U, 624'392U, 496'673U, 446'983U,
        577'395U, 574'711U, 516'260U, 654'778U,
    }};
    IntervalRandomGenerator mouse_natural_regression(0x1122334455667788ULL);
    for (const std::uint64_t expected : accepted_mouse_natural_sequence) {
        Expect(mouse_natural_regression.NextNaturalInclusive(300'000U, 800'000U) ==
                   expected,
               "Keyboard calibration must not change the accepted mouse Natural interval sequence");
    }

    IntervalRandomGenerator keyboard_natural_one(0xA1B2C3D4E5F60718ULL);
    IntervalRandomGenerator keyboard_natural_two(0xA1B2C3D4E5F60718ULL);
    bool keyboard_natural_varied = false;
    std::uint64_t previous_keyboard_natural = 0U;
    for (int index = 0; index < 1'024; ++index) {
        const std::uint64_t first =
            keyboard_natural_one.NextNaturalKeyboardInclusive(150'000U, 300'000U);
        const std::uint64_t second =
            keyboard_natural_two.NextNaturalKeyboardInclusive(150'000U, 300'000U);
        Expect(first == second,
               "Equal keyboard Natural seeds must produce equal deterministic sequences");
        Expect(first >= 150'000U && first <= 300'000U,
               "Keyboard Natural intervals must remain inside the inclusive configured range");
        if (index > 0 && first != previous_keyboard_natural) {
            keyboard_natural_varied = true;
        }
        previous_keyboard_natural = first;
    }
    Expect(keyboard_natural_varied,
           "Keyboard Natural timing should vary inside an ordinary interval range");
    Expect(keyboard_natural_one.NextNaturalKeyboardInclusive(42U, 42U) == 42U,
           "An equal keyboard Natural range must preserve the configured value");

    IntervalRandomGenerator narrow_natural(0x8877665544332211ULL);
    for (int index = 0; index < 512; ++index) {
        const std::uint64_t value = narrow_natural.NextNaturalInclusive(100'000, 101'000);
        Expect(value >= 100'000 && value <= 101'000,
               "Natural variation must remain bounded even for a very narrow interval range");
    }

    IntervalRandomGenerator slow_natural(0x13579BDF2468ACE0ULL);
    for (int index = 0; index < 1'024; ++index) {
        const std::uint64_t value =
            slow_natural.NextNaturalInclusive(500'000, 1'000'000);
        Expect(value >= 500'000 && value <= 1'000'000,
               "Natural variation must remain bounded after slow-tempo scaling");
    }

    IntervalRandomGenerator reseeded_natural(0xCAFEBABE12345678ULL);
    for (int index = 0; index < 256; ++index) {
        (void)reseeded_natural.NextNaturalInclusive(300'000, 800'000);
    }
    reseeded_natural.Reseed(0xCAFEBABE12345678ULL);
    IntervalRandomGenerator fresh_natural(0xCAFEBABE12345678ULL);
    for (int index = 0; index < 512; ++index) {
        Expect(
            reseeded_natural.NextNaturalInclusive(300'000, 800'000) ==
                fresh_natural.NextNaturalInclusive(300'000, 800'000),
            "Reseeding must reset every Natural Variation pace component");
    }

    Expect(NaturalDownDurationGenerator::AutomaticBaselineCenter(100'000) ==
               46'000 &&
               NaturalDownDurationGenerator::AutomaticBaselineCenter(200'000) ==
                   97'000 &&
               NaturalDownDurationGenerator::AutomaticBaselineCenter(400'000) ==
                   154'000 &&
               NaturalDownDurationGenerator::AutomaticBaselineCenter(800'000) ==
                   160'000,
           "Natural Down automatic centers should follow the accepted saturating pace curve");

    Expect(NaturalDownDurationGenerator::KeyboardAutomaticBaselineCenter(100'000U) ==
               45'000U &&
               NaturalDownDurationGenerator::KeyboardAutomaticBaselineCenter(200'000U) ==
                   87'000U &&
               NaturalDownDurationGenerator::KeyboardAutomaticBaselineCenter(300'000U) ==
                   122'000U &&
               NaturalDownDurationGenerator::KeyboardAutomaticBaselineCenter(800'000U) ==
                   130'000U,
           "Keyboard Natural Down automatic centers should follow the calibrated saturating pace curve");

    // Natural Down Duration determinism, bounds, input calibration, and
    // edge cases.
    NaturalDownDurationGenerator natural_down_one(0x1234ABCD9876FEDCULL);
    NaturalDownDurationGenerator natural_down_two(0x1234ABCD9876FEDCULL);
    bool natural_down_varied = false;
    std::uint64_t previous_down = 0;
    for (int index = 0; index < 1'024; ++index) {
        const std::uint64_t first = natural_down_one.NextAutomatic(
            500'000, 550'000, 500'000);
        const std::uint64_t second = natural_down_two.NextAutomatic(
            500'000, 550'000, 500'000);
        Expect(first == second,
               "Equal Natural Down seeds must produce equal deterministic sequences");
        Expect(first >= 1 && first <= 500'000,
               "Natural Down durations must remain inside the immediate timing envelope");
        if (index > 0 && first != previous_down) {
            natural_down_varied = true;
        }
        previous_down = first;
    }
    Expect(natural_down_varied,
           "Natural Down should vary inside an ordinary timing envelope");

    constexpr std::array<std::uint64_t, 16> accepted_mouse_down_sequence{{
        157'209U, 106'088U, 156'902U, 143'889U,
        178'597U, 153'817U, 139'910U, 152'414U,
        175'841U, 121'466U, 171'054U, 146'651U,
        149'038U, 176'839U, 150'390U, 141'357U,
    }};
    NaturalDownDurationGenerator mouse_down_regression(0x0123456789ABCDEFULL);
    for (const std::uint64_t expected : accepted_mouse_down_sequence) {
        Expect(mouse_down_regression.NextConfigured(
                   154'000U, 500'000U, 550'000U, 500'000U) == expected,
               "Keyboard calibration must not change the accepted mouse Natural Down sequence");
    }

    NaturalDownDurationGenerator keyboard_down_one(0xBADC0FFEE0DDF00DULL);
    NaturalDownDurationGenerator keyboard_down_two(0xBADC0FFEE0DDF00DULL);
    bool keyboard_down_varied = false;
    std::uint64_t previous_keyboard_down = 0U;
    for (int index = 0; index < 1'024; ++index) {
        const std::uint64_t first = keyboard_down_one.NextAutomaticKeyboard(
            400'000U, 350'000U, 340'000U);
        const std::uint64_t second = keyboard_down_two.NextAutomaticKeyboard(
            400'000U, 350'000U, 340'000U);
        Expect(first == second,
               "Equal keyboard Natural Down seeds must produce equal deterministic sequences");
        Expect(first >= 1U && first <= 400'000U,
               "Keyboard Natural Down durations must remain inside the timing envelope");
        if (index > 0 && first != previous_keyboard_down) {
            keyboard_down_varied = true;
        }
        previous_keyboard_down = first;
    }
    Expect(keyboard_down_varied,
           "Keyboard Natural Down should vary inside an ordinary timing envelope");

    NaturalDownDurationGenerator configured_edge(0xA55AA55AA55AA55AULL);
    for (int index = 0; index < 512; ++index) {
        const std::uint64_t value = configured_edge.NextConfigured(
            199'000, 200'000, 200'000, 200'000);
        Expect(value >= 1 && value <= 200'000,
               "Configured-center Natural Down must remain bounded near a hard timing limit");
    }

    NaturalDownDurationGenerator configured_exact_limit(0xB16B00B512345678ULL);
    for (int index = 0; index < 64; ++index) {
        Expect(configured_exact_limit.NextConfigured(
                   200'000U, 200'000U, 200'000U, 200'000U) == 200'000U,
               "Configured-center Natural Down must fully attenuate variation when no symmetric timing room exists");
    }

    NaturalDownDurationGenerator maximum_duration_down(0xF00DBAAD12345678ULL);
    for (int index = 0; index < 512; ++index) {
        const std::uint64_t value = maximum_duration_down.NextConfigured(
            MaximumCombinedDurationMicroseconds - 1'000U,
            MaximumCombinedDurationMicroseconds,
            MaximumCombinedDurationMicroseconds,
            MaximumCombinedDurationMicroseconds);
        Expect(value >= 1U && value <= MaximumCombinedDurationMicroseconds,
               "Natural Down boundary attenuation must remain overflow-safe at the maximum supported duration");
    }
    NaturalDownDurationGenerator minimum_envelope_down(0x0BADF00D11223344ULL);
    for (int index = 0; index < 64; ++index) {
        Expect(minimum_envelope_down.NextAutomatic(1U, 1U, 1U) == 1U,
               "Natural Down automatic mode must remain valid in a one-microsecond timing envelope");
    }

    NaturalDownDurationGenerator minimum_configured_down(0x13579BDF2468ACE0ULL);
    for (int index = 0; index < 64; ++index) {
        Expect(minimum_configured_down.NextConfigured(
                   1U, 1U, 100'000U, 1U) == 1U,
               "Natural Down configured-center mode must not quantize a positive one-microsecond center to zero");
    }
    NaturalDownDurationGenerator minimum_keyboard_configured_down(
        0x2468ACE013579BDFULL);
    for (int index = 0; index < 64; ++index) {
        Expect(minimum_keyboard_configured_down.NextConfiguredKeyboard(
                   1U, 1U, 100'000U, 1U) == 1U,
               "Keyboard Natural Down configured-center mode must not quantize a positive one-microsecond center to zero");
    }

    NaturalDownDurationGenerator reseeded_down(0x0102030405060708ULL);
    for (int index = 0; index < 128; ++index) {
        (void)reseeded_down.NextAutomatic(400'000, 550'000, 500'000);
    }
    reseeded_down.Reseed(0x0102030405060708ULL);
    NaturalDownDurationGenerator fresh_down(0x0102030405060708ULL);
    for (int index = 0; index < 512; ++index) {
        Expect(reseeded_down.NextAutomatic(400'000, 550'000, 500'000) ==
                   fresh_down.NextAutomatic(400'000, 550'000, 500'000),
               "Reseeding must reset every Natural Down pace component");
    }

    IntervalRandomGenerator interval_with_down(0x89ABCDEF01234567ULL);
    IntervalRandomGenerator interval_without_down(0x89ABCDEF01234567ULL);
    NaturalDownDurationGenerator isolated_down(0x76543210FEDCBA98ULL);
    for (int index = 0; index < 1'024; ++index) {
        const std::uint64_t interval_a =
            interval_with_down.NextNaturalInclusive(300'000, 800'000);
        (void)isolated_down.NextAutomatic(
            interval_a, 550'000, interval_a);
        const std::uint64_t interval_b =
            interval_without_down.NextNaturalInclusive(300'000, 800'000);
        Expect(interval_a == interval_b,
               "Natural Down generation must not perturb the established Natural interval sequence");
    }

    auto five_second_action_count = [](const std::uint64_t seed,
                                       const bool drifting) {
        IntervalRandomGenerator generator(seed);
        std::uint64_t elapsed = 0;
        std::uint64_t actions = 1;
        while (elapsed <= 5'000'000) {
            elapsed += drifting
                ? generator.NextDriftingInclusive(10'000, 100'000)
                : generator.NextInclusive(10'000, 100'000);
            if (elapsed <= 5'000'000) {
                ++actions;
            }
        }
        return actions;
    };
    std::uint64_t independent_minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t independent_maximum = 0;
    std::uint64_t drifting_minimum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t drifting_maximum = 0;
    for (std::uint64_t seed_index = 1; seed_index <= 128; ++seed_index) {
        const std::uint64_t seed = seed_index * 0x9E3779B97F4A7C15ULL;
        const std::uint64_t independent_count =
            five_second_action_count(seed, false);
        const std::uint64_t drifting_count = five_second_action_count(seed, true);
        independent_minimum = std::min(independent_minimum, independent_count);
        independent_maximum = std::max(independent_maximum, independent_count);
        drifting_minimum = std::min(drifting_minimum, drifting_count);
        drifting_maximum = std::max(drifting_maximum, drifting_count);
    }
    Expect(drifting_maximum - drifting_minimum >= 80 &&
               drifting_minimum < independent_minimum &&
               drifting_maximum > independent_maximum,
           "Drifting style should create pronounced bounded short-run count variation");

    Expect(ScheduledInputDeadline(100, 10, 10, 0, 0) == 100,
           "The first input of the first action should use the session start");
    Expect(ScheduledInputDeadline(100, 10, 10, 0, 1) == 110,
           "Action spacing should offset later inputs inside one action");
    Expect(ScheduledInputDeadline(100, 10, 10, 1, 0) == 110,
           "Input interval should start the next action independently");
    Expect(ScheduledInputDeadline(100, 10, 10, 2, 2) == 140,
           "Overlapping action streams should retain both interval and spacing offsets");
    Expect(ScheduledReleaseDeadline(100, 10) == 110,
           "Release timing should remain anchored to the scheduled press phase");
    Expect(ScheduledReleaseDeadline(
               std::numeric_limits<std::int64_t>::max() - 2, 10) ==
               std::numeric_limits<std::int64_t>::max(),
           "Scheduled release timing should saturate instead of overflowing");
    Expect(FirstUnlimitedActionToKeep(219, 100, 10, 20, 100) == 0,
           "No complete action should be skipped while its final input is within the lag window");
    Expect(FirstUnlimitedActionToKeep(250, 100, 10, 20, 100) == 3,
           "Only whole actions whose final input is older than the lag window should be skipped");
    Expect(FirstUnlimitedActionToKeep(251, 100, 10, 20, 100) == 4,
           "A partial interval beyond the lag boundary should advance to the next whole action");
    Expect(SaturatingAddTime(std::numeric_limits<std::int64_t>::max() - 2, 10) ==
               std::numeric_limits<std::int64_t>::max(),
           "Deadline addition should saturate instead of overflowing");
    Expect(SaturatingMultiplyTime(std::numeric_limits<std::int64_t>::max(), 2) ==
               std::numeric_limits<std::int64_t>::max(),
           "Deadline multiplication should saturate instead of overflowing");
    Expect(MaximumGeneratedInputsPerSecond == 10'000,
           "The runtime safety ceiling should remain explicit and test-visible");
    Expect(!DefaultRunSettings().force_exit_on_emergency_stop,
           "Force exit on Emergency Stop must remain off by default");
    Expect(!DefaultRunSettings().hide_from_screen_capture,
           "Screen-capture hiding must remain off by default");
    Expect(!DefaultRunSettings().keep_window_on_top,
           "Keep on top must remain off by default");
    Expect(!DefaultRunSettings().randomize_interval,
           "Random input intervals must remain off by default");
    Expect(DefaultRunSettings().random_interval_style ==
               RandomIntervalStyle::Independent,
           "Independent random intervals must remain the compatibility default");
    Expect(DefaultRunSettings().down_duration_behavior ==
               DownDurationBehavior::Fixed,
           "Fixed Down duration must remain the compatibility default");
    Expect(DefaultRunSettings().process_priority_mode ==
               ProcessPriorityMode::SystemDefault,
           "Process priority must remain system-managed by default");
    Expect(DefaultRunSettings().timing_worker_priority_mode ==
               TimingWorkerPriorityMode::SystemDefault,
           "Timing worker priority must remain system-managed by default");
    Expect(DefaultRunSettings().hotkey_control_priority_mode ==
               HotkeyControlPriorityMode::SystemDefault,
           "Hotkey / control priority must remain system-managed by default");
    Expect(DefaultRunSettings().timing_worker_qos_mode ==
               TimingWorkerQosMode::SystemManaged,
           "Timing worker QoS must remain Windows-managed by default");
    Expect(DefaultRunSettings().windows_notification_mode == RunFeedbackMode::Off &&
               DefaultRunSettings().system_sound_mode == RunFeedbackMode::Off &&
               !DefaultRunSettings().show_running_indicator,
           "Run notifications, sounds, and the active indicator must remain off by default");
    Expect(!RunFeedbackIncludesStarted(RunFeedbackMode::Off) &&
               RunFeedbackIncludesStarted(RunFeedbackMode::Started) &&
               !RunFeedbackIncludesStarted(RunFeedbackMode::Stopped) &&
               RunFeedbackIncludesStarted(RunFeedbackMode::StartedAndStopped) &&
               !RunFeedbackIncludesStopped(RunFeedbackMode::Off) &&
               !RunFeedbackIncludesStopped(RunFeedbackMode::Started) &&
               RunFeedbackIncludesStopped(RunFeedbackMode::Stopped) &&
               RunFeedbackIncludesStopped(RunFeedbackMode::StartedAndStopped),
           "Run feedback modes must select start and stop events independently");
    // Settings validation and duration decomposition boundaries.
    RunSettings natural_random_style = DefaultRunSettings();
    natural_random_style.randomize_interval = true;
    natural_random_style.random_interval_style = RandomIntervalStyle::Natural;
    Expect(!HasErrors(ValidateRunSettings(natural_random_style)),
           "Natural variation must be accepted as a supported random interval style");

    RunSettings natural_down_settings = DefaultRunSettings();
    natural_down_settings.down_duration_behavior =
        DownDurationBehavior::NaturalConfiguredCenter;
    Expect(!HasErrors(ValidateRunSettings(natural_down_settings)),
           "Natural configured-center Down duration must be valid for mouse clicks");
    natural_down_settings.down_duration_behavior =
        DownDurationBehavior::NaturalAutomaticCenter;
    natural_down_settings.button_down_microseconds = 500'000;
    natural_down_settings.button_down_components = {0, 0, 500'000};
    Expect(!HasErrors(ValidateRunSettings(natural_down_settings)),
           "Natural automatic-center Down duration must not be rejected by the disabled configured value");
    natural_down_settings.action_type = ActionType::KeyboardPress;
    Expect(!HasErrors(ValidateRunSettings(natural_down_settings)),
           "Natural Down duration must support keyboard presses as well as mouse clicks");
    natural_down_settings.down_duration_behavior =
        DownDurationBehavior::NaturalConfiguredCenter;
    natural_down_settings.button_down_microseconds = 1'000;
    natural_down_settings.button_down_components = {0, 0, 1'000};
    Expect(!HasErrors(ValidateRunSettings(natural_down_settings)),
           "Natural configured-center Down duration must support keyboard presses");

    RunSettings invalid_down_behavior = DefaultRunSettings();
    invalid_down_behavior.down_duration_behavior =
        static_cast<DownDurationBehavior>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_down_behavior)),
           "Unknown Down duration behaviors must fail settings validation");

    RunSettings invalid_priority = DefaultRunSettings();
    invalid_priority.process_priority_mode =
        static_cast<ProcessPriorityMode>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_priority)),
           "Unknown process priority modes must fail settings validation");
    RunSettings invalid_worker_priority = DefaultRunSettings();
    invalid_worker_priority.timing_worker_priority_mode =
        static_cast<TimingWorkerPriorityMode>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_worker_priority)),
           "Unknown timing worker priority modes must fail settings validation");

    RunSettings invalid_hotkey_priority = DefaultRunSettings();
    invalid_hotkey_priority.hotkey_control_priority_mode =
        static_cast<HotkeyControlPriorityMode>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_hotkey_priority)),
           "Unknown hotkey / control priority modes must fail settings validation");

    RunSettings invalid_worker_qos = DefaultRunSettings();
    invalid_worker_qos.timing_worker_qos_mode =
        static_cast<TimingWorkerQosMode>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_worker_qos)),
           "Unknown timing worker QoS modes must fail settings validation");

    RunSettings invalid_notification_mode = DefaultRunSettings();
    invalid_notification_mode.windows_notification_mode =
        static_cast<RunFeedbackMode>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_notification_mode)),
           "Unknown Windows notification modes must fail settings validation");

    RunSettings invalid_sound_mode = DefaultRunSettings();
    invalid_sound_mode.system_sound_mode = static_cast<RunFeedbackMode>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_sound_mode)),
           "Unknown system sound modes must fail settings validation");

    RunSettings high_process_worker_plus_one = DefaultRunSettings();
    high_process_worker_plus_one.process_priority_mode = ProcessPriorityMode::High;
    high_process_worker_plus_one.timing_worker_priority_mode =
        TimingWorkerPriorityMode::AboveNormal;
    Expect(!HasErrors(ValidateRunSettings(high_process_worker_plus_one)),
           "High process plus worker +1 must be available in the bounded interaction diagnostic");

    RunSettings high_process_worker_plus_two = DefaultRunSettings();
    high_process_worker_plus_two.process_priority_mode = ProcessPriorityMode::High;
    high_process_worker_plus_two.timing_worker_priority_mode =
        TimingWorkerPriorityMode::Highest;
    Expect(HasErrors(ValidateRunSettings(high_process_worker_plus_two)),
           "High process plus worker +2 must remain blocked by the diagnostic safety envelope");

    RunSettings above_process_worker_plus_two = DefaultRunSettings();
    above_process_worker_plus_two.process_priority_mode = ProcessPriorityMode::AboveNormal;
    above_process_worker_plus_two.timing_worker_priority_mode =
        TimingWorkerPriorityMode::Highest;
    Expect(!HasErrors(ValidateRunSettings(above_process_worker_plus_two)),
           "Above Normal process plus worker +2 must remain an allowed interaction comparison");

    RunSettings high_process_hotkey_plus_one = DefaultRunSettings();
    high_process_hotkey_plus_one.process_priority_mode = ProcessPriorityMode::High;
    high_process_hotkey_plus_one.hotkey_control_priority_mode =
        HotkeyControlPriorityMode::AboveNormal;
    Expect(!HasErrors(ValidateRunSettings(high_process_hotkey_plus_one)),
           "High process plus hotkey/control +1 must be available in this diagnostic");

    RunSettings high_process_both_plus_one = high_process_worker_plus_one;
    high_process_both_plus_one.hotkey_control_priority_mode =
        HotkeyControlPriorityMode::AboveNormal;
    Expect(!HasErrors(ValidateRunSettings(high_process_both_plus_one)),
           "High process may combine the two bounded +1 thread boosts");

    RunSettings high_qos_interaction = DefaultRunSettings();
    high_qos_interaction.process_priority_mode = ProcessPriorityMode::AboveNormal;
    high_qos_interaction.timing_worker_priority_mode =
        TimingWorkerPriorityMode::AboveNormal;
    high_qos_interaction.hotkey_control_priority_mode =
        HotkeyControlPriorityMode::AboveNormal;
    high_qos_interaction.timing_worker_qos_mode = TimingWorkerQosMode::HighQoS;
    Expect(!HasErrors(ValidateRunSettings(high_qos_interaction)),
           "HighQoS must be independently combinable with bounded priority settings");

    RunSettings eco_qos_interaction = high_qos_interaction;
    eco_qos_interaction.timing_worker_qos_mode = TimingWorkerQosMode::EcoQoS;
    Expect(!HasErrors(ValidateRunSettings(eco_qos_interaction)),
           "EcoQoS must remain independently combinable for interaction testing");

    DurationComponents visible_duration{0, 90, 1'500'000};
    std::uint64_t visible_duration_total = 0;
    Expect(ComposeDurationMicroseconds(visible_duration, visible_duration_total) &&
               visible_duration_total == 91'500'000,
           "Independent visible duration components must compose without normalization");
    Expect(visible_duration == DurationComponents{0, 90, 1'500'000},
           "Composing a duration must not rewrite the user's visible components");
    Expect(DurationComponentsMatch(visible_duration, 91'500'000) &&
               !DurationComponentsMatch(visible_duration, 90'000'000),
           "Presentation components must be accepted only when they exactly match the authoritative total");

    DurationComponents canonical_duration{};
    Expect(DecomposeDurationMicroseconds(90'000'000, canonical_duration) &&
               canonical_duration == DurationComponents{1, 30, 0},
           "An authoritative total without presentation metadata must receive a safe canonical display");

    DurationComponents maximum_duration{
        static_cast<std::uint32_t>(MaximumTimeComponent),
        static_cast<std::uint32_t>(MaximumTimeComponent),
        MaximumMillisecondsComponentMicroseconds,
    };
    std::uint64_t maximum_duration_total = 0;
    Expect(ComposeDurationMicroseconds(maximum_duration, maximum_duration_total) &&
               maximum_duration_total == MaximumCombinedDurationMicroseconds,
           "The inclusive one-million limit must compose safely across every duration field");
    DurationComponents decomposed_maximum{};
    Expect(DecomposeDurationMicroseconds(maximum_duration_total, decomposed_maximum) &&
               decomposed_maximum == maximum_duration,
           "The maximum supported authoritative duration must decompose without precision loss");

    DurationComponents invalid_components = maximum_duration;
    invalid_components.seconds = static_cast<std::uint32_t>(MaximumTimeComponent + 1);
    Expect(!ComposeDurationMicroseconds(invalid_components, maximum_duration_total),
           "A duration component above one million must fail safely");

    RunTimeLimitComponents run_time_components{1, 2, 3};
    std::uint64_t run_time_total = 0;
    Expect(ComposeRunTimeLimitMicroseconds(run_time_components, run_time_total) &&
               run_time_total == 3'723'000'000ULL,
           "Run time limit Hours / Minutes / Seconds must compose exactly");
    Expect(RunTimeLimitComponentsMatch(run_time_components, run_time_total),
           "Run time limit presentation components must match their authoritative total");
    RunTimeLimitComponents decomposed_run_time{};
    Expect(DecomposeRunTimeLimitMicroseconds(run_time_total, decomposed_run_time) &&
               decomposed_run_time == run_time_components,
           "Run time limit totals must decompose into canonical whole-second components");
    RunTimeLimitComponents maximum_run_time{
        static_cast<std::uint32_t>(MaximumTimeComponent),
        static_cast<std::uint32_t>(MaximumTimeComponent),
        static_cast<std::uint32_t>(MaximumTimeComponent),
    };
    std::uint64_t maximum_run_time_total = 0;
    Expect(ComposeRunTimeLimitMicroseconds(maximum_run_time, maximum_run_time_total) &&
               maximum_run_time_total == MaximumRunTimeLimitMicroseconds,
           "The inclusive one-million run time limit must compose safely across all three fields");
    RunTimeLimitComponents invalid_run_time = maximum_run_time;
    invalid_run_time.hours = static_cast<std::uint32_t>(MaximumTimeComponent + 1U);
    Expect(!ComposeRunTimeLimitMicroseconds(invalid_run_time, run_time_total),
           "A run time limit component above one million must fail safely");

    RunSettings oversized_duration = DefaultRunSettings();
    oversized_duration.interval_microseconds =
        MaximumCombinedDurationMicroseconds + 1U;
    Expect(HasErrors(ValidateRunSettings(oversized_duration)),
           "An authoritative timing total above the complete component range must fail validation");

    using vectorclick::win::layout::BaseOuterHeight;
    using vectorclick::win::layout::CalculateAdvancedViewportHeight;
    using vectorclick::win::layout::CalculateCenteredContentHost;
    using vectorclick::win::layout::CalculateClampedContentHost;
    using vectorclick::win::layout::CalculateMainLayout;
    using vectorclick::win::layout::Control;
    using vectorclick::win::layout::LayoutMetrics;
    using vectorclick::win::layout::MinimumOuterHeight;
    using vectorclick::win::layout::Page;
    using vectorclick::win::layout::Rect;
    using vectorclick::win::layout::ScaleForDpi;

    // Layout geometry across pages, status heights, scrolling, and
    // DPI-independent metrics.
    const auto basic_layout = CalculateMainLayout(LayoutMetrics{}, 40, Page::Basic);
    const auto advanced_layout = CalculateMainLayout(LayoutMetrics{}, 40, Page::Advanced);
    const auto full_advanced_layout =
        CalculateMainLayout(LayoutMetrics{}, 40, Page::Advanced, 650);
    const auto about_layout = CalculateMainLayout(LayoutMetrics{}, 40, Page::About);
    Expect(basic_layout.content_width == 680 && basic_layout.content_height == 585 &&
               advanced_layout.content_height == 585 &&
               full_advanced_layout.content_height == 805 &&
               about_layout.content_height == 585,
           "All pages must open at the compact height while an enlarged Advanced viewport can reach full height");
    Expect(basic_layout[Control::BasicPageHost] == Rect{8, 42, 664, 430} &&
               advanced_layout[Control::AdvancedPageHost] == Rect{8, 42, 664, 430} &&
               full_advanced_layout[Control::AdvancedPageHost] == Rect{8, 42, 664, 650} &&
               basic_layout[Control::AboutPageHost] == Rect{8, 42, 664, 430},
           "Advanced must use a compact default viewport and grow only with available window height");
    Expect(advanced_layout[Control::AdvancedPageHost].x +
                   advanced_layout[Control::AdvancedPageHost].width +
                   vectorclick::win::layout::AdvancedScrollbarRightInset +
                   vectorclick::win::layout::AdvancedScrollbarWidth ==
               advanced_layout.content_width,
           "The Advanced scrollbar must occupy the outer gutter instead of overlaying the cards");
    Expect(basic_layout[Control::BasicTab] == Rect{16, 2, 104, 36} &&
               basic_layout[Control::AdvancedTab] == Rect{126, 2, 124, 36} &&
               basic_layout[Control::AboutTab] == Rect{256, 2, 104, 36},
           "The three main tabs must retain their intended compact geometry");
    Expect(basic_layout[Control::InputGroup] == Rect{0, 0, 326, 272} &&
               basic_layout[Control::PositionGroup] == Rect{336, 0, 326, 272} &&
               basic_layout[Control::RepeatGroup] == Rect{0, 282, 326, 146} &&
               basic_layout[Control::HotkeysGroup] == Rect{336, 282, 326, 146},
           "The Basic-page cards must retain compact natural geometry");
    Expect(basic_layout[Control::IntervalMinutesEdit] == Rect{16, 190, 92, 28} &&
               basic_layout[Control::IntervalSecondsEdit] == Rect{115, 190, 92, 28} &&
               basic_layout[Control::IntervalEdit] == Rect{214, 190, 92, 28} &&
               basic_layout[Control::CapturePositionButton] == Rect{352, 174, 294, 46} &&
               basic_layout[Control::RepeatCountEdit] == Rect{140, 352, 88, 26} &&
               basic_layout[Control::RunTimeHoursEdit] == Rect{16, 397, 92, 26} &&
               basic_layout[Control::RunTimeMinutesEdit] == Rect{115, 397, 92, 26} &&
               basic_layout[Control::RunTimeSecondsEdit] == Rect{214, 397, 92, 26},
           "Representative Basic-page controls must retain compact geometry while the run time limit fits inside Repeat");
    Expect(basic_layout[Control::RepeatUnitLabel] == Rect{232, 352, 78, 26},
           "The Repeat unit label must reserve enough width for the complete key-press unit text");
    Expect(basic_layout[Control::StartHotkeyCombo] == Rect{508, 332, 138, 280} &&
               basic_layout[Control::EmergencyHotkeyCombo] == Rect{508, 376, 138, 280},
           "Safety-hotkey selectors must remain compact without entering their labels");
    Expect(advanced_layout[Control::AdvancedTimingGroup] == Rect{0, 0, 662, 358} &&
               advanced_layout[Control::TargetGroup] == Rect{0, 368, 662, 142} &&
               advanced_layout[Control::PerformanceGroup] == Rect{0, 520, 662, 206} &&
               advanced_layout[Control::NotificationsGroup] == Rect{0, 736, 662, 136} &&
               advanced_layout[Control::OptionsGroup] == Rect{0, 882, 662, 224} &&
               advanced_layout[Control::AdvancedTimingGroup].x +
                       advanced_layout[Control::AdvancedTimingGroup].width ==
                   basic_layout[Control::PositionGroup].x +
                       basic_layout[Control::PositionGroup].width,
           "The Advanced cards must share the Basic card envelope behind the compact viewport");
    Expect(advanced_layout[Control::ButtonDownMinutesEdit] == Rect{338, 60, 98, 26} &&
               advanced_layout[Control::ButtonDownSecondsEdit] == Rect{442, 60, 98, 26} &&
               advanced_layout[Control::ButtonDownEdit] == Rect{546, 60, 98, 26} &&
               advanced_layout[Control::DownDurationBehaviorCombo] == Rect{430, 90, 214, 180} &&
               advanced_layout[Control::BackendCombo] == Rect{430, 186, 214, 280} &&
               advanced_layout[Control::RandomIntervalCheck] == Rect{18, 222, 228, 18} &&
               advanced_layout[Control::RandomIntervalStyleCombo] == Rect{430, 250, 214, 180} &&
               advanced_layout[Control::MinimumIntervalMinutesEdit] == Rect{338, 286, 98, 26} &&
               advanced_layout[Control::MinimumIntervalSecondsEdit] == Rect{442, 286, 98, 26} &&
               advanced_layout[Control::MinimumIntervalEdit] == Rect{546, 286, 98, 26} &&
               advanced_layout[Control::MaximumIntervalMinutesEdit] == Rect{338, 318, 98, 26} &&
               advanced_layout[Control::MaximumIntervalSecondsEdit] == Rect{442, 318, 98, 26} &&
               advanced_layout[Control::MaximumIntervalEdit] == Rect{546, 318, 98, 26},
           "Representative Advanced timing controls must retain their accepted geometry");
    Expect(advanced_layout[Control::SelectTargetButton] == Rect{362, 410, 180, 34} &&
               advanced_layout[Control::ClearTargetButton] == Rect{548, 410, 96, 34},
           "Target actions must reserve enough width for the icon and complete label at scaled DPI");
    Expect(advanced_layout[Control::BackgroundInputCheck] == Rect{18, 467, 232, 24} &&
               advanced_layout[Control::AdminButton] == Rect{338, 464, 232, 30},
           "Target compatibility actions must share one balanced lower row without increasing the card height");
    Expect(advanced_layout[Control::WindowsNotificationLabel] == Rect{18, 782, 250, 20} &&
               advanced_layout[Control::WindowsNotificationCombo] == Rect{18, 804, 236, 150} &&
               advanced_layout[Control::SystemSoundLabel] == Rect{338, 782, 250, 20} &&
               advanced_layout[Control::SystemSoundCombo] == Rect{338, 804, 236, 150} &&
               advanced_layout[Control::RunningIndicatorCheck] == Rect{18, 844, 286, 18},
           "Notifications and Indicators must use the Scheduling and Performance two-column grid");
    Expect(advanced_layout[Control::ProcessPriorityLabel] == Rect{18, 566, 250, 20} &&
               advanced_layout[Control::ProcessPriorityCombo] == Rect{18, 588, 236, 180} &&
               advanced_layout[Control::TimingWorkerQosLabel] == Rect{338, 566, 250, 20} &&
               advanced_layout[Control::TimingWorkerQosCombo] == Rect{338, 588, 236, 160} &&
               advanced_layout[Control::TimingWorkerPriorityLabel] == Rect{18, 620, 250, 20} &&
               advanced_layout[Control::TimingWorkerPriorityCombo] == Rect{18, 642, 236, 160} &&
               advanced_layout[Control::HotkeyControlPriorityLabel] == Rect{338, 620, 250, 20} &&
               advanced_layout[Control::HotkeyControlPriorityCombo] == Rect{338, 642, 236, 140} &&
               advanced_layout[Control::PerformanceGuidanceText] == Rect{18, 674, 626, 40},
           "Scheduling controls must form the compact two-column performance card above Safety, display, and settings");
    Expect(advanced_layout[Control::DiagnosticsCheck] == Rect{18, 932, 206, 18} &&
               advanced_layout[Control::SafetyShieldCheck] == Rect{338, 932, 288, 18} &&
               advanced_layout[Control::ForceExitOnEmergencyStopCheck] == Rect{338, 954, 278, 18} &&
               advanced_layout[Control::CaptureExclusionCheck] == Rect{18, 976, 306, 18} &&
               advanced_layout[Control::KeepOnTopCheck] == Rect{338, 976, 220, 18} &&
               advanced_layout[Control::ProfileLabel] == Rect{18, 1002, 132, 20} &&
               advanced_layout[Control::ProfileCombo] == Rect{18, 1024, 236, 180} &&
               advanced_layout[Control::ManageProfilesButton] == Rect{338, 1020, 236, 34} &&
               advanced_layout[Control::RememberSettingsCheck] == Rect{18, 1070, 194, 18} &&
               advanced_layout[Control::ImportSettingsButton] == Rect{338, 1062, 236, 34} &&
               advanced_layout[Control::OptionsGroup].y +
                       advanced_layout[Control::OptionsGroup].height -
                       (advanced_layout[Control::ImportSettingsButton].y +
                        advanced_layout[Control::ImportSettingsButton].height) == 10,
           "Safety, display, and settings must retain the two-column safety rows, bounded local-profile controls, and the established ten-pixel card bottom padding");
    Expect(about_layout[Control::AboutIdentityGroup] == Rect{0, 0, 662, 174} &&
               about_layout[Control::AboutLinksGroup] == Rect{0, 184, 662, 244} &&
               about_layout[Control::AboutIdentityGroup].x +
                       about_layout[Control::AboutIdentityGroup].width ==
                   basic_layout[Control::PositionGroup].x +
                       basic_layout[Control::PositionGroup].width,
           "The About cards must share the Basic card envelope instead of entering the page gutter");
    Expect(about_layout[Control::AboutDetailsText] == Rect{428, 50, 208, 90} &&
               about_layout[Control::AboutLinksDescriptionText] == Rect{18, 228, 626, 34} &&
               about_layout[Control::OfficialDownloadsButton] == Rect{18, 296, 304, 34} &&
               about_layout[Control::SourceCodeButton] == Rect{340, 296, 304, 34} &&
               about_layout[Control::ViewLicenseButton] == Rect{18, 336, 304, 34} &&
               about_layout[Control::CopySupportEmailButton] == Rect{340, 336, 304, 34} &&
               about_layout[Control::CopyDiagnosticReportButton] == Rect{18, 376, 304, 34} &&
               about_layout[Control::ReportBugButton] == Rect{340, 376, 304, 34},
           "About support controls must retain a balanced three-row two-column grid inside the corrected card width");
    Expect(basic_layout[Control::RateText] == Rect{16, 225, 294, 40},
           "The buffered rate panel must retain its accepted rectangle");
    Expect(basic_layout[Control::StatusText] == Rect{12, 482, 656, 40} &&
               advanced_layout[Control::StatusText] == Rect{12, 482, 656, 40} &&
               advanced_layout[Control::StartButton] == Rect{12, 532, 196, 48} &&
               about_layout[Control::EmergencyButton] == Rect{424, 532, 240, 48},
           "All compact page viewports must share the same footer geometry");
    Expect(full_advanced_layout[Control::StatusText] == Rect{12, 702, 656, 40} &&
               full_advanced_layout[Control::StartButton] == Rect{12, 752, 196, 48} &&
               full_advanced_layout[Control::EmergencyButton] == Rect{424, 752, 240, 48},
           "A fully enlarged Advanced viewport must place the footer below all content");

    const auto expanded_basic = CalculateMainLayout(LayoutMetrics{}, 78, Page::Basic);
    const auto expanded_advanced = CalculateMainLayout(LayoutMetrics{}, 78, Page::Advanced);
    const auto expanded_full_advanced =
        CalculateMainLayout(LayoutMetrics{}, 78, Page::Advanced, 650);
    Expect(expanded_basic.content_height == 623 &&
               expanded_advanced.content_height == 623 &&
               expanded_full_advanced.content_height == 843,
           "Status growth must preserve the selected viewport height");
    Expect(expanded_basic[Control::StatusText] == Rect{12, 482, 656, 78} &&
               expanded_basic[Control::StartButton] == Rect{12, 570, 196, 48} &&
               expanded_advanced[Control::StatusText] == Rect{12, 482, 656, 78} &&
               expanded_advanced[Control::StartButton] == Rect{12, 570, 196, 48} &&
               expanded_full_advanced[Control::StatusText] == Rect{12, 702, 656, 78} &&
               expanded_full_advanced[Control::StartButton] == Rect{12, 790, 196, 48},
           "Footer controls must follow both status growth and the active Advanced viewport");
    Expect(BaseOuterHeight(Page::Basic) == 631 &&
               BaseOuterHeight(Page::About) == 631 &&
               BaseOuterHeight(Page::Advanced) == 631,
           "All pages must use the same compact minimum outer height");
    Expect(MinimumOuterHeight(78, Page::Basic) == 669 &&
               MinimumOuterHeight(78, Page::Advanced) == 669,
           "Every page minimum must grow equally with the status panel");
    const auto clamped_status_layout =
        CalculateMainLayout(LayoutMetrics{}, 0, Page::Advanced);
    Expect(clamped_status_layout.content_height == 585 &&
               clamped_status_layout[Control::StatusText] == Rect{12, 482, 656, 40} &&
               clamped_status_layout[Control::StartButton] == Rect{12, 532, 196, 48},
           "Status heights below the compact minimum must not create inconsistent geometry");

    Expect(CalculateAdvancedViewportHeight(593, 96, 40) == 430 &&
               CalculateAdvancedViewportHeight(640, 96, 40) == 477 &&
               CalculateAdvancedViewportHeight(687, 96, 40) == 524 &&
               CalculateAdvancedViewportHeight(779, 96, 40) == 616 &&
               CalculateAdvancedViewportHeight(813, 96, 40) == 650 &&
               CalculateAdvancedViewportHeight(900, 96, 40) == 737 &&
               CalculateAdvancedViewportHeight(950, 96, 40) == 787,
           "Advanced viewport height must grow with the client and clamp to the full content height");

    LayoutMetrics wider_metrics{};
    wider_metrics.limited_width = 150;
    const auto wider_layout = CalculateMainLayout(wider_metrics, 40, Page::Basic);
    Expect(wider_layout[Control::LimitedRadio].width == 150 &&
               wider_layout[Control::RepeatCountEdit].x == 174,
           "Measured control widths must influence only their intended row geometry");

    Expect(ScaleForDpi(680, 0) == 680,
           "A missing DPI value must fall back to 96 DPI");
    Expect(ScaleForDpi(680, 120) == 850 && ScaleForDpi(771, 120) == 964 &&
               ScaleForDpi(585, 120) == 731,
           "125 percent scaling must preserve the established MulDiv rounding");
    Expect(ScaleForDpi(680, 216) == 1530 && ScaleForDpi(771, 216) == 1735 &&
               ScaleForDpi(585, 216) == 1316,
           "225 percent scaling must preserve the established MulDiv rounding");
    Expect(CalculateCenteredContentHost(800, 700, 96, 40, Page::Basic) ==
               Rect{60, 56, 680, 585},
           "Compact content must remain centered when the user enlarges the window");
    Expect(CalculateCenteredContentHost(800, 700, 96, 40, Page::Advanced) ==
               Rect{60, 56, 680, 585},
           "Advanced must remain compact until an enlarged viewport is requested");
    Expect(CalculateCenteredContentHost(800, 900, 96, 40, Page::Advanced, 650) ==
               Rect{60, 46, 680, 805},
           "A fully enlarged Advanced viewport must retain the intended upward visual bias");
    Expect(CalculateCenteredContentHost(1700, 1500, 216, 40, Page::Basic) ==
               Rect{85, 90, 1530, 1316},
           "Compact content must remain centered at 225 percent scaling");
    Expect(CalculateCenteredContentHost(1700, 1500, 216, 40, Page::Advanced) ==
               Rect{85, 90, 1530, 1316},
           "Advanced must retain the compact scaled host by default");
    Expect(CalculateCenteredContentHost(1700, 1500, 216, 40, Page::Advanced, 650) ==
               Rect{85, 9, 1530, 1811},
           "A full Advanced viewport must retain the scaled upward visual bias");
    Expect(CalculateClampedContentHost(1700, 1500, 216, 40, Page::Basic, -500, -500) ==
               Rect{9, 9, 1530, 1316},
           "A compact retained layout must clamp to the scaled minimum margin");
    Expect(CalculateClampedContentHost(1700, 1500, 216, 40, Page::Basic, 5000, 5000) ==
               Rect{161, 175, 1530, 1316},
           "A compact retained layout must clamp fully inside the client area");
    Expect(CalculateClampedContentHost(1700, 1500, 216, 40, Page::Advanced, 5000, 5000) ==
               Rect{161, 175, 1530, 1316},
           "A compact Advanced retained layout must clamp fully inside the client area");
    Expect(CalculateClampedContentHost(1700, 1500, 216, 40, Page::Advanced, 5000, 5000, 650) ==
               Rect{161, 9, 1530, 1811},
           "A full Advanced retained layout must clamp fully inside the client area");

    using vectorclick::win::ActionUnitText;
    using vectorclick::win::BuildDiagnosticsPresentation;
    using vectorclick::win::BuildRatePresentation;
    using vectorclick::win::BuildStatusPresentation;
    using vectorclick::win::EvaluateStartAvailability;
    using vectorclick::win::PresentationEngineState;
    using vectorclick::win::PresentationSnapshot;
    using vectorclick::win::RatePresentationInput;
    using vectorclick::win::StartAvailabilityInput;
    using vectorclick::win::StatusCategory;

    RatePresentationInput single_rate{};
    single_rate.action_pattern = ActionPattern::Single;
    single_rate.interval_valid = true;
    single_rate.interval_microseconds = 10'000;
    single_rate.down_duration_valid = true;
    single_rate.down_duration_microseconds = 1'000;
    single_rate.action_spacing_valid = true;
    single_rate.action_spacing_microseconds = 10'000;
    single_rate.burst_count_valid = true;
    single_rate.burst_count = 4;
    // Rate, readiness, status, diagnostics, and snapshot presentation
    // contracts.
    const auto single_rate_presentation = BuildRatePresentation(single_rate);
    Expect(single_rate_presentation.text == L"Click rate: 100.0 CPS",
           "Presentation extraction must preserve the configured Single click rate text");
    Expect(ActionUnitText(ActionPattern::Single, false) == L"clicks" &&
               ActionUnitText(ActionPattern::Single, true) == L"key presses",
           "Single-action repeat units must use complete mouse and keyboard wording");

    RatePresentationInput slower_rate = single_rate;
    slower_rate.interval_microseconds = 100'000;
    const auto slower_rate_presentation = BuildRatePresentation(slower_rate);
    Expect(slower_rate_presentation.text == L"Click rate: 10.00 CPS",
           "Presentation extraction must preserve the established two-decimal slow-rate text");

    RatePresentationInput random_rate = single_rate;
    random_rate.randomize_interval = true;
    random_rate.minimum_interval_valid = true;
    random_rate.minimum_interval_microseconds = 10'000;
    random_rate.maximum_interval_valid = true;
    random_rate.maximum_interval_microseconds = 100'000;
    Expect(BuildRatePresentation(random_rate).text ==
               L"Click rate: 10.00 to 100.0 CPS",
           "Random interval presentation must show the configured slow-to-fast click-rate range");

    RatePresentationInput invalid_random_rate = random_rate;
    invalid_random_rate.maximum_interval_microseconds = 5'000;
    Expect(BuildRatePresentation(invalid_random_rate).text ==
               L"Maximum interval is below minimum",
           "Random interval presentation must reject an inverted range");

    RatePresentationInput burst_rate = single_rate;
    burst_rate.action_pattern = ActionPattern::Burst;
    burst_rate.interval_microseconds = 1'000;
    burst_rate.down_duration_microseconds = 0;
    burst_rate.action_spacing_microseconds = 1'000;
    burst_rate.burst_count = 4;
    const auto burst_rate_presentation = BuildRatePresentation(burst_rate);
    Expect(burst_rate_presentation.text == L"Click rate: 4000.0 CPS",
           "Presentation extraction must preserve overlapping Burst rate text");

    RatePresentationInput timing_limited_rate = burst_rate;
    timing_limited_rate.down_duration_microseconds = 1'000;
    const auto timing_rate_presentation = BuildRatePresentation(timing_limited_rate);
    Expect(timing_rate_presentation.text ==
               L"Click rate: 4000.0 CPS\nTiming limit: 1000.0 CPS",
           "Presentation extraction must preserve the ordered Down-duration timing line");

    RatePresentationInput keyboard_rate = single_rate;
    keyboard_rate.keyboard = true;
    Expect(BuildRatePresentation(keyboard_rate).text == L"Key rate: 100.0 per second",
           "Presentation extraction must preserve keyboard rate wording");

    RatePresentationInput hold_rate = single_rate;
    hold_rate.action_pattern = ActionPattern::Hold;
    Expect(BuildRatePresentation(hold_rate).text == L"Held until Stop / Emergency Stop",
           "Presentation extraction must preserve Hold presentation wording");

    RatePresentationInput time_limited_hold = hold_rate;
    time_limited_hold.run_time_limit_microseconds = 30'000'000;
    Expect(BuildRatePresentation(time_limited_hold).text == L"Held until time limit / Stop",
           "Hold presentation must identify an active run time limit");

    RatePresentationInput invalid_time_limit_rate = single_rate;
    invalid_time_limit_rate.run_time_limit_valid = false;
    Expect(BuildRatePresentation(invalid_time_limit_rate).text ==
               L"Check time limit values",
           "Invalid run time fields must have a compact rate-area validation message");

    RatePresentationInput safety_limited_rate = burst_rate;
    safety_limited_rate.burst_count = 100;
    const auto safety_rate_presentation = BuildRatePresentation(safety_limited_rate);
    Expect(safety_rate_presentation.text ==
               L"Click rate: 100000.0 CPS\nSafety limit: 10000.0 CPS",
           "Presentation extraction must preserve the explicit 10000 CPS safety line");

    RatePresentationInput invalid_interval_rate = single_rate;
    invalid_interval_rate.interval_valid = false;
    Expect(BuildRatePresentation(invalid_interval_rate).text ==
               L"Check interval values",
           "Presentation extraction must distinguish invalid interval fields from a zero interval");

    RatePresentationInput zero_interval_rate = single_rate;
    zero_interval_rate.interval_microseconds = 0;
    Expect(BuildRatePresentation(zero_interval_rate).text ==
               L"Interval must be greater than 0",
           "Presentation extraction must preserve the zero-interval rate wording");

    const auto ready_availability = EvaluateStartAvailability({
        PresentationEngineState::Ready,
        false,
        true,
        {},
        true,
        {},
        true,
        false,
        false,
        {},
    });
    Expect(ready_availability.start_enabled && ready_availability.available &&
               ready_availability.availability_changed,
           "A first valid Ready snapshot must enable Start and initialize availability");

    const auto hotkeys_unavailable = EvaluateStartAvailability({
        PresentationEngineState::Ready,
        false,
        false,
        L"Safety hotkeys are unavailable",
        true,
        {},
        true,
        true,
        true,
        {},
    });
    Expect(!hotkeys_unavailable.start_enabled &&
               hotkeys_unavailable.unavailable_reason ==
                   L"Safety hotkeys are unavailable",
           "Unconfirmed safety hotkeys must disable Start with an explicit reason");

    Expect(!RequiresCleanupRecovery(PresentationEngineState::Running, true),
           "Tracked input during an active run must not expose Retry cleanup");
    Expect(!RequiresCleanupRecovery(PresentationEngineState::Stopping, true),
           "Tracked input during ordinary stopping must not expose Retry cleanup prematurely");
    Expect(RequiresCleanupRecovery(PresentationEngineState::Ready, true) &&
               RequiresCleanupRecovery(PresentationEngineState::Disarmed, true) &&
               RequiresCleanupRecovery(PresentationEngineState::Faulted, true),
           "Tracked input after a terminal or idle state must expose cleanup recovery");
    Expect(!RequiresCleanupRecovery(PresentationEngineState::Ready, false),
           "An idle engine with no tracked input must not expose cleanup recovery");

    StartAvailabilityInput cleanup_input{
        PresentationEngineState::Ready,
        false,
        true,
        {},
        true,
        {},
        true,
        true,
        true,
        {},
    };
    cleanup_input.cleanup_required = true;
    const auto cleanup_required = EvaluateStartAvailability(cleanup_input);
    Expect(!cleanup_required.start_enabled && cleanup_required.attention_required &&
               cleanup_required.unavailable_reason ==
                   L"Cleanup required before another run can start",
           "Tracked input must persistently disable Start with an Attention reason");

    const auto cleanup_status = BuildStatusPresentation({
        PresentationEngineState::Ready,
        cleanup_required,
        false,
        false,
        ActionPattern::Single,
        0,
        L"",
    });
    Expect(cleanup_status.category == StatusCategory::Attention &&
               cleanup_status.text ==
                   L"Status: Attention required | Cleanup required before another run can start",
           "Cleanup-required readiness must use the Attention status category");

    StartAvailabilityInput latched_cleanup_input = cleanup_input;
    latched_cleanup_input.engine_state = PresentationEngineState::Disarmed;
    latched_cleanup_input.emergency_active = true;
    latched_cleanup_input.previous_initialized = true;
    latched_cleanup_input.previous_available = true;
    latched_cleanup_input.previous_unavailable_reason.clear();
    const auto latched_cleanup_required =
        EvaluateStartAvailability(latched_cleanup_input);
    Expect(!latched_cleanup_required.start_enabled &&
               !latched_cleanup_required.available &&
               latched_cleanup_required.attention_required &&
               latched_cleanup_required.unavailable_reason ==
                   L"Cleanup required before another run can start",
           "Cleanup-required state must override a cached Ready result while Emergency Stop remains latched");

    const auto not_ready_availability = EvaluateStartAvailability({
        PresentationEngineState::Ready,
        false,
        true,
        {},
        false,
        L"Interval must be greater than 0",
        true,
        true,
        true,
        {},
    });
    Expect(!not_ready_availability.start_enabled &&
               not_ready_availability.unavailable_reason ==
                   L"Interval must be greater than 0",
           "Invalid settings must disable Start and retain the validation reason");

    const auto running_availability = EvaluateStartAvailability({
        PresentationEngineState::Running,
        false,
        true,
        {},
        false,
        {},
        true,
        true,
        true,
        {},
    });
    Expect(!running_availability.start_enabled &&
               !running_availability.update_cached_availability &&
               running_availability.available,
           "Running must disable Start without replacing the last Ready availability snapshot");

    const auto not_ready_status = BuildStatusPresentation({
        PresentationEngineState::Ready,
        not_ready_availability,
        false,
        false,
        ActionPattern::Single,
        0,
        L"",
    });
    Expect(not_ready_status.category == StatusCategory::NotReady &&
               not_ready_status.text ==
                   L"Status: Not ready | Interval must be greater than 0",
           "Not-ready text and category must be calculated together");

    const auto running_status = BuildStatusPresentation({
        PresentationEngineState::Running,
        ready_availability,
        false,
        false,
        ActionPattern::Single,
        12,
        L"",
    });
    Expect(running_status.category == StatusCategory::Running &&
               running_status.text == L"Status: Running",
           "Running text and blue status category must be calculated together");

    const auto stopping_status = BuildStatusPresentation({
        PresentationEngineState::Stopping,
        running_availability,
        false,
        false,
        ActionPattern::Single,
        12,
        L"",
    });
    Expect(stopping_status.category == StatusCategory::Transition &&
               stopping_status.text == L"Status: Stopping...",
           "Stopping text and violet status category must be calculated together");

    const auto disarmed_status = BuildStatusPresentation({
        PresentationEngineState::Disarmed,
        ready_availability,
        false,
        false,
        ActionPattern::Single,
        12,
        L"",
    });
    Expect(disarmed_status.category == StatusCategory::Transition &&
               disarmed_status.text ==
                   L"Status: Emergency stop complete | Completed clicks: 12",
           "Emergency-stop-complete text and violet transition category must stay paired");

    const auto time_limit_status = BuildStatusPresentation({
        PresentationEngineState::Ready,
        ready_availability,
        false,
        false,
        ActionPattern::Single,
        12,
        L"Time limit reached",
    });
    Expect(time_limit_status.category == StatusCategory::Ready &&
               time_limit_status.text ==
                   L"Status: Ready | Time limit reached | Completed clicks: 12",
           "A natural time-limit stop must remain Ready while identifying why the run ended");

    const auto faulted_status = BuildStatusPresentation({
        PresentationEngineState::Faulted,
        running_availability,
        false,
        false,
        ActionPattern::Single,
        12,
        L"",
    });
    Expect(faulted_status.category == StatusCategory::Attention &&
               faulted_status.text == L"Status: Input stopped with an error",
           "Fault text and white attention category must be calculated together");

    const auto diagnostics_presentation = BuildDiagnosticsPresentation({
        PresentationEngineState::Ready,
        not_ready_availability,
        false,
        ActionPattern::Single,
        12,
        34,
        2.0,
        6.0,
        17.0,
        L"",
    });
    Expect(diagnostics_presentation.category == StatusCategory::NotReady &&
               diagnostics_presentation.text ==
                   L"Not ready | Interval must be greater than 0\n"
                   L"Last run: 12 actions | 34 clicks | 17.0 CPS",
           "Diagnostics readiness text, decimal precision, and indicator category must share one snapshot");

    const auto running_diagnostics = BuildDiagnosticsPresentation({
        PresentationEngineState::Running,
        running_availability,
        false,
        ActionPattern::Single,
        12,
        34,
        2.0,
        6.0,
        17.0,
        L"",
    });
    Expect(running_diagnostics.category == StatusCategory::Running &&
               running_diagnostics.text ==
                   L"Running | Completed actions: 12 | Generated clicks: 34\n"
                   L"Action rate: 6.0/s | Click rate: 17.0 CPS | Elapsed: 2.00 s",
           "Optimized diagnostics formatting must preserve the accepted running text and precision");

    const auto repeat_limit_diagnostics = BuildDiagnosticsPresentation({
        PresentationEngineState::Ready,
        ready_availability,
        false,
        ActionPattern::Single,
        12,
        34,
        2.0,
        6.0,
        17.0,
        L"Repeat limit reached",
    });
    Expect(repeat_limit_diagnostics.category == StatusCategory::Ready &&
               repeat_limit_diagnostics.text ==
                   L"Ready | Repeat limit reached | Completed actions: 12 | Generated clicks: 34\n"
                   L"Action rate: 6.0/s | Click rate: 17.0 CPS | Elapsed: 2.00 s",
           "Idle diagnostics must identify a natural repeat-limit stop");

    const auto disarmed_diagnostics = BuildDiagnosticsPresentation({
        PresentationEngineState::Disarmed,
        running_availability,
        false,
        ActionPattern::Single,
        12,
        34,
        2.0,
        6.0,
        17.0,
        L"",
    });
    Expect(disarmed_diagnostics.category == StatusCategory::Transition &&
               disarmed_diagnostics.text ==
                   L"Emergency stop complete | Completed actions: 12 | Generated clicks: 34\n"
                   L"Action rate: 6.0/s | Click rate: 17.0 CPS | Elapsed: 2.00 s",
           "Live diagnostics must pair Emergency-stop-complete text with the violet transition category");

    const auto hold_diagnostics = BuildDiagnosticsPresentation({
        PresentationEngineState::Running,
        running_availability,
        true,
        ActionPattern::Hold,
        1,
        1,
        2.0,
        0.0,
        0.0,
        L"",
    });
    Expect(hold_diagnostics.category == StatusCategory::Running &&
               hold_diagnostics.text ==
                   L"Running | Hold active: Yes | Generated key presses: 1\n"
                   L"Completed holds: 1 | Elapsed: 2.00 s",
           "Optimized diagnostics formatting must preserve Hold text and elapsed precision");

    const auto rounding_boundary_diagnostics = BuildDiagnosticsPresentation({
        PresentationEngineState::Running,
        running_availability,
        false,
        ActionPattern::Single,
        1,
        1,
        2.675,
        0.25,
        std::nextafter(1.35, std::numeric_limits<double>::infinity()),
        L"",
    });
    Expect(rounding_boundary_diagnostics.text ==
               L"Running | Completed actions: 1 | Generated clicks: 1\n"
               L"Action rate: 0.2/s | Click rate: 1.4 CPS | Elapsed: 2.67 s",
           "Optimized fixed-decimal formatting must preserve established ties-to-even and binary-boundary behavior");

    // Keep a deterministic equivalence sample in the permanent test suite.
    // The reference deliberately remains test-only so floating std::to_chars does
    // not pull its large conversion tables back into the production executable.
    std::uint64_t formatter_state = 0x9e3779b97f4a7c15ULL;
    for (std::uint32_t index = 0; index < 20'000U; ++index) {
        formatter_state ^= formatter_state >> 12U;
        formatter_state ^= formatter_state << 25U;
        formatter_state ^= formatter_state >> 27U;
        const std::uint64_t sample = formatter_state * 0x2545F4914F6CDD1DULL;
        const double rate = static_cast<double>(sample % 100'000'001ULL) / 10'000.0;
        const double elapsed = static_cast<double>((sample >> 17U) % 10'000'000'001ULL) / 100.0;
        const auto formatted = BuildDiagnosticsPresentation({
            PresentationEngineState::Running,
            running_availability,
            false,
            ActionPattern::Single,
            1,
            1,
            elapsed,
            rate,
            rate,
            L"",
        });
        const std::wstring expected =
            L"Running | Completed actions: 1 | Generated clicks: 1\n"
            L"Action rate: " + ReferenceFixed(rate, 1) +
            L"/s | Click rate: " + ReferenceFixed(rate, 1) +
            L" CPS | Elapsed: " + ReferenceFixed(elapsed, 2) + L" s";
        Expect(formatted.text == expected,
               "Optimized fixed-decimal formatting must match the accepted std::to_chars presentation over deterministic bounded samples");
    }

    PresentationSnapshot presentation_snapshot{};
    presentation_snapshot.rate = single_rate_presentation;
    presentation_snapshot.availability = ready_availability;
    presentation_snapshot.status = BuildStatusPresentation({
        PresentationEngineState::Ready,
        ready_availability,
        false,
        false,
        ActionPattern::Single,
        0,
        L"",
    });
    presentation_snapshot.diagnostics = diagnostics_presentation;
    Expect(presentation_snapshot == presentation_snapshot,
           "Equivalent presentation snapshots must compare equal");
    PresentationSnapshot changed_presentation = presentation_snapshot;
    changed_presentation.status.category = StatusCategory::Attention;
    Expect(!(changed_presentation == presentation_snapshot),
           "A changed presentation category must be visible to snapshot change detection");
    changed_presentation = presentation_snapshot;
    changed_presentation.diagnostics_visible = true;
    Expect(!(changed_presentation == presentation_snapshot),
           "A changed diagnostics mode must be visible to snapshot change detection");

    const RunSettings defaults = DefaultRunSettings();
    Expect(defaults == DefaultRunSettings(),
           "Equivalent settings snapshots should compare equal for change-only persistence");
    RunSettings changed_snapshot = defaults;
    changed_snapshot.interval_microseconds = 22'000;
    Expect(!(changed_snapshot == defaults),
           "A changed persisted value should make settings snapshots differ");
    Expect(defaults.interval_microseconds == 10'000,
           "Fresh settings should default to a 10 ms click interval");
    Expect(!defaults.randomize_interval &&
               defaults.random_interval_style == RandomIntervalStyle::Independent &&
               defaults.minimum_interval_microseconds == 10'000 &&
               defaults.maximum_interval_microseconds == 100'000,
           "Fresh settings should keep an independent disabled 10 through 100 ms random interval range");
    Expect(defaults.button_down_microseconds == 1'000,
           "Fresh settings should default to a 1 ms button-down duration");
    Expect(defaults.repeat_count == 100,
           "Fresh settings should default to 100 limited actions");
    Expect(defaults.run_time_limit_microseconds == 0 &&
               defaults.run_time_limit_components == RunTimeLimitComponents{0, 0, 0},
           "Fresh settings should keep the run time limit off by default");
    Expect(defaults.action_pattern == ActionPattern::Single,
           "Fresh settings should default to a single-input action");
    Expect(defaults.burst_count == 4,
           "Fresh settings should default to four inputs in custom burst mode");
    Expect(defaults.action_spacing_microseconds == 10'000,
           "Fresh settings should default to 10 ms spacing within multi-input actions");
    Expect(InputsPerAction(defaults) == 1,
           "Single action mode should contain one generated input");
    Expect(defaults.action_type == ActionType::MouseClick,
           "Fresh settings should default to mouse clicking");
    Expect(defaults.position_mode == PositionMode::CurrentCursor,
           "Fresh settings should default to the current cursor position");
    Expect(defaults.backend == InputBackend::Automatic,
           "Fresh settings should default to automatic backend selection");
    Expect(!defaults.allow_background_input,
           "Fresh settings should keep background targeted input off");
    Expect(!defaults.show_click_position_indicator,
           "Fresh settings should keep the click-position indicator off");
    RunSettings indicator_snapshot = defaults;
    indicator_snapshot.show_click_position_indicator = true;
    Expect(!(indicator_snapshot == defaults),
           "Changing the indicator setting should update the persisted settings snapshot");
    Expect(defaults.start_stop_hotkey.virtual_key == 0x74,
           "Fresh settings should default Start / Stop to F5");
    Expect(defaults.emergency_hotkey.virtual_key == 0x77,
           "Fresh settings should default Emergency Stop to F8");

    // Input-method validation, safety conflicts, replacement policy, and
    // shutdown boundaries.
    RunSettings valid = defaults;
    auto issues = ValidateRunSettings(valid);
    Expect(!HasErrors(issues), "Default settings should be valid");

    RunSettings invalid_random_style = valid;
    invalid_random_style.random_interval_style =
        static_cast<RandomIntervalStyle>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_random_style)),
           "Unknown random interval styles must fail validation");

    RunSettings invalid_backend = valid;
    invalid_backend.backend = static_cast<InputBackend>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_backend)),
           "Unknown input methods must fail validation");

    RunSettings standard_backend = valid;
    standard_backend.backend = InputBackend::StandardInput;
    Expect(!HasErrors(ValidateRunSettings(standard_backend)),
           "Standard input should be a valid input method");

    RunSettings foreground_backend = valid;
    foreground_backend.backend = InputBackend::ForegroundTargetInput;
    Expect(!HasErrors(ValidateRunSettings(foreground_backend)),
           "Foreground target input should be a valid input method");

    RunSettings targeted_backend = valid;
    targeted_backend.backend = InputBackend::TargetedWindowMessages;
    Expect(!HasErrors(ValidateRunSettings(targeted_backend)),
           "Targeted window messages should be valid for mouse clicking");

    targeted_backend.action_type = ActionType::KeyboardPress;
    targeted_backend.generated_virtual_key = 0x41;
    Expect(!HasErrors(ValidateRunSettings(targeted_backend)),
           "Targeted window messages should support keyboard pressing");

    RunSettings unicode_backend = valid;
    unicode_backend.backend = InputBackend::UnicodeTextInput;
    Expect(HasErrors(ValidateRunSettings(unicode_backend)),
           "Unicode text input should reject mouse clicking");

    unicode_backend.action_type = ActionType::KeyboardPress;
    unicode_backend.generated_virtual_key = 0x41;
    Expect(!HasErrors(ValidateRunSettings(unicode_backend)),
           "Unicode text input should be a valid keyboard input method");
    unicode_backend.action_pattern = ActionPattern::Hold;
    Expect(HasErrors(ValidateRunSettings(unicode_backend)),
           "Unicode text input should reject physical-key Hold semantics");

    RunSettings targeted_unicode_backend = valid;
    targeted_unicode_backend.backend = InputBackend::TargetedUnicodeText;
    Expect(HasErrors(ValidateRunSettings(targeted_unicode_backend)),
           "Targeted Unicode text should reject mouse clicking");
    targeted_unicode_backend.action_type = ActionType::KeyboardPress;
    targeted_unicode_backend.generated_virtual_key = 0x41;
    Expect(!HasErrors(ValidateRunSettings(targeted_unicode_backend)),
           "Targeted Unicode text should be a valid keyboard input method");
    targeted_unicode_backend.action_pattern = ActionPattern::Hold;
    Expect(HasErrors(ValidateRunSettings(targeted_unicode_backend)),
           "Targeted Unicode text should reject physical-key Hold semantics");

    Expect(ResolveInputBackend(InputBackend::Automatic, false, false) ==
               InputBackend::StandardInput,
           "Automatic should use Standard input when no target is selected");
    Expect(ResolveInputBackend(InputBackend::Automatic, true, false) ==
               InputBackend::ForegroundTargetInput,
           "Automatic should use Foreground target input for a selected foreground-only target");
    Expect(ResolveInputBackend(InputBackend::Automatic, true, true) ==
               InputBackend::TargetedWindowMessages,
           "Automatic should use Targeted window messages when background input is enabled");
    Expect(ResolveInputBackend(InputBackend::ForegroundTargetInput, true, true) ==
               InputBackend::ForegroundTargetInput,
           "Explicit Foreground target input should ignore the background preference");
    Expect(ResolveInputBackend(InputBackend::TargetedWindowMessages, true, false) ==
               InputBackend::TargetedWindowMessages,
           "Explicit Targeted window messages should remain selected");
    Expect(ResolveInputBackend(InputBackend::UnicodeTextInput, true, true) ==
               InputBackend::UnicodeTextInput,
           "Explicit Unicode text input should ignore target and background selection");
    Expect(ResolveInputBackend(InputBackend::TargetedUnicodeText, true, true) ==
               InputBackend::TargetedUnicodeText,
           "Explicit Targeted Unicode text should remain selected");

    RunSettings double_action = valid;
    double_action.action_pattern = ActionPattern::Double;
    double_action.action_spacing_microseconds = 10'000;
    Expect(InputsPerAction(double_action) == 2,
           "Double action mode should contain two generated inputs");
    Expect(!HasErrors(ValidateRunSettings(double_action)),
           "A valid double action should pass validation");

    RunSettings triple_action = valid;
    triple_action.action_pattern = ActionPattern::Triple;
    Expect(InputsPerAction(triple_action) == 3,
           "Triple action mode should contain three generated inputs");

    RunSettings burst_action = valid;
    burst_action.action_pattern = ActionPattern::Burst;
    burst_action.burst_count = 12;
    Expect(InputsPerAction(burst_action) == 12,
           "Custom burst mode should use the configured burst count");

    RunSettings hold_action = valid;
    hold_action.action_pattern = ActionPattern::Hold;
    hold_action.interval_microseconds = 0;
    hold_action.button_down_microseconds = 999'999;
    hold_action.repeat_mode = RepeatMode::Limited;
    hold_action.repeat_count = 0;
    hold_action.run_time_limit_microseconds = 30'000'000;
    hold_action.run_time_limit_components = {0, 0, 30};
    Expect(InputsPerAction(hold_action) == 1,
           "Hold mode should contain one generated down state");
    Expect(!HasErrors(ValidateRunSettings(hold_action)),
           "Inactive interval, down duration, and repeat values must not invalidate Hold while a valid time limit remains active");

    RunSettings invalid_burst = burst_action;
    invalid_burst.burst_count = MaximumBurstCount + 1;
    Expect(HasErrors(ValidateRunSettings(invalid_burst)),
           "Burst counts above the safety limit must fail validation");

    RunSettings inactive_invalid_burst = valid;
    inactive_invalid_burst.burst_count = MaximumBurstCount + 1;
    Expect(!HasErrors(ValidateRunSettings(inactive_invalid_burst)),
           "An inactive Burst count must not invalidate Single mode");

    RunSettings invalid_action_spacing = double_action;
    invalid_action_spacing.action_spacing_microseconds = 500;
    invalid_action_spacing.button_down_microseconds = 1'000;
    Expect(HasErrors(ValidateRunSettings(invalid_action_spacing)),
           "Multi-input spacing shorter than the down duration must fail");

    RunSettings invalid_pattern = valid;
    invalid_pattern.action_pattern = static_cast<ActionPattern>(255);
    Expect(HasErrors(ValidateRunSettings(invalid_pattern)),
           "Unknown action patterns must fail validation");

    RunSettings invalid_duration = valid;
    invalid_duration.interval_microseconds = 1'000;
    invalid_duration.button_down_microseconds = 2'000;
    Expect(HasErrors(ValidateRunSettings(invalid_duration)),
           "Down duration greater than the interval must fail");

    RunSettings valid_random_interval = valid;
    valid_random_interval.randomize_interval = true;
    valid_random_interval.minimum_interval_microseconds = 10'000;
    valid_random_interval.maximum_interval_microseconds = 100'000;
    Expect(!HasErrors(ValidateRunSettings(valid_random_interval)),
           "A valid inclusive random interval range should pass validation");

    RunSettings zero_random_minimum = valid_random_interval;
    zero_random_minimum.minimum_interval_microseconds = 0;
    Expect(HasErrors(ValidateRunSettings(zero_random_minimum)),
           "A zero minimum random interval must fail validation");

    RunSettings inverted_random_range = valid_random_interval;
    inverted_random_range.maximum_interval_microseconds = 5'000;
    Expect(HasErrors(ValidateRunSettings(inverted_random_range)),
           "A maximum random interval below the minimum must fail validation");

    RunSettings random_duration_too_long = valid_random_interval;
    random_duration_too_long.minimum_interval_microseconds = 500;
    random_duration_too_long.button_down_microseconds = 1'000;
    Expect(HasErrors(ValidateRunSettings(random_duration_too_long)),
           "Down duration longer than the minimum random interval must fail validation");

    RunSettings duplicate_hotkeys = valid;
    duplicate_hotkeys.emergency_hotkey = duplicate_hotkeys.start_stop_hotkey;
    Expect(HasErrors(ValidateRunSettings(duplicate_hotkeys)),
           "Duplicate safety hotkeys must fail");

    RunSettings keyboard_conflict = valid;
    keyboard_conflict.action_type = ActionType::KeyboardPress;
    keyboard_conflict.generated_virtual_key = keyboard_conflict.start_stop_hotkey.virtual_key;
    Expect(HasErrors(ValidateRunSettings(keyboard_conflict)),
           "Keyboard input matching Start / Stop must fail");

    RunSettings keyboard_emergency_conflict = valid;
    keyboard_emergency_conflict.action_type = ActionType::KeyboardPress;
    keyboard_emergency_conflict.generated_virtual_key =
        keyboard_emergency_conflict.emergency_hotkey.virtual_key;
    Expect(HasErrors(ValidateRunSettings(keyboard_emergency_conflict)),
           "Keyboard input matching Emergency Stop must fail");

    RunSettings keyboard_valid = valid;
    keyboard_valid.action_type = ActionType::KeyboardPress;
    keyboard_valid.generated_virtual_key = 0x41;
    Expect(!HasErrors(ValidateRunSettings(keyboard_valid)),
           "A non-reserved keyboard key should be valid");

    RunSettings shifted_key = keyboard_valid;
    shifted_key.generated_virtual_key = 0xBD; // VK_OEM_MINUS
    shifted_key.generated_key_modifiers = KeyModifierShift;
    Expect(!HasErrors(ValidateRunSettings(shifted_key)),
           "A supported shifted generated key should be valid");

    RunSettings shifted_numpad = keyboard_valid;
    shifted_numpad.generated_virtual_key = 0x61; // VK_NUMPAD1
    shifted_numpad.generated_key_modifiers = KeyModifierShift;
    Expect(!HasErrors(ValidateRunSettings(shifted_numpad)),
           "A supported shifted numpad navigation assignment should be valid");

    RunSettings unsupported_generated_modifier = keyboard_valid;
    unsupported_generated_modifier.generated_key_modifiers = 0x0002;
    Expect(HasErrors(ValidateRunSettings(unsupported_generated_modifier)),
           "Unsupported generated-key modifiers must fail validation");

    RunSettings same_physical_hotkeys = valid;
    same_physical_hotkeys.start_stop_hotkey = {0xBD, 0};
    same_physical_hotkeys.emergency_hotkey = {0xBD, KeyModifierShift};
    Expect(HasErrors(ValidateRunSettings(same_physical_hotkeys)),
           "Safety hotkeys must not share one physical key across Shift states");

    RunSettings shifted_generated_conflict = keyboard_valid;
    shifted_generated_conflict.generated_virtual_key = 0xBD;
    shifted_generated_conflict.generated_key_modifiers = KeyModifierShift;
    shifted_generated_conflict.start_stop_hotkey = {0xBD, 0};
    Expect(HasErrors(ValidateRunSettings(shifted_generated_conflict)),
           "A shifted generated key must conflict with a safety hotkey on the same physical key");

    const std::vector<HotkeyBinding> ordered_keys{
        {0x41, 0}, {0x42, 0}, {0xBD, 0}, {0xBD, KeyModifierShift}, {0x44, 0}};
    const auto replacement_one =
        FindNextAvailableGeneratedKey(ordered_keys, {0x41, 0}, {0x41, 0}, {0x44, 0});
    Expect(replacement_one.value_or(HotkeyBinding{}).virtual_key == 0x42,
           "A reserved A key should advance to B when D is also reserved");
    const auto replacement_two =
        FindNextAvailableGeneratedKey(ordered_keys, {0xBD, 0}, {0x41, 0}, {0x44, 0});
    Expect(replacement_two.value_or(HotkeyBinding{}) ==
               HotkeyBinding{0xBD, KeyModifierShift},
           "Replacement selection should preserve a distinct shifted choice when its physical key is available");
    Expect(!FindNextAvailableGeneratedKey(
                {{0x41, 0}, {0x44, KeyModifierShift}},
                {0x41, 0},
                {0x41, KeyModifierShift},
                {0x44, 0})
                .has_value(),
           "Replacement selection should report when every physical key is reserved");

    std::atomic<bool> session_running{true};
    std::atomic<std::uint64_t> session_id{7};
    const vectorclick::win::InputSessionCancellation active_session{
        &session_running, &session_id, 7};
    Expect(!active_session.Requested(),
           "An active matching input session must not report cancellation");
    session_running.store(false, std::memory_order_release);
    Expect(active_session.Requested(),
           "A stopped input session must report cancellation");
    session_running.store(true, std::memory_order_release);
    session_id.store(8, std::memory_order_release);
    Expect(active_session.Requested(),
           "A superseded input session must report cancellation");

    RunSettings limited = valid;
    limited.repeat_mode = RepeatMode::Limited;
    limited.repeat_count = 1;
    Expect(!HasErrors(ValidateRunSettings(limited)),
           "Limited mode must accept one complete action");
    limited.repeat_count = MaximumRepeatCount;
    Expect(!HasErrors(ValidateRunSettings(limited)),
           "Limited mode must accept the maximum repeat count");
    limited.repeat_count = 0;
    Expect(HasErrors(ValidateRunSettings(limited)),
           "Limited mode with zero complete actions must fail");
    limited.repeat_count = MaximumRepeatCount + 1U;
    Expect(HasErrors(ValidateRunSettings(limited)),
           "Limited mode above the maximum repeat count must fail");

    RunSettings maximum_time_limited = valid;
    maximum_time_limited.run_time_limit_microseconds = MaximumRunTimeLimitMicroseconds;
    Expect(!HasErrors(ValidateRunSettings(maximum_time_limited)),
           "The maximum supported run time limit must pass validation");
    maximum_time_limited.run_time_limit_microseconds = MaximumRunTimeLimitMicroseconds + 1U;
    Expect(HasErrors(ValidateRunSettings(maximum_time_limited)),
           "A run time limit above the supported component range must fail validation");
    RunSettings fractional_second_time_limit = valid;
    fractional_second_time_limit.run_time_limit_microseconds = 1U;
    Expect(HasErrors(ValidateRunSettings(fractional_second_time_limit)),
           "Run time limits must remain representable by whole Hours / Minutes / Seconds fields");


    Expect(vectorclick::win::ShutdownCleanupSucceeded(true, false),
           "Shutdown may finalize only after release submission succeeds and no tracked input remains");
    Expect(!vectorclick::win::ShutdownCleanupSucceeded(false, false),
           "A failed shutdown release submission must not be treated as complete");
    Expect(!vectorclick::win::ShutdownCleanupSucceeded(true, true),
           "Tracked input must block ordinary shutdown even after a nominally successful release call");

    std::cout << "All Vector Click core tests passed.\n";
    return EXIT_SUCCESS;
}

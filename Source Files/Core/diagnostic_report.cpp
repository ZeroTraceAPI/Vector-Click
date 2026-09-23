#include "Core/diagnostic_report.h"

#include <cmath>
#include <limits>
#include <string_view>

namespace vectorclick::core {
namespace {

// Keep report formatting platform-neutral. Windows-specific collection code
// supplies normalized values; this file only converts them into stable support
// text suitable for the clipboard report.
constexpr std::uint16_t VkBack = 0x08;
constexpr std::uint16_t VkTab = 0x09;
constexpr std::uint16_t VkReturn = 0x0D;
constexpr std::uint16_t VkEscape = 0x1B;
constexpr std::uint16_t VkSpace = 0x20;
constexpr std::uint16_t VkPrior = 0x21;
constexpr std::uint16_t VkNext = 0x22;
constexpr std::uint16_t VkEnd = 0x23;
constexpr std::uint16_t VkHome = 0x24;
constexpr std::uint16_t VkLeft = 0x25;
constexpr std::uint16_t VkUp = 0x26;
constexpr std::uint16_t VkRight = 0x27;
constexpr std::uint16_t VkDown = 0x28;
constexpr std::uint16_t VkInsert = 0x2D;
constexpr std::uint16_t VkDelete = 0x2E;
constexpr std::uint16_t VkNumpad0 = 0x60;
constexpr std::uint16_t VkNumpad9 = 0x69;
constexpr std::uint16_t VkMultiply = 0x6A;
constexpr std::uint16_t VkAdd = 0x6B;
constexpr std::uint16_t VkSubtract = 0x6D;
constexpr std::uint16_t VkDecimal = 0x6E;
constexpr std::uint16_t VkDivide = 0x6F;
constexpr std::uint16_t VkF1 = 0x70;
constexpr std::uint16_t VkF24 = 0x87;
constexpr std::uint16_t VkOem1 = 0xBA;
constexpr std::uint16_t VkOemPlus = 0xBB;
constexpr std::uint16_t VkOemComma = 0xBC;
constexpr std::uint16_t VkOemMinus = 0xBD;
constexpr std::uint16_t VkOemPeriod = 0xBE;
constexpr std::uint16_t VkOem2 = 0xBF;
constexpr std::uint16_t VkOem3 = 0xC0;
constexpr std::uint16_t VkOem4 = 0xDB;
constexpr std::uint16_t VkOem5 = 0xDC;
constexpr std::uint16_t VkOem6 = 0xDD;
constexpr std::uint16_t VkOem7 = 0xDE;

void AddLine(std::wstring& output,
             const std::wstring_view label,
             const std::wstring_view value) {
    output.append(label);
    output.append(L": ");
    output.append(value);
    output.append(L"\r\n");
}

void AddBoolLine(std::wstring& output,
                 const std::wstring_view label,
                 const bool value) {
    AddLine(output, label, value ? L"Yes" : L"No");
}

std::wstring FixedDecimal(const double value, const unsigned decimals) {
    if (!std::isfinite(value) || value < 0.0) {
        return L"Unavailable";
    }
    std::uint64_t scale = 1;
    for (unsigned index = 0; index < decimals; ++index) {
        scale *= 10U;
    }
    const double scaled_value = value * static_cast<double>(scale);
    if (!std::isfinite(scaled_value) ||
        scaled_value > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return L"Unavailable";
    }
    const auto scaled = static_cast<std::uint64_t>(std::llround(scaled_value));
    const std::uint64_t whole = scaled / scale;
    const std::uint64_t fraction = scaled % scale;
    std::wstring result = std::to_wstring(whole);
    if (decimals != 0) {
        result.push_back(L'.');
        std::wstring fraction_text = std::to_wstring(fraction);
        if (fraction_text.size() < decimals) {
            result.append(decimals - fraction_text.size(), L'0');
        }
        result += fraction_text;
    }
    return result;
}

std::wstring FormatMillisecondsWide(const std::uint64_t microseconds) {
    const std::uint64_t whole = microseconds / 1'000U;
    const std::uint64_t remainder = microseconds % 1'000U;
    std::wstring result = std::to_wstring(whole);
    if (remainder == 0) {
        return result + L" ms";
    }
    result.push_back(L'.');
    std::wstring fraction = std::to_wstring(remainder);
    if (fraction.size() < 3U) {
        result.append(3U - fraction.size(), L'0');
    }
    while (!fraction.empty() && fraction.back() == L'0') {
        fraction.pop_back();
    }
    result += fraction;
    result += L" ms";
    return result;
}

std::wstring FormatRunTime(const RunTimeLimitComponents& components,
                           const std::uint64_t total_microseconds) {
    if (total_microseconds == 0) {
        return L"None";
    }
    return std::to_wstring(components.hours) + L" h / " +
           std::to_wstring(components.minutes) + L" min / " +
           std::to_wstring(components.seconds) + L" s";
}

// Stable report vocabulary. These helpers intentionally centralize public
// diagnostic wording so the live application and copied report do not drift.
const wchar_t* EngineStateText(const DiagnosticEngineState state) noexcept {
    switch (state) {
    case DiagnosticEngineState::Ready: return L"Ready";
    case DiagnosticEngineState::Running: return L"Running";
    case DiagnosticEngineState::Stopping: return L"Stopping";
    case DiagnosticEngineState::Disarmed: return L"Disarmed";
    case DiagnosticEngineState::Faulted: return L"Faulted";
    }
    return L"Unknown";
}

const wchar_t* ReadinessText(const DiagnosticReadiness readiness) noexcept {
    switch (readiness) {
    case DiagnosticReadiness::Ready: return L"Ready";
    case DiagnosticReadiness::Running: return L"Unavailable while running";
    case DiagnosticReadiness::Stopping: return L"Unavailable while stopping";
    case DiagnosticReadiness::EmergencyActive: return L"Blocked, Emergency Stop recovery is active";
    case DiagnosticReadiness::ShutdownActive: return L"Unavailable during shutdown";
    case DiagnosticReadiness::CleanupRequired: return L"Blocked, release cleanup is required";
    case DiagnosticReadiness::HotkeysUnavailable: return L"Blocked, safety hotkeys are not ready";
    case DiagnosticReadiness::SettingsInvalid: return L"Blocked, current settings are invalid";
    case DiagnosticReadiness::TargetUnavailable: return L"Not ready, required target is currently unavailable";
    case DiagnosticReadiness::AdministratorConfirmationRequired:
        return L"Confirmation required, selected target is elevated";
    case DiagnosticReadiness::TargetNotForeground:
        return L"Blocked, selected target is not foreground";
    case DiagnosticReadiness::TargetMinimized:
        return L"Blocked, selected target is minimized for mouse input";
    case DiagnosticReadiness::Faulted: return L"Blocked, input engine is faulted";
    case DiagnosticReadiness::Unavailable: return L"Unavailable";
    }
    return L"Unavailable";
}

const wchar_t* ActionTypeText(const ActionType type) noexcept {
    switch (type) {
    case ActionType::MouseClick: return L"Mouse click";
    case ActionType::KeyboardPress: return L"Keyboard press";
    }
    return L"Unknown";
}

const wchar_t* MouseButtonText(const MouseButton button) noexcept {
    switch (button) {
    case MouseButton::Left: return L"Left";
    case MouseButton::Right: return L"Right";
    case MouseButton::Middle: return L"Middle";
    case MouseButton::X1: return L"X1";
    case MouseButton::X2: return L"X2";
    }
    return L"Unknown";
}

const wchar_t* PatternText(const ActionPattern pattern) noexcept {
    switch (pattern) {
    case ActionPattern::Single: return L"Single";
    case ActionPattern::Double: return L"Double";
    case ActionPattern::Triple: return L"Triple";
    case ActionPattern::Burst: return L"Burst";
    case ActionPattern::Hold: return L"Hold";
    }
    return L"Unknown";
}

const wchar_t* BackendText(const InputBackend backend) noexcept {
    switch (backend) {
    case InputBackend::Automatic: return L"Automatic";
    case InputBackend::StandardInput: return L"Standard Input";
    case InputBackend::ForegroundTargetInput: return L"Foreground Target Input";
    case InputBackend::TargetedWindowMessages: return L"Targeted Window Messages";
    case InputBackend::UnicodeTextInput: return L"Unicode Text Input";
    case InputBackend::TargetedUnicodeText: return L"Targeted Unicode Text";
    }
    return L"Unknown";
}

const wchar_t* RandomStyleText(const RandomIntervalStyle style) noexcept {
    switch (style) {
    case RandomIntervalStyle::Independent: return L"Independent";
    case RandomIntervalStyle::Drifting: return L"Drifting";
    case RandomIntervalStyle::Natural: return L"Natural variation";
    }
    return L"Unknown";
}

const wchar_t* DownDurationBehaviorText(
    const DownDurationBehavior behavior) noexcept {
    switch (behavior) {
    case DownDurationBehavior::Fixed: return L"Fixed";
    case DownDurationBehavior::NaturalConfiguredCenter:
        return L"Natural (configured center)";
    case DownDurationBehavior::NaturalAutomaticCenter:
        return L"Natural (automatic center)";
    }
    return L"Unknown";
}

const wchar_t* ProcessPriorityText(const ProcessPriorityMode mode) noexcept {
    switch (mode) {
    case ProcessPriorityMode::SystemDefault: return L"System default";
    case ProcessPriorityMode::AboveNormalWhileActive: return L"Above Normal while active";
    case ProcessPriorityMode::AboveNormal: return L"Above Normal";
    case ProcessPriorityMode::HighWhileActive: return L"High while active";
    case ProcessPriorityMode::High: return L"High";
    }
    return L"Unknown";
}

const wchar_t* TimingWorkerPriorityText(const TimingWorkerPriorityMode mode) noexcept {
    switch (mode) {
    case TimingWorkerPriorityMode::SystemDefault: return L"System default";
    case TimingWorkerPriorityMode::AboveNormal: return L"Above Normal (+1)";
    case TimingWorkerPriorityMode::Highest: return L"Highest (+2)";
    }
    return L"Unknown";
}

const wchar_t* HotkeyPriorityText(const HotkeyControlPriorityMode mode) noexcept {
    switch (mode) {
    case HotkeyControlPriorityMode::SystemDefault: return L"System default";
    case HotkeyControlPriorityMode::AboveNormal: return L"Above Normal (+1)";
    }
    return L"Unknown";
}

const wchar_t* QosText(const TimingWorkerQosMode mode) noexcept {
    switch (mode) {
    case TimingWorkerQosMode::SystemManaged: return L"System managed";
    case TimingWorkerQosMode::HighQoS: return L"High performance (HighQoS)";
    case TimingWorkerQosMode::EcoQoS: return L"Efficiency (EcoQoS)";
    }
    return L"Unknown";
}

const wchar_t* ArchitectureText(const DiagnosticArchitecture architecture) noexcept {
    switch (architecture) {
    case DiagnosticArchitecture::X86: return L"x86";
    case DiagnosticArchitecture::X64: return L"x64";
    case DiagnosticArchitecture::Arm: return L"ARM";
    case DiagnosticArchitecture::Arm64: return L"ARM64";
    case DiagnosticArchitecture::Unknown: return L"Unknown";
    }
    return L"Unknown";
}

const wchar_t* HostOsText(const DiagnosticHostOs host) noexcept {
    switch (host) {
    case DiagnosticHostOs::Linux: return L"Linux";
    case DiagnosticHostOs::MacOs: return L"macOS / Darwin";
    case DiagnosticHostOs::FreeBsd: return L"FreeBSD";
    case DiagnosticHostOs::OtherUnix: return L"Unix-like host";
    case DiagnosticHostOs::Unknown: return L"Unknown";
    case DiagnosticHostOs::NotApplicable: return L"Not applicable";
    }
    return L"Unknown";
}

const wchar_t* TargetElevationText(const DiagnosticTargetElevation elevation) noexcept {
    switch (elevation) {
    case DiagnosticTargetElevation::Standard: return L"Standard";
    case DiagnosticTargetElevation::Elevated: return L"Elevated";
    case DiagnosticTargetElevation::Unknown: return L"Unknown";
    case DiagnosticTargetElevation::NotApplicable: return L"Not applicable";
    }
    return L"Unknown";
}

std::wstring PrivilegeRelationshipText(const bool application_elevated,
                                       const DiagnosticTargetElevation target_elevation) {
    if (target_elevation == DiagnosticTargetElevation::NotApplicable) {
        return L"Not applicable";
    }
    if (target_elevation == DiagnosticTargetElevation::Unknown) {
        return L"Unknown";
    }
    if (target_elevation == DiagnosticTargetElevation::Elevated && !application_elevated) {
        return L"Target elevated, Vector Click standard";
    }
    return L"Compatible";
}

std::wstring KeyName(const HotkeyBinding binding) {
    const std::uint16_t key = binding.virtual_key;
    if (key == 0) {
        return L"Unassigned";
    }
    std::wstring name;
    if (key >= L'A' && key <= L'Z') {
        name.assign(1, static_cast<wchar_t>(key));
    } else if (key >= L'0' && key <= L'9') {
        name.assign(1, static_cast<wchar_t>(key));
    } else if (key >= VkF1 && key <= VkF24) {
        name = L"F" + std::to_wstring(key - VkF1 + 1U);
    } else if (key >= VkNumpad0 && key <= VkNumpad9) {
        name = L"Numpad " + std::to_wstring(key - VkNumpad0);
    } else {
        switch (key) {
        case VkBack: name = L"Backspace"; break;
        case VkTab: name = L"Tab"; break;
        case VkReturn: name = L"Enter"; break;
        case VkEscape: name = L"Escape"; break;
        case VkSpace: name = L"Space"; break;
        case VkPrior: name = L"Page Up"; break;
        case VkNext: name = L"Page Down"; break;
        case VkEnd: name = L"End"; break;
        case VkHome: name = L"Home"; break;
        case VkLeft: name = L"Left Arrow"; break;
        case VkUp: name = L"Up Arrow"; break;
        case VkRight: name = L"Right Arrow"; break;
        case VkDown: name = L"Down Arrow"; break;
        case VkInsert: name = L"Insert"; break;
        case VkDelete: name = L"Delete"; break;
        case VkMultiply: name = L"Numpad Multiply"; break;
        case VkAdd: name = L"Numpad Add"; break;
        case VkSubtract: name = L"Numpad Subtract"; break;
        case VkDecimal: name = L"Numpad Decimal"; break;
        case VkDivide: name = L"Numpad Divide"; break;
        case VkOem1: name = L"OEM ;/:"; break;
        case VkOemPlus: name = L"OEM =/+"; break;
        case VkOemComma: name = L"OEM ,/<"; break;
        case VkOemMinus: name = L"OEM -/_"; break;
        case VkOemPeriod: name = L"OEM ./>"; break;
        case VkOem2: name = L"OEM //?"; break;
        case VkOem3: name = L"OEM `/~"; break;
        case VkOem4: name = L"OEM [/{"; break;
        case VkOem5: name = L"OEM \\/|"; break;
        case VkOem6: name = L"OEM ]/}"; break;
        case VkOem7: name = L"OEM '/\""; break;
        default: name = L"Virtual key " + std::to_wstring(key); break;
        }
    }
    if ((binding.modifiers & KeyModifierShift) != 0) {
        return L"Shift + " + name;
    }
    return name;
}

// Report sections below describe the current setup and the most recent run
// without exposing transient implementation details or unsupported raw state.
void AddTarget(std::wstring& output,
               const bool required,
               const bool selected,
               const bool available,
               const bool foreground,
               const DiagnosticTargetElevation elevation,
               const bool application_elevated,
               const bool foreground_label_is_start) {
    if (!required && !selected) {
        return;
    }
    output.append(L"\r\n[Target]\r\n");
    AddBoolLine(output, L"Required by effective input method", required);
    AddBoolLine(output, L"Selected", selected);
    AddBoolLine(output, L"Available", available);
    if (required && available) {
        AddBoolLine(output,
                    foreground_label_is_start ? L"Foreground at run start" : L"Currently foreground",
                    foreground);
        AddLine(output, L"Elevation", TargetElevationText(elevation));
        AddLine(output,
                L"Privilege relationship",
                PrivilegeRelationshipText(application_elevated, elevation));
    }
    AddLine(output, L"Target identity", L"Intentionally omitted");
}

void AddSettings(std::wstring& output,
                 const RunSettings& settings,
                 const InputBackend effective_backend) {
    AddLine(output, L"Action", ActionTypeText(settings.action_type));
    AddLine(output, L"Pattern", PatternText(settings.action_pattern));
    if (settings.action_pattern == ActionPattern::Burst) {
        AddLine(output, L"Burst count", std::to_wstring(settings.burst_count));
    }

    if (settings.action_type == ActionType::MouseClick) {
        AddLine(output, L"Mouse button", MouseButtonText(settings.mouse_button));
        if (settings.position_mode == PositionMode::FixedScreen) {
            AddLine(output,
                    L"Position",
                    L"Fixed screen, X=" + std::to_wstring(settings.fixed_x) +
                        L", Y=" + std::to_wstring(settings.fixed_y));
        } else {
            AddLine(output, L"Position", L"Current cursor");
        }
    } else {
        if (effective_backend == InputBackend::UnicodeTextInput ||
            effective_backend == InputBackend::TargetedUnicodeText) {
            AddLine(output, L"Generated character", L"Content intentionally omitted");
        } else {
            AddLine(output,
                    L"Generated key",
                    KeyName({settings.generated_virtual_key, settings.generated_key_modifiers}));
        }
    }

    AddLine(output, L"Configured input method", BackendText(settings.backend));
    AddLine(output, L"Effective input method", BackendText(effective_backend));
    AddBoolLine(output, L"Allow background input", settings.allow_background_input);

    if (settings.randomize_interval) {
        AddLine(output, L"Interval mode", L"Randomized");
        AddLine(output, L"Random interval style", RandomStyleText(settings.random_interval_style));
        AddLine(output, L"Minimum interval", FormatMillisecondsWide(settings.minimum_interval_microseconds));
        AddLine(output, L"Maximum interval", FormatMillisecondsWide(settings.maximum_interval_microseconds));
    } else {
        AddLine(output, L"Interval", FormatMillisecondsWide(settings.interval_microseconds));
    }

    if (settings.action_pattern != ActionPattern::Hold) {
        AddLine(output, L"Down duration behavior",
                DownDurationBehaviorText(settings.down_duration_behavior));
        if (settings.down_duration_behavior !=
            DownDurationBehavior::NaturalAutomaticCenter) {
            AddLine(output, L"Button / key down duration",
                    FormatMillisecondsWide(settings.button_down_microseconds));
        } else {
            AddLine(output, L"Button / key down duration", L"Automatic natural center");
        }
    }
    if (settings.action_pattern == ActionPattern::Double ||
        settings.action_pattern == ActionPattern::Triple ||
        settings.action_pattern == ActionPattern::Burst) {
        AddLine(output, L"Action spacing", FormatMillisecondsWide(settings.action_spacing_microseconds));
    }

    if (settings.repeat_mode == RepeatMode::Limited) {
        AddLine(output, L"Repeat", L"Limited, " + std::to_wstring(settings.repeat_count) + L" actions");
    } else {
        AddLine(output, L"Repeat", L"Unlimited");
    }
    AddLine(output,
            L"Run time limit",
            FormatRunTime(settings.run_time_limit_components, settings.run_time_limit_microseconds));

    AddLine(output, L"Process priority", ProcessPriorityText(settings.process_priority_mode));
    AddLine(output, L"Timing worker priority", TimingWorkerPriorityText(settings.timing_worker_priority_mode));
    AddLine(output, L"Timing worker QoS", QosText(settings.timing_worker_qos_mode));
    AddLine(output, L"Hotkey / control priority", HotkeyPriorityText(settings.hotkey_control_priority_mode));
}

const wchar_t* OutcomeText(const DiagnosticRunOutcome outcome) noexcept {
    switch (outcome) {
    case DiagnosticRunOutcome::InProgress: return L"In progress";
    case DiagnosticRunOutcome::BackendFailure: return L"Backend failure";
    case DiagnosticRunOutcome::RepeatLimit: return L"Repeat limit reached";
    case DiagnosticRunOutcome::TimeLimit: return L"Time limit reached";
    case DiagnosticRunOutcome::EmergencyStop: return L"Emergency Stop requested";
    case DiagnosticRunOutcome::NormalStop: return L"Normal Stop requested";
    case DiagnosticRunOutcome::Faulted: return L"Faulted";
    case DiagnosticRunOutcome::EndedUnclassified: return L"Ended, no more specific reason was recorded";
    }
    return L"Unknown";
}

} // namespace

// Assemble a deterministic, human-readable support snapshot. Section order
// and labels are part of the diagnostic report contract and should remain
// stable unless the public report format is intentionally revised.
std::wstring FormatDiagnosticReport(const DiagnosticReportInput& input) {
    std::wstring output;
    output.reserve(3'500);
    output += L"Vector Click Diagnostic Report\r\n";
    AddLine(output, L"Report format", L"3");
    AddLine(output, L"Vector Click version", input.application_version.empty() ? L"Unavailable" : input.application_version);
    AddLine(output, L"Build configuration", input.release_build ? L"Release" : L"Debug / development");

    output += L"\r\n[Environment]\r\n";
    if (input.environment.wine_detected) {
        AddLine(output, L"Runtime", L"Windows compatibility layer");
        AddLine(output, L"Compatibility layer", L"Wine");
        AddLine(output,
                L"Wine version",
                input.environment.wine_version.empty() ? L"Unavailable" : input.environment.wine_version);
        AddLine(output, L"Host operating system", HostOsText(input.environment.host_os));
    } else {
        AddLine(output, L"Runtime", L"Windows API runtime, no Wine compatibility layer detected");
    }
    if (input.environment.windows_version_available) {
        AddLine(output,
                input.environment.wine_detected ? L"Windows compatibility version" : L"Windows API-reported version",
                L"NT " + std::to_wstring(input.environment.windows_major) + L"." +
                    std::to_wstring(input.environment.windows_minor) + L", build " +
                    std::to_wstring(input.environment.windows_build));
    } else {
        AddLine(output,
                input.environment.wine_detected ? L"Windows compatibility version" : L"Windows API-reported version",
                L"Unavailable");
    }
    AddLine(output, L"Native architecture", ArchitectureText(input.environment.native_architecture));
    AddLine(output, L"Vector Click architecture", ArchitectureText(input.environment.application_architecture));
    AddLine(output, L"Vector Click elevation", input.environment.application_elevated ? L"Administrator" : L"Standard");
    if (input.environment.ui_scaling_percent != 0U) {
        AddLine(output, L"UI scaling", std::to_wstring(input.environment.ui_scaling_percent) + L"%");
    } else {
        AddLine(output, L"UI scaling", L"Unavailable");
    }

    output += L"\r\n[Current Setup]\r\n";
    AddLine(output, L"Engine state", EngineStateText(input.current.engine_state));
    AddLine(output, L"Start readiness", ReadinessText(input.current.readiness));
    if (input.current.settings_available) {
        AddSettings(output, input.current.settings, input.current.effective_backend);
    } else {
        AddLine(output, L"Settings details", L"Not included because the current settings are invalid or unavailable");
    }
    AddBoolLine(output, L"Named local profile active", input.current.named_profile_active);
    if (input.current.named_profile_active) {
        AddBoolLine(output, L"Named local profile modified", input.current.named_profile_modified);
    }
    AddTarget(output,
              input.current.target_required,
              input.current.target_selected,
              input.current.target_available,
              input.current.target_foreground,
              input.current.target_elevation,
              input.environment.application_elevated,
              false);

    output += L"\r\n[Safety]\r\n";
    AddBoolLine(output, L"Safety hotkeys ready", input.current.safety_hotkeys_ready);
    if (input.current.settings_available) {
        AddLine(output, L"Start / Stop hotkey", KeyName(input.current.settings.start_stop_hotkey));
        AddLine(output, L"Emergency Stop hotkey", KeyName(input.current.settings.emergency_hotkey));
        AddBoolLine(output, L"Safety Shield enabled", input.current.settings.show_safety_shield);
        AddBoolLine(output, L"Force exit on Emergency Stop",
                    input.current.settings.force_exit_on_emergency_stop);
    }
    AddBoolLine(output, L"Tracked input requiring release", input.current.tracked_input);
    AddBoolLine(output, L"Cleanup required", input.current.cleanup_required);

    output += L"\r\n[";
    output += input.run.active ? L"Active Run" : L"Last Run";
    output += L"]\r\n";
    if (!input.run.available) {
        output += L"No run has occurred since Vector Click started.\r\n";
        return output;
    }

    AddLine(output, L"Outcome", OutcomeText(input.run.outcome));
    AddSettings(output, input.run.settings, input.run.effective_backend);
    AddTarget(output,
              input.run.target_required,
              input.run.target_selected,
              input.run.target_available,
              input.run.target_foreground_at_start,
              input.run.target_elevation,
              input.environment.application_elevated,
              true);
    output += L"\r\n[Run Measurements]\r\n";
    AddLine(output, L"Completed actions", std::to_wstring(input.run.completed_actions));
    AddLine(output, L"Generated inputs", std::to_wstring(input.run.generated_inputs));
    AddLine(output, L"Elapsed", FixedDecimal(input.run.elapsed_seconds, 2) + L" s");
    AddLine(output, L"Action rate", FixedDecimal(input.run.actual_actions_per_second, 2) + L"/s");
    AddLine(output, L"Input rate", FixedDecimal(input.run.actual_inputs_per_second, 2) + L"/s");
    AddBoolLine(output, L"Tracked input requiring release", input.run.tracked_input);
    AddBoolLine(output, L"Cleanup required", input.run.cleanup_required);
    return output;
}

} // namespace vectorclick::core

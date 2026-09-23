#pragma once

#include "Core/settings.h"

#include <cstdint>
#include <string>

namespace vectorclick::core {

enum class DiagnosticEngineState : std::uint8_t {
    Ready,
    Running,
    Stopping,
    Disarmed,
    Faulted,
};

enum class DiagnosticReadiness : std::uint8_t {
    Ready,
    Running,
    Stopping,
    EmergencyActive,
    ShutdownActive,
    CleanupRequired,
    HotkeysUnavailable,
    SettingsInvalid,
    TargetUnavailable,
    AdministratorConfirmationRequired,
    TargetNotForeground,
    TargetMinimized,
    Faulted,
    Unavailable,
};

enum class DiagnosticRunOutcome : std::uint8_t {
    InProgress,
    BackendFailure,
    RepeatLimit,
    TimeLimit,
    EmergencyStop,
    NormalStop,
    Faulted,
    EndedUnclassified,
};

enum class DiagnosticTargetElevation : std::uint8_t {
    NotApplicable,
    Unknown,
    Standard,
    Elevated,
};

enum class DiagnosticHostOs : std::uint8_t {
    NotApplicable,
    Unknown,
    Linux,
    MacOs,
    FreeBsd,
    OtherUnix,
};

enum class DiagnosticArchitecture : std::uint8_t {
    Unknown,
    X86,
    X64,
    Arm,
    Arm64,
};

struct DiagnosticEnvironment {
    bool wine_detected{};
    std::wstring wine_version;
    DiagnosticHostOs host_os{DiagnosticHostOs::NotApplicable};
    bool windows_version_available{};
    std::uint32_t windows_major{};
    std::uint32_t windows_minor{};
    std::uint32_t windows_build{};
    DiagnosticArchitecture native_architecture{DiagnosticArchitecture::Unknown};
    DiagnosticArchitecture application_architecture{DiagnosticArchitecture::Unknown};
    bool application_elevated{};
    std::uint32_t ui_scaling_percent{};
};

struct DiagnosticSetup {
    DiagnosticEngineState engine_state{DiagnosticEngineState::Ready};
    DiagnosticReadiness readiness{DiagnosticReadiness::Unavailable};
    bool settings_available{};
    RunSettings settings{};
    InputBackend effective_backend{InputBackend::StandardInput};
    bool target_required{};
    bool target_selected{};
    bool target_available{};
    bool target_foreground{};
    DiagnosticTargetElevation target_elevation{DiagnosticTargetElevation::NotApplicable};
    bool named_profile_active{};
    bool named_profile_modified{};
    bool safety_hotkeys_ready{};
    bool cleanup_required{};
    bool tracked_input{};
};

struct DiagnosticRun {
    bool available{};
    bool active{};
    RunSettings settings{};
    InputBackend effective_backend{InputBackend::StandardInput};
    bool target_required{};
    bool target_selected{};
    bool target_available{};
    bool target_foreground_at_start{};
    DiagnosticTargetElevation target_elevation{DiagnosticTargetElevation::NotApplicable};
    std::uint64_t completed_actions{};
    std::uint64_t generated_inputs{};
    double elapsed_seconds{};
    double actual_actions_per_second{};
    double actual_inputs_per_second{};
    DiagnosticRunOutcome outcome{DiagnosticRunOutcome::EndedUnclassified};
    bool cleanup_required{};
    bool tracked_input{};
};

struct DiagnosticReportInput {
    std::wstring application_version;
    bool release_build{};
    DiagnosticEnvironment environment{};
    DiagnosticSetup current{};
    DiagnosticRun run{};
};

[[nodiscard]] std::wstring FormatDiagnosticReport(const DiagnosticReportInput& input);

} // namespace vectorclick::core

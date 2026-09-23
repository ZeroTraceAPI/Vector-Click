#pragma once

#include "Core/settings.h"

#include <cstdint>
#include <string>

namespace vectorclick::win {

enum class PresentationEngineState : std::uint8_t {
    Ready,
    Running,
    Stopping,
    Disarmed,
    Faulted,
};

enum class StatusCategory : std::uint8_t {
    Ready,
    Running,
    Transition,
    Attention,
    NotReady,
};

struct RatePresentationInput {
    core::ActionPattern action_pattern{core::ActionPattern::Single};
    bool keyboard{};
    bool interval_valid{};
    std::uint64_t interval_microseconds{};
    bool randomize_interval{};
    bool minimum_interval_valid{};
    std::uint64_t minimum_interval_microseconds{};
    bool maximum_interval_valid{};
    std::uint64_t maximum_interval_microseconds{};
    bool down_duration_valid{};
    std::uint64_t down_duration_microseconds{};
    bool action_spacing_valid{};
    std::uint64_t action_spacing_microseconds{};
    bool burst_count_valid{};
    std::uint32_t burst_count{};
    bool run_time_limit_valid{true};
    std::uint64_t run_time_limit_microseconds{};

    friend bool operator==(const RatePresentationInput&, const RatePresentationInput&) = default;
};

struct RatePresentation {
    std::wstring text;
    std::wstring tooltip;

    friend bool operator==(const RatePresentation&, const RatePresentation&) = default;
};

struct StartAvailabilityInput {
    PresentationEngineState engine_state{PresentationEngineState::Ready};
    bool emergency_active{};
    bool safety_hotkeys_ready{true};
    std::wstring safety_hotkey_reason;
    bool settings_valid{};
    std::wstring validation_error;
    bool target_ready{true};
    bool previous_initialized{};
    bool previous_available{};
    std::wstring previous_unavailable_reason;
    bool cleanup_required{};

    friend bool operator==(const StartAvailabilityInput&, const StartAvailabilityInput&) = default;
};

struct StartAvailabilityPresentation {
    bool start_enabled{};
    bool update_cached_availability{};
    bool initialized{};
    bool available{};
    std::wstring unavailable_reason;
    bool availability_changed{};
    bool attention_required{};

    friend bool operator==(const StartAvailabilityPresentation&,
                           const StartAvailabilityPresentation&) = default;
};

struct StatusPresentationInput {
    PresentationEngineState engine_state{PresentationEngineState::Ready};
    StartAvailabilityPresentation availability;
    bool diagnostics_enabled{};
    bool keyboard{};
    core::ActionPattern action_pattern{core::ActionPattern::Single};
    std::uint64_t completed_actions{};
    std::wstring completion_note;

    friend bool operator==(const StatusPresentationInput&, const StatusPresentationInput&) = default;
};

struct StatusPresentation {
    StatusCategory category{StatusCategory::Ready};
    std::wstring text;
    std::wstring tooltip;

    friend bool operator==(const StatusPresentation&, const StatusPresentation&) = default;
};

struct DiagnosticsPresentationInput {
    PresentationEngineState engine_state{PresentationEngineState::Ready};
    StartAvailabilityPresentation availability;
    bool keyboard{};
    core::ActionPattern action_pattern{core::ActionPattern::Single};
    std::uint64_t completed_actions{};
    std::uint64_t generated_inputs{};
    double elapsed_seconds{};
    double actual_actions_per_second{};
    double actual_inputs_per_second{};
    std::wstring completion_note;

    friend bool operator==(const DiagnosticsPresentationInput&,
                           const DiagnosticsPresentationInput&) = default;
};

struct DiagnosticsPresentation {
    StatusCategory category{StatusCategory::Ready};
    std::wstring text;

    friend bool operator==(const DiagnosticsPresentation&,
                           const DiagnosticsPresentation&) = default;
};

struct PresentationSnapshot {
    bool diagnostics_visible{};
    RatePresentation rate;
    StartAvailabilityPresentation availability;
    StatusPresentation status;
    DiagnosticsPresentation diagnostics;

    friend bool operator==(const PresentationSnapshot&, const PresentationSnapshot&) = default;
};

[[nodiscard]] RatePresentation BuildRatePresentation(const RatePresentationInput& input);
[[nodiscard]] StartAvailabilityPresentation EvaluateStartAvailability(
    const StartAvailabilityInput& input);
[[nodiscard]] StatusPresentation BuildStatusPresentation(const StatusPresentationInput& input);
[[nodiscard]] DiagnosticsPresentation BuildDiagnosticsPresentation(
    const DiagnosticsPresentationInput& input);
[[nodiscard]] StatusCategory StatusCategoryForEngineState(
    PresentationEngineState state) noexcept;
[[nodiscard]] bool RequiresCleanupRecovery(
    PresentationEngineState state, bool has_tracked_input) noexcept;
[[nodiscard]] std::wstring StateText(PresentationEngineState state);
[[nodiscard]] std::wstring ActionUnitText(core::ActionPattern pattern, bool keyboard);

} // namespace vectorclick::win

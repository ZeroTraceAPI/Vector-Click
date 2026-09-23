#pragma once

#include <cstdint>
#include <string>

namespace vectorclick::core {

enum class ActionType : std::uint8_t {
    MouseClick,
    KeyboardPress,
};

enum class MouseButton : std::uint8_t {
    Left,
    Right,
    Middle,
    X1,
    X2,
};

enum class PositionMode : std::uint8_t {
    CurrentCursor,
    FixedScreen,
};

enum class RepeatMode : std::uint8_t {
    Unlimited,
    Limited,
};

enum class RandomIntervalStyle : std::uint8_t {
    Independent,
    Drifting,
    Natural,
};

enum class DownDurationBehavior : std::uint8_t {
    Fixed,
    NaturalConfiguredCenter,
    NaturalAutomaticCenter,
};

enum class ActionPattern : std::uint8_t {
    Single,
    Double,
    Triple,
    Burst,
    Hold,
};

enum class ProcessPriorityMode : std::uint8_t {
    SystemDefault,
    AboveNormalWhileActive,
    AboveNormal,
    HighWhileActive,
    High,
};

// Experimental relative priority for the long-lived timing / input worker.
// SystemDefault leaves the worker thread's existing relative priority alone.
// The elevated modes are applied only for a run and restored afterward.
enum class TimingWorkerPriorityMode : std::uint8_t {
    SystemDefault,
    AboveNormal,
    Highest,
};

// Experimental relative priority for the dedicated global hotkey / control
// thread. The current scope intentionally exposes only a modest +1 boost.
enum class HotkeyControlPriorityMode : std::uint8_t {
    SystemDefault,
    AboveNormal,
};

// Experimental Quality of Service policy for the long-lived timing / input
// worker. SystemManaged leaves Windows responsible for QoS classification.
// HighQoS explicitly opts that worker out of execution-speed throttling while
// EcoQoS explicitly opts it in. QoS and relative / process priority remain
// independent scheduling dimensions.
enum class TimingWorkerQosMode : std::uint8_t {
    SystemManaged,
    HighQoS,
    EcoQoS,
};

// Optional user-facing feedback for run lifecycle events. Each channel is
// independently selectable so notification banners and sounds do not imply
// one another. Missing persisted values default safely to Off.
enum class RunFeedbackMode : std::uint8_t {
    Off,
    Started,
    Stopped,
    StartedAndStopped,
};

[[nodiscard]] constexpr bool RunFeedbackIncludesStarted(
    const RunFeedbackMode mode) noexcept {
    return mode == RunFeedbackMode::Started ||
           mode == RunFeedbackMode::StartedAndStopped;
}

[[nodiscard]] constexpr bool RunFeedbackIncludesStopped(
    const RunFeedbackMode mode) noexcept {
    return mode == RunFeedbackMode::Stopped ||
           mode == RunFeedbackMode::StartedAndStopped;
}

inline constexpr std::uint32_t MinimumBurstCount = 2;
inline constexpr std::uint32_t MaximumBurstCount = 100;
inline constexpr std::uint64_t MaximumRepeatCount = 1'000'000;

inline constexpr std::uint64_t MaximumTimeComponent = 1'000'000;
inline constexpr std::uint64_t MicrosecondsPerMillisecond = 1'000;
inline constexpr std::uint64_t MicrosecondsPerSecond = 1'000'000;
inline constexpr std::uint64_t MicrosecondsPerMinute = 60'000'000;
inline constexpr std::uint64_t MicrosecondsPerHour = 3'600'000'000;
inline constexpr std::uint64_t MaximumMillisecondsComponentMicroseconds =
    MaximumTimeComponent * MicrosecondsPerMillisecond;
inline constexpr std::uint64_t MaximumCombinedDurationMicroseconds =
    MaximumTimeComponent * MicrosecondsPerMinute +
    MaximumTimeComponent * MicrosecondsPerSecond +
    MaximumMillisecondsComponentMicroseconds;
inline constexpr std::uint64_t MaximumRunTimeLimitMicroseconds =
    MaximumTimeComponent * MicrosecondsPerHour +
    MaximumTimeComponent * MicrosecondsPerMinute +
    MaximumTimeComponent * MicrosecondsPerSecond;

struct DurationComponents {
    std::uint32_t minutes{};
    std::uint32_t seconds{};
    // Milliseconds retain up to three decimal places by storing the visible
    // millisecond component as exact microseconds. 10.5 ms is therefore 10,500.
    std::uint64_t milliseconds_microseconds{};

    friend bool operator==(const DurationComponents&,
                           const DurationComponents&) = default;
};

struct RunTimeLimitComponents {
    std::uint32_t hours{};
    std::uint32_t minutes{};
    std::uint32_t seconds{};

    friend bool operator==(const RunTimeLimitComponents&,
                           const RunTimeLimitComponents&) = default;
};

// Runtime input protection. VectorClick may display a higher configured rate,
// but the worker will not submit more than this many complete clicks or key
// presses per second. Each generated input still includes an ordered press and
// release, and Stop / Emergency Stop remain checked between submissions.
inline constexpr std::uint32_t MaximumGeneratedInputsPerSecond = 10'000;

// Unlimited runs discard complete actions whose final scheduled input is more
// than this far behind. This keeps system stalls or deliberately excessive
// settings from creating a long catch-up flood. Limited runs remain exact and
// finish their requested action count at the safely paced rate.
inline constexpr std::uint64_t MaximumUnlimitedScheduleLagMicroseconds = 100'000;

enum class InputBackend : std::uint8_t {
    Automatic,
    StandardInput,
    ForegroundTargetInput,
    TargetedWindowMessages,
    UnicodeTextInput,
    TargetedUnicodeText,
};

inline constexpr std::uint16_t KeyModifierShift = 0x0004;
inline constexpr std::uint16_t SupportedKeyModifiers = KeyModifierShift;

struct HotkeyBinding {
    std::uint16_t virtual_key{};
    std::uint16_t modifiers{};

    [[nodiscard]] bool IsAssigned() const noexcept { return virtual_key != 0; }
    friend bool operator==(const HotkeyBinding&, const HotkeyBinding&) = default;
};

struct RunSettings {
    ActionType action_type{ActionType::MouseClick};
    MouseButton mouse_button{MouseButton::Left};
    std::uint16_t generated_virtual_key{0x41}; // A
    std::uint16_t generated_key_modifiers{};
    InputBackend backend{InputBackend::Automatic};
    PositionMode position_mode{PositionMode::CurrentCursor};
    std::int32_t fixed_x{0};
    std::int32_t fixed_y{0};
    std::uint64_t interval_microseconds{10'000};
    DurationComponents interval_components{0, 0, 10'000};
    bool randomize_interval{false};
    RandomIntervalStyle random_interval_style{RandomIntervalStyle::Independent};
    std::uint64_t minimum_interval_microseconds{10'000};
    DurationComponents minimum_interval_components{0, 0, 10'000};
    std::uint64_t maximum_interval_microseconds{100'000};
    DurationComponents maximum_interval_components{0, 0, 100'000};
    std::uint64_t button_down_microseconds{1'000};
    DurationComponents button_down_components{0, 0, 1'000};
    DownDurationBehavior down_duration_behavior{DownDurationBehavior::Fixed};
    ActionPattern action_pattern{ActionPattern::Single};
    std::uint32_t burst_count{4};
    std::uint64_t action_spacing_microseconds{10'000};
    DurationComponents action_spacing_components{0, 0, 10'000};
    RepeatMode repeat_mode{RepeatMode::Unlimited};
    std::uint64_t repeat_count{100};
    std::uint64_t run_time_limit_microseconds{};
    RunTimeLimitComponents run_time_limit_components{};

    HotkeyBinding start_stop_hotkey{0x74, 0}; // F5
    HotkeyBinding emergency_hotkey{0x77, 0};  // F8

    RunFeedbackMode windows_notification_mode{RunFeedbackMode::Off};
    RunFeedbackMode system_sound_mode{RunFeedbackMode::Off};
    bool show_running_indicator{false};
    bool enable_live_diagnostics{false};
    bool show_safety_shield{false};
    bool force_exit_on_emergency_stop{false};
    bool hide_from_screen_capture{false};
    bool keep_window_on_top{false};
    bool remember_settings{false};
    bool allow_background_input{false};
    bool show_click_position_indicator{false};
    ProcessPriorityMode process_priority_mode{ProcessPriorityMode::SystemDefault};
    TimingWorkerPriorityMode timing_worker_priority_mode{
        TimingWorkerPriorityMode::SystemDefault};
    HotkeyControlPriorityMode hotkey_control_priority_mode{
        HotkeyControlPriorityMode::SystemDefault};
    TimingWorkerQosMode timing_worker_qos_mode{
        TimingWorkerQosMode::SystemManaged};

    friend bool operator==(const RunSettings&, const RunSettings&) = default;
};

[[nodiscard]] RunSettings DefaultRunSettings() noexcept;
[[nodiscard]] InputBackend ResolveInputBackend(InputBackend requested,
                                               bool has_selected_target,
                                               bool allow_background_input) noexcept;
[[nodiscard]] std::string FormatMilliseconds(std::uint64_t microseconds);
[[nodiscard]] bool ComposeDurationMicroseconds(
    const DurationComponents& components,
    std::uint64_t& total_microseconds) noexcept;
[[nodiscard]] bool DecomposeDurationMicroseconds(
    std::uint64_t total_microseconds,
    DurationComponents& components) noexcept;
[[nodiscard]] bool DurationComponentsMatch(
    const DurationComponents& components,
    std::uint64_t total_microseconds) noexcept;
[[nodiscard]] bool ComposeRunTimeLimitMicroseconds(
    const RunTimeLimitComponents& components,
    std::uint64_t& total_microseconds) noexcept;
[[nodiscard]] bool DecomposeRunTimeLimitMicroseconds(
    std::uint64_t total_microseconds,
    RunTimeLimitComponents& components) noexcept;
[[nodiscard]] bool RunTimeLimitComponentsMatch(
    const RunTimeLimitComponents& components,
    std::uint64_t total_microseconds) noexcept;
[[nodiscard]] std::uint32_t InputsPerAction(const RunSettings& settings) noexcept;
[[nodiscard]] bool SamePhysicalKey(const HotkeyBinding& left,
                                   const HotkeyBinding& right) noexcept;

} // namespace vectorclick::core

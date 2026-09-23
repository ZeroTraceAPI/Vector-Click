#include "Windows/click_worker.h"

#include "Core/natural_down_duration.h"
#include "Core/timing.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vectorclick::win {
namespace {

UniqueHandle CreateHighResolutionTimer() noexcept {
    HANDLE timer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    return UniqueHandle(timer);
}

std::int64_t QueryCounter() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t MicrosecondsToTicks(const std::uint64_t microseconds, const std::int64_t frequency) noexcept {
    const long double ticks = static_cast<long double>(microseconds) *
                              static_cast<long double>(frequency) / 1'000'000.0L;
    if (ticks >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (ticks <= 1.0L) {
        return 1;
    }
    return static_cast<std::int64_t>(ticks);
}

struct InputScheduleStream {
    std::uint64_t action_index{};
    std::int64_t deadline{};
    std::uint32_t input_index{};
    bool active{true};
    core::IntervalRandomGenerator interval_random{};
};

std::uint64_t MakeIntervalRandomSeed(const std::int64_t started_at,
                                     const std::uint64_t session,
                                     const std::uint64_t salt = 0) noexcept {
    const std::uint64_t process_thread =
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U) |
        static_cast<std::uint64_t>(GetCurrentThreadId());
    core::IntervalRandomGenerator mixer(
        static_cast<std::uint64_t>(started_at) ^
        (session * 0xD1342543DE82EF95ULL) ^ process_thread ^ salt);
    return mixer.NextInclusive(0, std::numeric_limits<std::uint64_t>::max());
}

std::uint64_t NextActionIntervalMicroseconds(
    InputScheduleStream& stream,
    const core::RunSettings& settings) noexcept {
    if (!settings.randomize_interval) {
        return settings.interval_microseconds;
    }

    switch (settings.random_interval_style) {
    case core::RandomIntervalStyle::Independent:
        return stream.interval_random.NextInclusive(
            settings.minimum_interval_microseconds,
            settings.maximum_interval_microseconds);
    case core::RandomIntervalStyle::Drifting:
        return stream.interval_random.NextDriftingInclusive(
            settings.minimum_interval_microseconds,
            settings.maximum_interval_microseconds);
    case core::RandomIntervalStyle::Natural:
        if (settings.action_type == core::ActionType::KeyboardPress) {
            return stream.interval_random.NextNaturalKeyboardInclusive(
                settings.minimum_interval_microseconds,
                settings.maximum_interval_microseconds);
        }
        return stream.interval_random.NextNaturalInclusive(
            settings.minimum_interval_microseconds,
            settings.maximum_interval_microseconds);
    }
    return settings.interval_microseconds;
}

std::uint64_t DownDurationTempoReferenceMicroseconds(
    const core::RunSettings& settings) noexcept {
    if (!settings.randomize_interval) {
        return settings.interval_microseconds;
    }
    if (settings.maximum_interval_microseconds <
        settings.minimum_interval_microseconds) {
        return settings.minimum_interval_microseconds;
    }
    return settings.minimum_interval_microseconds +
           (settings.maximum_interval_microseconds -
            settings.minimum_interval_microseconds) /
               2U;
}

std::uint64_t NaturalDownTimingLimitMicroseconds(
    const core::RunSettings& settings,
    const std::uint32_t inputs_per_action,
    const std::uint64_t upcoming_interval_microseconds) noexcept {
    std::uint64_t limit = upcoming_interval_microseconds;
    if (limit == 0U) {
        limit = DownDurationTempoReferenceMicroseconds(settings);
    }
    if (inputs_per_action > 1U && settings.action_spacing_microseconds > 0U) {
        limit = limit == 0U
                    ? settings.action_spacing_microseconds
                    : std::min(limit, settings.action_spacing_microseconds);
    }
    return limit;
}

bool ScheduleStreamComesFirst(const InputScheduleStream& candidate,
                              const InputScheduleStream& current) noexcept {
    if (candidate.deadline != current.deadline) {
        return candidate.deadline < current.deadline;
    }
    if (candidate.action_index != current.action_index) {
        return candidate.action_index < current.action_index;
    }
    return candidate.input_index < current.input_index;
}

int ThreadPriorityRank(const int priority) noexcept {
    switch (priority) {
    case THREAD_PRIORITY_IDLE:
        return 0;
    case THREAD_PRIORITY_LOWEST:
        return 1;
    case THREAD_PRIORITY_BELOW_NORMAL:
        return 2;
    case THREAD_PRIORITY_NORMAL:
        return 3;
    case THREAD_PRIORITY_ABOVE_NORMAL:
        return 4;
    case THREAD_PRIORITY_HIGHEST:
        return 5;
    case THREAD_PRIORITY_TIME_CRITICAL:
        return 6;
    default:
        return -1;
    }
}

int ProcessPriorityRank(const DWORD priority_class) noexcept {
    switch (priority_class) {
    case IDLE_PRIORITY_CLASS:
        return 0;
    case BELOW_NORMAL_PRIORITY_CLASS:
        return 1;
    case NORMAL_PRIORITY_CLASS:
        return 2;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return 3;
    case HIGH_PRIORITY_CLASS:
        return 4;
    case REALTIME_PRIORITY_CLASS:
        return 5;
    default:
        return -1;
    }
}

int RequestedTimingWorkerPriority(
    const core::TimingWorkerPriorityMode mode) noexcept {
    switch (mode) {
    case core::TimingWorkerPriorityMode::SystemDefault:
        return THREAD_PRIORITY_NORMAL;
    case core::TimingWorkerPriorityMode::AboveNormal:
        return THREAD_PRIORITY_ABOVE_NORMAL;
    case core::TimingWorkerPriorityMode::Highest:
        return THREAD_PRIORITY_HIGHEST;
    }
    return THREAD_PRIORITY_ERROR_RETURN;
}


} // namespace

ClickWorker::ClickWorker(StateCallback callback,
                         MousePointCallback mouse_point_callback,
                         SessionReadyCallback session_ready_callback)
    : callback_(std::move(callback)),
      mouse_point_callback_(std::move(mouse_point_callback)),
      session_ready_callback_(std::move(session_ready_callback)),
      shutdown_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      start_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      emergency_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      timer_(CreateHighResolutionTimer()) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency_ = frequency.QuadPart;

    thread_ = std::thread([this] { ThreadMain(); });
}

ClickWorker::~ClickWorker() {
    running_.store(false, std::memory_order_release);
    session_id_.fetch_add(1, std::memory_order_acq_rel);
    if (shutdown_event_) {
        SetEvent(shutdown_event_.get());
    }
    if (stop_event_) {
        SetEvent(stop_event_.get());
    }
    if (emergency_event_) {
        SetEvent(emergency_event_.get());
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    (void)ReleaseAll();
}

bool ClickWorker::PrepareTimingWorkerPriority(
    const core::TimingWorkerPriorityMode mode) noexcept {
    if (!thread_.joinable()) {
        return false;
    }

    std::scoped_lock lock(timing_worker_priority_mutex_);
    timing_worker_priority_active_ = false;
    timing_worker_priority_owns_ = false;
    timing_worker_priority_baseline_ = THREAD_PRIORITY_NORMAL;
    timing_worker_priority_last_applied_ = THREAD_PRIORITY_NORMAL;
    HANDLE worker_handle = thread_.native_handle();
    SetLastError(ERROR_SUCCESS);
    const int actual_before = GetThreadPriority(worker_handle);
    if (actual_before == THREAD_PRIORITY_ERROR_RETURN) {
        return false;
    }
    timing_worker_priority_active_ = true;

    if (mode == core::TimingWorkerPriorityMode::SystemDefault) {
        return true;
    }

    const DWORD process_priority = GetPriorityClass(GetCurrentProcess());
    if (process_priority == 0U) {
        timing_worker_priority_active_ = false;
        return false;
    }

    // Keep the experimental setting inside the deliberate base-priority
    // envelope even if an external program changes the process class. +2 is
    // allowed only through Above Normal process priority (base <= 12). +1 is
    // allowed through High (base <= 14). Realtime never receives a relative
    // worker boost.
    const int process_rank = ProcessPriorityRank(process_priority);
    if ((mode == core::TimingWorkerPriorityMode::Highest && process_rank > 3) ||
        (mode == core::TimingWorkerPriorityMode::AboveNormal && process_rank > 4) ||
        process_rank < 0) {
        timing_worker_priority_active_ = false;
        return false;
    }

    const int requested = RequestedTimingWorkerPriority(mode);
    if (ThreadPriorityRank(actual_before) >= ThreadPriorityRank(requested)) {
        // Respect an equal or higher externally established relative thread
        // priority rather than lowering it to satisfy the selected setting.
        return true;
    }

    if (SetThreadPriority(worker_handle, requested) == FALSE) {
        timing_worker_priority_active_ = false;
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const int actual_after = GetThreadPriority(worker_handle);
    if (actual_after == THREAD_PRIORITY_ERROR_RETURN) {
        (void)SetThreadPriority(worker_handle, actual_before);
        timing_worker_priority_active_ = false;
        return false;
    }

    timing_worker_priority_baseline_ = actual_before;
    timing_worker_priority_last_applied_ = actual_after;
    timing_worker_priority_owns_ = true;
    return true;
}


void ClickWorker::RestoreTimingWorkerPriority() noexcept {
    std::scoped_lock lock(timing_worker_priority_mutex_);
    if (!timing_worker_priority_active_) {
        return;
    }

    const int terminal = GetThreadPriority(GetCurrentThread());
    if (terminal != THREAD_PRIORITY_ERROR_RETURN && timing_worker_priority_owns_) {
        if (terminal == timing_worker_priority_last_applied_) {
            (void)SetThreadPriority(
                GetCurrentThread(), timing_worker_priority_baseline_);
        } else {
            // An external tool changed the worker after Vector Click applied
            // its value. Do not overwrite that newer decision during cleanup.
            timing_worker_priority_owns_ = false;
        }
    }

    timing_worker_priority_active_ = false;
    timing_worker_priority_owns_ = false;
    timing_worker_priority_baseline_ = THREAD_PRIORITY_NORMAL;
    timing_worker_priority_last_applied_ = THREAD_PRIORITY_NORMAL;
}

bool ClickWorker::PrepareTimingWorkerQos(
    const core::TimingWorkerQosMode mode) noexcept {
    if (!thread_.joinable()) {
        return false;
    }

    std::scoped_lock lock(timing_worker_qos_mutex_);
    timing_worker_qos_active_ = false;

    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    if (mode == core::TimingWorkerQosMode::HighQoS) {
        // Explicit HighQoS controls execution-speed throttling and disables
        // that mechanism for this worker.
        state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = 0;
    } else if (mode == core::TimingWorkerQosMode::EcoQoS) {
        // EcoQoS controls the same mechanism and enables it so Windows may
        // favor efficient execution for this worker.
        state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    } else {
        // ControlMask = 0 returns QoS / power-throttling classification to the
        // Windows heuristic rather than forcing an explicit QoS level.
        state.ControlMask = 0;
        state.StateMask = 0;
    }

    SetLastError(ERROR_SUCCESS);
    if (SetThreadInformation(
            thread_.native_handle(),
            ThreadPowerThrottling,
            &state,
            sizeof(state)) == FALSE) {
        return false;
    }

    timing_worker_qos_active_ = true;
    return true;
}


void ClickWorker::RestoreTimingWorkerQos() noexcept {
    std::scoped_lock lock(timing_worker_qos_mutex_);
    if (timing_worker_qos_active_) {
        THREAD_POWER_THROTTLING_STATE state{};
        state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = 0;
        state.StateMask = 0;
        (void)SetThreadInformation(
            thread_.native_handle(),
            ThreadPowerThrottling,
            &state,
            sizeof(state));
    }
    timing_worker_qos_active_ = false;
}

bool ClickWorker::Start(const core::RunSettings& settings,
                        const TargetWindowInfo& target) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);

    const auto state = state_.load(std::memory_order_acquire);
    if (state != EngineState::Ready && state != EngineState::Disarmed) {
        return false;
    }

    if (tracked_input_.load(std::memory_order_acquire)) {
        return false;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }

    try {
        std::scoped_lock settings_lock(settings_mutex_);
        pending_settings_ = settings;
        pending_target_ = target;
    } catch (...) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    if (!PrepareTimingWorkerPriority(settings.timing_worker_priority_mode)) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    if (!PrepareTimingWorkerQos(settings.timing_worker_qos_mode)) {
        RestoreTimingWorkerPriority();
        running_.store(false, std::memory_order_release);
        return false;
    }

    completed_actions_.store(0, std::memory_order_relaxed);
    generated_inputs_.store(0, std::memory_order_relaxed);
    backend_failure_.store(false, std::memory_order_release);
    run_limit_stop_reason_.store(RunLimitStopReason::None, std::memory_order_release);
    session_start_qpc_.store(0, std::memory_order_relaxed);
    session_end_qpc_.store(0, std::memory_order_relaxed);
    ResetEvent(stop_event_.get());
    ResetEvent(emergency_event_.get());
    session_id_.fetch_add(1, std::memory_order_acq_rel);
    if (SetEvent(start_event_.get()) == FALSE) {
        running_.store(false, std::memory_order_release);
        RestoreTimingWorkerQos();
        RestoreTimingWorkerPriority();
        return false;
    }
    return true;
}


DiagnosticsSnapshot ClickWorker::Diagnostics() const noexcept {
    DiagnosticsSnapshot snapshot{};
    snapshot.completed_actions = completed_actions_.load(std::memory_order_relaxed);
    snapshot.generated_inputs = generated_inputs_.load(std::memory_order_relaxed);

    const auto start = session_start_qpc_.load(std::memory_order_acquire);
    if (start <= 0 || qpc_frequency_ <= 0) {
        return snapshot;
    }

    auto end = session_end_qpc_.load(std::memory_order_acquire);
    if (end <= start || running_.load(std::memory_order_acquire)) {
        end = QueryCounter();
    }
    if (end <= start) {
        return snapshot;
    }

    snapshot.elapsed_seconds = static_cast<double>(end - start) /
                               static_cast<double>(qpc_frequency_);
    if (snapshot.elapsed_seconds > 0.0) {
        snapshot.actual_actions_per_second = static_cast<double>(snapshot.completed_actions) /
                                             snapshot.elapsed_seconds;
        snapshot.actual_inputs_per_second = static_cast<double>(snapshot.generated_inputs) /
                                            snapshot.elapsed_seconds;
    }
    return snapshot;
}

bool ClickWorker::RequestStop() noexcept {
    bool was_running = false;
    {
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        was_running = running_.load(std::memory_order_acquire);
        if (was_running) {
            // Publish Stopping before cancellation becomes observable. This keeps
            // the worker's later Ready notification terminal and prevents a late
            // Stopping notification from overwriting it.
            PublishState(EngineState::Stopping);
            running_.store(false, std::memory_order_release);
            session_id_.fetch_add(1, std::memory_order_acq_rel);
            SetEvent(stop_event_.get());
            if (timer_) {
                CancelWaitableTimer(timer_.get());
            }
        }
    }
    return was_running;
}

bool ClickWorker::EmergencyStop() noexcept {
    const bool was_running = RequestEmergencyStop();
    return CompleteEmergencyStop(was_running);
}

bool ClickWorker::RequestEmergencyStop() noexcept {
    bool was_running = false;
    {
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        was_running = running_.load(std::memory_order_acquire);
        if (was_running) {
            // Publish Stopping before release cleanup so the terminal Disarmed
            // notification remains the final state observed by the UI.
            PublishState(EngineState::Stopping);
        }
        running_.store(false, std::memory_order_release);
        session_id_.fetch_add(1, std::memory_order_acq_rel);
        SetEvent(emergency_event_.get());
        if (timer_) {
            CancelWaitableTimer(timer_.get());
        }
    }
    return was_running;
}

bool ClickWorker::CompleteEmergencyStop(const bool was_running) noexcept {
    const bool releases_submitted = ReleaseAll();
    if (!was_running) {
        PublishState(EngineState::Disarmed);
    }
    return releases_submitted;
}

bool ClickWorker::HasTrackedInput() const noexcept {
    return tracked_input_.load(std::memory_order_acquire);
}

bool ClickWorker::RetryCleanup() noexcept {
    return ReleaseAll();
}

void ClickWorker::ThreadMain() noexcept {
    if (!shutdown_event_ || !start_event_ || !stop_event_ || !emergency_event_ || !timer_ || qpc_frequency_ <= 0) {
        PublishState(EngineState::Faulted);
        return;
    }

    HANDLE idle_handles[]{shutdown_event_.get(), start_event_.get()};

    for (;;) {
        const DWORD result = WaitForMultipleObjects(2, idle_handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            break;
        }
        if (result != WAIT_OBJECT_0 + 1) {
            PublishState(EngineState::Faulted);
            break;
        }


        core::RunSettings settings;
        TargetWindowInfo target;
        {
            std::scoped_lock lock(settings_mutex_);
            settings = pending_settings_;
            target = std::move(pending_target_);
        }

        std::uint64_t session = 0;
        {
            std::scoped_lock lifecycle_lock(lifecycle_mutex_);
            const bool emergency_requested =
                WaitForSingleObject(emergency_event_.get(), 0) == WAIT_OBJECT_0;
            const bool stop_requested =
                WaitForSingleObject(stop_event_.get(), 0) == WAIT_OBJECT_0;

            if (!running_.load(std::memory_order_acquire) ||
                emergency_requested || stop_requested) {
                RestoreTimingWorkerQos();
                RestoreTimingWorkerPriority();
                PublishState(emergency_requested
                                 ? EngineState::Disarmed
                                 : EngineState::Ready);
                continue;
            }

            session = session_id_.load(std::memory_order_acquire);
            PublishState(EngineState::Running);
        }
        RunSession(settings, std::move(target), session);
    }

    (void)ReleaseAll();
}

void ClickWorker::RunSession(const core::RunSettings settings,
                             TargetWindowInfo target,
                             const std::uint64_t session) noexcept {
    const core::InputBackend effective_backend =
        InputBackendDispatcher::EffectiveBackend(
            settings.backend, target, settings.allow_background_input);

    {
        std::scoped_lock lock(input_gate_);
        if (!backend_.BeginSession(settings, target)) {
            tracked_input_.store(backend_.HasTrackedInput(), std::memory_order_release);
            backend_failure_.store(true, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            RestoreTimingWorkerQos();
            RestoreTimingWorkerPriority();
            std::scoped_lock lifecycle_lock(lifecycle_mutex_);
            PublishState(WaitForSingleObject(emergency_event_.get(), 0) == WAIT_OBJECT_0
                             ? EngineState::Disarmed
                             : EngineState::Faulted);
            return;
        }
        tracked_input_.store(backend_.HasTrackedInput(), std::memory_order_release);
    }

    // EngineState::Running is published before backend setup so the existing UI
    // transition and control gating remain unchanged. Run feedback needs a
    // stricter event: announce readiness only after the backend session exists,
    // and only while this same run has not already been cancelled.
    const bool session_still_active =
        running_.load(std::memory_order_acquire) &&
        session_id_.load(std::memory_order_acquire) == session &&
        WaitForSingleObject(stop_event_.get(), 0) != WAIT_OBJECT_0 &&
        WaitForSingleObject(emergency_event_.get(), 0) != WAIT_OBJECT_0;
    if (session_still_active && session_ready_callback_) {
        session_ready_callback_();
    }

    const auto started_at = QueryCounter();
    session_start_qpc_.store(started_at, std::memory_order_release);
    session_end_qpc_.store(0, std::memory_order_release);
    const bool run_time_limit_enabled = settings.run_time_limit_microseconds > 0U;
    const std::int64_t run_limit_deadline = run_time_limit_enabled
        ? core::SaturatingAddTime(
              started_at,
              MicrosecondsToTicks(
                  settings.run_time_limit_microseconds, qpc_frequency_))
        : std::numeric_limits<std::int64_t>::max();

    if (settings.action_pattern == core::ActionPattern::Hold) {
        bool emergency = false;
        const bool press_accepted = SubmitPress(settings, session);
        if (press_accepted) {
            generated_inputs_.store(1, std::memory_order_relaxed);
            const bool target_guarded =
                effective_backend == core::InputBackend::ForegroundTargetInput ||
                effective_backend == core::InputBackend::TargetedWindowMessages;
            const bool background_allowed =
                effective_backend == core::InputBackend::TargetedWindowMessages &&
                settings.allow_background_input;
            const WaitResult wait_result = WaitForHoldEnd(
                target_guarded, background_allowed, target, run_limit_deadline);
            emergency = wait_result == WaitResult::Emergency;
            if (wait_result == WaitResult::RunLimit) {
                run_limit_stop_reason_.store(
                    RunLimitStopReason::TimeLimit, std::memory_order_release);
            } else if (wait_result == WaitResult::Error) {
                backend_failure_.store(true, std::memory_order_release);
            }

            // Stop, Emergency Stop, target loss, shutdown, and worker cleanup
            // all use the same backend ledger. ReleaseAll is idempotent and
            // guarantees that a generated hold is never intentionally left down.
            (void)ReleaseAll();
            if (wait_result == WaitResult::Stop ||
                wait_result == WaitResult::Emergency ||
                wait_result == WaitResult::RunLimit) {
                completed_actions_.store(1, std::memory_order_relaxed);
            }
        } else if (WaitForSingleObject(emergency_event_.get(), 0) == WAIT_OBJECT_0) {
            emergency = true;
        }

        const std::int64_t work_end_qpc = QueryCounter();
        session_end_qpc_.store(work_end_qpc, std::memory_order_release);
        (void)ReleaseAll();
        RestoreTimingWorkerQos();
        RestoreTimingWorkerPriority();
        running_.store(false, std::memory_order_release);
        {
            std::scoped_lock lifecycle_lock(lifecycle_mutex_);
            PublishState(emergency ||
                             WaitForSingleObject(emergency_event_.get(), 0) == WAIT_OBJECT_0
                             ? EngineState::Disarmed
                             : EngineState::Ready);
        }
        return;
    }

    const auto action_interval_ticks = MicrosecondsToTicks(
        settings.interval_microseconds, qpc_frequency_);
    const auto fixed_down_ticks = settings.button_down_microseconds == 0
                                      ? std::int64_t{0}
                                      : MicrosecondsToTicks(
                                            settings.button_down_microseconds,
                                            qpc_frequency_);
    const auto action_spacing_ticks = settings.action_spacing_microseconds == 0
                                          ? std::int64_t{0}
                                          : MicrosecondsToTicks(
                                                settings.action_spacing_microseconds, qpc_frequency_);
    const std::uint32_t inputs_per_action = core::InputsPerAction(settings);
    const std::uint64_t safety_spacing_microseconds =
        (1'000'000ULL + core::MaximumGeneratedInputsPerSecond - 1ULL) /
        core::MaximumGeneratedInputsPerSecond;
    const auto safety_spacing_ticks = MicrosecondsToTicks(
        safety_spacing_microseconds, qpc_frequency_);
    const auto maximum_lag_ticks = MicrosecondsToTicks(
        core::MaximumUnlimitedScheduleLagMicroseconds, qpc_frequency_);
    const auto final_input_offset_ticks = core::SaturatingMultiplyTime(
        action_spacing_ticks,
        inputs_per_action > 0
            ? static_cast<std::uint64_t>(inputs_per_action - 1U)
            : 0U);

    // Each input position inside an action is an independent periodic stream.
    // This keeps memory bounded to at most 100 streams while allowing a new
    // Double, Triple, or Burst action to begin at every Input interval. The
    // streams are merged by deadline, with older actions winning ties.
    const std::uint64_t interval_random_seed =
        MakeIntervalRandomSeed(started_at, session);
    constexpr std::uint64_t natural_down_seed_salt =
        0x8CB92BA72F3D8DD7ULL;
    const std::uint64_t natural_down_seed =
        MakeIntervalRandomSeed(started_at, session, natural_down_seed_salt);
    core::NaturalDownDurationGenerator natural_down_random(natural_down_seed);
    const std::uint64_t down_tempo_reference_microseconds =
        DownDurationTempoReferenceMicroseconds(settings);
    std::vector<InputScheduleStream> streams;
    streams.reserve(static_cast<std::size_t>(inputs_per_action));
    for (std::uint32_t input_index = 0; input_index < inputs_per_action; ++input_index) {
        streams.push_back({
            0,
            core::ScheduledInputDeadline(
                started_at, action_interval_ticks, action_spacing_ticks, 0, input_index),
            input_index,
            true,
            core::IntervalRandomGenerator(interval_random_seed)});
    }

    std::int64_t next_submission_deadline = started_at;
    bool emergency = false;

    while (running_.load(std::memory_order_acquire) &&
           session_id_.load(std::memory_order_acquire) == session) {
        if (settings.repeat_mode == core::RepeatMode::Unlimited) {
            const auto now = QueryCounter();
            if (!settings.randomize_interval) {
                const auto first_action_to_keep = core::FirstUnlimitedActionToKeep(
                    now,
                    started_at,
                    action_interval_ticks,
                    final_input_offset_ticks,
                    maximum_lag_ticks);
                if (first_action_to_keep > 0) {
                    for (auto& stream : streams) {
                        if (stream.active && stream.action_index < first_action_to_keep) {
                            stream.action_index = first_action_to_keep;
                            stream.deadline = core::ScheduledInputDeadline(
                                started_at,
                                action_interval_ticks,
                                action_spacing_ticks,
                                stream.action_index,
                                stream.input_index);
                        }
                    }
                }
            } else if (now > maximum_lag_ticks) {
                const InputScheduleStream* final_stream = nullptr;
                for (const auto& candidate : streams) {
                    if (candidate.active &&
                        candidate.input_index + 1U == inputs_per_action) {
                        final_stream = &candidate;
                        break;
                    }
                }
                const auto oldest_allowed_deadline = now - maximum_lag_ticks;
                if (final_stream != nullptr &&
                    final_stream->deadline < oldest_allowed_deadline) {
                    std::uint64_t next_action_index = 0;
                    for (const auto& stream : streams) {
                        if (stream.active) {
                            next_action_index = std::max(
                                next_action_index, stream.action_index);
                        }
                    }
                    if (next_action_index <
                        std::numeric_limits<std::uint64_t>::max()) {
                        ++next_action_index;
                    }

                    const std::uint64_t replacement_seed =
                        MakeIntervalRandomSeed(
                            started_at,
                            session,
                            static_cast<std::uint64_t>(now) ^ next_action_index);
                    const std::uint64_t replacement_down_seed =
                        replacement_seed ^ natural_down_seed_salt;
                    for (auto& stream : streams) {
                        if (!stream.active) {
                            continue;
                        }
                        stream.action_index = next_action_index;
                        stream.deadline = core::SaturatingAddTime(
                            now,
                            core::SaturatingMultiplyTime(
                                action_spacing_ticks,
                                static_cast<std::uint64_t>(stream.input_index)));
                        stream.interval_random.Reseed(replacement_seed);
                    }
                    natural_down_random.Reseed(replacement_down_seed);
                }
            }
        }

        std::size_t earliest_index = streams.size();
        for (std::size_t index = 0; index < streams.size(); ++index) {
            if (!streams[index].active) {
                continue;
            }
            if (earliest_index == streams.size() ||
                ScheduleStreamComesFirst(streams[index], streams[earliest_index])) {
                earliest_index = index;
            }
        }
        if (earliest_index == streams.size()) {
            break;
        }

        auto& stream = streams[earliest_index];
        const auto press_deadline = std::max(
            stream.deadline, next_submission_deadline);
        const auto before_press = WaitUntil(press_deadline, run_limit_deadline);
        if (before_press != WaitResult::Deadline) {
            emergency = before_press == WaitResult::Emergency;
            if (before_press == WaitResult::RunLimit) {
                run_limit_stop_reason_.store(
                    RunLimitStopReason::TimeLimit, std::memory_order_release);
            }
            break;
        }
        if (run_time_limit_enabled && QueryCounter() >= run_limit_deadline) {
            run_limit_stop_reason_.store(
                RunLimitStopReason::TimeLimit, std::memory_order_release);
            break;
        }

        const bool natural_down_active =
            settings.down_duration_behavior != core::DownDurationBehavior::Fixed;
        const bool has_next_action =
            settings.repeat_mode == core::RepeatMode::Unlimited ||
            stream.action_index + 1U < settings.repeat_count;
        std::uint64_t next_interval_microseconds = 0U;
        if (natural_down_active) {
            if (settings.randomize_interval && has_next_action) {
                // Natural Down conditions the current press on the already
                // established upcoming interval. Cache that same interval here;
                // the separate Down RNG never consumes interval RNG state.
                next_interval_microseconds =
                    NextActionIntervalMicroseconds(stream, settings);
            } else if (!settings.randomize_interval) {
                next_interval_microseconds = settings.interval_microseconds;
            }
        }

        std::uint64_t down_microseconds = settings.button_down_microseconds;
        core::RunSettings press_settings = settings;
        if (natural_down_active) {
            const std::uint64_t timing_limit_microseconds =
                NaturalDownTimingLimitMicroseconds(
                    settings,
                    inputs_per_action,
                    next_interval_microseconds);
            const bool keyboard_profile =
                settings.action_type == core::ActionType::KeyboardPress;
            if (settings.down_duration_behavior ==
                core::DownDurationBehavior::NaturalAutomaticCenter) {
                down_microseconds = keyboard_profile
                    ? natural_down_random.NextAutomaticKeyboard(
                          timing_limit_microseconds,
                          down_tempo_reference_microseconds,
                          next_interval_microseconds)
                    : natural_down_random.NextAutomatic(
                          timing_limit_microseconds,
                          down_tempo_reference_microseconds,
                          next_interval_microseconds);
            } else {
                down_microseconds = keyboard_profile
                    ? natural_down_random.NextConfiguredKeyboard(
                          settings.button_down_microseconds,
                          timing_limit_microseconds,
                          down_tempo_reference_microseconds,
                          next_interval_microseconds)
                    : natural_down_random.NextConfigured(
                          settings.button_down_microseconds,
                          timing_limit_microseconds,
                          down_tempo_reference_microseconds,
                          next_interval_microseconds);
            }
            press_settings.button_down_microseconds = down_microseconds;
        }
        const std::int64_t current_down_ticks =
            settings.down_duration_behavior == core::DownDurationBehavior::Fixed
                ? fixed_down_ticks
                : (down_microseconds == 0U
                       ? std::int64_t{0}
                       : MicrosecondsToTicks(
                             down_microseconds, qpc_frequency_));

        if (!SubmitPress(press_settings, session)) {
            break;
        }

        const auto pressed_at = QueryCounter();
        next_submission_deadline = core::SaturatingAddTime(
            pressed_at, safety_spacing_ticks);

        // Anchor release timing to the scheduled press phase. Measuring a new
        // full duration from pressed_at makes ordinary SendInput and wake-up
        // overhead accumulate after every click, which reduces a 1 ms / 1 ms
        // Single run to roughly 630 CPS. A late wake may shorten this nominal
        // duration slightly, but the absolute cadence remains stable and the
        // existing backlog and safety ceilings still bound recovery behavior.
        const auto release_deadline =
            core::ScheduledReleaseDeadline(press_deadline, current_down_ticks);
        const auto release_result = WaitUntil(release_deadline, run_limit_deadline);
        const bool released = SubmitRelease(press_settings, session);
        if (release_result != WaitResult::Deadline || !released) {
            emergency = release_result == WaitResult::Emergency;
            if (release_result == WaitResult::RunLimit) {
                run_limit_stop_reason_.store(
                    RunLimitStopReason::TimeLimit, std::memory_order_release);
            }
            break;
        }
        if (run_time_limit_enabled && QueryCounter() >= run_limit_deadline) {
            run_limit_stop_reason_.store(
                RunLimitStopReason::TimeLimit, std::memory_order_release);
            break;
        }

        generated_inputs_.fetch_add(1, std::memory_order_relaxed);
        if (stream.input_index + 1U == inputs_per_action) {
            completed_actions_.fetch_add(1, std::memory_order_relaxed);
        }

        ++stream.action_index;
        if (settings.repeat_mode == core::RepeatMode::Limited &&
            stream.action_index >= settings.repeat_count) {
            stream.active = false;
            const bool all_streams_complete = std::none_of(
                streams.begin(), streams.end(),
                [](const InputScheduleStream& candidate) {
                    return candidate.active;
                });
            if (all_streams_complete) {
                run_limit_stop_reason_.store(
                    RunLimitStopReason::RepeatLimit, std::memory_order_release);
                break;
            }
        } else if (settings.randomize_interval) {
            if (!natural_down_active) {
                // Preserve Fixed Down behavior when Natural Down is off: draw the
                // next random interval only after the current release.
                next_interval_microseconds =
                    NextActionIntervalMicroseconds(stream, settings);
            }
            stream.deadline = core::SaturatingAddTime(
                stream.deadline,
                MicrosecondsToTicks(
                    next_interval_microseconds, qpc_frequency_));
        } else {
            stream.deadline = core::ScheduledInputDeadline(
                started_at,
                action_interval_ticks,
                action_spacing_ticks,
                stream.action_index,
                stream.input_index);
        }
    }

    const std::int64_t work_end_qpc = QueryCounter();
    session_end_qpc_.store(work_end_qpc, std::memory_order_release);
    (void)ReleaseAll();

    const bool emergency_requested =
        emergency || WaitForSingleObject(emergency_event_.get(), 0) == WAIT_OBJECT_0;
    RestoreTimingWorkerQos();
    RestoreTimingWorkerPriority();

    running_.store(false, std::memory_order_release);
    {
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        PublishState(emergency_requested ? EngineState::Disarmed : EngineState::Ready);
    }
}

ClickWorker::WaitResult ClickWorker::WaitForHoldEnd(
    const bool target_guarded,
    const bool allow_background_input,
    const TargetWindowInfo& target,
    const std::int64_t run_limit_deadline) noexcept {
    HANDLE handles[]{shutdown_event_.get(), emergency_event_.get(), stop_event_.get(), timer_.get()};
    for (;;) {
        const auto now = QueryCounter();
        if (run_limit_deadline != std::numeric_limits<std::int64_t>::max()) {
            const auto remaining_ticks = run_limit_deadline - now;
            if (remaining_ticks <= 0) {
                return WaitResult::RunLimit;
            }
            const long double remaining_100ns =
                static_cast<long double>(remaining_ticks) * 10'000'000.0L /
                static_cast<long double>(qpc_frequency_);
            LARGE_INTEGER due_time{};
            due_time.QuadPart = -std::max<LONGLONG>(
                1, static_cast<LONGLONG>(remaining_100ns));
            if (!SetWaitableTimer(timer_.get(), &due_time, 0, nullptr, nullptr, FALSE)) {
                return WaitResult::Error;
            }
        } else if (timer_) {
            CancelWaitableTimer(timer_.get());
        }

        const DWORD handle_count =
            run_limit_deadline == std::numeric_limits<std::int64_t>::max()
                ? 3U
                : 4U;
        const DWORD result = WaitForMultipleObjects(
            handle_count, handles, FALSE, 100);
        switch (result) {
        case WAIT_OBJECT_0:
            return WaitResult::Shutdown;
        case WAIT_OBJECT_0 + 1:
            return WaitResult::Emergency;
        case WAIT_OBJECT_0 + 2:
            return WaitResult::Stop;
        case WAIT_OBJECT_0 + 3:
            return WaitResult::RunLimit;
        case WAIT_TIMEOUT:
            if (target_guarded &&
                (!IsTargetWindowValid(target) ||
                 (!allow_background_input && !IsTargetWindowForeground(target)))) {
                return WaitResult::Error;
            }
            break;
        default:
            return WaitResult::Error;
        }
    }
}

ClickWorker::WaitResult ClickWorker::WaitUntil(
    const std::int64_t qpc_deadline,
    const std::int64_t run_limit_deadline) noexcept {
    for (;;) {
        if (!running_.load(std::memory_order_acquire)) {
            if (WaitForSingleObject(emergency_event_.get(), 0) == WAIT_OBJECT_0) {
                return WaitResult::Emergency;
            }
            return WaitResult::Stop;
        }

        const bool run_limit_first =
            run_limit_deadline != std::numeric_limits<std::int64_t>::max() &&
            run_limit_deadline <= qpc_deadline;
        const std::int64_t effective_deadline =
            run_limit_first ? run_limit_deadline : qpc_deadline;
        const auto now = QueryCounter();
        const auto remaining_ticks = effective_deadline - now;
        if (remaining_ticks <= 0) {
            return run_limit_first ? WaitResult::RunLimit : WaitResult::Deadline;
        }

        const long double remaining_100ns =
            static_cast<long double>(remaining_ticks) * 10'000'000.0L /
            static_cast<long double>(qpc_frequency_);
        LARGE_INTEGER due_time{};
        due_time.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(remaining_100ns));

        if (!SetWaitableTimer(timer_.get(), &due_time, 0, nullptr, nullptr, FALSE)) {
            return WaitResult::Error;
        }

        HANDLE handles[]{shutdown_event_.get(), emergency_event_.get(), stop_event_.get(), timer_.get()};
        const DWORD result = WaitForMultipleObjects(4, handles, FALSE, INFINITE);
        switch (result) {
        case WAIT_OBJECT_0:
            return WaitResult::Shutdown;
        case WAIT_OBJECT_0 + 1:
            return WaitResult::Emergency;
        case WAIT_OBJECT_0 + 2:
            return WaitResult::Stop;
        case WAIT_OBJECT_0 + 3:
            return run_limit_first ? WaitResult::RunLimit : WaitResult::Deadline;
        default:
            return WaitResult::Error;
        }
    }
}

bool ClickWorker::SubmitPress(const core::RunSettings& settings, const std::uint64_t session) noexcept {
    std::scoped_lock lock(input_gate_);
    const InputSessionCancellation cancellation{&running_, &session_id_, session};
    if (cancellation.Requested()) {
        return false;
    }

    const bool submitted = backend_.Press(settings, cancellation);
    tracked_input_.store(backend_.HasTrackedInput(), std::memory_order_release);
    const bool cancelled = cancellation.Requested();
    if (!submitted && !cancelled) {
        backend_failure_.store(true, std::memory_order_release);
    }

    const bool accepted = submitted && !cancelled;
    if (accepted &&
        settings.action_type == core::ActionType::MouseClick &&
        settings.show_click_position_indicator &&
        mouse_point_callback_) {
        ScreenPoint point{};
        if (backend_.LastMousePressScreenPoint(point)) {
            try {
                mouse_point_callback_(point);
            } catch (...) {
                // Indicator reporting must never interfere with input timing.
            }
        }
    }
    return accepted;
}

bool ClickWorker::SubmitRelease(const core::RunSettings& settings,
                                const std::uint64_t session) noexcept {
    std::scoped_lock lock(input_gate_);
    const InputSessionCancellation cancellation{&running_, &session_id_, session};
    const bool released = backend_.Release(settings);
    tracked_input_.store(backend_.HasTrackedInput(), std::memory_order_release);
    const bool cancelled = cancellation.Requested();
    if (!released && !cancelled) {
        backend_failure_.store(true, std::memory_order_release);
    }
    return released && !cancelled;
}

bool ClickWorker::ReleaseAll() noexcept {
    std::scoped_lock lock(input_gate_);
    const bool released = backend_.ReleaseAll();
    tracked_input_.store(backend_.HasTrackedInput(), std::memory_order_release);
    return released;
}

void ClickWorker::PublishState(const EngineState state) noexcept {
    state_.store(state, std::memory_order_release);
    try {
        if (callback_) {
            callback_(state, completed_actions_.load(std::memory_order_relaxed));
        }
    } catch (...) {
        // No exception may escape the input or safety threads.
    }
}

} // namespace vectorclick::win

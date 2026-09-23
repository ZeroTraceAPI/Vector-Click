#pragma once

#include "Core/settings.h"
#include "Windows/input_backend_dispatcher.h"
#include "Windows/target_window.h"
#include "Windows/win32_raii.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace vectorclick::win {

enum class EngineState : std::uint8_t {
    Ready,
    Running,
    Stopping,
    Disarmed,
    Faulted,
};

enum class RunLimitStopReason : std::uint8_t {
    None,
    RepeatLimit,
    TimeLimit,
};

struct DiagnosticsSnapshot {
    std::uint64_t completed_actions{};
    std::uint64_t generated_inputs{};
    double elapsed_seconds{};
    double actual_actions_per_second{};
    double actual_inputs_per_second{};
};

class ClickWorker {
public:
    using StateCallback = std::function<void(EngineState, std::uint64_t)>;
    using MousePointCallback = std::function<void(ScreenPoint)>;
    using SessionReadyCallback = std::function<void()>;

    ClickWorker(StateCallback callback,
                MousePointCallback mouse_point_callback,
                SessionReadyCallback session_ready_callback);
    ~ClickWorker();

    ClickWorker(const ClickWorker&) = delete;
    ClickWorker& operator=(const ClickWorker&) = delete;

    [[nodiscard]] bool Start(const core::RunSettings& settings,
                             const TargetWindowInfo& target);
    // Publishes cancellation and wakes the worker without waiting for a
    // backend release call. Normal UI and global-hotkey Stop paths use this so
    // the hotkey thread remains free to receive Emergency Stop while cleanup
    // continues on the worker thread.
    [[nodiscard]] bool RequestStop() noexcept;
    // Synchronous cancellation plus release cleanup for lifecycle and
    // emergency paths that must not finalize until the release ledger has
    // been attempted. Ordinary Stop uses RequestStop() instead.
    [[nodiscard]] bool EmergencyStop() noexcept;
    [[nodiscard]] bool RequestEmergencyStop() noexcept;
    [[nodiscard]] bool CompleteEmergencyStop(bool was_running) noexcept;
    [[nodiscard]] bool HasTrackedInput() const noexcept;
    [[nodiscard]] bool RetryCleanup() noexcept;

    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] EngineState State() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t CompletedActions() const noexcept {
        return completed_actions_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] DiagnosticsSnapshot Diagnostics() const noexcept;
    [[nodiscard]] bool LastRunHadBackendFailure() const noexcept {
        return backend_failure_.load(std::memory_order_acquire);
    }
    [[nodiscard]] RunLimitStopReason LastRunLimitStopReason() const noexcept {
        return run_limit_stop_reason_.load(std::memory_order_acquire);
    }

private:
    enum class WaitResult {
        Deadline,
        RunLimit,
        Stop,
        Emergency,
        Shutdown,
        Error,
    };

    void ThreadMain() noexcept;
    void RunSession(core::RunSettings settings,
                    TargetWindowInfo target,
                    std::uint64_t session) noexcept;
    [[nodiscard]] WaitResult WaitUntil(std::int64_t qpc_deadline,
                                       std::int64_t run_limit_deadline) noexcept;
    [[nodiscard]] WaitResult WaitForHoldEnd(bool target_guarded,
                                               bool allow_background_input,
                                               const TargetWindowInfo& target,
                                               std::int64_t run_limit_deadline) noexcept;
    [[nodiscard]] bool SubmitPress(const core::RunSettings& settings, std::uint64_t session) noexcept;
    [[nodiscard]] bool SubmitRelease(const core::RunSettings& settings, std::uint64_t session) noexcept;
    [[nodiscard]] bool ReleaseAll() noexcept;
    void PublishState(EngineState state) noexcept;
    [[nodiscard]] bool PrepareTimingWorkerPriority(
        core::TimingWorkerPriorityMode mode) noexcept;
    void RestoreTimingWorkerPriority() noexcept;
    [[nodiscard]] bool PrepareTimingWorkerQos(
        core::TimingWorkerQosMode mode) noexcept;
    void RestoreTimingWorkerQos() noexcept;

    StateCallback callback_;
    MousePointCallback mouse_point_callback_;
    SessionReadyCallback session_ready_callback_;
    std::thread thread_;

    UniqueHandle shutdown_event_;
    UniqueHandle start_event_;
    UniqueHandle stop_event_;
    UniqueHandle emergency_event_;
    UniqueHandle timer_;

    // Serializes Start, Stop, Emergency Stop, and the worker's transition into
    // Running so terminal state notifications cannot be published out of order.
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex settings_mutex_;
    core::RunSettings pending_settings_{};
    TargetWindowInfo pending_target_{};

    std::mutex input_gate_;
    InputBackendDispatcher backend_;
    // Updated under input_gate_ after every backend ledger mutation so UI and
    // controller readiness checks remain lock-free.
    std::atomic<bool> tracked_input_{false};

    std::atomic<bool> running_{false};
    std::atomic<EngineState> state_{EngineState::Ready};
    std::atomic<std::uint64_t> session_id_{0};
    std::atomic<std::uint64_t> completed_actions_{0};
    std::atomic<std::uint64_t> generated_inputs_{0};
    std::atomic<bool> backend_failure_{false};
    std::atomic<RunLimitStopReason> run_limit_stop_reason_{RunLimitStopReason::None};
    std::atomic<std::int64_t> session_start_qpc_{0};
    std::atomic<std::int64_t> session_end_qpc_{0};

    // Experimental relative-priority control for the long-lived timing / input
    // worker. Applied for a run and restored afterward without overwriting a
    // newer external priority decision.
    mutable std::mutex timing_worker_priority_mutex_;
    bool timing_worker_priority_active_{};
    bool timing_worker_priority_owns_{};
    int timing_worker_priority_baseline_{};
    int timing_worker_priority_last_applied_{};

    // Experimental QoS control for the same long-lived worker. System managed
    // returns execution-speed throttling classification to Windows.
    mutable std::mutex timing_worker_qos_mutex_;
    bool timing_worker_qos_active_{};

    std::int64_t qpc_frequency_{};
};

} // namespace vectorclick::win

#pragma once

#include "Core/settings.h"
#include "Windows/win32_raii.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace vectorclick::win {

struct HotkeyRegistrationResult {
    std::uint64_t request_id{};
    bool initial_request{};
    bool requested_pair_registered{};
    bool fallback_used{};
    core::HotkeyBinding requested_start_stop{};
    core::HotkeyBinding requested_emergency{};
    core::HotkeyBinding active_start_stop{};
    core::HotkeyBinding active_emergency{};
    std::wstring error;

    [[nodiscard]] bool HasActivePair() const noexcept {
        return active_start_stop.IsAssigned() && active_emergency.IsAssigned();
    }
};

class HotkeyThread {
public:
    using Callback = std::function<void()>;
    using ErrorCallback = std::function<void()>;
    using RegistrationCallback = std::function<void(HotkeyRegistrationResult)>;

    static constexpr std::uint64_t InitialRequestId = 1;

    HotkeyThread(core::HotkeyBinding start_stop,
                 core::HotkeyBinding emergency,
                 Callback start_stop_callback,
                 Callback emergency_callback,
                 ErrorCallback error_callback,
                 RegistrationCallback registration_callback,
                 core::HotkeyControlPriorityMode priority_mode);
    ~HotkeyThread();

    HotkeyThread(const HotkeyThread&) = delete;
    HotkeyThread& operator=(const HotkeyThread&) = delete;

    // Returns a nonzero request identifier when the request was queued. The
    // result callback reports which pair is actually active after Windows has
    // accepted or rejected the request.
    [[nodiscard]] std::uint64_t Reconfigure(core::HotkeyBinding start_stop,
                                            core::HotkeyBinding emergency);
    // RegisterHotKey matches an exact modifier combination. While a run is
    // active, temporary aliases keep the configured safety keys reachable when
    // Ctrl, Alt, or Shift is additionally held. The aliases exist only across
    // the active run / release-cleanup boundary.
    [[nodiscard]] bool SetActiveRunModifierGuard(bool enabled,
                                                 std::wstring& error) noexcept;
    [[nodiscard]] bool SetPriorityMode(core::HotkeyControlPriorityMode mode) noexcept;
    [[nodiscard]] bool RefreshPriorityPolicy() noexcept;

private:
    struct RegistrationAttempt {
        bool success{};
        std::wstring error;
    };

    void ThreadMain() noexcept;
    void RefreshPriorityPolicyOnThread() noexcept;
    [[nodiscard]] RegistrationAttempt ApplyPairTransactionally(
        core::HotkeyBinding start_stop,
        core::HotkeyBinding emergency) noexcept;
    [[nodiscard]] RegistrationAttempt ApplyActiveRunModifierGuard(bool enabled) noexcept;
    [[nodiscard]] bool RegisterBinding(int id,
                                       core::HotkeyBinding binding,
                                       const wchar_t* role,
                                       std::wstring& error) noexcept;
    [[nodiscard]] int FreeRegistrationId(int first_reserved = 0) const noexcept;
    void UnregisterActiveRunModifierGuard() noexcept;
    void UnregisterPair() noexcept;
    void ReportError() noexcept;
    void ReportRegistration(HotkeyRegistrationResult result) noexcept;

    static constexpr UINT ReconfigureMessage = WM_APP + 100;
    static constexpr UINT ClearRetiredMappingsMessage = WM_APP + 101;
    static constexpr UINT PriorityPolicyMessage = WM_APP + 102;
    static constexpr UINT ActiveRunModifierGuardMessage = WM_APP + 103;
    static constexpr int MinimumRegistrationId = 1;
    static constexpr int MaximumRegistrationId = 32;
    static constexpr std::size_t MaximumGuardAliasesPerRole = 7;

    std::thread thread_;
    std::atomic<DWORD> thread_id_{0};
    UniqueHandle ready_event_;
    std::atomic<bool> shutdown_requested_{false};

    std::mutex desired_mutex_;
    core::HotkeyBinding desired_start_stop_{};
    core::HotkeyBinding desired_emergency_{};
    std::uint64_t desired_request_id_{InitialRequestId};
    std::uint64_t next_request_id_{InitialRequestId + 1};
    std::uint64_t processed_request_id_{};

    core::HotkeyBinding active_start_stop_{};
    core::HotkeyBinding active_emergency_{};
    int active_start_stop_id_{};
    int active_emergency_id_{};
    int retired_start_stop_id_{};
    int retired_emergency_id_{};
    std::array<int, MaximumGuardAliasesPerRole> run_guard_start_stop_ids_{};
    std::array<int, MaximumGuardAliasesPerRole> run_guard_emergency_ids_{};
    std::size_t run_guard_start_stop_count_{};
    std::size_t run_guard_emergency_count_{};
    bool active_run_modifier_guard_active_{};

    UniqueHandle run_guard_event_;
    std::mutex run_guard_mutex_;
    bool desired_run_guard_enabled_{};
    std::uint64_t desired_run_guard_request_id_{};
    std::uint64_t next_run_guard_request_id_{1};
    std::uint64_t processed_run_guard_request_id_{};
    bool run_guard_result_success_{};
    std::wstring run_guard_result_error_;

    Callback start_stop_callback_;
    Callback emergency_callback_;
    ErrorCallback error_callback_;
    RegistrationCallback registration_callback_;

    std::atomic<core::HotkeyControlPriorityMode> desired_priority_mode_{
        core::HotkeyControlPriorityMode::SystemDefault};
    bool priority_owns_{};
    int priority_baseline_{THREAD_PRIORITY_NORMAL};
    int priority_last_applied_{THREAD_PRIORITY_NORMAL};
};

} // namespace vectorclick::win

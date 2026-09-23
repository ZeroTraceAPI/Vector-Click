#include "Windows/hotkey_thread.h"
#include "Windows/safety_hotkey_catalog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace vectorclick::win {
namespace {

[[nodiscard]] bool IsValidPair(const core::HotkeyBinding start_stop,
                               const core::HotkeyBinding emergency) noexcept {
    return IsSupportedSafetyHotkeyPair(start_stop, emergency);
}

[[nodiscard]] bool IsSamePair(const core::HotkeyBinding left_start,
                              const core::HotkeyBinding left_emergency,
                              const core::HotkeyBinding right_start,
                              const core::HotkeyBinding right_emergency) noexcept {
    return left_start == right_start && left_emergency == right_emergency;
}

[[nodiscard]] bool ProcessClassAllowsHotkeyBoost(const DWORD priority_class) noexcept {
    // The experimental +1 hotkey priority remains permitted through High
    // process priority. Realtime remains excluded even when established by an
    // external tool.
    return priority_class != 0 && priority_class != REALTIME_PRIORITY_CLASS;
}


} // namespace

HotkeyThread::HotkeyThread(core::HotkeyBinding start_stop,
                           core::HotkeyBinding emergency,
                           Callback start_stop_callback,
                           Callback emergency_callback,
                           ErrorCallback error_callback,
                           RegistrationCallback registration_callback,
                           const core::HotkeyControlPriorityMode priority_mode)
    : ready_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      desired_start_stop_(start_stop),
      desired_emergency_(emergency),
      run_guard_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      start_stop_callback_(std::move(start_stop_callback)),
      emergency_callback_(std::move(emergency_callback)),
      error_callback_(std::move(error_callback)),
      registration_callback_(std::move(registration_callback)),
      desired_priority_mode_(priority_mode) {
    thread_ = std::thread([this] { ThreadMain(); });
    if (ready_event_) {
        WaitForSingleObject(ready_event_.get(), 5'000);
    }
}

HotkeyThread::~HotkeyThread() {
    // Shutdown must remain authoritative even if destruction races the worker
    // before its Win32 message queue exists. The atomic covers that early
    // window; once thread_id_ is published, the queue is guaranteed to exist.
    shutdown_requested_.store(true, std::memory_order_release);
    const DWORD id = thread_id_.load(std::memory_order_acquire);
    if (id != 0) {
        (void)PostThreadMessageW(id, WM_QUIT, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::uint64_t HotkeyThread::Reconfigure(const core::HotkeyBinding start_stop,
                                        const core::HotkeyBinding emergency) {
    std::uint64_t request_id = 0;
    {
        std::scoped_lock lock(desired_mutex_);
        request_id = next_request_id_++;
        desired_start_stop_ = start_stop;
        desired_emergency_ = emergency;
        desired_request_id_ = request_id;
    }

    const DWORD id = thread_id_.load(std::memory_order_acquire);
    if (id == 0 || PostThreadMessageW(id, ReconfigureMessage, 0, 0) == FALSE) {
        return 0;
    }
    return request_id;
}

bool HotkeyThread::SetActiveRunModifierGuard(const bool enabled,
                                             std::wstring& error) noexcept {
    error.clear();
    if (!run_guard_event_) {
        error = L"The safety-hotkey modifier guard could not create its synchronization event.";
        return false;
    }

    std::uint64_t request_id = 0;
    {
        std::scoped_lock lock(run_guard_mutex_);
        request_id = next_run_guard_request_id_++;
        desired_run_guard_enabled_ = enabled;
        desired_run_guard_request_id_ = request_id;
        run_guard_result_success_ = false;
        run_guard_result_error_.clear();
        (void)ResetEvent(run_guard_event_.get());
    }

    const DWORD id = thread_id_.load(std::memory_order_acquire);
    if (id == 0 ||
        PostThreadMessageW(id, ActiveRunModifierGuardMessage, 0, 0) == FALSE) {
        error = L"The safety-hotkey thread could not receive the active-run modifier guard request.";
        return false;
    }

    if (WaitForSingleObject(run_guard_event_.get(), 5'000) != WAIT_OBJECT_0) {
        error = L"The safety-hotkey thread did not confirm the active-run modifier guard in time.";
        return false;
    }

    std::scoped_lock lock(run_guard_mutex_);
    if (processed_run_guard_request_id_ != request_id) {
        error = L"The safety-hotkey modifier guard confirmation did not match the current request.";
        return false;
    }
    error = run_guard_result_error_;
    return run_guard_result_success_;
}

bool HotkeyThread::SetPriorityMode(
    const core::HotkeyControlPriorityMode mode) noexcept {
    desired_priority_mode_.store(mode, std::memory_order_release);
    return RefreshPriorityPolicy();
}

bool HotkeyThread::RefreshPriorityPolicy() noexcept {
    const DWORD id = thread_id_.load(std::memory_order_acquire);
    return id != 0 && PostThreadMessageW(id, PriorityPolicyMessage, 0, 0) != FALSE;
}

void HotkeyThread::ThreadMain() noexcept {
    // Create the message queue before publishing the thread ID. A nonzero ID
    // therefore means PostThreadMessageW has a real queue to target. The
    // shutdown atomic also covers destruction before this worker starts.
    MSG queue_initializer{};
    PeekMessageW(&queue_initializer, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    thread_id_.store(GetCurrentThreadId(), std::memory_order_release);

    if (shutdown_requested_.load(std::memory_order_acquire)) {
        if (ready_event_) {
            SetEvent(ready_event_.get());
        }
        thread_id_.store(0, std::memory_order_release);
        return;
    }

    RefreshPriorityPolicyOnThread();

    core::HotkeyBinding initial_start;
    core::HotkeyBinding initial_emergency;
    {
        std::scoped_lock lock(desired_mutex_);
        initial_start = desired_start_stop_;
        initial_emergency = desired_emergency_;
    }

    HotkeyRegistrationResult initial_result;
    initial_result.request_id = InitialRequestId;
    initial_result.initial_request = true;
    initial_result.requested_start_stop = initial_start;
    initial_result.requested_emergency = initial_emergency;

    RegistrationAttempt initial_attempt =
        ApplyPairTransactionally(initial_start, initial_emergency);
    initial_result.requested_pair_registered = initial_attempt.success;
    initial_result.error = initial_attempt.error;

    if (!initial_attempt.success) {
        const core::RunSettings defaults = core::DefaultRunSettings();
        if (!IsSamePair(initial_start,
                        initial_emergency,
                        defaults.start_stop_hotkey,
                        defaults.emergency_hotkey)) {
            RegistrationAttempt fallback_attempt = ApplyPairTransactionally(
                defaults.start_stop_hotkey, defaults.emergency_hotkey);
            if (fallback_attempt.success) {
                initial_result.fallback_used = true;
            } else if (!fallback_attempt.error.empty()) {
                if (!initial_result.error.empty()) {
                    initial_result.error += L" ";
                }
                initial_result.error +=
                    L"The default \"F5\" / \"F8\" pair was also unavailable: " +
                    fallback_attempt.error;
            }
        }
    }

    initial_result.active_start_stop = active_start_stop_;
    initial_result.active_emergency = active_emergency_;
    processed_request_id_ = InitialRequestId;

    // Destruction can be requested while the initial RegisterHotKey calls are
    // in progress. Do not publish a late registration callback into a window
    // that is already tearing down; release any pair and finish instead.
    if (shutdown_requested_.load(std::memory_order_acquire)) {
        UnregisterPair();
        if (ready_event_) {
            SetEvent(ready_event_.get());
        }
        thread_id_.store(0, std::memory_order_release);
        return;
    }

    ReportRegistration(std::move(initial_result));

    if (ready_event_) {
        SetEvent(ready_event_.get());
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_HOTKEY) {
            RefreshPriorityPolicyOnThread();
            const int registration_id = static_cast<int>(message.wParam);
            try {
                // Preserve the role of any hotkey event that was already
                // queued before a successful pair transaction committed. A
                // posted marker clears these retired mappings after all older
                // queue entries have been dispatched.
                if (registration_id == retired_emergency_id_ &&
                    emergency_callback_) {
                    emergency_callback_();
                } else if (registration_id == retired_start_stop_id_ &&
                           start_stop_callback_) {
                    start_stop_callback_();
                } else if (std::find(run_guard_emergency_ids_.begin(),
                                     run_guard_emergency_ids_.begin() +
                                         static_cast<std::ptrdiff_t>(run_guard_emergency_count_),
                                     registration_id) !=
                               run_guard_emergency_ids_.begin() +
                                   static_cast<std::ptrdiff_t>(run_guard_emergency_count_) &&
                           emergency_callback_) {
                    emergency_callback_();
                } else if (std::find(run_guard_start_stop_ids_.begin(),
                                     run_guard_start_stop_ids_.begin() +
                                         static_cast<std::ptrdiff_t>(run_guard_start_stop_count_),
                                     registration_id) !=
                               run_guard_start_stop_ids_.begin() +
                                   static_cast<std::ptrdiff_t>(run_guard_start_stop_count_) &&
                           start_stop_callback_) {
                    start_stop_callback_();
                } else if (registration_id == active_emergency_id_ &&
                           emergency_callback_) {
                    emergency_callback_();
                } else if (registration_id == active_start_stop_id_ &&
                           start_stop_callback_) {
                    start_stop_callback_();
                }
            } catch (...) {
                ReportError();
            }
            continue;
        }

        if (message.message == ClearRetiredMappingsMessage) {
            retired_start_stop_id_ = 0;
            retired_emergency_id_ = 0;
            continue;
        }

        if (message.message == PriorityPolicyMessage) {
            RefreshPriorityPolicyOnThread();
            continue;
        }

        if (message.message == ActiveRunModifierGuardMessage) {
            bool enabled = false;
            std::uint64_t request_id = 0;
            {
                std::scoped_lock lock(run_guard_mutex_);
                enabled = desired_run_guard_enabled_;
                request_id = desired_run_guard_request_id_;
            }

            RegistrationAttempt result = ApplyActiveRunModifierGuard(enabled);
            {
                std::scoped_lock lock(run_guard_mutex_);
                processed_run_guard_request_id_ = request_id;
                run_guard_result_success_ = result.success;
                run_guard_result_error_ = std::move(result.error);
            }
            (void)SetEvent(run_guard_event_.get());
            continue;
        }

        if (message.message == ReconfigureMessage) {
            core::HotkeyBinding new_start;
            core::HotkeyBinding new_emergency;
            std::uint64_t request_id = 0;
            {
                std::scoped_lock lock(desired_mutex_);
                new_start = desired_start_stop_;
                new_emergency = desired_emergency_;
                request_id = desired_request_id_;
            }
            if (request_id <= processed_request_id_) {
                continue;
            }

            HotkeyRegistrationResult result;
            result.request_id = request_id;
            result.requested_start_stop = new_start;
            result.requested_emergency = new_emergency;

            RegistrationAttempt attempt =
                ApplyPairTransactionally(new_start, new_emergency);
            result.requested_pair_registered = attempt.success;
            result.error = std::move(attempt.error);

            if (!attempt.success && !active_start_stop_.IsAssigned()) {
                const core::RunSettings defaults = core::DefaultRunSettings();
                if (!IsSamePair(new_start,
                                new_emergency,
                                defaults.start_stop_hotkey,
                                defaults.emergency_hotkey)) {
                    RegistrationAttempt fallback_attempt = ApplyPairTransactionally(
                        defaults.start_stop_hotkey, defaults.emergency_hotkey);
                    if (fallback_attempt.success) {
                        result.fallback_used = true;
                    } else if (!fallback_attempt.error.empty()) {
                        if (!result.error.empty()) {
                            result.error += L" ";
                        }
                        result.error +=
                            L"The default \"F5\" / \"F8\" pair was also unavailable: " +
                            fallback_attempt.error;
                    }
                }
            }

            result.active_start_stop = active_start_stop_;
            result.active_emergency = active_emergency_;
            processed_request_id_ = request_id;
            ReportRegistration(std::move(result));
        }
    }

    UnregisterPair();
    thread_id_.store(0, std::memory_order_release);
}

void HotkeyThread::RefreshPriorityPolicyOnThread() noexcept {

    const int actual = GetThreadPriority(GetCurrentThread());
    if (actual == THREAD_PRIORITY_ERROR_RETURN) {
        return;
    }
    if (priority_owns_ && actual != priority_last_applied_) {
        priority_owns_ = false;
        priority_baseline_ = actual;
    }

    const auto requested = desired_priority_mode_.load(std::memory_order_acquire);
    const DWORD process_class = GetPriorityClass(GetCurrentProcess());
    const bool safe_process_class = ProcessClassAllowsHotkeyBoost(process_class);

    if (requested == core::HotkeyControlPriorityMode::SystemDefault ||
        !safe_process_class) {
        if (priority_owns_) {
            const int current = GetThreadPriority(GetCurrentThread());
            if (current == THREAD_PRIORITY_ERROR_RETURN) {
                return;
            }
            if (current == priority_last_applied_) {
                if (SetThreadPriority(GetCurrentThread(), priority_baseline_) == FALSE) {
                    return;
                }
            }
            priority_owns_ = false;
        }
        return;
    }

    if (requested != core::HotkeyControlPriorityMode::AboveNormal) {
        return;
    }

    const int current = GetThreadPriority(GetCurrentThread());
    if (current == THREAD_PRIORITY_ERROR_RETURN) {
        return;
    }
    if (priority_owns_ && current == THREAD_PRIORITY_ABOVE_NORMAL) {
        return;
    }
    if (!priority_owns_ && current >= THREAD_PRIORITY_ABOVE_NORMAL) {
        priority_baseline_ = current;
        return;
    }

    const int baseline = current;
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) == FALSE) {
        return;
    }
    const int applied = GetThreadPriority(GetCurrentThread());
    if (applied == THREAD_PRIORITY_ERROR_RETURN) {
        return;
    }
    priority_baseline_ = baseline;
    priority_last_applied_ = applied;
    priority_owns_ = true;
}

HotkeyThread::RegistrationAttempt HotkeyThread::ApplyPairTransactionally(
    const core::HotkeyBinding start_stop,
    const core::HotkeyBinding emergency) noexcept {
    if (!IsSupportedSafetyHotkeyBinding(start_stop)) {
        return {
            false,
            L"The Start / Stop hotkey is not supported by Vector Click.",
        };
    }
    if (!IsSupportedSafetyHotkeyBinding(emergency)) {
        return {
            false,
            L"The Emergency Stop hotkey is not supported by Vector Click.",
        };
    }
    if (!IsValidPair(start_stop, emergency)) {
        return {
            false,
            L"Start / Stop and Emergency Stop must use different physical keys.",
        };
    }

    if (active_run_modifier_guard_active_ &&
        !IsSamePair(start_stop,
                    emergency,
                    active_start_stop_,
                    active_emergency_)) {
        return {
            false,
            L"Safety hotkeys cannot be changed while an active run is protected by modifier-compatible aliases.",
        };
    }

    if (IsSamePair(start_stop,
                   emergency,
                   active_start_stop_,
                   active_emergency_)) {
        return {true, {}};
    }

    int candidate_start_id = 0;
    int candidate_emergency_id = 0;

    if (start_stop == active_start_stop_) {
        candidate_start_id = active_start_stop_id_;
    } else if (start_stop == active_emergency_) {
        candidate_start_id = active_emergency_id_;
    }

    if (emergency == active_emergency_) {
        candidate_emergency_id = active_emergency_id_;
    } else if (emergency == active_start_stop_) {
        candidate_emergency_id = active_start_stop_id_;
    }

    std::array<int, 2> newly_registered{};
    std::size_t newly_registered_count = 0;
    std::wstring error;

    if (candidate_start_id == 0) {
        candidate_start_id = FreeRegistrationId(candidate_emergency_id);
        if (candidate_start_id == 0 ||
            !RegisterBinding(candidate_start_id,
                             start_stop,
                             L"Start / Stop",
                             error)) {
            return {false, std::move(error)};
        }
        newly_registered[newly_registered_count++] = candidate_start_id;
    }

    if (candidate_emergency_id == 0) {
        candidate_emergency_id = FreeRegistrationId(candidate_start_id);
        if (candidate_emergency_id == 0 ||
            !RegisterBinding(candidate_emergency_id,
                             emergency,
                             L"Emergency Stop",
                             error)) {
            for (std::size_t index = 0; index < newly_registered_count; ++index) {
                UnregisterHotKey(nullptr, newly_registered[index]);
            }
            return {false, std::move(error)};
        }
        newly_registered[newly_registered_count++] = candidate_emergency_id;
    }

    const int previous_start_stop_id = active_start_stop_id_;
    const int previous_emergency_id = active_emergency_id_;
    const int previous_ids[] = {previous_start_stop_id, previous_emergency_id};
    for (const int previous_id : previous_ids) {
        if (previous_id != 0 && previous_id != candidate_start_id &&
            previous_id != candidate_emergency_id) {
            UnregisterHotKey(nullptr, previous_id);
        }
    }

    active_start_stop_ = start_stop;
    active_emergency_ = emergency;
    active_start_stop_id_ = candidate_start_id;
    active_emergency_id_ = candidate_emergency_id;

    if (previous_start_stop_id != 0 || previous_emergency_id != 0) {
        retired_start_stop_id_ = previous_start_stop_id;
        retired_emergency_id_ = previous_emergency_id;
        if (PostThreadMessageW(GetCurrentThreadId(),
                               ClearRetiredMappingsMessage,
                               0,
                               0) == FALSE) {
            retired_start_stop_id_ = 0;
            retired_emergency_id_ = 0;
        }
    }
    return {true, {}};
}

HotkeyThread::RegistrationAttempt HotkeyThread::ApplyActiveRunModifierGuard(
    const bool enabled) noexcept {
    if (!enabled) {
        UnregisterActiveRunModifierGuard();
        return {true, {}};
    }
    if (active_run_modifier_guard_active_) {
        return {true, {}};
    }
    if (!active_start_stop_.IsAssigned() || !active_emergency_.IsAssigned()) {
        return {
            false,
            L"The active safety-hotkey pair is unavailable.",
        };
    }

    // The guard is installed transactionally. Build directly into the member
    // arrays so FreeRegistrationId can see every provisional ID immediately;
    // any failure unregisters the whole provisional set before returning.
    run_guard_start_stop_ids_.fill(0);
    run_guard_emergency_ids_.fill(0);
    run_guard_start_stop_count_ = 0;
    run_guard_emergency_count_ = 0;

    std::wstring error;
    const auto register_modifier_aliases =
        [this, &error](const core::HotkeyBinding binding,
                       const wchar_t* role,
                       std::array<int, MaximumGuardAliasesPerRole>& ids,
                       std::size_t& count) noexcept {
            constexpr std::array<UINT, 8> extra_modifier_sets = {
                0U,
                MOD_CONTROL,
                MOD_ALT,
                MOD_SHIFT,
                MOD_CONTROL | MOD_ALT,
                MOD_CONTROL | MOD_SHIFT,
                MOD_ALT | MOD_SHIFT,
                MOD_CONTROL | MOD_ALT | MOD_SHIFT,
            };

            const UINT required_modifiers = static_cast<UINT>(binding.modifiers);
            std::array<UINT, MaximumGuardAliasesPerRole> registered_modifiers{};
            std::size_t registered_modifier_count = 0;

            for (const UINT extra_modifiers : extra_modifier_sets) {
                const UINT alias_modifiers = required_modifiers | extra_modifiers;
                if (alias_modifiers == required_modifiers) {
                    continue;
                }

                const auto registered_end =
                    registered_modifiers.begin() +
                    static_cast<std::ptrdiff_t>(registered_modifier_count);
                if (std::find(registered_modifiers.begin(),
                              registered_end,
                              alias_modifiers) != registered_end) {
                    continue;
                }

                const int id = FreeRegistrationId();
                if (id == 0 ||
                    RegisterHotKey(nullptr,
                                   id,
                                   alias_modifiers | MOD_NOREPEAT,
                                   binding.virtual_key) == FALSE) {
                    error = role;
                    error +=
                        L" hotkey could not reserve every Ctrl / Alt / Shift-compatible combination because Windows or another application already owns one of them.";
                    return false;
                }

                ids[count++] = id;
                registered_modifiers[registered_modifier_count++] = alias_modifiers;
            }
            return true;
        };

    if (!register_modifier_aliases(active_start_stop_,
                                   L"Start / Stop",
                                   run_guard_start_stop_ids_,
                                   run_guard_start_stop_count_) ||
        !register_modifier_aliases(active_emergency_,
                                   L"Emergency Stop",
                                   run_guard_emergency_ids_,
                                   run_guard_emergency_count_)) {
        UnregisterActiveRunModifierGuard();
        return {false, std::move(error)};
    }

    active_run_modifier_guard_active_ = true;
    return {true, {}};
}

bool HotkeyThread::RegisterBinding(const int id,
                                   const core::HotkeyBinding binding,
                                   const wchar_t* role,
                                   std::wstring& error) noexcept {
    const UINT modifiers = static_cast<UINT>(binding.modifiers) | MOD_NOREPEAT;
    if (RegisterHotKey(nullptr, id, modifiers, binding.virtual_key) != FALSE) {
        return true;
    }

    error = role;
    error += L" hotkey is already in use by Windows or another application.";
    return false;
}

int HotkeyThread::FreeRegistrationId(const int first_reserved) const noexcept {
    const auto contains_id = [](const auto& ids,
                                const std::size_t count,
                                const int candidate) noexcept {
        return std::find(ids.begin(),
                         ids.begin() + static_cast<std::ptrdiff_t>(count),
                         candidate) !=
               ids.begin() + static_cast<std::ptrdiff_t>(count);
    };

    for (int id = MinimumRegistrationId; id <= MaximumRegistrationId; ++id) {
        if (id != active_start_stop_id_ && id != active_emergency_id_ &&
            id != retired_start_stop_id_ && id != retired_emergency_id_ &&
            !contains_id(run_guard_start_stop_ids_, run_guard_start_stop_count_, id) &&
            !contains_id(run_guard_emergency_ids_, run_guard_emergency_count_, id) &&
            id != first_reserved) {
            return id;
        }
    }
    return 0;
}

void HotkeyThread::UnregisterActiveRunModifierGuard() noexcept {
    for (std::size_t index = 0; index < run_guard_start_stop_count_; ++index) {
        if (run_guard_start_stop_ids_[index] != 0) {
            (void)UnregisterHotKey(nullptr, run_guard_start_stop_ids_[index]);
        }
    }
    for (std::size_t index = 0; index < run_guard_emergency_count_; ++index) {
        if (run_guard_emergency_ids_[index] != 0) {
            (void)UnregisterHotKey(nullptr, run_guard_emergency_ids_[index]);
        }
    }
    run_guard_start_stop_ids_.fill(0);
    run_guard_emergency_ids_.fill(0);
    run_guard_start_stop_count_ = 0;
    run_guard_emergency_count_ = 0;
    active_run_modifier_guard_active_ = false;
}

void HotkeyThread::UnregisterPair() noexcept {
    UnregisterActiveRunModifierGuard();
    if (active_start_stop_id_ != 0) {
        UnregisterHotKey(nullptr, active_start_stop_id_);
    }
    if (active_emergency_id_ != 0 &&
        active_emergency_id_ != active_start_stop_id_) {
        UnregisterHotKey(nullptr, active_emergency_id_);
    }
    active_start_stop_ = {};
    active_emergency_ = {};
    active_start_stop_id_ = 0;
    active_emergency_id_ = 0;
    retired_start_stop_id_ = 0;
    retired_emergency_id_ = 0;
}

void HotkeyThread::ReportError() noexcept {
    try {
        if (error_callback_) {
            error_callback_();
        }
    } catch (...) {
    }
}

void HotkeyThread::ReportRegistration(HotkeyRegistrationResult result) noexcept {
    try {
        if (registration_callback_) {
            registration_callback_(std::move(result));
        }
    } catch (...) {
    }
}

} // namespace vectorclick::win

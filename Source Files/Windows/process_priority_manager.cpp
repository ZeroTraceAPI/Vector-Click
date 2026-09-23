#include "Windows/process_priority_manager.h"


namespace vectorclick::win {

bool ProcessPriorityManager::IsTemporaryMode(
    const core::ProcessPriorityMode mode) noexcept {
    return mode == core::ProcessPriorityMode::AboveNormalWhileActive ||
           mode == core::ProcessPriorityMode::HighWhileActive;
}

bool ProcessPriorityManager::IsPersistentMode(
    const core::ProcessPriorityMode mode) noexcept {
    return mode == core::ProcessPriorityMode::AboveNormal ||
           mode == core::ProcessPriorityMode::High;
}

DWORD ProcessPriorityManager::RequestedClass(
    const core::ProcessPriorityMode mode) noexcept {
    switch (mode) {
    case core::ProcessPriorityMode::AboveNormalWhileActive:
    case core::ProcessPriorityMode::AboveNormal:
        return ABOVE_NORMAL_PRIORITY_CLASS;
    case core::ProcessPriorityMode::HighWhileActive:
    case core::ProcessPriorityMode::High:
        return HIGH_PRIORITY_CLASS;
    case core::ProcessPriorityMode::SystemDefault:
        return 0U;
    }
    return 0U;
}

int ProcessPriorityManager::PriorityRank(const DWORD priority_class) noexcept {
    switch (priority_class) {
    case IDLE_PRIORITY_CLASS:
        return 1;
    case BELOW_NORMAL_PRIORITY_CLASS:
        return 2;
    case NORMAL_PRIORITY_CLASS:
        return 3;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return 4;
    case HIGH_PRIORITY_CLASS:
        return 5;
    case REALTIME_PRIORITY_CLASS:
        return 6;
    default:
        return 0;
    }
}


DWORD ProcessPriorityManager::QueryCurrent(std::wstring& error) const noexcept {
    SetLastError(ERROR_SUCCESS);
    const DWORD priority_class = GetPriorityClass(GetCurrentProcess());
    if (priority_class == 0U) {
        const DWORD windows_error = GetLastError();
        error = L"Windows could not read Vector Click's current process priority.";
        if (windows_error != ERROR_SUCCESS) {
            error += L" Windows error code: " + std::to_wstring(windows_error) + L".";
        }
    }
    return priority_class;
}

bool ProcessPriorityManager::SetCurrent(const DWORD priority_class,
                                        std::wstring& error,
                                        DWORD& windows_error) noexcept {
    windows_error = ERROR_SUCCESS;
    SetLastError(ERROR_SUCCESS);
    if (SetPriorityClass(GetCurrentProcess(), priority_class) != FALSE) {
        return true;
    }
    windows_error = GetLastError();
    error = L"Windows could not apply the selected Vector Click process priority.";
    if (windows_error != ERROR_SUCCESS) {
        error += L" Windows error code: " + std::to_wstring(windows_error) + L".";
    }
    return false;
}

void ProcessPriorityManager::ObserveExternalOverride(const DWORD actual) noexcept {
    if (owns_priority_ && actual != last_applied_class_) {
        // The current class no longer equals the value Vector Click last set.
        // Treat the newer state as externally controlled and never restore the
        // older baseline across it.
        ClearOwnedState();
    }
}

void ProcessPriorityManager::ClearOwnedState() noexcept {
    baseline_valid_ = false;
    baseline_class_ = 0U;
    owns_priority_ = false;
    last_applied_class_ = 0U;
}

bool ProcessPriorityManager::RestoreOwnedPriority(std::wstring& error) noexcept {
    DWORD actual_before = QueryCurrent(error);
    if (actual_before == 0U) {
        return false;
    }

    ObserveExternalOverride(actual_before);
    if (!owns_priority_ || !baseline_valid_) {
        ClearOwnedState();
        return true;
    }

    DWORD windows_error = ERROR_SUCCESS;
    if (!SetCurrent(baseline_class_, error, windows_error)) {
        return false;
    }

    ClearOwnedState();
    return true;
}

bool ProcessPriorityManager::ApplyPersistentMode(
    const core::ProcessPriorityMode mode,
    std::wstring& error) noexcept {
    const DWORD requested = RequestedClass(mode);
    DWORD actual_before = QueryCurrent(error);
    if (actual_before == 0U || requested == 0U) {
        return false;
    }

    ObserveExternalOverride(actual_before);

    // If Vector Click still owns the current value, the user's new managed
    // choice replaces Vector Click's own earlier choice exactly. This permits
    // High -> Above Normal without treating our own High state as external.
    if (owns_priority_) {
        if (actual_before == requested) {
            return true;
        }
        DWORD windows_error = ERROR_SUCCESS;
        if (!SetCurrent(requested, error, windows_error)) {
            return false;
        }
        last_applied_class_ = requested;
        return true;
    }

    // A priority already at or above the requested class is external. Leave it
    // untouched. This is how an externally established High or Realtime class
    // remains authoritative when the user selects Above Normal.
    if (PriorityRank(actual_before) >= PriorityRank(requested)) {
        ClearOwnedState();
        return true;
    }

    baseline_valid_ = true;
    baseline_class_ = actual_before;
    DWORD windows_error = ERROR_SUCCESS;
    if (!SetCurrent(requested, error, windows_error)) {
        ClearOwnedState();
        return false;
    }
    owns_priority_ = true;
    last_applied_class_ = requested;
    return true;
}

bool ProcessPriorityManager::ApplyTemporaryPriority(
    std::wstring& error) noexcept {
    const DWORD requested = RequestedClass(mode_);
    DWORD actual_before = QueryCurrent(error);
    if (actual_before == 0U || requested == 0U) {
        return false;
    }

    ObserveExternalOverride(actual_before);
    if (owns_priority_) {
        // This can occur only after an earlier restoration failure. Continue
        // using the existing baseline rather than replacing it with our own
        // still-elevated class.
        if (actual_before != requested) {
            DWORD windows_error = ERROR_SUCCESS;
            if (!SetCurrent(requested, error, windows_error)) {
                return false;
            }
            last_applied_class_ = requested;
        }
        return true;
    }

    if (PriorityRank(actual_before) >= PriorityRank(requested)) {
        ClearOwnedState();
        return true;
    }

    baseline_valid_ = true;
    baseline_class_ = actual_before;
    DWORD windows_error = ERROR_SUCCESS;
    if (!SetCurrent(requested, error, windows_error)) {
        ClearOwnedState();
        return false;
    }
    owns_priority_ = true;
    last_applied_class_ = requested;
    return true;
}

bool ProcessPriorityManager::SetMode(const core::ProcessPriorityMode mode,
                                     std::wstring& error) noexcept {
    error.clear();
    if (active_) {
        error = L"Process priority cannot be changed while a Vector Click run or cleanup is active.";
        return false;
    }

    if (mode == mode_) {
        return true;
    }

    const core::ProcessPriorityMode previous_mode = mode_;

    if (mode == core::ProcessPriorityMode::SystemDefault) {
        if (!RestoreOwnedPriority(error)) {
            return false;
        }
        mode_ = mode;
        return true;
    }

    if (IsTemporaryMode(mode)) {
        // Leaving a persistent Vector Click-owned mode for an idle temporary
        // mode restores the external baseline first. If the current class was
        // changed externally, RestoreOwnedPriority detects that and leaves it.
        if (IsPersistentMode(previous_mode) || owns_priority_) {
            if (!RestoreOwnedPriority(error)) {
                return false;
            }
        } else {
            ClearOwnedState();
        }
        mode_ = mode;
        return true;
    }

    if (!IsPersistentMode(mode)) {
        error = L"The selected process-priority mode is not supported.";
        return false;
    }

    // For persistent -> persistent changes, ApplyPersistentMode deliberately
    // keeps the original external baseline when Vector Click still owns the
    // current class. That makes VC-owned High -> Above Normal reversible back
    // to the environment that existed before Vector Click began managing it.
    if (!ApplyPersistentMode(mode, error)) {
        return false;
    }
    mode_ = mode;
    return true;
}

bool ProcessPriorityManager::BeginActive(std::wstring& error) noexcept {
    error.clear();
    if (active_) {
        return true;
    }

    if (IsTemporaryMode(mode_)) {
        if (!ApplyTemporaryPriority(error)) {
            return false;
        }
    }
    active_ = true;
    return true;
}

bool ProcessPriorityManager::EndActive(std::wstring& error) noexcept {
    error.clear();
    if (!active_) {
        return true;
    }

    // Mark the lifecycle inactive only after the temporary restore attempt.
    // If restoration fails, preserve ownership metadata so System default or a
    // later cleanup can retry rather than forgetting the original baseline.
    if (IsTemporaryMode(mode_)) {
        if (!RestoreOwnedPriority(error)) {
            active_ = false;
            return false;
        }
    }
    active_ = false;
    return true;
}


} // namespace vectorclick::win

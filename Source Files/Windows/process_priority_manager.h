#pragma once

#include "Core/settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace vectorclick::win {

class ProcessPriorityManager final {
public:
    ProcessPriorityManager() noexcept = default;
    ~ProcessPriorityManager() noexcept = default;

    ProcessPriorityManager(const ProcessPriorityManager&) = delete;
    ProcessPriorityManager& operator=(const ProcessPriorityManager&) = delete;

    [[nodiscard]] core::ProcessPriorityMode Mode() const noexcept { return mode_; }
    [[nodiscard]] bool IsActive() const noexcept { return active_; }

    // Changes the user-selected policy while Vector Click is idle. Persistent
    // modes apply immediately. Temporary modes remain inert until BeginActive.
    [[nodiscard]] bool SetMode(core::ProcessPriorityMode mode,
                               std::wstring& error) noexcept;

    // Marks the accepted Start -> stop / release-cleanup lifecycle. Temporary
    // modes apply their requested priority at this boundary and restore only
    // priority state that Vector Click can still identify as its own.
    [[nodiscard]] bool BeginActive(std::wstring& error) noexcept;
    [[nodiscard]] bool EndActive(std::wstring& error) noexcept;

private:
    [[nodiscard]] static bool IsTemporaryMode(core::ProcessPriorityMode mode) noexcept;
    [[nodiscard]] static bool IsPersistentMode(core::ProcessPriorityMode mode) noexcept;
    [[nodiscard]] static DWORD RequestedClass(core::ProcessPriorityMode mode) noexcept;
    [[nodiscard]] static int PriorityRank(DWORD priority_class) noexcept;

    [[nodiscard]] DWORD QueryCurrent(std::wstring& error) const noexcept;
    [[nodiscard]] bool SetCurrent(DWORD priority_class,
                                  std::wstring& error,
                                  DWORD& windows_error) noexcept;
    void ObserveExternalOverride(DWORD actual) noexcept;
    void ClearOwnedState() noexcept;
    [[nodiscard]] bool RestoreOwnedPriority(std::wstring& error) noexcept;
    [[nodiscard]] bool ApplyPersistentMode(core::ProcessPriorityMode mode,
                                           std::wstring& error) noexcept;
    [[nodiscard]] bool ApplyTemporaryPriority(std::wstring& error) noexcept;


    core::ProcessPriorityMode mode_{core::ProcessPriorityMode::SystemDefault};
    bool active_{};
    bool baseline_valid_{};
    DWORD baseline_class_{};
    bool owns_priority_{};
    DWORD last_applied_class_{};
};

} // namespace vectorclick::win

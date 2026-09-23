#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vectorclick::win {

enum class TargetElevation {
    Unknown,
    Standard,
    Elevated,
};

enum class TargetRecoveryPolicy {
    ExactWindowOnly,
    SameApplicationAndClass,
    SameApplicationAndTitle,
};

struct TargetWindowIdentity {
    std::uintptr_t window_value{};
    DWORD process_id{};
    DWORD thread_id{};
    std::wstring window_class;
    TargetRecoveryPolicy recovery_policy{TargetRecoveryPolicy::ExactWindowOnly};
};

struct TargetWindowInfo {
    HWND window{};
    DWORD process_id{};
    DWORD thread_id{};
    std::wstring title;
    std::wstring process_name;
    std::wstring process_path;
    std::wstring window_class;
    TargetElevation elevation{TargetElevation::Unknown};
    TargetRecoveryPolicy recovery_policy{TargetRecoveryPolicy::ExactWindowOnly};
};

[[nodiscard]] bool InspectTargetWindow(HWND candidate,
                                       DWORD excluded_process_id,
                                       TargetWindowInfo& result,
                                       std::wstring& error);
[[nodiscard]] bool IsTargetWindowValid(const TargetWindowInfo& target) noexcept;
[[nodiscard]] TargetWindowIdentity CaptureTargetWindowIdentity(const TargetWindowInfo& target);
[[nodiscard]] bool TargetWindowMatchesIdentity(const TargetWindowInfo& target,
                                               const TargetWindowIdentity& identity) noexcept;
[[nodiscard]] bool RestoreTargetWindow(const TargetWindowIdentity& identity,
                                       DWORD excluded_process_id,
                                       TargetWindowInfo& result,
                                       std::wstring& error);
[[nodiscard]] bool IsTargetWindowForeground(const TargetWindowInfo& target) noexcept;
[[nodiscard]] bool IsWindowWithinTarget(const TargetWindowInfo& target, HWND recipient) noexcept;
[[nodiscard]] HWND ResolveTargetKeyboardRecipient(const TargetWindowInfo& target) noexcept;
[[nodiscard]] bool TryRecoverTargetWindow(TargetWindowInfo& target,
                                          DWORD excluded_process_id,
                                          std::wstring& error);
[[nodiscard]] std::vector<TargetWindowInfo> EnumerateTargetWindows(DWORD excluded_process_id);
[[nodiscard]] const wchar_t* TargetElevationText(TargetElevation elevation) noexcept;
[[nodiscard]] const wchar_t* TargetRecoveryPolicyText(TargetRecoveryPolicy policy) noexcept;
[[nodiscard]] const wchar_t* TargetRecoveryPolicyDescription(TargetRecoveryPolicy policy) noexcept;
[[nodiscard]] std::wstring TargetWindowSummary(const TargetWindowInfo& target);
[[nodiscard]] std::wstring TargetWindowDetails(const TargetWindowInfo& target);

} // namespace vectorclick::win

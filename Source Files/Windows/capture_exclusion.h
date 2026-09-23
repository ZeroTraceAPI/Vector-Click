#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win {

enum class CaptureExclusionResult : unsigned char {
    Applied,
    Unsupported,
    InvalidWindow,
    SystemFailure,
};

struct CaptureExclusionOutcome final {
    CaptureExclusionResult result{CaptureExclusionResult::SystemFailure};
    DWORD error{};
    bool affinity_verified{};
    DWORD observed_affinity{};
};

[[nodiscard]] bool IsFullCaptureExclusionSupported() noexcept;
[[nodiscard]] bool IsCaptureExclusionRequested() noexcept;
void SetCaptureExclusionRequested(bool enabled) noexcept;
[[nodiscard]] CaptureExclusionOutcome ApplyCaptureExclusion(HWND window,
                                                            bool enabled) noexcept;
[[nodiscard]] CaptureExclusionOutcome ApplyRequestedCaptureExclusion(HWND window) noexcept;
[[nodiscard]] CaptureExclusionOutcome SynchronizeRequestedCaptureExclusion(HWND window) noexcept;

} // namespace vectorclick::win

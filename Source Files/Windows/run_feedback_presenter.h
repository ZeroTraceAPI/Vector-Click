#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string_view>

namespace vectorclick::win {

// Owns only presentation-side run feedback. It never changes engine state,
// input ownership, scheduling, or settings. Notifications use the classic
// notification-area API so Vector Click remains installer-free and does not
// require persistent shell registration.
class RunFeedbackPresenter final {
public:
    RunFeedbackPresenter() noexcept = default;
    ~RunFeedbackPresenter();

    RunFeedbackPresenter(const RunFeedbackPresenter&) = delete;
    RunFeedbackPresenter& operator=(const RunFeedbackPresenter&) = delete;

    [[nodiscard]] bool Initialize(HWND owner, HINSTANCE instance) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] bool ShowNotification(std::wstring_view title,
                                        std::wstring_view body) noexcept;
    void RemoveNotificationIcon() noexcept;

    void PlayStartedSound() noexcept;
    void PlayStoppedSound() noexcept;

    void SetRunningIndicator(bool visible) noexcept;
    void RefreshRunningIndicator() noexcept;

private:
    [[nodiscard]] HICON LoadRunningIcon(int width, int height) noexcept;
    void DestroyBadgedIcons() noexcept;
    void RestoreOriginalIcons() noexcept;

    HWND owner_{};
    HINSTANCE instance_{};
    HICON original_large_icon_{};
    HICON original_small_icon_{};
    HICON badged_large_icon_{};
    HICON badged_small_icon_{};
    bool notification_icon_present_{};
    bool running_indicator_visible_{};
};

} // namespace vectorclick::win

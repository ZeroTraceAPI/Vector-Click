#include "Windows/run_feedback_presenter.h"

#include "Windows/app_identity.h"
#include <wtypes.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>

namespace vectorclick::win {
namespace {

constexpr UINT NotificationIconId = 0x5643U;
constexpr int VectorClickRunningIconResourceId = 102;

[[nodiscard]] HICON WindowIcon(const HWND window,
                               const WPARAM kind,
                               const int class_index) noexcept {
    HICON icon = reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, kind, 0));
    if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, class_index));
    }
    return icon;
}

template <std::size_t N>
void CopyNotificationText(wchar_t (&destination)[N],
                          const std::wstring_view source) noexcept {
    static_assert(N > 0);
    const std::size_t count = std::min<std::size_t>(N - 1U, source.size());
    if (count > 0U) {
        std::wmemcpy(destination, source.data(), count);
    }
    destination[count] = L'\0';
}


} // namespace

RunFeedbackPresenter::~RunFeedbackPresenter() {
    Shutdown();
}

bool RunFeedbackPresenter::Initialize(const HWND owner,
                                      const HINSTANCE instance) noexcept {
    Shutdown();
    if (owner == nullptr || IsWindow(owner) == FALSE || instance == nullptr) {
        return false;
    }

    owner_ = owner;
    instance_ = instance;
    original_large_icon_ = WindowIcon(owner_, ICON_BIG, GCLP_HICON);
    original_small_icon_ = WindowIcon(owner_, ICON_SMALL, GCLP_HICONSM);

    return true;
}

void RunFeedbackPresenter::Shutdown() noexcept {
    RemoveNotificationIcon();
    RestoreOriginalIcons();
    DestroyBadgedIcons();
    owner_ = nullptr;
    instance_ = nullptr;
    original_large_icon_ = nullptr;
    original_small_icon_ = nullptr;
    running_indicator_visible_ = false;
}

bool RunFeedbackPresenter::ShowNotification(const std::wstring_view title,
                                            const std::wstring_view body) noexcept {
    if (owner_ == nullptr || IsWindow(owner_) == FALSE) {
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = NotificationIconId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    data.hIcon = original_small_icon_ != nullptr
                     ? original_small_icon_
                     : original_large_icon_;
    CopyNotificationText(data.szTip, L"Vector Click");
    CopyNotificationText(data.szInfoTitle, title);
    CopyNotificationText(data.szInfo, body);
    data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;

    const DWORD operation = notification_icon_present_ ? NIM_MODIFY : NIM_ADD;
    if (Shell_NotifyIconW(operation, &data) == FALSE) {
        return false;
    }
    notification_icon_present_ = true;
    return true;
}

void RunFeedbackPresenter::RemoveNotificationIcon() noexcept {
    if (!notification_icon_present_ || owner_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = NotificationIconId;
    (void)Shell_NotifyIconW(NIM_DELETE, &data);
    notification_icon_present_ = false;
}

void RunFeedbackPresenter::PlayStartedSound() noexcept {
    (void)PlaySoundW(L"SystemAsterisk",
                     nullptr,
                     SND_ALIAS | SND_ASYNC | SND_SYSTEM);
}

void RunFeedbackPresenter::PlayStoppedSound() noexcept {
    // SystemExit is not assigned a waveform in every modern Windows sound
    // scheme. Allow PlaySound to fall back to the scheme's default event so a
    // requested Stopped sound does not become silent merely because that alias
    // is unassigned. SND_SYSTEM keeps both run sounds in the Windows system-
    // notification audio session rather than Vector Click's ordinary session.
    (void)PlaySoundW(L"SystemExit",
                     nullptr,
                     SND_ALIAS | SND_ASYNC | SND_SYSTEM);
}

HICON RunFeedbackPresenter::LoadRunningIcon(const int width,
                                             const int height) noexcept {
    if (instance_ == nullptr || width <= 0 || height <= 0) {
        return nullptr;
    }

    // Load the running-state icon through the same native resource path as the
    // normal application icon. Each embedded size is derived from the matching
    // original frame, with pixels outside the small violet badge left unchanged.
    // This avoids rebuilding the complete icon through GDI+ and therefore keeps
    // Windows' original alpha / edge treatment intact.
    return reinterpret_cast<HICON>(LoadImageW(
        instance_,
        MAKEINTRESOURCEW(VectorClickRunningIconResourceId),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR));
}

void RunFeedbackPresenter::SetRunningIndicator(const bool visible) noexcept {
    if (owner_ == nullptr || IsWindow(owner_) == FALSE) {
        return;
    }
    if (!visible) {
        RestoreOriginalIcons();
        DestroyBadgedIcons();
        running_indicator_visible_ = false;
        return;
    }

    if (running_indicator_visible_ &&
        (badged_large_icon_ != nullptr || badged_small_icon_ != nullptr)) {
        return;
    }

    DestroyBadgedIcons();
    const UINT dpi = GetDpiForWindow(owner_);
    const int large_width = GetSystemMetricsForDpi(SM_CXICON, dpi);
    const int large_height = GetSystemMetricsForDpi(SM_CYICON, dpi);
    const int small_width = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    const int small_height = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
    badged_large_icon_ = LoadRunningIcon(large_width, large_height);
    badged_small_icon_ = LoadRunningIcon(small_width, small_height);

    if (badged_large_icon_ != nullptr) {
        SendMessageW(owner_, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(badged_large_icon_));
    }
    if (badged_small_icon_ != nullptr) {
        SendMessageW(owner_, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(badged_small_icon_));
    }
    running_indicator_visible_ =
        badged_large_icon_ != nullptr || badged_small_icon_ != nullptr;
}

void RunFeedbackPresenter::RefreshRunningIndicator() noexcept {
    if (!running_indicator_visible_) {
        return;
    }
    SetRunningIndicator(false);
    SetRunningIndicator(true);
}

void RunFeedbackPresenter::RestoreOriginalIcons() noexcept {
    if (owner_ == nullptr || IsWindow(owner_) == FALSE ||
        !running_indicator_visible_) {
        return;
    }
    SendMessageW(owner_, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(original_large_icon_));
    SendMessageW(owner_, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(original_small_icon_));
}

void RunFeedbackPresenter::DestroyBadgedIcons() noexcept {
    if (badged_large_icon_ != nullptr) {
        DestroyIcon(badged_large_icon_);
        badged_large_icon_ = nullptr;
    }
    if (badged_small_icon_ != nullptr) {
        DestroyIcon(badged_small_icon_);
        badged_small_icon_ = nullptr;
    }
}

} // namespace vectorclick::win

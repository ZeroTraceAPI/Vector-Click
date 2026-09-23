#include "Windows/app_identity.h"
#include "Windows/app_messages.h"
#include "Windows/centered_message_box.h"
#include "Windows/main_window.h"
#include "Windows/startup_options.h"
#include "Windows/win32_raii.h"

#include <commctrl.h>
#include <shellapi.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>

namespace {

enum class InstanceAcquireResult {
    Acquired,
    InUse,
    Error,
};

class SingleInstanceGuard {
public:
    SingleInstanceGuard() noexcept = default;
    ~SingleInstanceGuard() {
        if (owns_mutex_ && mutex_) {
            ReleaseMutex(mutex_.get());
        }
    }

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    InstanceAcquireResult Acquire(const bool wait_for_previous_instance) noexcept {
        mutex_.reset(CreateMutexW(nullptr, FALSE, vectorclick::win::SingleInstanceMutexName));
        if (!mutex_) {
            return InstanceAcquireResult::Error;
        }

        const DWORD timeout = wait_for_previous_instance ? 10'000 : 0;
        const DWORD result = WaitForSingleObject(mutex_.get(), timeout);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            owns_mutex_ = true;
            return InstanceAcquireResult::Acquired;
        }
        if (result == WAIT_TIMEOUT) {
            return InstanceAcquireResult::InUse;
        }
        return InstanceAcquireResult::Error;
    }

private:
    vectorclick::win::UniqueHandle mutex_;
    bool owns_mutex_{};
};

HWND FindExistingWindow(const bool wait_briefly) noexcept {
    const int attempts = wait_briefly ? 30 : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (HWND existing = FindWindowW(vectorclick::win::MainWindowClassName, nullptr);
            existing != nullptr) {
            return existing;
        }
        if (wait_briefly) {
            Sleep(50);
        }
    }
    return nullptr;
}

void ShowExistingWindowWithoutActivation(const HWND existing) noexcept {
    if (existing == nullptr || IsWindow(existing) == FALSE) {
        return;
    }

    if (IsIconic(existing) != FALSE) {
        ShowWindowAsync(existing, SW_RESTORE);
    } else {
        ShowWindowAsync(existing, SW_SHOWNOACTIVATE);
    }
}

void ActivateExistingWindow(const HWND existing) noexcept {
    if (existing == nullptr || IsWindow(existing) == FALSE) {
        return;
    }

    // Restore only when necessary. Re-showing an already visible owner with
    // SW_SHOW can reorder it after one of its enabled owned popups, which is
    // exactly the hierarchy we are trying to preserve here.
    if (IsIconic(existing) != FALSE) {
        ShowWindowAsync(existing, SW_RESTORE);
    } else if (IsWindowVisible(existing) == FALSE) {
        ShowWindowAsync(existing, SW_SHOWNOACTIVATE);
    }

    // Walk the enabled-popup ownership chain instead of stopping at the first
    // popup. This reaches Vector Click's active modal alert even when that
    // alert is owned by another owned dialog such as the Profile Manager.
    HWND activation_target = existing;
    for (int depth = 0; depth < 8; ++depth) {
        const HWND popup = GetWindow(activation_target, GW_ENABLEDPOPUP);
        if (popup == nullptr || popup == activation_target ||
            IsWindow(popup) == FALSE || IsWindowVisible(popup) == FALSE) {
            break;
        }
        activation_target = popup;
    }

    if (SetForegroundWindow(activation_target) == FALSE) {
        FLASHWINFO flash{};
        flash.cbSize = sizeof(flash);
        flash.hwnd = activation_target;
        flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        flash.uCount = 3;
        flash.dwTimeout = 0;
        FlashWindowEx(&flash);
    }
}

bool NotifyExistingInstance(const HWND existing) noexcept {
    if (existing == nullptr || IsWindow(existing) == FALSE) {
        return false;
    }

    // The user-launched secondary process owns the foreground transition. Put
    // the established application, or its current enabled owned popup, back in
    // front before asking the primary UI thread to create its normal alert.
    ActivateExistingWindow(existing);

    DWORD_PTR response = 0;
    SetLastError(ERROR_SUCCESS);
    const LRESULT delivered = SendMessageTimeoutW(
        existing,
        vectorclick::win::WM_APP_SINGLE_INSTANCE_NOTICE_REQUEST,
        vectorclick::win::SingleInstanceNoticeRequestTag,
        vectorclick::win::SingleInstanceNoticeRequestCheck,
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        1'000,
        &response);
    return delivered != 0 &&
           static_cast<LRESULT>(response) ==
               vectorclick::win::SingleInstanceNoticeAck;
}

void ShowAlreadyRunningFallback(const HWND existing) noexcept {
    // This path is used only if the primary-process notification cannot be
    // posted. Do not make the fallback globally topmost: as an owned window it
    // should follow the same z-order behavior as Vector Click's normal alerts.
    vectorclick::win::ShowCenteredMessageBox(
        existing,
        L"Vector Click is already running. Only one instance should run at a time so global hotkeys, portable settings, and generated input do not conflict. The existing window has been restored.",
        L"Vector Click is already running",
        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void HandleExistingInstance(const HWND existing) noexcept {
    ShowExistingWindowWithoutActivation(existing);
    if (NotifyExistingInstance(existing)) {
        return;
    }

    // Primary-process delivery is the normal route. Keep this lock
    // only as a defensive fallback so repeated launches still cannot create a
    // stack of secondary-process dialogs if IPC is unavailable.
    vectorclick::win::UniqueHandle notice_mutex(CreateMutexW(
        nullptr, FALSE, vectorclick::win::SingleInstanceNoticeMutexName));
    if (!notice_mutex) {
        ActivateExistingWindow(existing);
        return;
    }

    const DWORD acquire = WaitForSingleObject(notice_mutex.get(), 0);
    if (acquire != WAIT_OBJECT_0 && acquire != WAIT_ABANDONED) {
        ActivateExistingWindow(existing);
        return;
    }

    ShowAlreadyRunningFallback(existing);
    ReleaseMutex(notice_mutex.get());
    ActivateExistingWindow(existing);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    const vectorclick::win::StartupOptions startup_options =
        vectorclick::win::ParseStartupOptions();

    if (!startup_options.elevated_restart) {
        if (HWND existing = FindExistingWindow(false); existing != nullptr) {
            HandleExistingInstance(existing);
            return 0;
        }
    }

    SingleInstanceGuard instance_guard;
    const InstanceAcquireResult acquire_result =
        instance_guard.Acquire(startup_options.elevated_restart);
    if (acquire_result == InstanceAcquireResult::InUse) {
        HandleExistingInstance(FindExistingWindow(true));
        return 0;
    }
    if (acquire_result == InstanceAcquireResult::Error) {
        MessageBoxW(
            nullptr,
            L"Vector Click could not initialize its single-instance safety lock.",
            L"Vector Click could not start",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    vectorclick::win::MainWindow main_window(
        instance,
        startup_options.target_to_restore,
        startup_options.page_to_restore,
        startup_options.settings_to_restore,
        startup_options.profile_id_to_restore,
        startup_options.settings_restore_invalid);
    if (!main_window.Create()) {
        MessageBoxW(nullptr, L"Vector Click could not create its main window.", L"Vector Click", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Prime the dark client and child-control surfaces while the main window
    // is hidden. A temporary dark client cover remains above the controls for
    // the first visible frame and is removed by the posted startup-finish
    // transaction after the native children complete a synchronous repaint.
    RedrawWindow(main_window.Handle(),
                 nullptr,
                 nullptr,
                 RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
    ShowWindow(main_window.Handle(), show_command);
    UpdateWindow(main_window.Handle());

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Key-capture fields must see physical virtual-key messages before
        // dialog navigation or native combo-box type-ahead can reinterpret
        // them. This is especially important for Enter, Tab, Space, arrows,
        // and letters that prefix named entries such as S / Space or D / Down.
        if (main_window.PreTranslateMessage(message)) {
            continue;
        }
        if (!IsDialogMessageW(main_window.Handle(), &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

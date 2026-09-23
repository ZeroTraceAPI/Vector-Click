#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Windows/win32_raii.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace vectorclick::win {

class SafetyShield {
public:
    SafetyShield() = default;
    ~SafetyShield();

    SafetyShield(const SafetyShield&) = delete;
    SafetyShield& operator=(const SafetyShield&) = delete;

    [[nodiscard]] bool Initialize(HINSTANCE instance, HWND notification_window);
    void Shutdown() noexcept;

    [[nodiscard]] bool Show() noexcept;
    void MarkComplete() noexcept;
    void MarkFailed() noexcept;
    void MarkRetryFailed() noexcept;
    void BeginForceExit() noexcept;
    void Close() noexcept;
    [[nodiscard]] bool IsVisible() const noexcept {
        return requested_visible_.load(std::memory_order_acquire);
    }

private:
    enum class Status : std::uint8_t {
        Stopping,
        Complete,
        Failed,
        RetryFailed,
        Retrying,
        ForceStopping,
    };

    static constexpr UINT CommandShow = WM_APP + 200;
    static constexpr UINT CommandMarkComplete = WM_APP + 201;
    static constexpr UINT CommandMarkFailed = WM_APP + 202;
    static constexpr UINT CommandMarkRetryFailed = WM_APP + 203;
    static constexpr UINT CommandBeginForceExit = WM_APP + 204;
    static constexpr UINT CommandClose = WM_APP + 205;
    static constexpr UINT CommandShutdown = WM_APP + 206;

    void ThreadMain() noexcept;
    [[nodiscard]] bool PostCommand(UINT command) noexcept;
    [[nodiscard]] bool ShowOnThread();
    void BeginForceExitOnThread();
    void SetTerminalStatusOnThread(Status status);
    void CloseOnThread(bool notify_main_window);

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    [[nodiscard]] bool CanClose() const noexcept {
        return status_ == Status::Complete || status_ == Status::Failed ||
               status_ == Status::RetryFailed;
    }
    void Layout();
    void Paint();
    void DrawButton(const DRAWITEMSTRUCT& item,
                    bool force_exit_button,
                    bool retry_cleanup_button);
    void RecreateFontsForDpi();
    void CoverVirtualDesktop();
    void ReassertTopmost() noexcept;

    std::thread thread_;
    std::atomic<DWORD> thread_id_{0};
    UniqueHandle ready_event_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> requested_visible_{false};
    std::atomic<bool> visible_{false};
    std::atomic<bool> shutting_down_{false};

    HINSTANCE instance_{};
    HWND notification_window_{};

    // The following state is owned exclusively by the shield UI thread.
    HWND window_{};
    HWND close_button_{};
    HWND retry_cleanup_button_{};
    HWND force_exit_button_{};
    UINT dpi_{96};
    UniqueGdiObject title_font_;
    UniqueGdiObject status_font_;
    UniqueGdiObject normal_font_;
    UniqueGdiObject button_font_;
    Status status_{Status::Stopping};
    bool notify_on_destroy_{};
};

} // namespace vectorclick::win

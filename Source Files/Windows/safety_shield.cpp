#include "Windows/safety_shield.h"

#include "Windows/capture_exclusion.h"
#include "Windows/ui_theme.h"
#include "Windows/app_messages.h"
#include "Windows/force_exit.h"
#include "Windows/shared_image_loader.h"

#include <algorithm>
#include <string>
#include <utility>

namespace vectorclick::win {
namespace {

constexpr wchar_t ShieldClassName[] = L"VectorClickSafetyShield";
constexpr int CloseButtonId = 1;
constexpr int ForceExitButtonId = 2;
constexpr int RetryCleanupButtonId = 3;
constexpr UINT MainWindowNotificationFallbackTimeoutMilliseconds = 1'000;

int Scale(const int value, const UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

} // namespace

SafetyShield::~SafetyShield() {
    Shutdown();
}

bool SafetyShield::Initialize(const HINSTANCE instance,
                              const HWND notification_window) {
    if (initialized_.load(std::memory_order_acquire)) {
        return true;
    }
    if (thread_.joinable()) {
        return false;
    }

    instance_ = instance;
    notification_window_ = notification_window;
    ready_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ready_event_) {
        return false;
    }

    shutting_down_.store(false, std::memory_order_release);
    try {
        thread_ = std::thread([this] { ThreadMain(); });
    } catch (...) {
        return false;
    }
    if (WaitForSingleObject(ready_event_.get(), 5'000) != WAIT_OBJECT_0) {
        Shutdown();
        return false;
    }
    return initialized_.load(std::memory_order_acquire);
}

void SafetyShield::Shutdown() noexcept {
    if (!thread_.joinable()) {
        initialized_.store(false, std::memory_order_release);
        requested_visible_.store(false, std::memory_order_release);
        visible_.store(false, std::memory_order_release);
        return;
    }

    shutting_down_.store(true, std::memory_order_release);
    const DWORD id = thread_id_.load(std::memory_order_acquire);
    if (id != 0) {
        if (PostThreadMessageW(id, CommandShutdown, 0, 0) == FALSE) {
            PostThreadMessageW(id, WM_QUIT, 0, 0);
        }
    }
    thread_.join();
    initialized_.store(false, std::memory_order_release);
    requested_visible_.store(false, std::memory_order_release);
    visible_.store(false, std::memory_order_release);
    thread_id_.store(0, std::memory_order_release);
}

bool SafetyShield::Show() noexcept {
    requested_visible_.store(true, std::memory_order_release);
    if (PostCommand(CommandShow)) {
        return true;
    }
    requested_visible_.store(false, std::memory_order_release);
    return false;
}

void SafetyShield::MarkComplete() noexcept {
    (void)PostCommand(CommandMarkComplete);
}

void SafetyShield::MarkFailed() noexcept {
    (void)PostCommand(CommandMarkFailed);
}

void SafetyShield::MarkRetryFailed() noexcept {
    (void)PostCommand(CommandMarkRetryFailed);
}

void SafetyShield::BeginForceExit() noexcept {
    (void)PostCommand(CommandBeginForceExit);
}

void SafetyShield::Close() noexcept {
    (void)PostCommand(CommandClose);
}

bool SafetyShield::PostCommand(const UINT command) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) {
        return false;
    }
    const DWORD id = thread_id_.load(std::memory_order_acquire);
    return id != 0 && PostThreadMessageW(id, command, 0, 0) != FALSE;
}

void SafetyShield::ThreadMain() noexcept {
    // Publish the thread ID only after the message queue exists. Shutdown can
    // then safely post to every published ID, while shutting_down_ covers the
    // interval before this worker starts or reaches queue initialization.
    MSG queue_initializer{};
    PeekMessageW(&queue_initializer, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    thread_id_.store(GetCurrentThreadId(), std::memory_order_release);

    if (shutting_down_.load(std::memory_order_acquire)) {
        if (ready_event_) {
            SetEvent(ready_event_.get());
        }
        thread_id_.store(0, std::memory_order_release);
        return;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = ShieldClassName;
    const ATOM registered = RegisterClassExW(&window_class);
    const bool class_ready = registered != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    initialized_.store(class_ready, std::memory_order_release);
    if (ready_event_) {
        SetEvent(ready_event_.get());
    }
    if (!class_ready) {
        thread_id_.store(0, std::memory_order_release);
        return;
    }

    bool exit_requested = false;
    MSG message{};
    while (!exit_requested && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.hwnd == nullptr) {
            switch (message.message) {
            case CommandShow:
                (void)ShowOnThread();
                continue;
            case CommandMarkComplete:
                SetTerminalStatusOnThread(Status::Complete);
                continue;
            case CommandMarkFailed:
                SetTerminalStatusOnThread(Status::Failed);
                continue;
            case CommandMarkRetryFailed:
                SetTerminalStatusOnThread(Status::RetryFailed);
                continue;
            case CommandBeginForceExit:
                BeginForceExitOnThread();
                continue;
            case CommandClose:
                CloseOnThread(true);
                continue;
            case CommandShutdown:
                CloseOnThread(false);
                exit_requested = true;
                continue;
            default:
                break;
            }
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CloseOnThread(false);
    requested_visible_.store(false, std::memory_order_release);
    visible_.store(false, std::memory_order_release);
    initialized_.store(false, std::memory_order_release);
    thread_id_.store(0, std::memory_order_release);
}

bool SafetyShield::ShowOnThread() {
    if (window_ != nullptr && IsWindow(window_) != FALSE) {
        CoverVirtualDesktop();
        ShowWindow(window_, SW_SHOW);
        ReassertTopmost();
        SetForegroundWindow(window_);
        SetActiveWindow(window_);
        SetFocus(window_);
        InvalidateRect(window_, nullptr, TRUE);
        visible_.store(true, std::memory_order_release);
        return true;
    }

    status_ = Status::Stopping;
    notify_on_destroy_ = false;

    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // Deliberately unowned. Owned top-level windows are automatically hidden
    // whenever their owner is minimized. During the taskbar stress case the
    // VectorClick main window can be minimized and restored repeatedly, so the
    // protective shield must have an independent visibility lifecycle.
    window_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        ShieldClassName,
        L"Vector Click Safety Shield",
        WS_POPUP,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance_,
        this);

    if (window_ == nullptr) {
        requested_visible_.store(false, std::memory_order_release);
        return false;
    }

    // The Safety Shield remains available even if Windows refuses capture
    // exclusion. Emergency cleanup and recovery take priority over cosmetic
    // privacy behavior, but apply the requested policy before first show.
    if (IsCaptureExclusionRequested()) {
        (void)ApplyRequestedCaptureExclusion(window_);
    }

    dpi_ = GetDpiForWindow(window_);
    RecreateFontsForDpi();

    force_exit_button_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"Force Stop and Exit",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0,
        0,
        260,
        40,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ForceExitButtonId)),
        instance_,
        nullptr);

    retry_cleanup_button_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"Retry cleanup",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0,
        0,
        200,
        40,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(RetryCleanupButtonId)),
        instance_,
        nullptr);

    close_button_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"Close Safety Shield",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0,
        0,
        220,
        40,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(CloseButtonId)),
        instance_,
        nullptr);

    if (force_exit_button_ == nullptr || retry_cleanup_button_ == nullptr ||
        close_button_ == nullptr) {
        CloseOnThread(false);
        requested_visible_.store(false, std::memory_order_release);
        return false;
    }

    EnableWindow(close_button_, FALSE);
    if (button_font_.get() != nullptr) {
        SendMessageW(force_exit_button_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(button_font_.get()), TRUE);
        SendMessageW(retry_cleanup_button_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(button_font_.get()), TRUE);
        SendMessageW(close_button_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(button_font_.get()), TRUE);
    }
    Layout();

    visible_.store(true, std::memory_order_release);
    ShowWindow(window_, SW_SHOW);
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
    SetForegroundWindow(window_);
    SetActiveWindow(window_);
    SetFocus(window_);
    InvalidateRect(window_, nullptr, TRUE);
    return true;
}

void SafetyShield::BeginForceExitOnThread() {
    if (window_ == nullptr || status_ == Status::ForceStopping) {
        return;
    }

    status_ = Status::ForceStopping;
    if (force_exit_button_ != nullptr) {
        SetWindowTextW(force_exit_button_, L"Force stopping...");
        EnableWindow(force_exit_button_, FALSE);
        InvalidateRect(force_exit_button_, nullptr, FALSE);
    }
    if (retry_cleanup_button_ != nullptr) {
        EnableWindow(retry_cleanup_button_, FALSE);
        InvalidateRect(retry_cleanup_button_, nullptr, FALSE);
    }
    if (close_button_ != nullptr) {
        EnableWindow(close_button_, FALSE);
        InvalidateRect(close_button_, nullptr, FALSE);
    }
    InvalidateRect(window_, nullptr, TRUE);
    ReassertTopmost();
    SetFocus(window_);
}

void SafetyShield::SetTerminalStatusOnThread(const Status status) {
    if (window_ == nullptr || status == Status::Stopping ||
        status_ == Status::ForceStopping) {
        return;
    }

    if (status_ == status) {
        if ((status == Status::Failed || status == Status::RetryFailed) &&
            retry_cleanup_button_ != nullptr) {
            SetFocus(retry_cleanup_button_);
        } else if (close_button_ != nullptr) {
            SetFocus(close_button_);
        }
        return;
    }

    // A release warning takes precedence over a previously published success.
    status_ = status;
    const bool failed = status == Status::Failed || status == Status::RetryFailed;
    if (retry_cleanup_button_ != nullptr) {
        SetWindowTextW(retry_cleanup_button_, L"Retry cleanup");
        ShowWindow(retry_cleanup_button_, failed ? SW_SHOW : SW_HIDE);
        EnableWindow(retry_cleanup_button_, failed);
    }
    EnableWindow(close_button_, TRUE);
    Layout();
    InvalidateRect(window_, nullptr, TRUE);
    ReassertTopmost();
    SetFocus(failed && retry_cleanup_button_ != nullptr
                 ? retry_cleanup_button_
                 : close_button_);
}

void SafetyShield::CloseOnThread(const bool notify_main_window) {
    if (window_ == nullptr) {
        return;
    }
    notify_on_destroy_ = notify_main_window;
    DestroyWindow(window_);
}

LRESULT CALLBACK SafetyShield::WindowProc(const HWND window,
                                           const UINT message,
                                           const WPARAM w_param,
                                           const LPARAM l_param) {
    SafetyShield* self = reinterpret_cast<SafetyShield*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = static_cast<SafetyShield*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }

    if (self != nullptr) {
        return self->HandleMessage(message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT SafetyShield::HandleMessage(const UINT message,
                                    const WPARAM w_param,
                                    const LPARAM l_param) {
    switch (message) {
    case WM_SIZE:
        Layout();
        return 0;

    case WM_DPICHANGED:
        dpi_ = HIWORD(w_param);
        RecreateFontsForDpi();
        CoverVirtualDesktop();
        Layout();
        InvalidateRect(window_, nullptr, TRUE);
        return 0;

    case WM_DISPLAYCHANGE:
        CoverVirtualDesktop();
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(w_param) == WA_INACTIVE &&
            visible_.load(std::memory_order_acquire)) {
            ReassertTopmost();
        }
        break;

    case WM_PAINT:
        Paint();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_COMMAND:
        if (LOWORD(w_param) == CloseButtonId && CanClose()) {
            CloseOnThread(true);
        } else if (LOWORD(w_param) == RetryCleanupButtonId &&
                   (status_ == Status::Failed || status_ == Status::RetryFailed)) {
            status_ = Status::Retrying;
            SetWindowTextW(retry_cleanup_button_, L"Retrying cleanup...");
            EnableWindow(retry_cleanup_button_, FALSE);
            EnableWindow(close_button_, FALSE);
            InvalidateRect(window_, nullptr, TRUE);
            ReassertTopmost();

            const HWND notification = notification_window_;
            if (notification == nullptr || IsWindow(notification) == FALSE ||
                PostMessageW(notification, WM_APP_RETRY_CLEANUP, 0, 0) == FALSE) {
                SetTerminalStatusOnThread(Status::RetryFailed);
            }
        } else if (LOWORD(w_param) == ForceExitButtonId &&
                   status_ != Status::ForceStopping) {
            BeginForceExitOnThread();

            // The shield thread must retain a hard end-task guarantee even if
            // the main UI thread is saturated or no longer dispatching its
            // queue. The main window starts the same bounded watchdog when it
            // receives the request and performs the cleanup passes; this local
            // fallback independently guarantees process termination.
            force_exit::StartWatchdogOrTerminate();

            const HWND notification = notification_window_;
            if (notification == nullptr || IsWindow(notification) == FALSE ||
                PostMessageW(notification, WM_APP_FORCE_STOP_EXIT, 0, 0) == FALSE) {
                force_exit::TerminateCurrentProcess();
            }
        }
        return 0;

    case WM_DRAWITEM:
        if (l_param != 0 &&
            (w_param == CloseButtonId || w_param == RetryCleanupButtonId ||
             w_param == ForceExitButtonId)) {
            DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(l_param),
                       w_param == ForceExitButtonId,
                       w_param == RetryCleanupButtonId);
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        if (w_param == VK_ESCAPE && CanClose()) {
            CloseOnThread(true);
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((w_param & 0xFFF0U) == SC_CLOSE && !CanClose()) {
            return 0;
        }
        break;

    case WM_CLOSE:
        if (CanClose()) {
            CloseOnThread(true);
        }
        return 0;

    case WM_DESTROY: {
        const bool notify = notify_on_destroy_ &&
                            !shutting_down_.load(std::memory_order_acquire);
        notify_on_destroy_ = false;
        window_ = nullptr;
        close_button_ = nullptr;
        retry_cleanup_button_ = nullptr;
        force_exit_button_ = nullptr;
        status_ = Status::Stopping;
        requested_visible_.store(false, std::memory_order_release);
        visible_.store(false, std::memory_order_release);
        if (notify && notification_window_ != nullptr &&
            IsWindow(notification_window_) != FALSE &&
            PostMessageW(notification_window_,
                         WM_APP_SAFETY_SHIELD_CLOSED,
                         0,
                         0) == FALSE) {
            // Closing the shield clears its atomic visibility state before the
            // main window is notified. If Windows cannot queue that notification,
            // the Emergency Stop latch could otherwise remain locked forever.
            // Use a bounded synchronous fallback only for this failure path. The
            // corresponding main-window handler does not call back into the
            // shield once visibility is false, so this does not introduce a
            // cross-thread call cycle.
            DWORD_PTR ignored_result{};
            (void)SendMessageTimeoutW(
                notification_window_,
                WM_APP_SAFETY_SHIELD_CLOSED,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                MainWindowNotificationFallbackTimeoutMilliseconds,
                &ignored_result);
        }
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(window_, message, w_param, l_param);
}

void SafetyShield::Layout() {
    if (window_ == nullptr || close_button_ == nullptr ||
        retry_cleanup_button_ == nullptr || force_exit_button_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const int force_width = Scale(260, dpi_);
    const int retry_width = Scale(200, dpi_);
    const int close_width = Scale(220, dpi_);
    const int height = Scale(40, dpi_);
    const int gap = Scale(16, dpi_);
    const bool show_retry = status_ == Status::Failed ||
                            status_ == Status::RetryFailed ||
                            status_ == Status::Retrying;
    const int total_width = show_retry
        ? force_width + gap + retry_width + gap + close_width
        : force_width + gap + close_width;
    int x = (client.right - total_width) / 2;
    const int y = (client.bottom / 2) + Scale(82, dpi_);
    MoveWindow(force_exit_button_, x, y, force_width, height, TRUE);
    x += force_width + gap;
    if (show_retry) {
        MoveWindow(retry_cleanup_button_, x, y, retry_width, height, TRUE);
        x += retry_width + gap;
    }
    MoveWindow(close_button_, x, y, close_width, height, TRUE);
}

void SafetyShield::RecreateFontsForDpi() {
    UniqueGdiObject new_title(CreateFontW(
        -MulDiv(20, static_cast<int>(dpi_), 72),
        0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
    UniqueGdiObject new_status(CreateFontW(
        -MulDiv(13, static_cast<int>(dpi_), 72),
        0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
    UniqueGdiObject new_normal(CreateFontW(
        -MulDiv(11, static_cast<int>(dpi_), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
    UniqueGdiObject new_button(CreateFontW(
        -MulDiv(10, static_cast<int>(dpi_), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));

    if (new_title.get() != nullptr) {
        title_font_ = std::move(new_title);
    }
    if (new_status.get() != nullptr) {
        status_font_ = std::move(new_status);
    }
    if (new_normal.get() != nullptr) {
        normal_font_ = std::move(new_normal);
    }
    if (new_button.get() != nullptr) {
        if (force_exit_button_ != nullptr) {
            SendMessageW(force_exit_button_, WM_SETFONT,
                         reinterpret_cast<WPARAM>(new_button.get()), TRUE);
        }
        if (retry_cleanup_button_ != nullptr) {
            SendMessageW(retry_cleanup_button_, WM_SETFONT,
                         reinterpret_cast<WPARAM>(new_button.get()), TRUE);
        }
        if (close_button_ != nullptr) {
            SendMessageW(close_button_, WM_SETFONT,
                         reinterpret_cast<WPARAM>(new_button.get()), TRUE);
        }
        button_font_ = std::move(new_button);
    }
}

void SafetyShield::DrawButton(const DRAWITEMSTRUCT& item,
                              const bool force_exit_button,
                              const bool retry_cleanup_button) {
    RECT rect = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;

    const COLORREF background = force_exit_button
        ? (disabled ? ui::SurfaceAlt
                    : pressed ? ui::DangerPressed : ui::SurfaceAlt)
        : retry_cleanup_button
              ? (disabled ? ui::SurfaceAlt
                          : pressed ? ui::CleanupSurfacePressed
                                    : ui::CleanupSurface)
              : (disabled ? ui::SurfaceAlt
                          : pressed ? ui::SurfacePressed : ui::SurfaceHover);
    const COLORREF border = force_exit_button
        ? ui::Danger
        : retry_cleanup_button
              ? (disabled ? ui::BorderSoft
                          : focused ? ui::CleanupActionHover
                                    : ui::CleanupAction)
              : disabled ? ui::BorderSoft
                         : focused ? ui::AccentHover : ui::Accent;
    const COLORREF text = force_exit_button
        ? (disabled ? ui::Disabled : ui::Danger)
        : retry_cleanup_button
              ? (disabled ? ui::Disabled : ui::CleanupActionHover)
              : (disabled ? ui::Disabled : ui::Text);

    HBRUSH background_brush = CreateSolidBrush(background);
    FillRect(item.hDC, &rect, background_brush);
    DeleteObject(background_brush);

    HBRUSH border_brush = CreateSolidBrush(border);
    FrameRect(item.hDC, &rect, border_brush);
    DeleteObject(border_brush);

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    const HGDIOBJ button_font = button_font_.get() != nullptr
        ? button_font_.get()
        : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ old_font = SelectObject(item.hDC, button_font);
    wchar_t caption[64]{};
    GetWindowTextW(item.hwndItem, caption,
                   static_cast<int>(sizeof(caption) / sizeof(caption[0])));

    if (retry_cleanup_button) {
        const int caption_length = lstrlenW(caption);
        SIZE text_size{};
        (void)GetTextExtentPoint32W(item.hDC,
                                    caption,
                                    caption_length,
                                    &text_size);
        const int icon_size = Scale(20, dpi_);
        const int icon_gap = Scale(8, dpi_);
        const int group_width = icon_size + icon_gap + text_size.cx;
        const int button_width = static_cast<int>(rect.right - rect.left);
        int group_left = static_cast<int>(rect.left) +
                         std::max(Scale(8, dpi_),
                                  (button_width - group_width) / 2);
        int vertical_offset = 0;
        if (pressed) {
            group_left += Scale(1, dpi_);
            vertical_offset = Scale(1, dpi_);
        }

        RECT glyph_rect{};
        glyph_rect.left = group_left;
        glyph_rect.right = glyph_rect.left + icon_size;
        glyph_rect.top = rect.top +
                         (((rect.bottom - rect.top) - icon_size) / 2) +
                         vertical_offset;
        glyph_rect.bottom = glyph_rect.top + icon_size;
        ui::DrawGlyph(item.hDC,
                      ui::Glyph::Repeat,
                      glyph_rect,
                      text,
                      std::max(1, Scale(1, dpi_)));

        RECT text_rect = rect;
        text_rect.left = glyph_rect.right + icon_gap;
        text_rect.right = std::min(rect.right - Scale(8, dpi_),
                                   text_rect.left + text_size.cx + Scale(2, dpi_));
        if (pressed) {
            OffsetRect(&text_rect, 0, vertical_offset);
        }
        DrawTextW(item.hDC,
                  caption,
                  -1,
                  &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        DrawTextW(item.hDC,
                  caption,
                  -1,
                  &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(item.hDC, old_font);
}

void SafetyShield::CoverVirtualDesktop() {
    if (window_ == nullptr) {
        return;
    }
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void SafetyShield::ReassertTopmost() noexcept {
    if (window_ == nullptr || IsWindow(window_) == FALSE) {
        return;
    }
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void SafetyShield::Paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    RECT client{};
    GetClientRect(window_, &client);

    HBRUSH background = CreateSolidBrush(ui::Window);
    FillRect(dc, &client, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::Text);

    const HGDIOBJ old_font = SelectObject(
        dc, title_font_.get() != nullptr
                ? title_font_.get()
                : GetStockObject(DEFAULT_GUI_FONT));
    RECT title_rect{client.left + Scale(40, dpi_),
                    client.top + client.bottom / 2 - Scale(138, dpi_),
                    client.right - Scale(40, dpi_),
                    client.top + client.bottom / 2 - Scale(88, dpi_)};
    DrawTextW(dc, L"Emergency Stop", -1, &title_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const wchar_t* status_text = L"Status: Stopping generated input...";
    COLORREF status_color = ui::Warning;
    const wchar_t* message =
        L"Vector Click has stopped scheduling input and is releasing tracked mouse buttons and keys.\n"
        L"Keep this shield open until cleanup finishes. Use Force Stop and Exit only if needed.";

    if (status_ == Status::Complete) {
        status_text = L"Status: Emergency Stop complete";
        status_color = ui::Success;
        message =
            L"Cleanup finished and all tracked release events were submitted.\n"
            L"Verify that the target has stopped. Force Stop and Exit remains available as a final fallback.";
    } else if (status_ == Status::Failed) {
        status_text = L"Status: Attention required";
        status_color = ui::Danger;
        message =
            L"Vector Click could not confirm every tracked release event.\n"
            L"Resume or unblock the target if it is suspended or unresponsive, then use Retry cleanup. No new press is generated. Force Stop and Exit remains the final fallback.";
    } else if (status_ == Status::RetryFailed) {
        status_text = L"Status: Cleanup retry failed";
        status_color = ui::Danger;
        message =
            L"The target still did not accept every tracked release event. It may be suspended, unresponsive, closed, or blocked by privilege boundaries.\n"
            L"Correct the target condition and retry again, or use Force Stop and Exit as the final fallback.";
    } else if (status_ == Status::Retrying) {
        status_text = L"Status: Retrying release cleanup...";
        status_color = ui::Warning;
        message =
            L"Vector Click is resubmitting release events for every tracked mouse button and key.\n"
            L"No new press events are generated during this retry.";
    } else if (status_ == Status::ForceStopping) {
        status_text = L"Status: Force stopping Vector Click...";
        status_color = ui::Danger;
        message =
            L"Vector Click is repeating Emergency Stop and release cleanup before terminating.\n"
            L"The application and this shield will close when the process ends.";
    }

    SelectObject(dc, status_font_.get() != nullptr
                         ? status_font_.get()
                         : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, status_color);
    RECT status_rect{client.left + Scale(40, dpi_),
                     client.top + client.bottom / 2 - Scale(78, dpi_),
                     client.right - Scale(40, dpi_),
                     client.top + client.bottom / 2 - Scale(38, dpi_)};
    DrawTextW(dc, status_text, -1, &status_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, normal_font_.get() != nullptr
                         ? normal_font_.get()
                         : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, ui::Text);
    const int message_margin = Scale(40, dpi_);
    const int client_width = static_cast<int>(client.right - client.left);
    const int available_message_width =
        std::max(Scale(320, dpi_), client_width - (message_margin * 2));
    const int message_width =
        std::min(available_message_width, Scale(880, dpi_));
    const int message_left =
        static_cast<int>(client.left) + ((client_width - message_width) / 2);
    RECT message_rect{message_left,
                      client.top + client.bottom / 2 - Scale(28, dpi_),
                      message_left + message_width,
                      client.top + client.bottom / 2 + Scale(68, dpi_)};
    DrawTextW(dc,
              message,
              -1,
              &message_rect,
              DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);

    SelectObject(dc, old_font);
    EndPaint(window_, &paint);
}

} // namespace vectorclick::win

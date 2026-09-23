#include "Windows/centered_message_box.h"

#include "Windows/capture_exclusion.h"

#include "Windows/shared_image_loader.h"
#include "Windows/ui_theme.h"
#include "Windows/win32_raii.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <commctrl.h>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace vectorclick::win {
namespace {

constexpr wchar_t MessageDialogClassName[] = L"VectorClickMessageDialog";
constexpr int VectorClickIconResourceId = 101;

constexpr int MessageTextId = 2001;
constexpr int MessageIconId = 2002;

std::atomic_bool message_box_active{false};
std::atomic<HWND> active_message_box_window{};

int Scale(const int value, const UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

HBRUSH SurfaceBrush() noexcept {
    static const HBRUSH brush = CreateSolidBrush(ui::Surface);
    return brush;
}

int SafeSuppressedResult(const UINT type) noexcept {
    switch (type & MB_TYPEMASK) {
    case MB_OK:
        return IDOK;
    case MB_OKCANCEL:
    case MB_RETRYCANCEL:
    case MB_CANCELTRYCONTINUE:
        return IDCANCEL;
    case MB_ABORTRETRYIGNORE:
        return IDABORT;
    case MB_YESNO:
        return IDNO;
    case MB_YESNOCANCEL:
        return IDCANCEL;
    default:
        return IDCANCEL;
    }
}

void BringActiveMessageBoxForward() noexcept {
    const HWND dialog = active_message_box_window.load(std::memory_order_acquire);
    if (dialog == nullptr || IsWindow(dialog) == FALSE) {
        return;
    }

    ShowWindow(dialog, SW_RESTORE);
    SetForegroundWindow(dialog);

    FLASHWINFO flash{};
    flash.cbSize = sizeof(flash);
    flash.hwnd = dialog;
    flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    flash.uCount = 3;
    flash.dwTimeout = 0;
    FlashWindowEx(&flash);
}

class MessageBoxActivityGuard {
public:
    MessageBoxActivityGuard() noexcept {
        bool expected = false;
        owns_activity_ = message_box_active.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    ~MessageBoxActivityGuard() {
        if (owns_activity_) {
            active_message_box_window.store(nullptr, std::memory_order_release);
            message_box_active.store(false, std::memory_order_release);
        }
    }

    MessageBoxActivityGuard(const MessageBoxActivityGuard&) = delete;
    MessageBoxActivityGuard& operator=(const MessageBoxActivityGuard&) = delete;

    [[nodiscard]] bool OwnsActivity() const noexcept { return owns_activity_; }

private:
    bool owns_activity_{};
};

struct DialogButton {
    int id{};
    std::wstring label;
    HWND window{};
    int width{};
};

bool HasMessageIcon(const UINT type) noexcept {
    switch (type & MB_ICONMASK) {
    case MB_ICONERROR:
    case MB_ICONWARNING:
    case MB_ICONINFORMATION:
    case MB_ICONQUESTION:
        return true;
    default:
        return false;
    }
}

bool UsesVectorWarningIcon(const UINT type) noexcept {
    return (type & MB_ICONMASK) == MB_ICONWARNING;
}

bool UsesVectorInformationIcon(const UINT type) noexcept {
    return (type & MB_ICONMASK) == MB_ICONINFORMATION;
}

HICON MessageIconForType(const UINT type) noexcept {
    switch (type & MB_ICONMASK) {
    case MB_ICONERROR:
        return LoadSharedDefaultIcon(nullptr, IDI_ERROR);
    case MB_ICONWARNING:
        // The stock warning icon can expose a detached mask pixel when a
        // high-DPI static control stretches it. VectorClick paints this one
        // as an anti-aliased vector symbol instead.
        return nullptr;
    case MB_ICONINFORMATION:
        // The stock information icon is a fixed raster asset. Stretching it in
        // the DPI-scaled message-box slot leaves visibly stepped edges, so the
        // dialog paints a procedural anti-aliased symbol instead.
        return nullptr;
    case MB_ICONQUESTION:
        return LoadSharedDefaultIcon(nullptr, IDI_QUESTION);
    default:
        return nullptr;
    }
}

class DarkMessageDialog final {
public:
    DarkMessageDialog(const HWND owner,
                      const wchar_t* const text,
                      const wchar_t* const caption,
                      const UINT type,
                      const MessageBoxButtonLabels* const labels)
        : owner_(owner),
          placement_window_(ResolvePlacementWindow(owner)),
          instance_(GetModuleHandleW(nullptr)),
          text_(text != nullptr ? text : L""),
          caption_(caption != nullptr ? caption : L"Vector Click"),
          type_(type),
          close_result_(SafeSuppressedResult(type)),
          result_(close_result_) {
        ConfigureButtons(labels);
    }

    int Run() noexcept {
        const HWND root_owner =
            owner_ != nullptr && IsWindow(owner_) != FALSE
                ? GetAncestor(owner_, GA_ROOTOWNER)
                : nullptr;
        const bool owner_topmost =
            root_owner != nullptr && IsWindow(root_owner) != FALSE &&
            (GetWindowLongPtrW(root_owner, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const bool dialog_topmost = (type_ & MB_TOPMOST) != 0 || owner_topmost;
        const UINT fallback_type = dialog_topmost ? (type_ | MB_TOPMOST) : type_;

        if (!RegisterDialogClass()) {
            return MessageBoxW(
                owner_, text_.c_str(), caption_.c_str(), fallback_type);
        }

        dpi_ = placement_window_ != nullptr ? GetDpiForWindow(placement_window_) : 96;
        const DWORD extended_style =
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT |
            (dialog_topmost ? WS_EX_TOPMOST : 0);
        window_ = CreateWindowExW(
            extended_style,
            MessageDialogClassName,
            caption_.c_str(),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            Scale(420, dpi_),
            Scale(220, dpi_),
            owner_,
            nullptr,
            instance_,
            this);
        if (window_ == nullptr) {
            return MessageBoxW(
                owner_, text_.c_str(), caption_.c_str(), fallback_type);
        }

        // Warnings and recovery dialogs must remain available if Windows
        // refuses the affinity, but request the same protection before first
        // show whenever capture exclusion is active.
        if (IsCaptureExclusionRequested()) {
            (void)ApplyRequestedCaptureExclusion(window_);
        }

        active_message_box_window.store(window_, std::memory_order_release);
        ui::ApplyDarkTitleBar(window_);
        ApplyWindowIcons();
        CenterOverPlacement();

        const bool disable_owner = owner_ != nullptr && IsWindow(owner_) != FALSE &&
                                   IsWindowEnabled(owner_) != FALSE;
        if (disable_owner) {
            EnableWindow(owner_, FALSE);
        }

        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        if ((type_ & MB_SETFOREGROUND) != 0 || (type_ & MB_TOPMOST) != 0) {
            SetForegroundWindow(window_);
        }
        FocusDefaultButton();

        MSG message{};
        bool received_quit = false;
        int quit_code = 0;
        while (window_ != nullptr && IsWindow(window_) != FALSE) {
            const BOOL status = GetMessageW(&message, nullptr, 0, 0);
            if (status <= 0) {
                if (status == 0) {
                    received_quit = true;
                    quit_code = static_cast<int>(message.wParam);
                }
                break;
            }

            if (message.message == WM_KEYDOWN && message.hwnd != nullptr &&
                (message.hwnd == window_ || IsChild(window_, message.hwnd) != FALSE)) {
                if (message.wParam == VK_ESCAPE) {
                    Complete(close_result_);
                    continue;
                }
                if (message.wParam == VK_RETURN) {
                    const HWND focus = GetFocus();
                    if (focus != nullptr && GetParent(focus) == window_) {
                        wchar_t class_name[16]{};
                        GetClassNameW(focus, class_name,
                                      static_cast<int>(std::size(class_name)));
                        if (_wcsicmp(class_name, L"Button") == 0) {
                            SendMessageW(focus, BM_CLICK, 0, 0);
                            continue;
                        }
                    }
                    ClickDefaultButton();
                    continue;
                }
            }

            if (IsDialogMessageW(window_, &message) == FALSE) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (disable_owner && owner_ != nullptr && IsWindow(owner_) != FALSE) {
            EnableWindow(owner_, TRUE);
            SetActiveWindow(owner_);
            SetForegroundWindow(owner_);

            // A centered alert can itself be owned by another Vector Click
            // modal window, such as Manage Local Profiles or Choose target
            // window. Restoring only that immediate popup to the foreground can
            // leave its disabled root owner behind an unrelated application in
            // the top-level Z-order. Keep the root owner immediately behind the
            // restored popup so the complete modal stack returns together.
            const HWND modal_root_owner = GetAncestor(owner_, GA_ROOTOWNER);
            if (modal_root_owner != nullptr && modal_root_owner != owner_ &&
                IsWindow(modal_root_owner) != FALSE &&
                IsWindowVisible(modal_root_owner) != FALSE) {
                (void)SetWindowPos(modal_root_owner,
                                   owner_,
                                   0,
                                   0,
                                   0,
                                   0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        if (received_quit) {
            PostQuitMessage(quit_code);
        }
        return result_;
    }

private:
    static HWND ResolvePlacementWindow(const HWND owner) noexcept {
        if (owner == nullptr || IsWindow(owner) == FALSE) {
            return nullptr;
        }
        const HWND root = GetAncestor(owner, GA_ROOT);
        return root != nullptr ? root : owner;
    }

    bool RegisterDialogClass() const noexcept {
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (GetClassInfoExW(instance_, MessageDialogClassName, &existing) != FALSE) {
            return true;
        }

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
        window_class.hIcon = LoadSharedDefaultIcon(
            instance_, MAKEINTRESOURCEW(VectorClickIconResourceId));
        window_class.hIconSm = window_class.hIcon;
        window_class.hbrBackground = nullptr;
        window_class.lpszClassName = MessageDialogClassName;
        return RegisterClassExW(&window_class) != 0;
    }

    static LRESULT CALLBACK WindowProc(const HWND window,
                                       const UINT message,
                                       const WPARAM w_param,
                                       const LPARAM l_param) noexcept {
        DarkMessageDialog* self = reinterpret_cast<DarkMessageDialog*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
            self = static_cast<DarkMessageDialog*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        return self != nullptr
                   ? self->HandleMessage(message, w_param, l_param)
                   : DefWindowProcW(window, message, w_param, l_param);
    }

    LRESULT HandleMessage(const UINT message,
                          const WPARAM w_param,
                          const LPARAM l_param) noexcept {
        switch (message) {
        case WM_CREATE:
            return OnCreate() ? 0 : -1;
        case WM_ERASEBKGND:
            PaintBackground(reinterpret_cast<HDC>(w_param));
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window_, &paint);
            PaintBackground(dc);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(w_param);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ui::Text);
            SetBkColor(dc, ui::Surface);
            return reinterpret_cast<LRESULT>(SurfaceBrush());
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
            if (draw != nullptr && draw->CtlType == ODT_BUTTON) {
                DrawButton(*draw);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND:
            if (HIWORD(w_param) == BN_CLICKED || HIWORD(w_param) == 0) {
                const int id = LOWORD(w_param);
                if (IsDialogButtonId(id) || id == IDCANCEL) {
                    Complete(IsDialogButtonId(id) ? id : close_result_);
                    return 0;
                }
            }
            break;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w_param);
            RecreateFont();
            const auto* suggested = reinterpret_cast<const RECT*>(l_param);
            if (suggested != nullptr) {
                suggested_left_ = suggested->left;
                suggested_top_ = suggested->top;
                has_suggested_position_ = true;
            }
            ApplyLayout();
            return 0;
        }
        case WM_CLOSE:
            Complete(close_result_);
            return 0;
        case WM_DESTROY:
            window_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, w_param, l_param);
    }

    bool OnCreate() noexcept {
        icon_window_ = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZECONTROL,
            0,
            0,
            1,
            1,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(MessageIconId)),
            instance_,
            nullptr);
        text_window_ = CreateWindowExW(
            0,
            L"STATIC",
            text_.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0,
            0,
            1,
            1,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(MessageTextId)),
            instance_,
            nullptr);
        if (icon_window_ == nullptr || text_window_ == nullptr) {
            return false;
        }

        message_icon_ = MessageIconForType(type_);
        if (message_icon_ != nullptr) {
            SendMessageW(icon_window_, STM_SETICON,
                         reinterpret_cast<WPARAM>(message_icon_), 0);
        } else {
            ShowWindow(icon_window_, SW_HIDE);
        }

        for (auto& button : buttons_) {
            button.window = CreateWindowExW(
                0,
                L"BUTTON",
                button.label.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0,
                0,
                1,
                1,
                window_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.id)),
                instance_,
                nullptr);
            if (button.window == nullptr) {
                return false;
            }
            ui::ApplyDarkControlTheme(button.window);
        }

        ui::ApplyDarkControlTheme(text_window_);
        ui::ApplyDarkControlTheme(icon_window_);
        RecreateFont();
        ApplyLayout();
        return true;
    }

    void ConfigureButtons(const MessageBoxButtonLabels* const labels) {
        const auto add = [this](const int id, const wchar_t* const label) {
            buttons_.push_back({id, label != nullptr ? label : L"", nullptr, 0});
        };

        switch (type_ & MB_TYPEMASK) {
        case MB_OK:
            add(IDOK, L"OK");
            break;
        case MB_OKCANCEL:
            add(IDOK, L"OK");
            add(IDCANCEL, L"Cancel");
            break;
        case MB_ABORTRETRYIGNORE:
            add(IDABORT, L"Abort");
            add(IDRETRY, L"Retry");
            add(IDIGNORE, L"Ignore");
            break;
        case MB_YESNO:
            add(IDYES, labels != nullptr && labels->yes != nullptr ? labels->yes : L"Yes");
            add(IDNO, labels != nullptr && labels->no != nullptr ? labels->no : L"No");
            break;
        case MB_YESNOCANCEL:
            add(IDYES, labels != nullptr && labels->yes != nullptr ? labels->yes : L"Yes");
            add(IDNO, labels != nullptr && labels->no != nullptr ? labels->no : L"No");
            add(IDCANCEL,
                labels != nullptr && labels->cancel != nullptr ? labels->cancel : L"Cancel");
            break;
        case MB_RETRYCANCEL:
            add(IDRETRY, L"Retry");
            add(IDCANCEL, L"Cancel");
            break;
        case MB_CANCELTRYCONTINUE:
            add(IDCANCEL, L"Cancel");
            add(IDTRYAGAIN, L"Try Again");
            add(IDCONTINUE, L"Continue");
            break;
        default:
            add(IDOK, L"OK");
            break;
        }

        std::size_t default_index = 0;
        switch (type_ & MB_DEFMASK) {
        case MB_DEFBUTTON2:
            default_index = 1;
            break;
        case MB_DEFBUTTON3:
            default_index = 2;
            break;
        case MB_DEFBUTTON4:
            default_index = 3;
            break;
        default:
            break;
        }
        if (buttons_.empty()) {
            default_button_id_ = IDOK;
        } else {
            default_button_id_ = buttons_[std::min(default_index, buttons_.size() - 1U)].id;
        }
    }

    void ApplyWindowIcons() const noexcept {
        if (window_ == nullptr) {
            return;
        }
        const HICON large = LoadSharedDefaultIcon(
            instance_, MAKEINTRESOURCEW(VectorClickIconResourceId));
        const HICON small = reinterpret_cast<HICON>(LoadImageW(
            instance_,
            MAKEINTRESOURCEW(VectorClickIconResourceId),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR | LR_SHARED));
        if (large != nullptr) {
            SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large));
        }
        if (small != nullptr) {
            SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
        }
    }

    void RecreateFont() noexcept {
        UniqueGdiObject font(CreateFontW(
            -MulDiv(9, static_cast<int>(dpi_), 72),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"));
        if (font.get() == nullptr) {
            return;
        }

        const WPARAM value = reinterpret_cast<WPARAM>(font.get());
        if (text_window_ != nullptr) {
            SendMessageW(text_window_, WM_SETFONT, value, TRUE);
        }
        for (const auto& button : buttons_) {
            if (button.window != nullptr) {
                SendMessageW(button.window, WM_SETFONT, value, TRUE);
            }
        }
        font_ = std::move(font);
    }

    void ApplyLayout() noexcept {
        if (window_ == nullptr || text_window_ == nullptr) {
            return;
        }

        const int margin = Scale(20, dpi_);
        const int content_top = Scale(22, dpi_);
        const int content_bottom = Scale(22, dpi_);
        const bool has_message_icon = HasMessageIcon(type_);
        const int icon_size = has_message_icon ? Scale(40, dpi_) : 0;
        const int icon_gap = has_message_icon ? Scale(16, dpi_) : 0;
        const int min_text_width = Scale(280, dpi_);
        const int max_text_width = Scale(500, dpi_);
        // Keep the button area visually distinct without leaving an
        // oversized band of unused space above and below a single button.
        const int footer_height = Scale(54, dpi_);
        const int button_height = Scale(34, dpi_);
        const int button_gap = Scale(8, dpi_);
        const int min_button_width = Scale(96, dpi_);
        const int button_padding = Scale(30, dpi_);

        HDC dc = GetDC(window_);
        const HFONT font = reinterpret_cast<HFONT>(
            font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));
        const HGDIOBJ previous = dc != nullptr ? SelectObject(dc, font) : nullptr;

        RECT measure{0, 0, max_text_width, 0};
        if (dc != nullptr) {
            DrawTextW(dc,
                      text_.c_str(),
                      static_cast<int>(text_.size()),
                      &measure,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        }
        int text_width = std::clamp(static_cast<int>(measure.right - measure.left),
                                    min_text_width,
                                    max_text_width);
        RECT wrapped{0, 0, text_width, 0};
        if (dc != nullptr) {
            DrawTextW(dc,
                      text_.c_str(),
                      static_cast<int>(text_.size()),
                      &wrapped,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        }
        const int text_height = std::max(Scale(20, dpi_), static_cast<int>(wrapped.bottom - wrapped.top));

        int button_total_width{};
        for (auto& button : buttons_) {
            SIZE size{};
            if (dc != nullptr) {
                GetTextExtentPoint32W(dc,
                                      button.label.c_str(),
                                      static_cast<int>(button.label.size()),
                                      &size);
            }
            button.width = std::max(min_button_width, static_cast<int>(size.cx) + button_padding);
            if (button_total_width != 0) {
                button_total_width += button_gap;
            }
            button_total_width += button.width;
        }

        if (previous != nullptr) {
            SelectObject(dc, previous);
        }
        if (dc != nullptr) {
            ReleaseDC(window_, dc);
        }

        const int content_width = margin + icon_size + icon_gap + text_width + margin;
        client_width_ = std::max(content_width, button_total_width + (margin * 2));
        content_height_ = content_top + std::max(icon_size, text_height) + content_bottom;
        client_height_ = content_height_ + footer_height;

        RECT outer{0, 0, client_width_, client_height_};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
        const DWORD extended_style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE));
        if (AdjustWindowRectExForDpi(&outer, style, FALSE, extended_style, dpi_) == FALSE) {
            AdjustWindowRectEx(&outer, style, FALSE, extended_style);
        }
        const int outer_width = outer.right - outer.left;
        const int outer_height = outer.bottom - outer.top;

        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
        int x = 0;
        int y = 0;
        if (has_suggested_position_) {
            x = suggested_left_;
            y = suggested_top_;
            has_suggested_position_ = false;
        } else {
            flags |= SWP_NOMOVE;
        }
        SetWindowPos(window_, nullptr, x, y, outer_width, outer_height, flags);

        const int text_x = margin + icon_size + icon_gap;
        const int body_height = content_height_ - content_top - content_bottom;
        if (has_message_icon) {
            icon_bounds_ = RECT{
                margin,
                content_top + (body_height - icon_size) / 2,
                margin + icon_size,
                content_top + (body_height - icon_size) / 2 + icon_size,
            };
        } else {
            SetRectEmpty(&icon_bounds_);
        }
        if (message_icon_ != nullptr) {
            MoveWindow(icon_window_,
                       icon_bounds_.left,
                       icon_bounds_.top,
                       icon_size,
                       icon_size,
                       TRUE);
            ShowWindow(icon_window_, SW_SHOWNA);
        } else {
            ShowWindow(icon_window_, SW_HIDE);
        }
        MoveWindow(text_window_,
                   text_x,
                   content_top + (body_height - text_height) / 2,
                   text_width,
                   text_height,
                   TRUE);

        int button_x = client_width_ - margin;
        const int button_y = content_height_ + (footer_height - button_height) / 2;
        for (auto iterator = buttons_.rbegin(); iterator != buttons_.rend(); ++iterator) {
            button_x -= iterator->width;
            MoveWindow(iterator->window,
                       button_x,
                       button_y,
                       iterator->width,
                       button_height,
                       TRUE);
            button_x -= button_gap;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void CenterOverPlacement() const noexcept {
        if (window_ == nullptr || placement_window_ == nullptr ||
            IsWindow(placement_window_) == FALSE) {
            return;
        }

        RECT dialog{};
        RECT placement{};
        if (GetWindowRect(window_, &dialog) == FALSE ||
            GetWindowRect(placement_window_, &placement) == FALSE) {
            return;
        }

        const int width = dialog.right - dialog.left;
        const int height = dialog.bottom - dialog.top;
        int x = placement.left + ((placement.right - placement.left) - width) / 2;
        int y = placement.top + ((placement.bottom - placement.top) - height) / 2;

        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        const HMONITOR handle = MonitorFromWindow(placement_window_, MONITOR_DEFAULTTONEAREST);
        if (handle != nullptr && GetMonitorInfoW(handle, &monitor) != FALSE) {
            const int maximum_x = std::max(static_cast<int>(monitor.rcWork.left),
                                           static_cast<int>(monitor.rcWork.right) - width);
            const int maximum_y = std::max(static_cast<int>(monitor.rcWork.top),
                                           static_cast<int>(monitor.rcWork.bottom) - height);
            x = std::clamp(x, static_cast<int>(monitor.rcWork.left), maximum_x);
            y = std::clamp(y, static_cast<int>(monitor.rcWork.top), maximum_y);
        }

        SetWindowPos(window_,
                     nullptr,
                     x,
                     y,
                     0,
                     0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    void PaintBackground(const HDC dc) const noexcept {
        if (dc == nullptr || window_ == nullptr) {
            return;
        }
        RECT client{};
        GetClientRect(window_, &client);
        RECT body = client;
        body.bottom = std::min(body.bottom, static_cast<LONG>(content_height_));
        ui::Fill(dc, body, ui::Surface);
        if (UsesVectorWarningIcon(type_) && !IsRectEmpty(&icon_bounds_)) {
            ui::DrawWarningIcon(dc, icon_bounds_);
        }
        if (UsesVectorInformationIcon(type_) && !IsRectEmpty(&icon_bounds_)) {
            ui::DrawInformationIcon(dc, icon_bounds_);
        }

        RECT footer = client;
        footer.top = body.bottom;
        ui::Fill(dc, footer, ui::Window);

        RECT divider{client.left, footer.top, client.right, footer.top + std::max(1, Scale(1, dpi_))};
        ui::Fill(dc, divider, ui::Divider);
    }

    void DrawButton(const DRAWITEMSTRUCT& draw) const noexcept {
        RECT bounds = draw.rcItem;
        const bool enabled = IsWindowEnabled(draw.hwndItem) != FALSE;
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const bool focused = (draw.itemState & ODS_FOCUS) != 0;
        const bool is_default = static_cast<int>(draw.CtlID) == default_button_id_;

        ui::Fill(draw.hDC, bounds, ui::Window);
        const COLORREF fill = !enabled
                                  ? ui::SurfaceAlt
                                  : pressed ? ui::SurfacePressed : ui::SurfaceAlt;
        const COLORREF border = !enabled
                                    ? ui::BorderSoft
                                    : (focused || is_default) ? ui::Accent : ui::Border;
        const COLORREF text_color = enabled ? ui::Text : ui::Disabled;
        ui::DrawRoundedPanel(draw.hDC,
                             bounds,
                             fill,
                             border,
                             Scale(7, dpi_),
                             std::max(1, Scale(1, dpi_)));

        RECT text_bounds = bounds;
        if (pressed) {
            OffsetRect(&text_bounds, Scale(1, dpi_), Scale(1, dpi_));
        }
        const HFONT font = reinterpret_cast<HFONT>(
            font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));
        wchar_t label[256]{};
        GetWindowTextW(draw.hwndItem, label, static_cast<int>(std::size(label)));
        ui::DrawTextLine(draw.hDC,
                         label,
                         text_bounds,
                         font,
                         text_color,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    bool IsDialogButtonId(const int id) const noexcept {
        return std::any_of(buttons_.begin(), buttons_.end(),
                           [id](const DialogButton& button) { return button.id == id; });
    }

    void FocusDefaultButton() const noexcept {
        const HWND button = GetDlgItem(window_, default_button_id_);
        if (button != nullptr) {
            SetFocus(button);
        }
    }

    void ClickDefaultButton() const noexcept {
        const HWND button = GetDlgItem(window_, default_button_id_);
        if (button != nullptr && IsWindowEnabled(button) != FALSE) {
            SendMessageW(button, BM_CLICK, 0, 0);
        }
    }

    void Complete(const int result) noexcept {
        result_ = result;
        if (window_ != nullptr && IsWindow(window_) != FALSE) {
            DestroyWindow(window_);
        }
    }

    HWND owner_{};
    HWND placement_window_{};
    HINSTANCE instance_{};
    std::wstring text_;
    std::wstring caption_;
    UINT type_{};
    int close_result_{};
    int result_{};
    int default_button_id_{IDOK};
    UINT dpi_{96};

    HWND window_{};
    HWND icon_window_{};
    HWND text_window_{};
    HICON message_icon_{};
    RECT icon_bounds_{};
    UniqueGdiObject font_;
    std::vector<DialogButton> buttons_;

    int client_width_{};
    int client_height_{};
    int content_height_{};
    bool has_suggested_position_{};
    int suggested_left_{};
    int suggested_top_{};
};

} // namespace

int ShowCenteredMessageBoxImpl(const HWND owner,
                               const wchar_t* const text,
                               const wchar_t* const caption,
                               const UINT type,
                               const MessageBoxButtonLabels* const labels) noexcept {
    MessageBoxActivityGuard activity_guard;
    if (!activity_guard.OwnsActivity()) {
        BringActiveMessageBoxForward();
        return SafeSuppressedResult(type);
    }

    DarkMessageDialog dialog(owner, text, caption, type, labels);
    return dialog.Run();
}

int ShowCenteredMessageBox(const HWND owner,
                           const wchar_t* const text,
                           const wchar_t* const caption,
                           const UINT type) noexcept {
    return ShowCenteredMessageBoxImpl(owner, text, caption, type, nullptr);
}

int ShowCenteredChoiceMessageBox(const HWND owner,
                                 const wchar_t* const text,
                                 const wchar_t* const caption,
                                 const UINT icon_type,
                                 const MessageBoxButtonLabels& labels) noexcept {
    return ShowCenteredMessageBoxImpl(
        owner, text, caption, MB_YESNOCANCEL | (icon_type & MB_ICONMASK), &labels);
}

void DismissActiveMessageBoxForEmergency() noexcept {
    const HWND dialog = active_message_box_window.load(std::memory_order_acquire);
    if (dialog == nullptr || IsWindow(dialog) == FALSE) {
        return;
    }

    PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    PostMessageW(dialog, WM_CLOSE, 0, 0);
}

} // namespace vectorclick::win

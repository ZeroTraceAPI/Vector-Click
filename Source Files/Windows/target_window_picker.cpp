#include "Windows/target_window_picker.h"

#include "Windows/capture_exclusion.h"
#include "Windows/centered_message_box.h"
#include "Windows/shared_image_loader.h"
#include "Windows/win32_raii.h"
#include "Windows/ui_theme.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vectorclick::win {
namespace {

constexpr wchar_t PickerClassName[] = L"VectorClickTargetWindowPicker";
constexpr wchar_t RecoveryPopupClassName[] = L"VectorClickTargetRecoveryPopup";
constexpr int RecoveryChoiceCount = 3;
constexpr int VectorClickIconResourceId = 101;
constexpr int InitialWidth = 690;
constexpr int InitialHeight = 500;
constexpr int MinimumWidth = 560;
constexpr int MinimumHeight = 410;

constexpr int WindowListId = 1001;
constexpr int RefreshButtonId = 1002;
constexpr int RecoveryComboId = 1003;
constexpr int AcceptButtonId = IDOK;
constexpr int CancelButtonId = IDCANCEL;
constexpr UINT_PTR WindowListSubclassId = 0x56434C54U; // "VCLT"

// Keep the selected row distinct without the bright native Explorer blue.
// The same color is used while the picker is inactive so screenshot tools or
// another temporary foreground window cannot turn the selection white.
constexpr COLORREF PickerSelection = ui::MakeColor(10, 42, 70);

int Scale(const int value, const UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void CenterOverOwner(const HWND window, const HWND owner) noexcept {
    RECT dialog{};
    RECT owner_rect{};
    if (window == nullptr || owner == nullptr ||
        GetWindowRect(window, &dialog) == FALSE ||
        GetWindowRect(owner, &owner_rect) == FALSE) {
        return;
    }

    const int width = dialog.right - dialog.left;
    const int height = dialog.bottom - dialog.top;
    int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    const HMONITOR monitor_handle = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    if (monitor_handle != nullptr && GetMonitorInfoW(monitor_handle, &monitor) != FALSE) {
        const int maximum_x = std::max(static_cast<int>(monitor.rcWork.left),
                                       static_cast<int>(monitor.rcWork.right) - width);
        const int maximum_y = std::max(static_cast<int>(monitor.rcWork.top),
                                       static_cast<int>(monitor.rcWork.bottom) - height);
        x = std::clamp(x, static_cast<int>(monitor.rcWork.left), maximum_x);
        y = std::clamp(y, static_cast<int>(monitor.rcWork.top), maximum_y);
    }

    SetWindowPos(window, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

class PickerDialog {
public:
    PickerDialog(HINSTANCE instance,
                 HWND owner,
                 const DWORD excluded_process_id,
                 const TargetWindowInfo& current,
                 TargetWindowInfo& output)
        : instance_(instance),
          owner_(owner),
          excluded_process_id_(excluded_process_id),
          current_window_(current.window),
          current_recovery_policy_(current.recovery_policy),
          output_(output) {}

    bool Run() {
        RegisterPickerClass();

        dpi_ = owner_ != nullptr ? GetDpiForWindow(owner_) : 96;
        window_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
            PickerClassName,
            L"Choose target window",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            Scale(InitialWidth, dpi_),
            Scale(InitialHeight, dpi_),
            owner_,
            nullptr,
            instance_,
            this);
        if (window_ == nullptr) {
            ShowCenteredMessageBox(
                owner_,
                L"The target-window picker could not be created.",
                L"Target window",
                MB_OK | MB_ICONERROR);
            return false;
        }

        if (IsCaptureExclusionRequested()) {
            (void)ApplyRequestedCaptureExclusion(window_);
        }

        ui::ApplyDarkTitleBar(window_);
        CenterOverOwner(window_, owner_);
        if (owner_ != nullptr && IsWindow(owner_) != FALSE) {
            EnableWindow(owner_, FALSE);
        }
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);

        MSG message{};
        bool received_quit = false;
        int quit_code = 0;
        while (window_ != nullptr && IsWindow(window_) != FALSE) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) {
                if (result == 0) {
                    received_quit = true;
                    quit_code = static_cast<int>(message.wParam);
                }
                break;
            }
            if (message.message == WM_KEYDOWN && message.hwnd != nullptr &&
                (message.hwnd == window_ || IsChild(window_, message.hwnd) != FALSE)) {
                const HWND focus = GetFocus();
                if (message.wParam == VK_ESCAPE) {
                    if (focus == recovery_combo_ && recovery_popup_open_) {
                        CloseRecoveryPopup(false, true);
                    } else {
                        Close(false);
                    }
                    continue;
                }
                if (message.wParam == VK_RETURN) {
                    if (focus == recovery_combo_) {
                        if (recovery_popup_open_) {
                            CloseRecoveryPopup(true, true);
                        }
                        continue;
                    }
                    if (focus == refresh_button_ || focus == accept_button_ ||
                        focus == cancel_button_) {
                        SendMessageW(focus, BM_CLICK, 0, 0);
                    } else {
                        AcceptSelection();
                    }
                    continue;
                }
                if (message.wParam == VK_TAB && focus == recovery_combo_ &&
                    recovery_popup_open_) {
                    CloseRecoveryPopup(true);
                }
            }
            if (IsDialogMessageW(window_, &message) == FALSE) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (owner_ != nullptr && IsWindow(owner_) != FALSE) {
            EnableWindow(owner_, TRUE);
            SetActiveWindow(owner_);
            SetForegroundWindow(owner_);
        }
        if (received_quit) {
            PostQuitMessage(quit_code);
        }
        return accepted_;
    }

private:
    static LRESULT CALLBACK WindowProc(const HWND window,
                                       const UINT message,
                                       const WPARAM w_param,
                                       const LPARAM l_param) {
        PickerDialog* self = reinterpret_cast<PickerDialog*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
            self = static_cast<PickerDialog*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self != nullptr
                   ? self->HandleMessage(message, w_param, l_param)
                   : DefWindowProcW(window, message, w_param, l_param);
    }

    static LRESULT CALLBACK HeaderSubclassProc(const HWND header,
                                               const UINT message,
                                               const WPARAM w_param,
                                               const LPARAM l_param,
                                               const UINT_PTR subclass_id,
                                               const DWORD_PTR reference_data) {
        auto* self = reinterpret_cast<PickerDialog*>(reference_data);
        if (self == nullptr) {
            return DefSubclassProc(header, message, w_param, l_param);
        }
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            self->PaintHeaderWindow(header);
            return 0;
        case WM_NCDESTROY:
            RemoveWindowSubclass(header, HeaderSubclassProc, subclass_id);
            break;
        default:
            break;
        }
        return DefSubclassProc(header, message, w_param, l_param);
    }

    static LRESULT CALLBACK WindowListSubclassProc(
        const HWND list,
        const UINT message,
        const WPARAM w_param,
        const LPARAM l_param,
        const UINT_PTR subclass_id,
        const DWORD_PTR reference_data) {
        auto* self = reinterpret_cast<PickerDialog*>(reference_data);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(list, WindowListSubclassProc, subclass_id);
            return DefSubclassProc(list, message, w_param, l_param);
        }

        const LRESULT result = DefSubclassProc(list, message, w_param, l_param);
        if (self != nullptr &&
            (message == WM_MOUSEMOVE || message == WM_TIMER ||
             message == WM_SHOWWINDOW)) {
            self->EnsureListTooltipCapturePolicy();
        }
        return result;
    }

    static LRESULT CALLBACK RecoveryComboSubclassProc(
        const HWND combo,
        const UINT message,
        const WPARAM w_param,
        const LPARAM l_param,
        const UINT_PTR subclass_id,
        const DWORD_PTR reference_data) {
        auto* self = reinterpret_cast<PickerDialog*>(reference_data);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(
                combo, RecoveryComboSubclassProc, subclass_id);
            return DefSubclassProc(combo, message, w_param, l_param);
        }
        if (self == nullptr) {
            return DefSubclassProc(combo, message, w_param, l_param);
        }

        if (message == WM_GETDLGCODE) {
            return DefSubclassProc(combo, message, w_param, l_param) |
                   DLGC_WANTARROWS | DLGC_WANTCHARS;
        }
        if (message == CB_GETDROPPEDSTATE) {
            return self->recovery_popup_open_ ? TRUE : FALSE;
        }
        if (message == CB_SHOWDROPDOWN) {
            if (w_param != FALSE) {
                self->OpenRecoveryPopup();
            } else {
                self->CloseRecoveryPopup(false, true);
            }
            return TRUE;
        }

        const bool enabled = IsWindowEnabled(combo) != FALSE;
        const bool key_message =
            message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const UINT key = static_cast<UINT>(w_param);
        const bool alt_down = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (enabled && key_message) {
            if (self->recovery_popup_open_) {
                switch (key) {
                case VK_ESCAPE:
                    self->CloseRecoveryPopup(false, true);
                    return 0;
                case VK_RETURN:
                case VK_SPACE:
                case VK_F4:
                    self->CloseRecoveryPopup(true, true);
                    return 0;
                case VK_UP:
                    if (alt_down) {
                        self->CloseRecoveryPopup(true, true);
                    } else {
                        self->MoveRecoveryPopupHighlight(
                            self->recovery_popup_highlight_ - 1);
                    }
                    return 0;
                case VK_DOWN:
                    if (alt_down) {
                        self->CloseRecoveryPopup(true, true);
                    } else {
                        self->MoveRecoveryPopupHighlight(
                            self->recovery_popup_highlight_ + 1);
                    }
                    return 0;
                case VK_HOME:
                case VK_PRIOR:
                    self->MoveRecoveryPopupHighlight(0);
                    return 0;
                case VK_END:
                case VK_NEXT:
                    self->MoveRecoveryPopupHighlight(RecoveryChoiceCount - 1);
                    return 0;
                case VK_TAB:
                    self->CloseRecoveryPopup(true);
                    break;
                default:
                    break;
                }
            } else {
                const bool open_key =
                    key == VK_F4 || key == VK_SPACE ||
                    (alt_down && (key == VK_DOWN || key == VK_UP));
                if (open_key) {
                    self->OpenRecoveryPopup();
                    return 0;
                }
                if (key == VK_UP) {
                    self->SetRecoveryComboSelection(
                        self->CurrentRecoverySelection() - 1);
                    return 0;
                }
                if (key == VK_DOWN) {
                    self->SetRecoveryComboSelection(
                        self->CurrentRecoverySelection() + 1);
                    return 0;
                }
                if (key == VK_HOME || key == VK_PRIOR) {
                    self->SetRecoveryComboSelection(0);
                    return 0;
                }
                if (key == VK_END || key == VK_NEXT) {
                    self->SetRecoveryComboSelection(RecoveryChoiceCount - 1);
                    return 0;
                }
            }
        }

        if (enabled && !self->recovery_popup_open_ &&
            message == WM_MOUSEWHEEL) {
            const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
            if (delta != 0) {
                self->SetRecoveryComboSelection(
                    self->CurrentRecoverySelection() + (delta < 0 ? 1 : -1));
            }
            return 0;
        }
        if (enabled &&
            (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK)) {
            SetFocus(combo);
            self->OpenRecoveryPopup();
            return 0;
        }
        if (message == WM_LBUTTONUP) {
            return 0;
        }
        if (message == WM_KILLFOCUS && self->recovery_popup_open_) {
            self->CloseRecoveryPopup(false);
        }

        if (message == WM_ERASEBKGND) {
            return 1;
        }
        if (message == WM_NCPAINT) {
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(combo, &paint);
            self->PaintRecoveryCombo(combo, dc);
            EndPaint(combo, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT) {
            self->PaintRecoveryCombo(
                combo, reinterpret_cast<HDC>(w_param));
            return 0;
        }

        const LRESULT result =
            DefSubclassProc(combo, message, w_param, l_param);
        if (message == WM_ENABLE || message == WM_SETFOCUS ||
            message == WM_KILLFOCUS || message == CB_SETCURSEL) {
            InvalidateRect(combo, nullptr, FALSE);
            UpdateWindow(combo);
        }
        return result;
    }

    static LRESULT CALLBACK RecoveryPopupProc(
        const HWND popup,
        const UINT message,
        const WPARAM w_param,
        const LPARAM l_param) {
        PickerDialog* self = reinterpret_cast<PickerDialog*>(
            GetWindowLongPtrW(popup, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
            self = static_cast<PickerDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(
                popup, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr) {
            return DefWindowProcW(popup, message, w_param, l_param);
        }

        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(popup, &paint);
            EndPaint(popup, &paint);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            const int item = self->RecoveryPopupItemAtPoint(point);
            if (item != self->recovery_popup_hover_) {
                self->recovery_popup_hover_ = item;
                self->RenderRecoveryPopup();
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            RECT client{};
            GetClientRect(popup, &client);
            if (PtInRect(&client, point) == FALSE) {
                self->CloseRecoveryPopup(false, true);
                return 0;
            }
            const int item = self->RecoveryPopupItemAtPoint(point);
            if (item >= 0) {
                self->MoveRecoveryPopupHighlight(item);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            const int item = self->RecoveryPopupItemAtPoint(point);
            if (item >= 0) {
                self->MoveRecoveryPopupHighlight(item);
                self->CloseRecoveryPopup(true, true);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
            if (delta != 0) {
                self->MoveRecoveryPopupHighlight(
                    self->recovery_popup_highlight_ + (delta < 0 ? 1 : -1));
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (self->recovery_popup_open_ &&
                reinterpret_cast<HWND>(l_param) != popup) {
                self->CloseRecoveryPopup(false, true);
            }
            return 0;
        case WM_CLOSE:
            self->CloseRecoveryPopup(false, true);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(popup, GWLP_USERDATA, 0);
            if (self->recovery_popup_ == popup) {
                self->recovery_popup_ = nullptr;
                self->recovery_popup_open_ = false;
            }
            return DefWindowProcW(popup, message, w_param, l_param);
        default:
            break;
        }
        return DefWindowProcW(popup, message, w_param, l_param);
    }

    void RegisterPickerClass() {
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (GetClassInfoExW(instance_, PickerClassName, &existing) == FALSE) {
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = WindowProc;
            window_class.hInstance = instance_;
            window_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
            window_class.hIcon = LoadSharedDefaultIcon(
                instance_, MAKEINTRESOURCEW(VectorClickIconResourceId));
            window_class.hIconSm = window_class.hIcon;
            window_class.hbrBackground = nullptr;
            window_class.lpszClassName = PickerClassName;
            RegisterClassExW(&window_class);
        }

        WNDCLASSEXW popup_existing{};
        popup_existing.cbSize = sizeof(popup_existing);
        if (GetClassInfoExW(
                instance_, RecoveryPopupClassName, &popup_existing) == FALSE) {
            WNDCLASSEXW popup_class{};
            popup_class.cbSize = sizeof(popup_class);
            popup_class.lpfnWndProc = RecoveryPopupProc;
            popup_class.hInstance = instance_;
            popup_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
            popup_class.hbrBackground = nullptr;
            popup_class.lpszClassName = RecoveryPopupClassName;
            RegisterClassExW(&popup_class);
        }
    }

    LRESULT HandleMessage(const UINT message,
                          const WPARAM w_param,
                          const LPARAM l_param) {
        switch (message) {
        case WM_CREATE:
            return OnCreate() ? 0 : -1;
        case WM_ENTERSIZEMOVE:
            interactive_resize_ = true;
            break;
        case WM_EXITSIZEMOVE:
            interactive_resize_ = false;
            break;
        case WM_SIZE:
            CloseRecoveryPopup(false);
            layout_in_progress_ = true;
            Layout(LOWORD(l_param), HIWORD(l_param));
            layout_in_progress_ = false;
            // Child moves can synchronously request parent background erases.
            // Repaint the picker background once after the complete layout instead
            // of repainting the full client area for every intermediate child move.
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(l_param);
            info->ptMinTrackSize.x = Scale(MinimumWidth, dpi_);
            info->ptMinTrackSize.y = Scale(MinimumHeight, dpi_);
            return 0;
        }
        case WM_DPICHANGED: {
            CloseRecoveryPopup(false);
            dpi_ = HIWORD(w_param);
            RecreateFont();
            const auto* suggested = reinterpret_cast<const RECT*>(l_param);
            SetWindowPos(window_, nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND: {
            if (layout_in_progress_ || interactive_resize_) {
                return 1;
            }
            HDC dc = reinterpret_cast<HDC>(w_param);
            PaintBackground(dc);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window_, &paint);
            PaintBackground(dc);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(w_param);
            const HWND control = reinterpret_cast<HWND>(l_param);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, IsWindowEnabled(control) ? ui::Text : ui::Disabled);
            static HBRUSH surface = CreateSolidBrush(ui::Window);
            return reinterpret_cast<LRESULT>(surface);
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(w_param);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, ui::SurfaceAlt);
            SetTextColor(dc, ui::Text);
            static HBRUSH surface = CreateSolidBrush(ui::SurfaceAlt);
            return reinterpret_cast<LRESULT>(surface);
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
            if (draw != nullptr && draw->CtlType == ODT_BUTTON) {
                DrawButton(*draw);
                return TRUE;
            }
            if (draw != nullptr && draw->CtlType == ODT_COMBOBOX &&
                draw->hwndItem == recovery_combo_) {
                DrawRecoveryComboItem(*draw);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND:
            switch (LOWORD(w_param)) {
            case RefreshButtonId:
                if (HIWORD(w_param) == BN_CLICKED) {
                    Refresh();
                }
                return 0;
            case AcceptButtonId:
                if (HIWORD(w_param) == BN_CLICKED) {
                    AcceptSelection();
                }
                return 0;
            case CancelButtonId:
                if (HIWORD(w_param) == BN_CLICKED) {
                    Close(false);
                }
                return 0;
            case RecoveryComboId:
                if (HIWORD(w_param) == CBN_SELCHANGE) {
                    UpdateRecoveryDescription();
                }
                return 0;
            default:
                break;
            }
            break;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(l_param);
            if (header != nullptr && header->hwndFrom == list_ &&
                header->code == NM_CUSTOMDRAW) {
                return DrawList(reinterpret_cast<NMLVCUSTOMDRAW*>(l_param));
            }
            if (header != nullptr && header->idFrom == WindowListId) {
                if (header->code == LVN_ITEMCHANGED) {
                    EnableWindow(accept_button_, SelectedIndex() >= 0);
                    return 0;
                }
                if (header->code == NM_DBLCLK) {
                    AcceptSelection();
                    return 0;
                }
            }
            break;
        }
        case WM_CLOSE:
            Close(false);
            return 0;
        case WM_DESTROY:
            CloseRecoveryPopup(false);
            if (recovery_popup_ != nullptr &&
                IsWindow(recovery_popup_) != FALSE) {
                DestroyWindow(recovery_popup_);
            }
            recovery_popup_ = nullptr;
            window_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, w_param, l_param);
    }

    bool OnCreate() {
        instruction_ = CreateChild(L"STATIC",
                                   L"Select a target window, then choose what Vector Click should do if that window closes or restarts.",
                                   SS_LEFT,
                                   -1);
        list_ = CreateChild(WC_LISTVIEWW,
                            L"",
                            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL,
                            WindowListId);
        recovery_label_ = CreateChild(L"STATIC", L"Target recovery", SS_LEFT, -1);
        recovery_combo_ = CreateChild(
            WC_COMBOBOXW,
            L"",
            CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
                WS_TABSTOP | WS_VSCROLL,
            RecoveryComboId);
        recovery_description_ = CreateChild(L"STATIC", L"", SS_LEFT, -1);
        status_ = CreateChild(L"STATIC", L"", SS_LEFT, -1);
        refresh_button_ = CreateChild(L"BUTTON", L"Refresh", BS_OWNERDRAW | WS_TABSTOP,
                                      RefreshButtonId);
        accept_button_ = CreateChild(L"BUTTON", L"Use selected", BS_OWNERDRAW | WS_TABSTOP,
                                     AcceptButtonId);
        cancel_button_ = CreateChild(L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP,
                                     CancelButtonId);
        recovery_popup_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            RecoveryPopupClassName,
            L"",
            WS_POPUP,
            0,
            0,
            1,
            1,
            window_,
            nullptr,
            instance_,
            this);
        if (recovery_popup_ != nullptr && IsCaptureExclusionRequested()) {
            (void)ApplyRequestedCaptureExclusion(recovery_popup_);
        }

        if (instruction_ == nullptr || list_ == nullptr ||
            recovery_label_ == nullptr || recovery_combo_ == nullptr ||
            recovery_description_ == nullptr || status_ == nullptr ||
            refresh_button_ == nullptr || accept_button_ == nullptr ||
            cancel_button_ == nullptr || recovery_popup_ == nullptr) {
            return false;
        }

        ListView_SetExtendedListViewStyle(
            list_,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        (void)SetWindowSubclass(list_,
                                WindowListSubclassProc,
                                WindowListSubclassId,
                                reinterpret_cast<DWORD_PTR>(this));
        EnsureListTooltipCapturePolicy();
        if (IsCaptureExclusionRequested() && list_tooltip_ == nullptr &&
            list_label_tips_enabled_) {
            // Do not leave a lazily created native popup available when it
            // cannot be protected before the picker is shown.
            (void)ListView_SetExtendedListViewStyleEx(
                list_, LVS_EX_LABELTIP, 0);
            list_label_tips_enabled_ = false;
        }
        ListView_SetBkColor(list_, ui::SurfaceAlt);
        ListView_SetTextBkColor(list_, ui::SurfaceAlt);
        ListView_SetTextColor(list_, ui::Text);
        ui::ApplyDarkControlTheme(list_);
        header_ = ListView_GetHeader(list_);
        if (header_ != nullptr) {
            ui::ApplyDarkControlTheme(header_);
            (void)SetWindowSubclass(header_,
                                    HeaderSubclassProc,
                                    1,
                                    reinterpret_cast<DWORD_PTR>(this));
        }
        for (const HWND control : {instruction_, recovery_label_, recovery_combo_,
                                   recovery_description_, status_, refresh_button_,
                                   accept_button_, cancel_button_}) {
            ui::ApplyDarkControlTheme(control);
        }
        (void)SetWindowSubclass(
            recovery_combo_,
            RecoveryComboSubclassProc,
            1,
            reinterpret_cast<DWORD_PTR>(this));

        SendMessageW(recovery_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Exact selected window only (recommended)"));
        SendMessageW(recovery_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Same application and window type"));
        SendMessageW(recovery_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Same application and exact title"));
        const int recovery_selection =
            current_recovery_policy_ == TargetRecoveryPolicy::SameApplicationAndClass
                ? 1
                : current_recovery_policy_ == TargetRecoveryPolicy::SameApplicationAndTitle
                      ? 2
                      : 0;
        SendMessageW(
            recovery_combo_,
            CB_SETCURSEL,
            static_cast<WPARAM>(recovery_selection),
            0);
        UpdateRecoveryDescription();

        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        const wchar_t* headings[]{L"Window title", L"Process", L"PID", L"Privilege"};
        for (int index = 0; index < 4; ++index) {
            column.iSubItem = index;
            column.pszText = const_cast<wchar_t*>(headings[index]);
            column.cx = Scale(index == 0 ? 310 : index == 1 ? 150 : index == 2 ? 70 : 120, dpi_);
            ListView_InsertColumn(list_, index, &column);
        }

        RecreateFont();
        EnableWindow(accept_button_, FALSE);
        Refresh();
        return true;
    }

    HWND CreateChild(const wchar_t* class_name,
                     const wchar_t* text,
                     const DWORD style,
                     const int id,
                     const DWORD extended_style = 0) const {
        return CreateWindowExW(
            extended_style,
            class_name,
            text,
            WS_CHILD | WS_VISIBLE | style,
            0, 0, 100, 24,
            window_,
            id >= 0 ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
            instance_,
            nullptr);
    }

    void RecreateFont() {
        UniqueGdiObject new_font(CreateFontW(
            -MulDiv(9, static_cast<int>(dpi_), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
        if (new_font.get() == nullptr) {
            return;
        }
        const WPARAM font = reinterpret_cast<WPARAM>(new_font.get());
        if (recovery_combo_ != nullptr) {
            const LPARAM item_height = static_cast<LPARAM>(Scale(26, dpi_));
            SendMessageW(
                recovery_combo_,
                CB_SETITEMHEIGHT,
                static_cast<WPARAM>(-1),
                item_height);
            SendMessageW(recovery_combo_, CB_SETITEMHEIGHT, 0, item_height);
        }
        for (const HWND control : {instruction_, list_, recovery_label_, recovery_combo_,
                                   recovery_description_, status_, refresh_button_,
                                   accept_button_, cancel_button_}) {
            if (control != nullptr) {
                SendMessageW(control, WM_SETFONT, font, TRUE);
            }
        }
        font_ = std::move(new_font);
    }

    int MeasureTextWidth(const HDC dc,
                         const std::wstring_view text) const noexcept {
        if (dc == nullptr || text.empty()) {
            return 0;
        }
        SIZE extent{};
        return GetTextExtentPoint32W(dc,
                                     text.data(),
                                     static_cast<int>(text.size()),
                                     &extent) != FALSE
                   ? static_cast<int>(extent.cx)
                   : 0;
    }

    void UpdateColumnWidths() const noexcept {
        if (list_ == nullptr || IsWindow(list_) == FALSE) {
            return;
        }

        RECT client{};
        if (GetClientRect(list_, &client) == FALSE) {
            return;
        }
        const int available_width =
            std::max(0, static_cast<int>(client.right - client.left) -
                            std::max(1, Scale(1, dpi_)));
        if (available_width < Scale(320, dpi_)) {
            return;
        }

        const HDC dc = GetDC(list_);
        if (dc == nullptr) {
            return;
        }
        const HFONT font = reinterpret_cast<HFONT>(
            font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));
        const HGDIOBJ old_font = SelectObject(dc, font);

        int process_text_width = MeasureTextWidth(dc, L"Process");
        int pid_text_width = MeasureTextWidth(dc, L"PID");
        int privilege_text_width = MeasureTextWidth(dc, L"Privilege");

        privilege_text_width = std::max(
            privilege_text_width,
            MeasureTextWidth(dc, L"Administrator privilege"));
        privilege_text_width = std::max(
            privilege_text_width,
            MeasureTextWidth(dc, L"Standard privilege"));

        for (const auto& target : windows_) {
            process_text_width = std::max(
                process_text_width,
                MeasureTextWidth(dc, target.process_name));
            pid_text_width = std::max(
                pid_text_width,
                MeasureTextWidth(dc, std::to_wstring(target.process_id)));
            privilege_text_width = std::max(
                privilege_text_width,
                MeasureTextWidth(dc, TargetElevationText(target.elevation)));
        }

        if (old_font != nullptr && old_font != HGDI_ERROR) {
            SelectObject(dc, old_font);
        }
        ReleaseDC(list_, dc);

        const int horizontal_padding = Scale(18, dpi_);
        const int privilege_width = std::clamp(
            privilege_text_width + horizontal_padding,
            Scale(138, dpi_),
            Scale(184, dpi_));
        const int pid_width = std::clamp(
            pid_text_width + horizontal_padding,
            Scale(62, dpi_),
            Scale(94, dpi_));
        const int preferred_process_width = std::clamp(
            process_text_width + horizontal_padding,
            Scale(116, dpi_),
            Scale(205, dpi_));

        const int minimum_title_width = Scale(165, dpi_);
        const int minimum_process_width = Scale(105, dpi_);
        const int flexible_width =
            std::max(1, available_width - pid_width - privilege_width);

        int process_width = std::min(
            preferred_process_width,
            std::max(minimum_process_width,
                     flexible_width - minimum_title_width));
        process_width = std::min(process_width,
                                 std::max(1, flexible_width - Scale(130, dpi_)));
        const int title_width = std::max(1, flexible_width - process_width);

        ListView_SetColumnWidth(list_, 0, title_width);
        ListView_SetColumnWidth(list_, 1, process_width);
        ListView_SetColumnWidth(list_, 2, pid_width);
        ListView_SetColumnWidth(list_, 3, privilege_width);
    }

    void Layout(const int width, const int height) {
        const int margin = Scale(14, dpi_);
        const int gap = Scale(8, dpi_);
        const int button_width = Scale(102, dpi_);
        const int button_height = Scale(30, dpi_);
        const int instruction_height = Scale(24, dpi_);
        const int status_height = Scale(24, dpi_);
        const int recovery_row_height = Scale(30, dpi_);
        const int recovery_description_height = Scale(72, dpi_);
        const int recovery_label_width = Scale(112, dpi_);

        MoveWindow(instruction_, margin, margin,
                   std::max(0, width - (margin * 2)), instruction_height, TRUE);

        const int bottom_row_y = height - margin - button_height;
        const int status_y = bottom_row_y - gap - status_height;
        const int recovery_description_y = status_y - gap - recovery_description_height;
        const int recovery_row_y = recovery_description_y - gap - recovery_row_height;
        const int list_y = margin + instruction_height + gap;
        const int list_height = std::max(0, recovery_row_y - gap - list_y);
        list_frame_ = {margin, list_y,
                       std::max(margin, width - margin),
                       list_y + list_height};
        const int list_inset = std::max(1, Scale(1, dpi_));
        MoveWindow(list_,
                   list_frame_.left + list_inset,
                   list_frame_.top + list_inset,
                   std::max(0, static_cast<int>(list_frame_.right - list_frame_.left) - (list_inset * 2)),
                   std::max(0, static_cast<int>(list_frame_.bottom - list_frame_.top) - (list_inset * 2)),
                   TRUE);
        UpdateColumnWidths();
        MoveWindow(recovery_label_,
                   margin,
                   recovery_row_y,
                   recovery_label_width,
                   recovery_row_height,
                   TRUE);
        MoveWindow(recovery_combo_,
                   margin + recovery_label_width + gap,
                   recovery_row_y,
                   std::max(0, width - (margin * 2) - recovery_label_width - gap),
                   Scale(220, dpi_),
                   TRUE);
        // The recovery description wraps as its width changes. The normal
        // MoveWindow(..., TRUE) repaint can leave stale glyph fragments when
        // parent background erases are coalesced during interactive resize.
        // Move it without an intermediate repaint, then explicitly erase and
        // redraw the complete static control once at its committed geometry.
        MoveWindow(recovery_description_,
                   margin,
                   recovery_description_y,
                   std::max(0, width - (margin * 2)),
                   recovery_description_height,
                   FALSE);
        RedrawWindow(recovery_description_,
                     nullptr,
                     nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        // The status static is transparent and moves vertically with the bottom
        // controls. With parent background erases coalesced during interactive
        // resize, MoveWindow(..., TRUE) can leave it partially painted or erase
        // it entirely as its old and new rectangles are exposed. Publish the
        // committed geometry first, then redraw the complete status control.
        MoveWindow(status_, margin, status_y,
                   std::max(0, width - (margin * 2)), status_height, FALSE);
        RedrawWindow(status_,
                     nullptr,
                     nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);

        int x = width - margin - button_width;
        MoveWindow(cancel_button_, x, bottom_row_y, button_width, button_height, TRUE);
        x -= gap + button_width;
        MoveWindow(accept_button_, x, bottom_row_y, button_width, button_height, TRUE);
        x -= gap + button_width;
        MoveWindow(refresh_button_, x, bottom_row_y, button_width, button_height, TRUE);
    }

    void PaintBackground(const HDC dc) const noexcept {
        if (dc == nullptr) {
            return;
        }
        RECT client{};
        GetClientRect(window_, &client);
        ui::Fill(dc, client, ui::Window);
        if (list_frame_.right > list_frame_.left &&
            list_frame_.bottom > list_frame_.top) {
            ui::DrawRoundedPanel(dc,
                                 list_frame_,
                                 ui::SurfaceAlt,
                                 ui::Border,
                                 Scale(5, dpi_),
                                 std::max(1, Scale(1, dpi_)));
        }
    }

    void PaintRecoveryCombo(const HWND combo,
                            const HDC dc) const noexcept {
        if (combo == nullptr || dc == nullptr || IsWindow(combo) == FALSE) {
            return;
        }

        RECT bounds{};
        if (GetClientRect(combo, &bounds) == FALSE ||
            bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
            return;
        }

        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const HDC buffer = CreateCompatibleDC(dc);
        const HBITMAP bitmap =
            buffer != nullptr ? CreateCompatibleBitmap(dc, width, height) : nullptr;
        HGDIOBJ previous_bitmap = nullptr;
        if (buffer != nullptr && bitmap != nullptr) {
            previous_bitmap = SelectObject(buffer, bitmap);
        }
        const bool buffered = previous_bitmap != nullptr &&
                              previous_bitmap != HGDI_ERROR;
        const HDC target = buffered ? buffer : dc;

        const UINT control_dpi =
            std::max<UINT>(96, GetDpiForWindow(combo));
        const bool enabled = IsWindowEnabled(combo) != FALSE;
        const bool focused = GetFocus() == combo;
        const bool dropped = recovery_popup_open_;
        const int arrow_width = Scale(28, control_dpi);

        ui::Fill(target, bounds, ui::SurfaceAlt);

        RECT arrow = bounds;
        arrow.left = std::max(
            bounds.left + Scale(20, control_dpi),
            bounds.right - arrow_width);
        InflateRect(&arrow, -Scale(1, control_dpi), -Scale(1, control_dpi));

        std::wstring text;
        const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (selection >= 0) {
            const LRESULT length = SendMessageW(
                combo,
                CB_GETLBTEXTLEN,
                static_cast<WPARAM>(selection),
                0);
            if (length >= 0) {
                text.resize(static_cast<std::size_t>(length) + 1U);
                if (length > 0) {
                    SendMessageW(
                        combo,
                        CB_GETLBTEXT,
                        static_cast<WPARAM>(selection),
                        reinterpret_cast<LPARAM>(text.data()));
                }
                text.resize(static_cast<std::size_t>(length));
            }
        }

        RECT text_bounds = bounds;
        text_bounds.left += Scale(11, control_dpi);
        text_bounds.right = arrow.left - Scale(8, control_dpi);
        const HFONT selected_font = reinterpret_cast<HFONT>(
            font_.get() != nullptr
                ? font_.get()
                : GetStockObject(DEFAULT_GUI_FONT));
        ui::DrawTextLine(
            target,
            text,
            text_bounds,
            selected_font,
            enabled ? ui::Text : ui::Disabled,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                DT_NOPREFIX);

        const COLORREF border_color =
            enabled && (focused || dropped)
                ? ui::AccentHover
                : enabled ? ui::Border : ui::BorderSoft;
        const HPEN border_pen = CreatePen(
            PS_SOLID,
            std::max(1, Scale(1, control_dpi)),
            border_color);
        if (border_pen != nullptr) {
            const HGDIOBJ old_pen = SelectObject(target, border_pen);
            const HGDIOBJ old_brush =
                SelectObject(target, GetStockObject(NULL_BRUSH));
            Rectangle(target,
                      bounds.left,
                      bounds.top,
                      bounds.right,
                      bounds.bottom);
            MoveToEx(
                target, arrow.left, bounds.top + Scale(2, control_dpi), nullptr);
            LineTo(
                target, arrow.left, bounds.bottom - Scale(2, control_dpi));
            SelectObject(target, old_brush);
            SelectObject(target, old_pen);
            DeleteObject(border_pen);
        }

        RECT chevron = arrow;
        InflateRect(
            &chevron, -Scale(7, control_dpi), -Scale(7, control_dpi));
        ui::DrawGlyph(
            target,
            ui::Glyph::ChevronDown,
            chevron,
            enabled ? ui::Icon : ui::Disabled,
            std::max(1, Scale(2, control_dpi)));

        if (buffered) {
            BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previous_bitmap);
        }
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (buffer != nullptr) {
            DeleteDC(buffer);
        }
    }

    int CurrentRecoverySelection() const noexcept {
        if (recovery_combo_ == nullptr ||
            IsWindow(recovery_combo_) == FALSE) {
            return 0;
        }
        const LRESULT selection =
            SendMessageW(recovery_combo_, CB_GETCURSEL, 0, 0);
        return selection == CB_ERR
                   ? 0
                   : std::clamp(
                         static_cast<int>(selection),
                         0,
                         RecoveryChoiceCount - 1);
    }

    void SetRecoveryComboSelection(const int selection,
                                   const bool update_description = true) {
        if (recovery_combo_ == nullptr ||
            IsWindow(recovery_combo_) == FALSE) {
            return;
        }
        const int next =
            std::clamp(selection, 0, RecoveryChoiceCount - 1);
        const int current = CurrentRecoverySelection();
        if (current != next) {
            SendMessageW(
                recovery_combo_,
                CB_SETCURSEL,
                static_cast<WPARAM>(next),
                0);
        }
        InvalidateRect(recovery_combo_, nullptr, FALSE);
        UpdateWindow(recovery_combo_);
        if (update_description && current != next) {
            UpdateRecoveryDescription();
        }
    }

    bool OpenRecoveryPopup() {
        if (recovery_popup_open_ || recovery_combo_ == nullptr ||
            recovery_popup_ == nullptr ||
            IsWindowEnabled(recovery_combo_) == FALSE) {
            return false;
        }

        RECT combo_bounds{};
        if (GetWindowRect(recovery_combo_, &combo_bounds) == FALSE) {
            return false;
        }
        const int field_width =
            static_cast<int>(combo_bounds.right - combo_bounds.left);
        if (field_width <= 0) {
            return false;
        }

        const UINT popup_dpi =
            std::max<UINT>(96, GetDpiForWindow(recovery_combo_));
        const int border = std::max(1, Scale(1, popup_dpi));
        const LRESULT native_item_height =
            SendMessageW(recovery_combo_, CB_GETITEMHEIGHT, 0, 0);
        recovery_popup_item_height_ = std::max(
            Scale(30, popup_dpi),
            native_item_height == CB_ERR
                ? 0
                : static_cast<int>(native_item_height) + Scale(4, popup_dpi));
        recovery_popup_width_ = field_width;
        recovery_popup_height_ =
            border * 2 + recovery_popup_item_height_ * RecoveryChoiceCount;

        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        const HMONITOR monitor_handle =
            MonitorFromRect(&combo_bounds, MONITOR_DEFAULTTONEAREST);
        if (monitor_handle == nullptr ||
            GetMonitorInfoW(monitor_handle, &monitor) == FALSE) {
            return false;
        }
        const int work_left = static_cast<int>(monitor.rcWork.left);
        const int work_top = static_cast<int>(monitor.rcWork.top);
        const int work_right = static_cast<int>(monitor.rcWork.right);
        const int work_bottom = static_cast<int>(monitor.rcWork.bottom);

        // Match the main-window dropdown anchor calculation. The native
        // combo window can report a slightly shorter closed-field rectangle
        // than the owner-drawn selection height plus its visual chrome. Using
        // the same effective field height preserves the small gap beneath the
        // focused blue outline instead of letting this popup crowd / cover it.
        const LRESULT selection_height_result = SendMessageW(
            recovery_combo_,
            CB_GETITEMHEIGHT,
            static_cast<WPARAM>(-1),
            0);
        const int field_height = std::max(
            Scale(28, popup_dpi),
            selection_height_result == CB_ERR
                ? 0
                : static_cast<int>(selection_height_result) + Scale(4, popup_dpi));

        recovery_popup_x_ = static_cast<int>(combo_bounds.left);
        recovery_popup_y_ =
            static_cast<int>(combo_bounds.top) + field_height - border;
        if (recovery_popup_y_ + recovery_popup_height_ > work_bottom &&
            combo_bounds.top - recovery_popup_height_ >= work_top) {
            recovery_popup_y_ =
                static_cast<int>(combo_bounds.top) - recovery_popup_height_ + border;
        }
        recovery_popup_x_ = std::clamp(
            recovery_popup_x_,
            work_left,
            std::max(work_left, work_right - recovery_popup_width_));
        recovery_popup_y_ = std::clamp(
            recovery_popup_y_,
            work_top,
            std::max(work_top, work_bottom - recovery_popup_height_));

        recovery_popup_highlight_ = CurrentRecoverySelection();
        recovery_popup_hover_ = -1;
        recovery_popup_open_ = true;
        SetFocus(recovery_combo_);
        if (!RenderRecoveryPopup()) {
            recovery_popup_open_ = false;
            recovery_popup_highlight_ = -1;
            recovery_popup_hover_ = -1;
            return false;
        }
        // Keep the application-owned recovery list in the same Z-order band
        // as the picker. This mirrors the main-window combo popup rule so a
        // topmost Vector Click surface cannot cover its own dropdown content.
        // Explicit HWND_NOTOPMOST also clears any topmost state retained by
        // this reusable hidden popup after the picker is no longer topmost.
        const bool picker_topmost =
            (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        SetWindowPos(
            recovery_popup_,
            picker_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            recovery_popup_x_,
            recovery_popup_y_,
            recovery_popup_width_,
            recovery_popup_height_,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        SetCapture(recovery_popup_);
        InvalidateRect(recovery_combo_, nullptr, FALSE);
        UpdateWindow(recovery_combo_);
        return true;
    }

    void CloseRecoveryPopup(const bool commit_selection,
                            const bool clear_transient_focus = false) {
        if (!recovery_popup_open_) {
            return;
        }
        const int selection = recovery_popup_highlight_;
        recovery_popup_open_ = false;
        recovery_popup_hover_ = -1;
        if (GetCapture() == recovery_popup_) {
            ReleaseCapture();
        }
        if (recovery_popup_ != nullptr &&
            IsWindow(recovery_popup_) != FALSE) {
            ShowWindow(recovery_popup_, SW_HIDE);
        }
        if (clear_transient_focus && GetFocus() == recovery_combo_ &&
            list_ != nullptr && IsWindow(list_) != FALSE &&
            IsWindowEnabled(list_) != FALSE && IsWindowVisible(list_) != FALSE) {
            SetFocus(list_);
        }
        if (commit_selection && selection >= 0 &&
            selection < RecoveryChoiceCount) {
            SetRecoveryComboSelection(selection);
        } else if (recovery_combo_ != nullptr &&
                   IsWindow(recovery_combo_) != FALSE) {
            InvalidateRect(recovery_combo_, nullptr, FALSE);
            UpdateWindow(recovery_combo_);
        }
    }

    void MoveRecoveryPopupHighlight(const int index) {
        if (!recovery_popup_open_) {
            return;
        }
        const int next =
            std::clamp(index, 0, RecoveryChoiceCount - 1);
        if (next == recovery_popup_highlight_) {
            return;
        }
        recovery_popup_highlight_ = next;
        recovery_popup_hover_ = -1;
        RenderRecoveryPopup();
    }

    int RecoveryPopupItemAtPoint(const POINT point) const noexcept {
        if (!recovery_popup_open_ || recovery_popup_item_height_ <= 0 ||
            recovery_popup_width_ <= 0 || recovery_popup_height_ <= 0) {
            return -1;
        }
        const UINT popup_dpi =
            std::max<UINT>(96, GetDpiForWindow(recovery_combo_));
        const int border = std::max(1, Scale(1, popup_dpi));
        if (point.x < border || point.x >= recovery_popup_width_ - border ||
            point.y < border || point.y >= recovery_popup_height_ - border) {
            return -1;
        }
        const int row =
            (point.y - border) / recovery_popup_item_height_;
        return row >= 0 && row < RecoveryChoiceCount ? row : -1;
    }

    bool RenderRecoveryPopup() {
        if (recovery_popup_ == nullptr || recovery_combo_ == nullptr ||
            IsWindow(recovery_popup_) == FALSE ||
            IsWindow(recovery_combo_) == FALSE ||
            recovery_popup_width_ <= 0 || recovery_popup_height_ <= 0) {
            return false;
        }

        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info.bmiHeader.biWidth = recovery_popup_width_;
        bitmap_info.bmiHeader.biHeight = -recovery_popup_height_;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;

        void* pixel_memory = nullptr;
        const HDC screen_dc = GetDC(nullptr);
        if (screen_dc == nullptr) {
            return false;
        }
        UniqueGdiObject bitmap(CreateDIBSection(
            screen_dc,
            &bitmap_info,
            DIB_RGB_COLORS,
            &pixel_memory,
            nullptr,
            0));
        if (bitmap.get() == nullptr || pixel_memory == nullptr) {
            ReleaseDC(nullptr, screen_dc);
            return false;
        }

        const HDC memory_dc = CreateCompatibleDC(screen_dc);
        if (memory_dc == nullptr) {
            ReleaseDC(nullptr, screen_dc);
            return false;
        }
        const HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap.get());
        if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) {
            DeleteDC(memory_dc);
            ReleaseDC(nullptr, screen_dc);
            return false;
        }

        RECT client{0, 0, recovery_popup_width_, recovery_popup_height_};
        ui::Fill(memory_dc, client, ui::SurfaceAlt);
        const UINT popup_dpi =
            std::max<UINT>(96, GetDpiForWindow(recovery_combo_));
        const int border = std::max(1, Scale(1, popup_dpi));
        const HFONT popup_font = reinterpret_cast<HFONT>(
            font_.get() != nullptr
                ? font_.get()
                : GetStockObject(DEFAULT_GUI_FONT));

        for (int index = 0; index < RecoveryChoiceCount; ++index) {
            RECT item{
                border,
                border + index * recovery_popup_item_height_,
                recovery_popup_width_ - border,
                std::min(
                    recovery_popup_height_ - border,
                    border + (index + 1) * recovery_popup_item_height_)};
            if (index == recovery_popup_highlight_) {
                ui::Fill(memory_dc, item, ui::SurfacePressed);
            } else if (index == recovery_popup_hover_) {
                ui::Fill(memory_dc, item, ui::SurfaceHover);
            }

            const LRESULT length = SendMessageW(
                recovery_combo_,
                CB_GETLBTEXTLEN,
                static_cast<WPARAM>(index),
                0);
            std::wstring text;
            if (length >= 0) {
                text.resize(static_cast<std::size_t>(length) + 1U);
                if (length > 0) {
                    SendMessageW(
                        recovery_combo_,
                        CB_GETLBTEXT,
                        static_cast<WPARAM>(index),
                        reinterpret_cast<LPARAM>(text.data()));
                }
                text.resize(static_cast<std::size_t>(length));
            }

            RECT text_bounds = item;
            text_bounds.left += Scale(11, popup_dpi);
            text_bounds.right -= Scale(8, popup_dpi);
            ui::DrawTextLine(
                memory_dc,
                text,
                text_bounds,
                popup_font,
                IsWindowEnabled(recovery_combo_) != FALSE
                    ? ui::Text
                    : ui::Disabled,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
        }

        const HPEN border_pen = CreatePen(PS_SOLID, border, ui::Border);
        if (border_pen != nullptr) {
            const HGDIOBJ old_pen = SelectObject(memory_dc, border_pen);
            const HGDIOBJ old_brush =
                SelectObject(memory_dc, GetStockObject(NULL_BRUSH));
            Rectangle(
                memory_dc,
                0,
                0,
                recovery_popup_width_,
                recovery_popup_height_);
            SelectObject(memory_dc, old_brush);
            SelectObject(memory_dc, old_pen);
            DeleteObject(border_pen);
        }

        auto* pixels = static_cast<std::uint32_t*>(pixel_memory);
        const std::size_t pixel_count =
            static_cast<std::size_t>(recovery_popup_width_) *
            static_cast<std::size_t>(recovery_popup_height_);
        for (std::size_t index = 0; index < pixel_count; ++index) {
            pixels[index] |= 0xFF000000U;
        }

        POINT destination{recovery_popup_x_, recovery_popup_y_};
        SIZE size{recovery_popup_width_, recovery_popup_height_};
        POINT source{};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        const BOOL updated = UpdateLayeredWindow(
            recovery_popup_,
            screen_dc,
            &destination,
            &size,
            memory_dc,
            &source,
            0,
            &blend,
            ULW_ALPHA);

        SelectObject(memory_dc, old_bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return updated != FALSE;
    }

    void DrawRecoveryComboItem(const DRAWITEMSTRUCT& draw) const noexcept {
        RECT bounds = draw.rcItem;
        const bool enabled = IsWindowEnabled(draw.hwndItem) != FALSE;
        const bool selected = (draw.itemState & ODS_SELECTED) != 0;
        const bool focused = (draw.itemState & ODS_FOCUS) != 0;
        const bool edit_portion = (draw.itemState & ODS_COMBOBOXEDIT) != 0;
        const COLORREF fill =
            selected && !edit_portion ? ui::SurfacePressed : ui::SurfaceAlt;
        ui::Fill(draw.hDC, bounds, fill);

        LRESULT item_index = static_cast<LRESULT>(draw.itemID);
        if (item_index < 0) {
            item_index = SendMessageW(draw.hwndItem, CB_GETCURSEL, 0, 0);
        }

        std::wstring text;
        if (item_index >= 0) {
            const LRESULT length = SendMessageW(
                draw.hwndItem,
                CB_GETLBTEXTLEN,
                static_cast<WPARAM>(item_index),
                0);
            if (length >= 0) {
                text.resize(static_cast<std::size_t>(length) + 1U);
                if (length > 0) {
                    SendMessageW(
                        draw.hwndItem,
                        CB_GETLBTEXT,
                        static_cast<WPARAM>(item_index),
                        reinterpret_cast<LPARAM>(text.data()));
                }
                text.resize(static_cast<std::size_t>(length));
            }
        }

        const UINT control_dpi =
            std::max<UINT>(96, GetDpiForWindow(draw.hwndItem));
        RECT text_bounds = bounds;
        if (edit_portion) {
            RECT client{};
            if (GetClientRect(draw.hwndItem, &client) != FALSE &&
                !IsRectEmpty(&client)) {
                const int arrow_width = Scale(28, control_dpi);
                RECT arrow = client;
                arrow.left = std::max(
                    client.left + Scale(20, control_dpi),
                    client.right - arrow_width);
                text_bounds = client;
                text_bounds.left += Scale(11, control_dpi);
                text_bounds.right = std::max(
                    text_bounds.left,
                    arrow.left - Scale(8, control_dpi));
            } else {
                text_bounds.left += Scale(11, control_dpi);
                text_bounds.right -= Scale(8, control_dpi);
            }
        } else {
            text_bounds.left += Scale(11, control_dpi);
            text_bounds.right -= Scale(8, control_dpi);
        }
        const HFONT selected_font = reinterpret_cast<HFONT>(
            font_.get() != nullptr
                ? font_.get()
                : GetStockObject(DEFAULT_GUI_FONT));
        ui::DrawTextLine(
            draw.hDC,
            text,
            text_bounds,
            selected_font,
            enabled ? ui::Text : ui::Disabled,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                DT_NOPREFIX);
        if (focused && !edit_portion) {
            ui::DrawFocusOutline(
                draw.hDC,
                bounds,
                ui::AccentHover,
                Scale(3, control_dpi));
        }
    }

    void DrawButton(const DRAWITEMSTRUCT& draw) const noexcept {
        RECT bounds = draw.rcItem;
        const bool enabled = IsWindowEnabled(draw.hwndItem) != FALSE;
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const bool focused = (draw.itemState & ODS_FOCUS) != 0;
        const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
        const bool primary = draw.hwndItem == accept_button_;

        ui::Fill(draw.hDC, bounds, ui::Window);
        const COLORREF fill = !enabled
                                  ? ui::SurfaceAlt
                                  : pressed ? ui::SurfacePressed
                                            : hot ? ui::SurfaceHover : ui::SurfaceAlt;
        const COLORREF border = !enabled
                                    ? ui::BorderSoft
                                    : (focused || primary) ? ui::Accent : ui::Border;
        const COLORREF text_color = enabled
                                        ? (primary ? ui::AccentHover : ui::Text)
                                        : ui::Disabled;
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
        wchar_t text[128]{};
        GetWindowTextW(draw.hwndItem, text, static_cast<int>(std::size(text)));
        const HFONT font = reinterpret_cast<HFONT>(
            font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));
        ui::DrawTextLine(draw.hDC,
                         text,
                         text_bounds,
                         font,
                         text_color,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void PaintHeaderWindow(const HWND header) const noexcept {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(header, &paint);
        if (dc == nullptr) {
            return;
        }

        RECT client{};
        GetClientRect(header, &client);
        ui::Fill(dc, client, ui::SurfaceHover);

        const HFONT font = reinterpret_cast<HFONT>(
            font_.get() != nullptr ? font_.get() : GetStockObject(DEFAULT_GUI_FONT));
        const int count = Header_GetItemCount(header);
        for (int index = 0; index < count; ++index) {
            RECT bounds{};
            if (Header_GetItemRect(header, index, &bounds) == FALSE) {
                continue;
            }
            wchar_t text[256]{};
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = text;
            item.cchTextMax = static_cast<int>(std::size(text));
            (void)Header_GetItem(header, index, &item);

            RECT text_bounds = bounds;
            text_bounds.left += Scale(7, dpi_);
            text_bounds.right -= Scale(5, dpi_);
            ui::DrawTextLine(dc,
                             text,
                             text_bounds,
                             font,
                             ui::Text,
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT right_divider{bounds.right - std::max(1, Scale(1, dpi_)),
                               bounds.top,
                               bounds.right,
                               bounds.bottom};
            ui::Fill(dc, right_divider, ui::Divider);
        }
        RECT bottom_divider{client.left,
                            client.bottom - std::max(1, Scale(1, dpi_)),
                            client.right,
                            client.bottom};
        ui::Fill(dc, bottom_divider, ui::Divider);
        EndPaint(header, &paint);
    }

    LRESULT DrawList(NMLVCUSTOMDRAW* const custom) const noexcept {
        if (custom == nullptr) {
            return CDRF_DODEFAULT;
        }
        if (custom->nmcd.dwDrawStage == CDDS_PREPAINT) {
            return CDRF_NOTIFYITEMDRAW;
        }
        if (custom->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            return CDRF_NOTIFYSUBITEMDRAW;
        }
        if (custom->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
            const int row = static_cast<int>(custom->nmcd.dwItemSpec);
            const int sub_item = custom->iSubItem;
            if (row < 0 || sub_item < 0 || header_ == nullptr) {
                return CDRF_DODEFAULT;
            }

            RECT row_bounds{};
            RECT column_bounds{};
            if (ListView_GetItemRect(list_, row, &row_bounds, LVIR_BOUNDS) == FALSE ||
                Header_GetItemRect(header_, sub_item, &column_bounds) == FALSE) {
                return CDRF_DODEFAULT;
            }

            RECT cell{
                column_bounds.left,
                row_bounds.top,
                column_bounds.right,
                row_bounds.bottom,
            };
            const bool selected =
                (ListView_GetItemState(list_, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            const bool hot = ListView_GetHotItem(list_) == row;
            ui::Fill(custom->nmcd.hdc,
                     cell,
                     selected ? PickerSelection
                              : hot ? ui::SurfaceHover : ui::SurfaceAlt);

            if (selected && sub_item == 0) {
                RECT accent = cell;
                accent.right = accent.left + std::max(1, Scale(2, dpi_));
                ui::Fill(custom->nmcd.hdc, accent, ui::AccentPressed);
            }

            std::array<wchar_t, 1'024> text{};
            ListView_GetItemText(list_,
                                 row,
                                 sub_item,
                                 text.data(),
                                 static_cast<int>(text.size()));
            RECT text_bounds = cell;
            text_bounds.left += Scale(sub_item == 0 ? 8 : 7, dpi_);
            text_bounds.right -= Scale(6, dpi_);
            const HFONT font = reinterpret_cast<HFONT>(
                font_.get() != nullptr
                    ? font_.get()
                    : GetStockObject(DEFAULT_GUI_FONT));
            ui::DrawTextLine(custom->nmcd.hdc,
                             text.data(),
                             text_bounds,
                             font,
                             ui::Text,
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                 DT_END_ELLIPSIS | DT_NOPREFIX);
            return CDRF_SKIPDEFAULT;
        }
        return CDRF_DODEFAULT;
    }

    void EnsureListTooltipCapturePolicy() noexcept {
        if (list_ == nullptr || !list_label_tips_enabled_ ||
            !IsCaptureExclusionRequested()) {
            return;
        }

        const HWND tooltip = ListView_GetToolTips(list_);
        if (tooltip == nullptr || IsWindow(tooltip) == FALSE) {
            return;
        }
        if (list_tooltip_ == tooltip && list_tooltip_protected_) {
            return;
        }

        list_tooltip_ = tooltip;
        const CaptureExclusionOutcome outcome =
            SynchronizeRequestedCaptureExclusion(tooltip);
        if (outcome.result == CaptureExclusionResult::Applied) {
            list_tooltip_protected_ = true;
            return;
        }

        // A clipped title is still available through selection and horizontal
        // scrolling. Prefer losing the optional native label tip over exposing
        // a separate unprotected popup while the picker itself is excluded.
        (void)ListView_SetExtendedListViewStyleEx(
            list_, LVS_EX_LABELTIP, 0);
        (void)SendMessageW(tooltip, TTM_POP, 0, 0);
        ShowWindow(tooltip, SW_HIDE);
        list_label_tips_enabled_ = false;
        list_tooltip_protected_ = false;
    }

    TargetRecoveryPolicy SelectedRecoveryPolicy() const noexcept {
        if (recovery_combo_ == nullptr) {
            return TargetRecoveryPolicy::ExactWindowOnly;
        }
        const LRESULT selection = SendMessageW(recovery_combo_, CB_GETCURSEL, 0, 0);
        if (selection == 1) {
            return TargetRecoveryPolicy::SameApplicationAndClass;
        }
        if (selection == 2) {
            return TargetRecoveryPolicy::SameApplicationAndTitle;
        }
        return TargetRecoveryPolicy::ExactWindowOnly;
    }

    void UpdateRecoveryDescription() const {
        if (recovery_description_ == nullptr) {
            return;
        }
        std::wstring description =
            TargetRecoveryPolicyDescription(SelectedRecoveryPolicy());
        description += L" Vector Click checks for a replacement only while idle and reconnects only when one safe match exists.";
        SetWindowTextW(recovery_description_, description.c_str());
    }

    void Refresh() {
        windows_ = EnumerateTargetWindows(excluded_process_id_);
        ListView_DeleteAllItems(list_);

        int current_index = -1;
        for (std::size_t index = 0; index < windows_.size(); ++index) {
            const auto& target = windows_[index];
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = static_cast<int>(index);
            item.iSubItem = 0;
            item.pszText = const_cast<wchar_t*>(target.title.c_str());
            item.lParam = static_cast<LPARAM>(index);
            const int row = ListView_InsertItem(list_, &item);
            if (row < 0) {
                continue;
            }

            const std::wstring pid = std::to_wstring(target.process_id);
            ListView_SetItemText(list_, row, 1,
                                 const_cast<wchar_t*>(target.process_name.c_str()));
            ListView_SetItemText(list_, row, 2,
                                 const_cast<wchar_t*>(pid.c_str()));
            ListView_SetItemText(list_, row, 3,
                                 const_cast<wchar_t*>(TargetElevationText(target.elevation)));
            if (target.window == current_window_) {
                current_index = row;
            }
        }

        UpdateColumnWidths();

        const std::wstring status = windows_.empty()
                                        ? L"No selectable application windows were found."
                                        : std::to_wstring(windows_.size()) +
                                              (windows_.size() == 1
                                                   ? L" selectable window"
                                                   : L" selectable windows");
        SetWindowTextW(status_, status.c_str());

        if (current_index >= 0) {
            ListView_SetItemState(list_, current_index,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list_, current_index, FALSE);
        } else if (!windows_.empty()) {
            ListView_SetItemState(list_, 0,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
        EnableWindow(accept_button_, SelectedIndex() >= 0);
        SetFocus(list_);
    }

    int SelectedIndex() const noexcept {
        if (list_ == nullptr) {
            return -1;
        }
        return ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    }

    void AcceptSelection() {
        const int row = SelectedIndex();
        if (row < 0) {
            return;
        }

        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (ListView_GetItem(list_, &item) == FALSE ||
            item.lParam < 0 ||
            static_cast<std::size_t>(item.lParam) >= windows_.size()) {
            return;
        }

        TargetWindowInfo candidate = windows_[static_cast<std::size_t>(item.lParam)];
        TargetWindowInfo refreshed{};
        std::wstring error;
        if (!InspectTargetWindow(candidate.window, excluded_process_id_, refreshed, error)) {
            ShowCenteredMessageBox(window_, error.c_str(),
                                   L"Target window unavailable",
                                   MB_OK | MB_ICONWARNING);
            Refresh();
            return;
        }

        refreshed.recovery_policy = SelectedRecoveryPolicy();
        if (refreshed.recovery_policy != TargetRecoveryPolicy::ExactWindowOnly &&
            (refreshed.process_path.empty() ||
             refreshed.elevation == TargetElevation::Unknown)) {
            const wchar_t* reason = refreshed.process_path.empty()
                                        ? L"Windows did not expose this application's executable path."
                                        : L"Windows did not expose this application's privilege level.";
            const std::wstring message =
                std::wstring(reason) +
                L" Select Exact selected window only, because Vector Click cannot identify a replacement safely.";
            ShowCenteredMessageBox(
                window_,
                message.c_str(),
                L"Target recovery unavailable",
                MB_OK | MB_ICONWARNING);
            return;
        }
        output_ = std::move(refreshed);
        Close(true);
    }

    void Close(const bool accepted) {
        accepted_ = accepted;
        CloseRecoveryPopup(false);
        if (recovery_popup_ != nullptr &&
            IsWindow(recovery_popup_) != FALSE) {
            DestroyWindow(recovery_popup_);
            recovery_popup_ = nullptr;
        }
        if (window_ != nullptr && IsWindow(window_) != FALSE) {
            DestroyWindow(window_);
        }
    }

    HINSTANCE instance_{};
    HWND owner_{};
    DWORD excluded_process_id_{};
    HWND current_window_{};
    TargetRecoveryPolicy current_recovery_policy_{TargetRecoveryPolicy::ExactWindowOnly};
    TargetWindowInfo& output_;
    bool accepted_{};
    bool layout_in_progress_{};
    bool interactive_resize_{};
    UINT dpi_{96};

    HWND window_{};
    HWND instruction_{};
    HWND list_{};
    HWND list_tooltip_{};
    HWND header_{};
    HWND recovery_label_{};
    HWND recovery_combo_{};
    HWND recovery_popup_{};
    HWND recovery_description_{};
    HWND status_{};
    HWND refresh_button_{};
    HWND accept_button_{};
    HWND cancel_button_{};
    UniqueGdiObject font_;
    RECT list_frame_{};
    bool list_label_tips_enabled_{true};
    bool list_tooltip_protected_{};
    bool recovery_popup_open_{};
    int recovery_popup_highlight_{-1};
    int recovery_popup_hover_{-1};
    int recovery_popup_item_height_{};
    int recovery_popup_x_{};
    int recovery_popup_y_{};
    int recovery_popup_width_{};
    int recovery_popup_height_{};
    std::vector<TargetWindowInfo> windows_;
};

} // namespace

bool ShowTargetWindowPicker(const HINSTANCE instance,
                            const HWND owner,
                            const DWORD excluded_process_id,
                            const TargetWindowInfo& current,
                            TargetWindowInfo& selected) {
    PickerDialog dialog(instance, owner, excluded_process_id, current, selected);
    return dialog.Run();
}

} // namespace vectorclick::win

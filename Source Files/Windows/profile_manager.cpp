#include "Windows/profile_manager.h"

#include "Windows/capture_exclusion.h"
#include "Windows/app_identity.h"
#include "Windows/centered_message_box.h"
#include "Windows/shared_image_loader.h"
#include "Windows/ui_theme.h"

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <string>
#include <utility>

namespace vectorclick::win {
namespace {

constexpr wchar_t ProfileManagerClassName[] = L"VectorClickProfileManager";
constexpr int BaseWidth = 650;
constexpr int BaseHeight = 350;
constexpr UINT_PTR ProfileListSubclassId = 1;

int Scale(const int value, const UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void SetFont(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

bool EqualInsensitive(const std::wstring_view left,
                      const std::wstring_view right) noexcept {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring GetText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    if (copied < 0) {
        return {};
    }
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

} // namespace

ProfileManagerWindow::ProfileManagerWindow(const HINSTANCE instance,
                                           const HWND owner,
                                           ProfileStore& store,
                                           Callbacks callbacks)
    : instance_(instance), owner_(owner), store_(store), callbacks_(std::move(callbacks)) {}

ProfileManagerWindow::~ProfileManagerWindow() {
    Close();
}

bool ProfileManagerWindow::RegisterWindowClass() const {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    window_class.hIcon = LoadSharedDefaultIcon(
        instance_, MAKEINTRESOURCEW(VectorClickIconResourceId));
    window_class.hIconSm = window_class.hIcon;
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = ProfileManagerClassName;
    if (RegisterClassExW(&window_class) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ProfileManagerWindow::Show() {
    if (IsOpen()) {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
        return true;
    }
    if (!RegisterWindowClass()) {
        return false;
    }

    const DWORD owner_exstyle = owner_ != nullptr
        ? static_cast<DWORD>(GetWindowLongPtrW(owner_, GWL_EXSTYLE)) : 0U;
    const DWORD exstyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT |
        ((owner_exstyle & WS_EX_TOPMOST) != 0 ? WS_EX_TOPMOST : 0U);

    dpi_ = owner_ != nullptr ? std::max<UINT>(96, GetDpiForWindow(owner_)) : 96;
    RECT rect{0, 0, Scale(BaseWidth, dpi_), Scale(BaseHeight, dpi_)};
    AdjustWindowRectExForDpi(&rect,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                            FALSE,
                            exstyle,
                            dpi_);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    RECT owner_rect{};
    GetWindowRect(owner_, &owner_rect);
    int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;

    window_ = CreateWindowExW(
        exstyle,
        ProfileManagerClassName,
        L"Manage Local Profiles",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        x, y, width, height,
        owner_, nullptr, instance_, this);
    if (window_ == nullptr) {
        return false;
    }

    ui::ApplyDarkTitleBar(window_);
    if (IsCaptureExclusionRequested()) {
        (void)ApplyRequestedCaptureExclusion(window_);
    }

    // Use the same owned-modal lifetime as the accepted target-window picker.
    // The owner must not enter its movement / resize presentation path while an
    // application popup overlaps it: that path intentionally captures or
    // replays a stable main-window frame and must never ingest popup pixels.
    // Disabling the owner also preserves the normal owned-window Z-order rule
    // instead of allowing the main window to temporarily activate above this
    // popup during a caption drag.
    if (owner_ != nullptr && IsWindow(owner_) != FALSE) {
        EnableWindow(owner_, FALSE);
    }

    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    SetForegroundWindow(window_);

    // Match Choose target window's modal message-pump contract. Keeping the
    // popup in the same UI thread while the owner is disabled preserves dialog
    // navigation and still allows application safety / system messages to run.
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
            if (message.wParam == VK_ESCAPE) {
                Close();
                continue;
            }
            if (message.hwnd == name_edit_ &&
                message.wParam == static_cast<WPARAM>(L'A') &&
                (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                (GetKeyState(VK_MENU) & 0x8000) == 0) {
                // The classic Win32 EDIT path used here does not consistently
                // implement Ctrl+A itself. Handle Select All at the dialog
                // pump so the keydown is consumed before TranslateMessage can
                // turn it into the Ctrl+A control character that causes the
                // default edit-control warning beep. This mirrors the explicit
                // Ctrl+A behavior already provided by Vector Click's numeric
                // edit subclass without changing any other text shortcuts.
                SendMessageW(name_edit_, EM_SETSEL, 0, -1);
                continue;
            }
            if (message.wParam == VK_RETURN) {
                const HWND focus = GetFocus();
                if (focus == new_button_ || focus == save_button_ ||
                    focus == rename_button_ || focus == duplicate_button_ ||
                    focus == import_button_ || focus == export_button_ ||
                    focus == delete_button_ || focus == open_folder_button_ ||
                    focus == close_button_) {
                    SendMessageW(focus, BM_CLICK, 0, 0);
                    continue;
                }
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
    return true;
}

void ProfileManagerWindow::Close() noexcept {
    if (window_ != nullptr && IsWindow(window_) != FALSE) {
        DestroyWindow(window_);
    }
    window_ = nullptr;
}

LRESULT CALLBACK ProfileManagerWindow::WindowProc(const HWND window,
                                                   const UINT message,
                                                   const WPARAM w_param,
                                                   const LPARAM l_param) {
    ProfileManagerWindow* self = reinterpret_cast<ProfileManagerWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        self = create != nullptr
            ? static_cast<ProfileManagerWindow*>(create->lpCreateParams) : nullptr;
        if (self != nullptr) {
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
    }
    return self != nullptr
        ? self->HandleMessage(message, w_param, l_param)
        : DefWindowProcW(window, message, w_param, l_param);
}

LRESULT CALLBACK ProfileManagerWindow::ProfileListSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM w_param,
    const LPARAM l_param,
    const UINT_PTR subclass_id,
    const DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<ProfileManagerWindow*>(reference_data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ProfileListSubclassProc, subclass_id);
        return DefSubclassProc(window, message, w_param, l_param);
    }

    // The profile list intentionally omits WS_VSCROLL because Vector Click
    // owns the visible scrollbar. Do not rely on the native LISTBOX wheel
    // path, which is not guaranteed to move a scrollbar-less list. Route
    // wheel input through the same top-index machinery used by the custom
    // scrollbar instead.
    if (self != nullptr && message == WM_MOUSEWHEEL) {
        POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        if (ScreenToClient(self->window_, &point) != FALSE &&
            IsRectEmpty(&self->list_frame_) == FALSE &&
            PtInRect(&self->list_frame_, point) != FALSE &&
            self->ScrollProfileListWheel(w_param)) {
            return 0;
        }
    }

    const LRESULT result = DefSubclassProc(window, message, w_param, l_param);
    if (self == nullptr) {
        return result;
    }

    if (message == WM_MOUSEMOVE && self->profile_scrollbar_hovered_ &&
        !self->profile_scrollbar_dragging_) {
        self->profile_scrollbar_hovered_ = false;
        self->InvalidateProfileScrollbar();
    }

    switch (message) {
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_CHAR:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_VSCROLL:
    case LB_SETTOPINDEX:
    case LB_SETCURSEL:
        self->InvalidateProfileScrollbar();
        break;
    default:
        break;
    }
    return result;
}

LRESULT ProfileManagerWindow::HandleMessage(const UINT message,
                                            const WPARAM w_param,
                                            const LPARAM l_param) {
    switch (message) {
    case WM_CREATE:
        if (!CreateControls()) {
            return -1;
        }
        RecreateFont();
        Layout();
        RefreshList();
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_DPICHANGED: {
        dpi_ = HIWORD(w_param);
        const auto* suggested = reinterpret_cast<const RECT*>(l_param);
        if (suggested != nullptr) {
            SetWindowPos(window_, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RecreateFont();
        Layout();
        InvalidateRect(window_, nullptr, TRUE);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(w_param) != WA_INACTIVE) {
            UpdateEnabledState();
        }
        break;

    case WM_COMMAND: {
        const int id = LOWORD(w_param);
        const int notification = HIWORD(w_param);
        switch (id) {
        case ProfileList:
            if (notification == LBN_SELCHANGE) {
                UpdateSelection();
            }
            return 0;
        case NameEdit:
            if (notification == EN_CHANGE) {
                UpdateEnabledState();
            }
            return 0;
        case NewButton:
            if (notification == BN_CLICKED) CreateFromCurrent();
            return 0;
        case SaveButton:
            if (notification == BN_CLICKED) SaveCurrent();
            return 0;
        case RenameButton:
            if (notification == BN_CLICKED) RenameSelected();
            return 0;
        case DuplicateButton:
            if (notification == BN_CLICKED) DuplicateSelected();
            return 0;
        case ImportButton:
            if (notification == BN_CLICKED) ImportProfile();
            return 0;
        case ExportButton:
            if (notification == BN_CLICKED) ExportProfile();
            return 0;
        case DeleteButton:
            if (notification == BN_CLICKED) DeleteSelected();
            return 0;
        case OpenFolderButton:
            if (notification == BN_CLICKED) OpenProfilesFolder();
            return 0;
        case CloseButton:
            if (notification == BN_CLICKED) Close();
            return 0;
        default:
            break;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        if (!profile_scrollbar_visible_) {
            break;
        }
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const RECT hover_rect = ProfileScrollbarHoverRect();
        const bool hovered = IsRectEmpty(&hover_rect) == FALSE &&
                             PtInRect(&hover_rect, point) != FALSE;
        if (hovered != profile_scrollbar_hovered_) {
            profile_scrollbar_hovered_ = hovered;
            InvalidateProfileScrollbar();
        }
        if (hovered) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window_;
            (void)TrackMouseEvent(&tracking);
        }
        if (profile_scrollbar_dragging_ && GetCapture() == window_) {
            const RECT track = ProfileScrollbarTrackRect();
            const RECT thumb = ProfileScrollbarThumbRect();
            const int thumb_height = std::max(
                1, static_cast<int>(thumb.bottom - thumb.top));
            const int travel = std::max(
                0,
                static_cast<int>(track.bottom - track.top) - thumb_height);
            const int maximum = ProfileListMaximumTopIndex();
            if (travel > 0 && maximum > 0) {
                const int thumb_top = std::clamp(
                    point.y - profile_scrollbar_drag_offset_,
                    track.top,
                    track.bottom - thumb_height);
                SetProfileListTopIndex(MulDiv(
                    thumb_top - track.top, maximum, travel));
            }
            return 0;
        }
        break;
    }

    case WM_MOUSELEAVE:
        if (profile_scrollbar_hovered_ && !profile_scrollbar_dragging_) {
            profile_scrollbar_hovered_ = false;
            InvalidateProfileScrollbar();
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!profile_scrollbar_visible_) {
            break;
        }
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const RECT hover_rect = ProfileScrollbarHoverRect();
        if (IsRectEmpty(&hover_rect) != FALSE ||
            PtInRect(&hover_rect, point) == FALSE) {
            break;
        }

        SetFocus(list_);
        profile_scrollbar_hovered_ = true;
        const RECT thumb = ProfileScrollbarThumbRect();
        if (IsRectEmpty(&thumb) == FALSE && PtInRect(&thumb, point) != FALSE) {
            profile_scrollbar_dragging_ = true;
            profile_scrollbar_drag_offset_ = point.y - thumb.top;
            SetCapture(window_);
            InvalidateProfileScrollbar();
            return 0;
        }

        const RECT track = ProfileScrollbarTrackRect();
        if (IsRectEmpty(&track) == FALSE && PtInRect(&track, point) != FALSE) {
            const int page = std::max(1, ProfileListVisibleRows() - 1);
            ScrollProfileListBy(point.y < thumb.top ? -page : page);
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (profile_scrollbar_dragging_) {
            profile_scrollbar_dragging_ = false;
            profile_scrollbar_drag_offset_ = 0;
            if (GetCapture() == window_) {
                ReleaseCapture();
            }
            const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            const RECT hover_rect = ProfileScrollbarHoverRect();
            profile_scrollbar_hovered_ =
                IsRectEmpty(&hover_rect) == FALSE &&
                PtInRect(&hover_rect, point) != FALSE;
            InvalidateProfileScrollbar();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if (profile_scrollbar_dragging_) {
            profile_scrollbar_dragging_ = false;
            profile_scrollbar_drag_offset_ = 0;
            profile_scrollbar_hovered_ = false;
            InvalidateProfileScrollbar();
        }
        break;

    case WM_MOUSEWHEEL: {
        if (!profile_scrollbar_visible_ || list_ == nullptr) {
            break;
        }
        POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        if (ScreenToClient(window_, &point) != FALSE &&
            IsRectEmpty(&list_frame_) == FALSE &&
            PtInRect(&list_frame_, point) != FALSE &&
            ScrollProfileListWheel(w_param)) {
            return 0;
        }
        break;
    }

    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
        if (draw == nullptr || draw->CtlType != ODT_BUTTON) {
            break;
        }
        PaintButton(*draw, draw->CtlID == DeleteButton);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(w_param);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(l_param)) ? ui::Text : ui::Disabled);
        static HBRUSH brush = CreateSolidBrush(ui::Window);
        return reinterpret_cast<LRESULT>(brush);
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(w_param);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, ui::SurfaceAlt);
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(l_param)) ? ui::Text : ui::Disabled);
        static HBRUSH brush = CreateSolidBrush(ui::SurfaceAlt);
        return reinterpret_cast<LRESULT>(brush);
    }

    case WM_ERASEBKGND:
        // The complete non-client-child presentation is composed during
        // WM_PAINT. Erasing directly into the visible DC first can expose a
        // background-only frame while the custom profile thumb is moving.
        // AdvancedScrollbarProc suppresses that same intermediate erase.
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);

        const int paint_width = std::max(
            0, static_cast<int>(paint.rcPaint.right - paint.rcPaint.left));
        const int paint_height = std::max(
            0, static_cast<int>(paint.rcPaint.bottom - paint.rcPaint.top));
        const HDC buffer = paint_width > 0 && paint_height > 0
                               ? CreateCompatibleDC(dc)
                               : nullptr;
        const HBITMAP buffer_bitmap =
            buffer != nullptr
                ? CreateCompatibleBitmap(dc, paint_width, paint_height)
                : nullptr;
        HGDIOBJ previous_buffer = nullptr;
        if (buffer != nullptr && buffer_bitmap != nullptr) {
            previous_buffer = SelectObject(buffer, buffer_bitmap);
        }
        const bool buffered = previous_buffer != nullptr &&
                              previous_buffer != HGDI_ERROR;
        const HDC target = buffered ? buffer : dc;
        const int origin_x = buffered ? paint.rcPaint.left : 0;
        const int origin_y = buffered ? paint.rcPaint.top : 0;
        const auto local_rect = [origin_x, origin_y](RECT rect) {
            OffsetRect(&rect, -origin_x, -origin_y);
            return rect;
        };

        ui::Fill(target, local_rect(client), ui::Window);
        if (list_frame_.right > list_frame_.left &&
            list_frame_.bottom > list_frame_.top) {
            ui::DrawRoundedPanel(target,
                                 local_rect(list_frame_),
                                 ui::SurfaceAlt,
                                 ui::Border,
                                 Scale(5, dpi_),
                                 std::max(1, Scale(1, dpi_)));
        }
        if (profile_scrollbar_visible_) {
            const RECT track = local_rect(ProfileScrollbarTrackRect());
            ui::DrawRoundedPanel(
                target,
                track,
                ui::ScrollbarTrack,
                ui::ScrollbarTrack,
                std::max(1, static_cast<int>((track.right - track.left) / 2)));
            const RECT thumb = local_rect(ProfileScrollbarThumbRect());
            const COLORREF thumb_color = ui::ScrollbarThumbColor(
                ui::ScrollbarState(profile_scrollbar_hovered_,
                                   profile_scrollbar_dragging_));
            ui::DrawRoundedPanel(
                target,
                thumb,
                thumb_color,
                thumb_color,
                std::max(1, static_cast<int>((thumb.right - thumb.left) / 2)));
        }

        if (buffered) {
            // Publish the complete invalid region in one copy. This mirrors
            // the accepted Advanced scrollbar anti-flicker path while keeping
            // the LISTBOX itself native and clipped as a child window.
            BitBlt(dc,
                   paint.rcPaint.left,
                   paint.rcPaint.top,
                   paint_width,
                   paint_height,
                   buffer,
                   0,
                   0,
                   SRCCOPY);
            SelectObject(buffer, previous_buffer);
        }
        if (buffer_bitmap != nullptr) {
            DeleteObject(buffer_bitmap);
        }
        if (buffer != nullptr) {
            DeleteDC(buffer);
        }
        EndPaint(window_, &paint);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        window_ = nullptr;
        list_label_ = list_ = name_label_ = name_edit_ = hint_text_ = status_text_ = nullptr;
        new_button_ = save_button_ = rename_button_ = duplicate_button_ = nullptr;
        import_button_ = export_button_ = delete_button_ = open_folder_button_ = close_button_ = nullptr;
        profile_scrollbar_visible_ = false;
        profile_scrollbar_hovered_ = false;
        profile_scrollbar_dragging_ = false;
        profile_scrollbar_drag_offset_ = 0;
        profile_scroll_wheel_remainder_ = 0;
        profiles_.clear();
        return DefWindowProcW(window_, message, w_param, l_param);

    default:
        break;
    }
    return DefWindowProcW(window_, message, w_param, l_param);
}

bool ProfileManagerWindow::CreateControls() {
    const DWORD button_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
    list_label_ = CreateWindowExW(0, L"STATIC", L"Profiles",
                                  WS_CHILD | WS_VISIBLE,
                                  0, 0, 100, 20, window_, nullptr, instance_, nullptr);
    list_ = CreateWindowExW(0, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                            0, 0, 100, 100, window_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ProfileList)),
                            instance_, nullptr);
    name_label_ = CreateWindowExW(0, L"STATIC", L"Profile name", WS_CHILD | WS_VISIBLE,
                                  0, 0, 100, 20, window_, nullptr, instance_, nullptr);
    name_edit_ = CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER,
                                 0, 0, 100, 22, window_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(NameEdit)),
                                 instance_, nullptr);
    SendMessageW(name_edit_, EM_SETLIMITTEXT, 80, 0);
    hint_text_ = CreateWindowExW(0, L"STATIC",
                                 L"Up to 40 Unicode characters. File names are made Windows-safe automatically.",
                                 WS_CHILD | WS_VISIBLE,
                                 0, 0, 100, 36, window_, nullptr, instance_, nullptr);
    status_text_ = CreateWindowExW(0, L"STATIC", L"",
                                   WS_CHILD | WS_VISIBLE,
                                   0, 0, 100, 36, window_, nullptr, instance_, nullptr);

    const auto make_button = [&](const wchar_t* text, const int id) {
        return CreateWindowExW(0, L"BUTTON", text, button_style,
                               0, 0, 100, 32, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               instance_, nullptr);
    };
    new_button_ = make_button(L"Create from Current", NewButton);
    save_button_ = make_button(L"Save Current", SaveButton);
    rename_button_ = make_button(L"Rename", RenameButton);
    duplicate_button_ = make_button(L"Duplicate", DuplicateButton);
    import_button_ = make_button(L"Import...", ImportButton);
    export_button_ = make_button(L"Export...", ExportButton);
    delete_button_ = make_button(L"Delete", DeleteButton);
    open_folder_button_ = make_button(L"Open Profiles Folder", OpenFolderButton);
    close_button_ = make_button(L"Close", CloseButton);

    const std::array<HWND, 15> controls = {
        list_label_, list_, name_label_, name_edit_, hint_text_, status_text_, new_button_, save_button_,
        rename_button_, duplicate_button_, import_button_, export_button_, delete_button_,
        open_folder_button_, close_button_};
    if (std::any_of(controls.begin(), controls.end(), [](const HWND control) { return control == nullptr; })) {
        return false;
    }
    if (SetWindowSubclass(list_,
                          ProfileListSubclassProc,
                          ProfileListSubclassId,
                          reinterpret_cast<DWORD_PTR>(this)) == FALSE) {
        return false;
    }
    for (const HWND control : controls) {
        ui::ApplyDarkControlTheme(control);
    }
    return true;
}

void ProfileManagerWindow::RecreateFont() {
    UniqueGdiObject font(CreateFontW(
        -MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"));
    UniqueGdiObject bold(CreateFontW(
        -MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"));
    if (font.get() != nullptr) {
        font_ = std::move(font);
    }
    if (bold.get() != nullptr) {
        bold_font_ = std::move(bold);
    }
    for (const HWND control : {list_, name_edit_, hint_text_, status_text_, new_button_, save_button_,
                               rename_button_, duplicate_button_, import_button_, export_button_, delete_button_,
                               open_folder_button_, close_button_}) {
        SetFont(control, reinterpret_cast<HFONT>(font_.get()));
    }
    SetFont(list_label_, reinterpret_cast<HFONT>(bold_font_.get()));
    SetFont(name_label_, reinterpret_cast<HFONT>(bold_font_.get()));
}

void ProfileManagerWindow::Layout() {
    if (window_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const int margin = Scale(18, dpi_);
    const int gap = Scale(10, dpi_);
    const int left_width = Scale(274, dpi_);
    const int right_x = margin + left_width + gap;
    const int client_right = static_cast<int>(client.right);
    const int client_bottom = static_cast<int>(client.bottom);
    const int right_width = std::max(Scale(260, dpi_), client_right - right_x - margin);
    const int button_height = Scale(34, dpi_);
    const int button_gap = Scale(8, dpi_);
    const int heading_height = Scale(22, dpi_);
    const int status_height = Scale(42, dpi_);
    const int list_gap = Scale(6, dpi_);

    MoveWindow(list_label_, margin, margin, left_width, heading_height, TRUE);
    const int list_top = margin + heading_height + list_gap;
    const int status_y = std::max(list_top + Scale(80, dpi_),
                                  client_bottom - margin - status_height);
    list_frame_ = {margin, list_top, margin + left_width, status_y - gap};
    LayoutProfileList();
    MoveWindow(status_text_, margin, status_y, left_width, status_height, TRUE);

    MoveWindow(name_label_, right_x, margin, right_width, heading_height, TRUE);
    MoveWindow(name_edit_, right_x, margin + Scale(26, dpi_), right_width, Scale(22, dpi_), TRUE);
    MoveWindow(hint_text_, right_x, margin + Scale(58, dpi_), right_width, Scale(38, dpi_), TRUE);

    const int half = (right_width - button_gap) / 2;
    int y = margin + Scale(108, dpi_);
    MoveWindow(new_button_, right_x, y, half, button_height, TRUE);
    MoveWindow(save_button_, right_x + half + button_gap, y, half, button_height, TRUE);
    y += button_height + button_gap;
    MoveWindow(rename_button_, right_x, y, half, button_height, TRUE);
    MoveWindow(duplicate_button_, right_x + half + button_gap, y, half, button_height, TRUE);
    y += button_height + button_gap;
    MoveWindow(import_button_, right_x, y, half, button_height, TRUE);
    MoveWindow(export_button_, right_x + half + button_gap, y, half, button_height, TRUE);
    y += button_height + button_gap;
    MoveWindow(delete_button_, right_x, y, half, button_height, TRUE);
    MoveWindow(open_folder_button_, right_x + half + button_gap, y, half, button_height, TRUE);

    // Keep a slightly larger safety gap above Close without making the
    // compact manager taller.
    const int close_y = client_bottom - margin - button_height + Scale(4, dpi_);
    MoveWindow(close_button_, right_x + right_width - half, close_y, half, button_height, TRUE);

    InvalidateRect(window_, nullptr, FALSE);
}

void ProfileManagerWindow::LayoutProfileList() {
    if (list_ == nullptr || IsWindow(list_) == FALSE ||
        list_frame_.right <= list_frame_.left ||
        list_frame_.bottom <= list_frame_.top) {
        return;
    }

    // Keep the rectangular LISTBOX inside the rounded parent-drawn panel.
    // The custom scrollbar occupies a sibling gutter in that same panel, so
    // the list keeps native selection / keyboard behavior without exposing a
    // second, differently themed Windows scrollbar.
    const int list_inset = std::max(1, Scale(5, dpi_));
    const int list_width = std::max(
        0,
        static_cast<int>(list_frame_.right - list_frame_.left) -
            (list_inset * 2));
    const int list_height = std::max(
        0,
        static_cast<int>(list_frame_.bottom - list_frame_.top) -
            (list_inset * 2));

    const LRESULT count_result = SendMessageW(list_, LB_GETCOUNT, 0, 0);
    const int count = count_result == LB_ERR ? 0 : static_cast<int>(count_result);
    const LRESULT item_height_result = SendMessageW(list_, LB_GETITEMHEIGHT, 0, 0);
    const int item_height = item_height_result == LB_ERR
                                ? std::max(1, Scale(18, dpi_))
                                : std::max(1, static_cast<int>(item_height_result));
    const int visible_rows = std::max(
        1,
        list_height / item_height);
    const bool scrollbar_visible = count > visible_rows;

    if (!scrollbar_visible && profile_scrollbar_dragging_) {
        profile_scrollbar_dragging_ = false;
        profile_scrollbar_drag_offset_ = 0;
        if (GetCapture() == window_) {
            ReleaseCapture();
        }
    }
    if (!scrollbar_visible) {
        profile_scrollbar_hovered_ = false;
        profile_scroll_wheel_remainder_ = 0;
    }
    profile_scrollbar_visible_ = scrollbar_visible;

    const int scrollbar_gutter =
        profile_scrollbar_visible_ ? Scale(13, dpi_) : 0;
    MoveWindow(list_,
               list_frame_.left + list_inset,
               list_frame_.top + list_inset,
               std::max(0, list_width - scrollbar_gutter),
               list_height,
               TRUE);

    SetProfileListTopIndex(static_cast<int>(
        SendMessageW(list_, LB_GETTOPINDEX, 0, 0)));
    InvalidateRect(window_, &list_frame_, FALSE);
}

int ProfileManagerWindow::ProfileListVisibleRows() const noexcept {
    if (list_ == nullptr || IsWindow(list_) == FALSE) {
        return 1;
    }
    RECT client{};
    if (GetClientRect(list_, &client) == FALSE) {
        return 1;
    }
    const int height = std::max(
        0, static_cast<int>(client.bottom - client.top));
    const LRESULT item_height_result = SendMessageW(list_, LB_GETITEMHEIGHT, 0, 0);
    const int item_height = item_height_result == LB_ERR
                                ? std::max(1, Scale(18, dpi_))
                                : std::max(1, static_cast<int>(item_height_result));
    return std::max(1, height / item_height);
}

int ProfileManagerWindow::ProfileListMaximumTopIndex() const noexcept {
    if (list_ == nullptr || IsWindow(list_) == FALSE) {
        return 0;
    }
    const LRESULT count_result = SendMessageW(list_, LB_GETCOUNT, 0, 0);
    const int count = count_result == LB_ERR ? 0 : static_cast<int>(count_result);
    return std::max(0, count - ProfileListVisibleRows());
}

RECT ProfileManagerWindow::ProfileScrollbarGutterRect() const noexcept {
    if (!profile_scrollbar_visible_) {
        return {};
    }
    const int list_inset = std::max(1, Scale(5, dpi_));
    const int gutter_width = std::max(1, Scale(13, dpi_));
    return RECT{
        std::max(list_frame_.left + list_inset,
                 list_frame_.right - list_inset - gutter_width),
        list_frame_.top + list_inset,
        list_frame_.right - list_inset,
        list_frame_.bottom - list_inset,
    };
}

RECT ProfileManagerWindow::ProfileScrollbarHoverRect() const noexcept {
    RECT hover_rect = ProfileScrollbarGutterRect();
    if (IsRectEmpty(&hover_rect) != FALSE) {
        return {};
    }

    // The gutter is intentionally wider than the visible scrollbar so the
    // control has breathing room beside the list. Do not let that invisible
    // margin activate hover. Use the highlighted thumb width as a stable hit
    // strip so the blue feedback begins where the pointer can actually engage
    // the scrollbar, without changing width as hover itself toggles.
    const int desired_width = std::max(
        1,
        Scale(ui::ScrollbarThumbLogicalWidth(
                  ui::ScrollbarVisualState::Hover),
              dpi_));
    const int available_width = std::max(
        1, static_cast<int>(hover_rect.right - hover_rect.left));
    const int hover_width = std::min(desired_width, available_width);
    const int hover_left =
        hover_rect.left + (available_width - hover_width) / 2;
    hover_rect.left = hover_left;
    hover_rect.right = hover_left + hover_width;
    return hover_rect;
}

RECT ProfileManagerWindow::ProfileScrollbarTrackRect() const noexcept {
    RECT track = ProfileScrollbarGutterRect();
    if (IsRectEmpty(&track) != FALSE) {
        return {};
    }
    const int desired_width = std::max(1, Scale(4, dpi_));
    const int available_width = std::max(
        1, static_cast<int>(track.right - track.left));
    const int track_width = std::min(desired_width, available_width);
    const int track_left = track.left + (available_width - track_width) / 2;
    const int vertical_inset = std::max(1, Scale(1, dpi_));
    track.left = track_left;
    track.right = track_left + track_width;
    track.top += vertical_inset;
    track.bottom = std::max<LONG>(
        track.top + 1, track.bottom - vertical_inset);
    return track;
}

RECT ProfileManagerWindow::ProfileScrollbarThumbRect() const noexcept {
    const RECT track = ProfileScrollbarTrackRect();
    if (IsRectEmpty(&track) != FALSE || list_ == nullptr ||
        IsWindow(list_) == FALSE) {
        return {};
    }

    const LRESULT count_result = SendMessageW(list_, LB_GETCOUNT, 0, 0);
    const int count = count_result == LB_ERR ? 0 : static_cast<int>(count_result);
    if (count <= 0) {
        return {};
    }
    const int visible_rows = std::min(count, ProfileListVisibleRows());
    const int track_height = std::max(
        1, static_cast<int>(track.bottom - track.top));
    const int minimum_thumb = std::min(Scale(18, dpi_), track_height);
    const int thumb_height = std::clamp(
        MulDiv(track_height, visible_rows, count),
        minimum_thumb,
        track_height);
    const int maximum = std::max(0, count - visible_rows);
    const LRESULT top_result = SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
    const int top_index = top_result == LB_ERR
                              ? 0
                              : std::clamp(static_cast<int>(top_result), 0, maximum);
    const int travel = std::max(0, track_height - thumb_height);
    const int offset = maximum > 0
                           ? MulDiv(travel, top_index, maximum)
                           : 0;

    const ui::ScrollbarVisualState state = ui::ScrollbarState(
        profile_scrollbar_hovered_, profile_scrollbar_dragging_);
    const RECT gutter = ProfileScrollbarGutterRect();
    const int available_width = std::max(
        1, static_cast<int>(gutter.right - gutter.left));
    const int desired_width = std::max(
        1, Scale(ui::ScrollbarThumbLogicalWidth(state), dpi_));
    const int thumb_width = std::min(desired_width, available_width);
    const int thumb_left = gutter.left + (available_width - thumb_width) / 2;
    return RECT{
        thumb_left,
        track.top + offset,
        thumb_left + thumb_width,
        track.top + offset + thumb_height,
    };
}

bool ProfileManagerWindow::ScrollProfileListWheel(const WPARAM w_param) noexcept {
    if (!profile_scrollbar_visible_ || list_ == nullptr ||
        IsWindow(list_) == FALSE || ProfileListMaximumTopIndex() <= 0) {
        profile_scroll_wheel_remainder_ = 0;
        return false;
    }

    profile_scroll_wheel_remainder_ += GET_WHEEL_DELTA_WPARAM(w_param);
    const int notches = profile_scroll_wheel_remainder_ / WHEEL_DELTA;
    profile_scroll_wheel_remainder_ %= WHEEL_DELTA;
    if (notches == 0) {
        return true;
    }

    UINT lines = 3;
    (void)SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const int rows =
        lines == WHEEL_PAGESCROLL
            ? std::max(1, ProfileListVisibleRows() - 1)
            : std::max(1, static_cast<int>(lines));
    ScrollProfileListBy(-notches * rows);
    return true;
}

void ProfileManagerWindow::SetProfileListTopIndex(const int top_index) noexcept {
    if (list_ == nullptr || IsWindow(list_) == FALSE) {
        return;
    }
    const int bounded = std::clamp(
        top_index, 0, ProfileListMaximumTopIndex());
    const LRESULT current_result = SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
    const int current = current_result == LB_ERR
                            ? 0
                            : static_cast<int>(current_result);
    if (current == bounded) {
        InvalidateProfileScrollbar();
        return;
    }
    // LB_SETTOPINDEX honors Windows' system-wide list-box smooth-scrolling
    // preference.  Because Vector Click already owns the scrolling gesture and
    // scrollbar presentation here, allowing the native LISTBOX to animate its
    // first programmatic top-index change produces a one-off slow slide that
    // does not match subsequent wheel movement.  Suppress intermediate list
    // paints for this atomic position change, then publish the final list
    // state once.  This avoids changing SPI_SETLISTBOXSMOOTHSCROLLING, which
    // would alter a user-wide Windows setting and affect other applications.
    (void)SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    (void)SendMessageW(list_, LB_SETTOPINDEX, static_cast<WPARAM>(bounded), 0);
    (void)SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    (void)RedrawWindow(list_, nullptr, nullptr,
                       RDW_ERASE | RDW_FRAME | RDW_INVALIDATE |
                           RDW_ALLCHILDREN | RDW_UPDATENOW);
    InvalidateProfileScrollbar();
}

void ProfileManagerWindow::ScrollProfileListBy(const int row_delta) noexcept {
    if (list_ == nullptr || IsWindow(list_) == FALSE || row_delta == 0) {
        return;
    }
    const LRESULT current_result = SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
    const int current = current_result == LB_ERR
                            ? 0
                            : static_cast<int>(current_result);
    SetProfileListTopIndex(current + row_delta);
}

void ProfileManagerWindow::InvalidateProfileScrollbar() noexcept {
    if (window_ == nullptr || IsWindow(window_) == FALSE) {
        return;
    }
    RECT area = profile_scrollbar_visible_
                    ? ProfileScrollbarGutterRect()
                    : list_frame_;
    if (IsRectEmpty(&area) == FALSE) {
        InvalidateRect(window_, &area, FALSE);
    }
}

void ProfileManagerWindow::Refresh() {
    if (IsOpen()) {
        const ProfileInfo* selected = SelectedProfile();
        const std::wstring id = selected != nullptr ? selected->id : std::wstring{};
        RefreshList(id);
    }
}

void ProfileManagerWindow::RefreshAvailability() {
    if (IsOpen()) {
        UpdateEnabledState();
    }
}

void ProfileManagerWindow::SynchronizeOwnerPresentation() {
    if (!IsOpen()) {
        return;
    }

    (void)SynchronizeRequestedCaptureExclusion(window_);

    const LONG_PTR owner_exstyle = owner_ != nullptr && IsWindow(owner_) != FALSE
        ? GetWindowLongPtrW(owner_, GWL_EXSTYLE) : 0;
    const bool owner_topmost = (owner_exstyle & WS_EX_TOPMOST) != 0;
    const LONG_PTR window_exstyle = GetWindowLongPtrW(window_, GWL_EXSTYLE);
    const bool window_topmost = (window_exstyle & WS_EX_TOPMOST) != 0;
    if (owner_topmost != window_topmost) {
        (void)SetWindowPos(window_,
                           owner_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                           0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                               SWP_NOOWNERZORDER);
    }
}

void ProfileManagerWindow::RefreshList(const std::wstring_view preferred_id) {
    if (list_ == nullptr) {
        return;
    }
    std::vector<std::wstring> warnings;
    std::wstring error;
    std::vector<ProfileInfo> loaded;
    if (!store_.List(loaded, &warnings, error)) {
        profiles_.clear();
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        SetWindowTextW(status_text_, error.c_str());
        LayoutProfileList();
        UpdateSelection();
        return;
    }
    profiles_ = std::move(loaded);
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    int preferred_index = -1;
    for (std::size_t index = 0; index < profiles_.size(); ++index) {
        SendMessageW(list_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(profiles_[index].name.c_str()));
        if (!preferred_id.empty() && EqualInsensitive(profiles_[index].id, preferred_id)) {
            preferred_index = static_cast<int>(index);
        }
    }
    if (preferred_index < 0 && !profiles_.empty()) {
        preferred_index = 0;
    }
    SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(preferred_index), 0);
    LayoutProfileList();
    if (warnings.empty()) {
        const std::wstring status = profiles_.empty()
            ? L"No local profiles yet. Enter a name and create one from the current settings."
            : std::to_wstring(profiles_.size()) +
                  (profiles_.size() == 1 ? L" local profile." : L" local profiles.");
        SetWindowTextW(status_text_, status.c_str());
    } else {
        const std::wstring status = std::to_wstring(warnings.size()) +
            (warnings.size() == 1
                ? L" unreadable or duplicate profile file was ignored. Other profiles remain available."
                : L" unreadable or duplicate profile files were ignored. Other profiles remain available.");
        SetWindowTextW(status_text_, status.c_str());
    }
    UpdateSelection();
}

int ProfileManagerWindow::SelectedIndex() const noexcept {
    if (list_ == nullptr) {
        return -1;
    }
    const LRESULT selection = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    return selection >= 0 && static_cast<std::size_t>(selection) < profiles_.size()
        ? static_cast<int>(selection) : -1;
}

ProfileInfo* ProfileManagerWindow::SelectedProfile() noexcept {
    const int index = SelectedIndex();
    return index >= 0 ? &profiles_[static_cast<std::size_t>(index)] : nullptr;
}

const ProfileInfo* ProfileManagerWindow::SelectedProfile() const noexcept {
    const int index = SelectedIndex();
    return index >= 0 ? &profiles_[static_cast<std::size_t>(index)] : nullptr;
}

void ProfileManagerWindow::UpdateSelection() {
    const ProfileInfo* profile = SelectedProfile();
    if (profile != nullptr) {
        SetNameText(profile->name);
    } else {
        SetNameText(L"");
    }
    UpdateEnabledState();
}

void ProfileManagerWindow::UpdateEnabledState() {
    const bool allowed = OperationsAllowed();
    const bool selected = SelectedProfile() != nullptr;
    std::wstring normalized_name;
    std::wstring name_error;
    const bool valid_name = ProfileStore::NormalizeAndValidateName(
        NameText(), normalized_name, name_error);

    EnableWindow(name_edit_, allowed);
    EnableWindow(new_button_, allowed && valid_name);
    EnableWindow(save_button_, allowed && selected);
    EnableWindow(rename_button_, allowed && selected && valid_name);
    EnableWindow(duplicate_button_, allowed && selected && valid_name);
    EnableWindow(import_button_, allowed);
    EnableWindow(export_button_, allowed && selected);
    EnableWindow(delete_button_, allowed && selected);
    EnableWindow(open_folder_button_, TRUE);
}

std::wstring ProfileManagerWindow::NameText() const {
    return name_edit_ != nullptr ? GetText(name_edit_) : std::wstring{};
}

void ProfileManagerWindow::SetNameText(const std::wstring_view text) {
    if (name_edit_ != nullptr) {
        const std::wstring copy(text);
        SetWindowTextW(name_edit_, copy.c_str());
    }
}

bool ProfileManagerWindow::OperationsAllowed() const {
    return !callbacks_.operations_allowed || callbacks_.operations_allowed();
}

bool ProfileManagerWindow::CaptureCurrent(core::RunSettings& settings,
                                          std::wstring& error) const {
    if (!OperationsAllowed()) {
        error = L"Profile changes are unavailable while Vector Click is running, stopping, recovering, or changing safety hotkeys.";
        return false;
    }
    if (!callbacks_.capture_current_settings) {
        error = L"The current Vector Click settings are unavailable.";
        return false;
    }
    return callbacks_.capture_current_settings(settings, error);
}

void ProfileManagerWindow::ShowOperationError(const std::wstring_view title,
                                              const std::wstring& error) const {
    const std::wstring title_copy(title);
    ShowCenteredMessageBox(window_,
                           error.empty() ? L"The profile operation could not be completed." : error.c_str(),
                           title_copy.c_str(),
                           MB_OK | MB_ICONWARNING);
}

void ProfileManagerWindow::CreateFromCurrent() {
    core::RunSettings settings;
    std::wstring error;
    if (!CaptureCurrent(settings, error)) {
        ShowOperationError(L"Profile not created", error);
        return;
    }
    ProfileInfo created;
    if (!store_.Create(NameText(), settings, created, error)) {
        ShowOperationError(L"Profile not created", error);
        return;
    }
    if (callbacks_.profiles_changed) callbacks_.profiles_changed();
    RefreshList(created.id);
}

void ProfileManagerWindow::SaveCurrent() {
    const ProfileInfo* selected = SelectedProfile();
    if (selected == nullptr) return;
    const std::wstring selected_id = selected->id;
    core::RunSettings settings;
    std::wstring error;
    if (!CaptureCurrent(settings, error) || !store_.Save(*selected, settings, error)) {
        ShowOperationError(L"Profile not saved", error);
        return;
    }
    if (callbacks_.profiles_changed) callbacks_.profiles_changed();
    RefreshList(selected_id);
}

void ProfileManagerWindow::RenameSelected() {
    const ProfileInfo* selected = SelectedProfile();
    if (selected == nullptr || !OperationsAllowed()) return;
    const std::wstring selected_id = selected->id;
    ProfileInfo renamed;
    std::wstring error;
    if (!store_.Rename(*selected, NameText(), renamed, error)) {
        ShowOperationError(L"Profile not renamed", error);
        return;
    }
    if (callbacks_.profiles_changed) callbacks_.profiles_changed();
    RefreshList(renamed.id.empty() ? selected_id : renamed.id);
}

void ProfileManagerWindow::DuplicateSelected() {
    const ProfileInfo* selected = SelectedProfile();
    if (selected == nullptr || !OperationsAllowed()) return;
    std::wstring requested = NameText();
    if (requested.empty() || EqualInsensitive(requested, selected->name)) {
        requested = selected->name;
        constexpr std::wstring_view suffix = L" Copy";
        std::size_t scalar_count = 0;
        if (ProfileStore::CountNameCharacters(requested, scalar_count) &&
            scalar_count + suffix.size() <= MaximumProfileNameCharacters) {
            requested += suffix;
        } else {
            requested = L"Profile Copy";
        }
    }
    ProfileInfo duplicated;
    std::wstring error;
    if (!store_.Duplicate(*selected, requested, duplicated, error)) {
        ShowOperationError(L"Profile not duplicated", error);
        return;
    }
    if (callbacks_.profiles_changed) callbacks_.profiles_changed();
    RefreshList(duplicated.id);
}

void ProfileManagerWindow::ImportProfile() {
    if (!OperationsAllowed()) return;
    std::vector<wchar_t> path(32'768U, L'\0');
    constexpr wchar_t filter[] =
        L"Vector Click profiles (*.VectorClickProfile)\0*.VectorClickProfile\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = L"Import Vector Click profile";
    dialog.lpstrDefExt = L"VectorClickProfile";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                   OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (GetOpenFileNameW(&dialog) == FALSE) {
        return;
    }
    if (!OperationsAllowed()) {
        ShowOperationError(L"Profile not imported",
                           L"Vector Click's state changed while the file picker was open. No profile was imported.");
        return;
    }
    ProfileInfo imported;
    std::wstring error;
    if (!store_.ImportFromPath(path.data(), imported, error)) {
        ShowOperationError(L"Profile not imported", error);
        return;
    }
    if (callbacks_.profiles_changed) callbacks_.profiles_changed();
    RefreshList(imported.id);
}

void ProfileManagerWindow::ExportProfile() {
    const ProfileInfo* selected = SelectedProfile();
    if (selected == nullptr || !OperationsAllowed()) return;
    std::vector<wchar_t> path(32'768U, L'\0');
    const std::wstring suggested = ProfileStore::SuggestedFileName(selected->name, selected->id);
    std::copy_n(suggested.c_str(), std::min(suggested.size() + 1U, path.size()), path.data());
    constexpr wchar_t filter[] =
        L"Vector Click profiles (*.VectorClickProfile)\0*.VectorClickProfile\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = L"Export Vector Click profile";
    dialog.lpstrDefExt = L"VectorClickProfile";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT |
                   OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (GetSaveFileNameW(&dialog) == FALSE) {
        return;
    }
    std::wstring error;
    if (!store_.ExportToPath(*selected, path.data(), error)) {
        ShowOperationError(L"Profile not exported", error);
    }
}

void ProfileManagerWindow::DeleteSelected() {
    const ProfileInfo* selected = SelectedProfile();
    if (selected == nullptr || !OperationsAllowed()) return;
    const std::wstring id = selected->id;
    const std::wstring message = L"Delete the local profile \"" + selected->name + L"\"?\n\nThis does not change the current Vector Click settings.";
    if (ShowCenteredMessageBox(window_, message.c_str(), L"Delete local profile?",
                               MB_YESNO | MB_ICONINFORMATION) != IDYES) {
        return;
    }
    std::wstring error;
    if (!store_.Remove(*selected, error)) {
        ShowOperationError(L"Profile not deleted", error);
        return;
    }
    if (callbacks_.profiles_changed) callbacks_.profiles_changed();
    RefreshList();
}

void ProfileManagerWindow::OpenProfilesFolder() {
    bool exists = false;
    std::wstring error;
    if (!store_.DirectoryExists(exists, error)) {
        ShowOperationError(L"Profiles folder unavailable", error);
        return;
    }
    if (!exists) {
        ShowCenteredMessageBox(
            window_,
            L"The local Profiles folder does not exist yet.\n\nCreate your first Vector Click profile to create the folder.",
            L"Profiles folder not created",
            MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open",
                                                store_.DirectoryPath().c_str(),
                                                nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        ShowOperationError(L"Profiles folder unavailable",
                           L"Windows could not open the local Profiles folder.");
    }
}

void ProfileManagerWindow::PaintButton(const DRAWITEMSTRUCT& draw,
                                       const bool danger) const {
    RECT bounds = draw.rcItem;
    const bool enabled = IsWindowEnabled(draw.hwndItem) != FALSE;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
    const bool primary = draw.hwndItem == new_button_;

    ui::Fill(draw.hDC, bounds, ui::Window);
    const COLORREF fill = !enabled
                              ? ui::SurfaceAlt
                              : danger && pressed ? ui::DangerPressed
                              : pressed ? ui::SurfacePressed
                              : hot ? ui::SurfaceHover : ui::SurfaceAlt;
    const COLORREF border = !enabled
                                ? ui::BorderSoft
                                : danger ? ui::Danger
                                : (focused || primary) ? ui::Accent : ui::Border;
    const COLORREF text_color = !enabled
                                    ? ui::Disabled
                                    : danger ? ui::Danger
                                    : primary ? ui::AccentHover : ui::Text;
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


} // namespace vectorclick::win

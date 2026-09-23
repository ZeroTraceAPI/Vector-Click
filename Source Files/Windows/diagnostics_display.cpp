#include "Windows/diagnostics_display.h"
#include "Windows/shared_image_loader.h"
#include "Windows/ui_theme.h"

#include <algorithm>
#include <cwchar>
#include <new>
#include <string>

namespace vectorclick::win {
namespace {

constexpr wchar_t DiagnosticsWindowClassName[] = L"VectorClickDiagnosticsDisplay";
constexpr UINT SetPresentationMessage = WM_APP + 1U;

struct PresentationUpdate {
    std::wstring_view text;
    StatusCategory category{StatusCategory::Ready};
};

int Scale(const int value, const UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

} // namespace

struct DiagnosticsDisplay::State {
    HFONT font{};
    std::wstring text;
    StatusCategory category{StatusCategory::Ready};
};

HWND DiagnosticsDisplay::Create(const HINSTANCE instance, const HWND parent, const int control_id) {
    if (!RegisterWindowClass(instance)) {
        return nullptr;
    }

    return CreateWindowExW(
        0,
        DiagnosticsWindowClassName,
        L"Ready | Completed actions: 0 | Generated clicks: 0\nAction rate: 0.0/s | Click rate: 0.0 CPS | Elapsed: 0.00 s",
        WS_CHILD,
        0,
        0,
        100,
        30,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
        instance,
        nullptr);
}

void DiagnosticsDisplay::SetPresentation(const HWND window,
                                         const std::wstring_view text,
                                         const StatusCategory category) noexcept {
    if (window == nullptr || IsWindow(window) == FALSE) {
        return;
    }

    const PresentationUpdate update{text, category};
    SendMessageW(window,
                 SetPresentationMessage,
                 0,
                 reinterpret_cast<LPARAM>(&update));
}

bool DiagnosticsDisplay::RegisterWindowClass(const HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadSharedDefaultCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = DiagnosticsWindowClassName;

    if (RegisterClassExW(&window_class) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

LRESULT CALLBACK DiagnosticsDisplay::WindowProc(const HWND window,
                                                 const UINT message,
                                                 const WPARAM w_param,
                                                 const LPARAM l_param) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        auto* new_state = new (std::nothrow) State();
        if (new_state == nullptr) {
            return FALSE;
        }
        try {
            if (create != nullptr && create->lpszName != nullptr) {
                new_state->text = create->lpszName;
            }
        } catch (...) {
            delete new_state;
            return FALSE;
        }
        state = new_state;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }

    if (state == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
    case SetPresentationMessage: {
        const auto* update = reinterpret_cast<const PresentationUpdate*>(l_param);
        if (update == nullptr) {
            return FALSE;
        }

        const bool category_changed = state->category != update->category;
        const bool text_changed =
            std::wstring_view(state->text) != update->text;
        try {
            if (text_changed) {
                state->text.assign(update->text);
            }
        } catch (...) {
            return FALSE;
        }
        state->category = update->category;
        if (text_changed || category_changed) {
            InvalidateRect(window, nullptr, FALSE);
        }
        return TRUE;
    }

    case WM_SETTEXT:
        try {
            state->text = l_param != 0 ? reinterpret_cast<const wchar_t*>(l_param) : L"";
        } catch (...) {
            return FALSE;
        }
        InvalidateRect(window, nullptr, FALSE);
        return TRUE;

    case WM_GETTEXT: {
        if (w_param == 0 || l_param == 0) {
            return 0;
        }
        auto* output = reinterpret_cast<wchar_t*>(l_param);
        const std::size_t capacity = static_cast<std::size_t>(w_param);
        const std::size_t count = std::min(capacity - 1, state->text.size());
        std::wmemcpy(output, state->text.data(), count);
        output[count] = L'\0';
        return static_cast<LRESULT>(count);
    }

    case WM_GETTEXTLENGTH:
        return static_cast<LRESULT>(state->text.size());

    case WM_SETFONT:
        state->font = reinterpret_cast<HFONT>(w_param);
        if (LOWORD(l_param) != 0) {
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;

    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->font);

    case WM_ENABLE:
    case WM_SIZE:
    case WM_DPICHANGED_AFTERPARENT:
        InvalidateRect(window, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint(window, *state);
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete state;
        return DefWindowProcW(window, message, w_param, l_param);

    default:
        break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

void DiagnosticsDisplay::Paint(const HWND window, State& state) {
    PAINTSTRUCT paint{};
    HDC destination = BeginPaint(window, &paint);

    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        EndPaint(window, &paint);
        return;
    }

    HDC buffer = CreateCompatibleDC(destination);
    HBITMAP bitmap = buffer != nullptr ? CreateCompatibleBitmap(destination, width, height) : nullptr;
    HGDIOBJ old_bitmap = nullptr;
    if (buffer != nullptr && bitmap != nullptr) {
        old_bitmap = SelectObject(buffer, bitmap);
    } else {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (buffer != nullptr) {
            DeleteDC(buffer);
        }
        buffer = destination;
    }

    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(window));
    ui::Fill(buffer, client, ui::Surface);
    ui::DrawRoundedPanel(buffer, client, ui::Surface, ui::Border, Scale(7, dpi));

    const COLORREF indicator_color = ui::StatusIndicatorColor(state.category);
    // This retained diagnostics text child is intentionally hidden and used
    // as an internal text / accessibility store. If Windows asks it to render
    // during a print / capture transaction, use the same shared lamp geometry
    // and anti-aliased renderer as the visible unified footer.
    ui::DrawStatusIndicator(buffer,
                            client,
                            dpi,
                            indicator_color);

    const int right_padding = Scale(8, dpi);
    RECT text_rect{
        client.left + Scale(36, dpi),
        client.top,
        client.right - right_padding,
        client.bottom,
    };

    SetBkMode(buffer, TRANSPARENT);
    SetTextColor(buffer, IsWindowEnabled(window) ? ui::Text : ui::Disabled);
    const HGDIOBJ selected_font = state.font != nullptr ? state.font : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ old_font = SelectObject(buffer, selected_font);
    RECT measured = text_rect;
    DrawTextW(buffer,
              state.text.c_str(),
              -1,
              &measured,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    const int measured_height = measured.bottom - measured.top;
    if (measured_height < height) {
        text_rect.top += (height - measured_height) / 2;
    }
    DrawTextW(buffer,
              state.text.c_str(),
              -1,
              &text_rect,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(buffer, old_font);

    if (buffer != destination) {
        BitBlt(destination, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
    }

    EndPaint(window, &paint);
}

} // namespace vectorclick::win

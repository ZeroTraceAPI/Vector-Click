#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include "Windows/win32_raii.h"

#include <string>
#include <vector>

namespace vectorclick::win {

class TooltipManager {
public:
    TooltipManager() = default;
    ~TooltipManager();

    TooltipManager(const TooltipManager&) = delete;
    TooltipManager& operator=(const TooltipManager&) = delete;

    [[nodiscard]] bool Create(HWND owner);
    void Add(HWND control, std::wstring text);
    void SetText(HWND control, std::wstring text);
    void RefreshVisible(HWND control);
    void ReassertTopmost() noexcept;
    void Dismiss(HWND control) noexcept;
    void SetActive(HWND control, bool active) noexcept;
    [[nodiscard]] bool HasPendingOrVisible() const noexcept;
    [[nodiscard]] bool DismissAll() noexcept;
    void Hide() noexcept;

private:
    struct Tool {
        HWND control{};
        std::wstring text;
        bool active{true};
    };

    static LRESULT CALLBACK TooltipWindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK ToolSubclassProc(HWND window,
                                             UINT message,
                                             WPARAM w_param,
                                             LPARAM l_param,
                                             UINT_PTR subclass_id,
                                             DWORD_PTR reference_data);

    LRESULT HandleTooltipMessage(UINT message, WPARAM w_param, LPARAM l_param);
    void DrawCurrentSurface(HDC dc, const RECT& client) const noexcept;
    [[nodiscard]] bool PresentCurrentSurface(int x,
                                             int y,
                                             int width,
                                             int height,
                                             int outer_corner_radius) const noexcept;
    [[nodiscard]] UINT_PTR StartGeneratedTimer(UINT_PTR& active_timer_id,
                                                UINT milliseconds) noexcept;
    void StopGeneratedTimer(UINT_PTR& active_timer_id) noexcept;
    void BeginHover(HWND control, bool keyboard_focus);
    void EndHover(HWND control) noexcept;
    void ShowCurrent();
    [[nodiscard]] const std::wstring* FindText(HWND control) const noexcept;
    void RecreateFont(UINT dpi);

    HINSTANCE instance_{};
    HWND owner_{};
    HWND tooltip_window_{};
    HWND current_control_{};
    HWND mouse_leave_tracking_control_{};
    UINT current_dpi_{96};
    bool visible_{false};
    bool auto_popped_{false};
    bool keyboard_focus_{false};
    UINT_PTR next_timer_id_{0x1000};
    UINT_PTR initial_delay_timer_id_{};
    UINT_PTR auto_pop_timer_id_{};
    UniqueGdiObject font_;
    std::vector<Tool> tools_;
};

} // namespace vectorclick::win

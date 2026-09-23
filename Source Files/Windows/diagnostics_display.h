#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string_view>

#include "Windows/ui_presentation_state.h"

namespace vectorclick::win {

class DiagnosticsDisplay {
public:
    [[nodiscard]] static HWND Create(HINSTANCE instance, HWND parent, int control_id);
    static void SetPresentation(HWND window,
                                std::wstring_view text,
                                StatusCategory category) noexcept;

private:
    struct State;

    [[nodiscard]] static bool RegisterWindowClass(HINSTANCE instance);
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static void Paint(HWND window, State& state);
};

} // namespace vectorclick::win

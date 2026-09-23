#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win {

struct MessageBoxButtonLabels {
    const wchar_t* yes{};
    const wchar_t* no{};
    const wchar_t* cancel{};
};

// Shows one native application message box at a time and places it over the
// center of the owning VectorClick window. Re-entrant warning attempts are
// coalesced into the active dialog instead of creating a stack of dialogs. If
// the owner or placement hook cannot be used, Windows' normal placement is
// retained.
int ShowCenteredMessageBox(HWND owner,
                           const wchar_t* text,
                           const wchar_t* caption,
                           UINT type) noexcept;

// Shows a centered MB_YESNOCANCEL question with user-facing button labels.
// The return values remain IDYES, IDNO, and IDCANCEL.
int ShowCenteredChoiceMessageBox(HWND owner,
                                 const wchar_t* text,
                                 const wchar_t* caption,
                                 UINT icon_type,
                                 const MessageBoxButtonLabels& labels) noexcept;

// Requests that the currently active application message box close so an
// Emergency Stop UI, including the Safety Shield, cannot remain blocked behind
// a prior warning or question. Safe to call from the hotkey control thread.
void DismissActiveMessageBoxForEmergency() noexcept;

} // namespace vectorclick::win

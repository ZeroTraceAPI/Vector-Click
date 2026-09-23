#pragma once

#include "Windows/target_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win {

// Shows an owned, modal list of selectable top-level windows. Returns true
// only when the user confirms a currently valid window.
[[nodiscard]] bool ShowTargetWindowPicker(HINSTANCE instance,
                                          HWND owner,
                                          DWORD excluded_process_id,
                                          const TargetWindowInfo& current,
                                          TargetWindowInfo& selected);

} // namespace vectorclick::win

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win {

[[nodiscard]] HICON LoadSharedDefaultIcon(HINSTANCE instance,
                                          LPCWSTR resource) noexcept;
[[nodiscard]] HCURSOR LoadSharedDefaultCursor(HINSTANCE instance,
                                              LPCWSTR resource) noexcept;

} // namespace vectorclick::win

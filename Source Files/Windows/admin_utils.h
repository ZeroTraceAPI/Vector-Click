#pragma once

#include <string>

namespace vectorclick::win {

[[nodiscard]] bool IsRunningAsAdministrator() noexcept;
[[nodiscard]] bool RestartAsAdministrator(const std::wstring& arguments = L"") noexcept;
[[nodiscard]] std::wstring ExecutableDirectory();
[[nodiscard]] std::wstring ExecutablePath();

} // namespace vectorclick::win

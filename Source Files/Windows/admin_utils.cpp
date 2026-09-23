#include "Windows/admin_utils.h"

#include "Windows/win32_raii.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <vector>

namespace vectorclick::win {

bool IsRunningAsAdministrator() noexcept {
    HANDLE raw_token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return false;
    }
    UniqueHandle token(raw_token);

    TOKEN_ELEVATION elevation{};
    DWORD size{};
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring ExecutableDirectory() {
    const std::wstring path = ExecutablePath();
    const std::wstring::size_type separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    // Preserve a drive root such as C:\ rather than returning the drive-relative
    // form C:, which has different Win32 path semantics.
    if (separator == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, separator);
}

bool RestartAsAdministrator(const std::wstring& arguments) noexcept {
    const auto executable = ExecutablePath();
    if (executable.empty()) {
        return false;
    }

    const auto directory = ExecutableDirectory();
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr,
        L"runas",
        executable.c_str(),
        arguments.empty() ? nullptr : arguments.c_str(),
        directory.empty() ? nullptr : directory.c_str(),
        SW_SHOWNORMAL));
    return result > 32;
}

} // namespace vectorclick::win

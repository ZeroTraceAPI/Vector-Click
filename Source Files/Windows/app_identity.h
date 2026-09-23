#pragma once

namespace vectorclick::win {

inline constexpr wchar_t ProductDisplayName[] = L"Vector Click";
inline constexpr wchar_t DeveloperDisplayName[] = L"ZeroTraceAPI";
inline constexpr wchar_t SupportEmail[] = L"ZeroTraceAPI@proton.me";
inline constexpr wchar_t RepositoryUrl[] =
    L"https://github.com/ZeroTraceAPI/Vector-Click";
inline constexpr wchar_t ReleasesUrl[] =
    L"https://github.com/ZeroTraceAPI/Vector-Click/releases";
inline constexpr wchar_t IssuesUrl[] =
    L"https://github.com/ZeroTraceAPI/Vector-Click/issues";
inline constexpr wchar_t LicenseUrl[] =
    L"https://github.com/ZeroTraceAPI/Vector-Click/blob/main/LICENSE";

inline constexpr int VectorClickIconResourceId = 101;
inline constexpr wchar_t MainWindowClassName[] = L"VectorClickMainWindow";
inline constexpr wchar_t SingleInstanceMutexName[] =
    L"Local\\VectorClick.SingleInstance.{6C83BC47-4A42-44CD-94A5-E75F0CC9A31E}";
inline constexpr wchar_t SingleInstanceNoticeMutexName[] =
    L"Local\\VectorClick.SingleInstanceNotice.{7B1B7E51-50E5-4D54-8B71-5F7359BA128E}";
inline constexpr wchar_t ElevatedRestartArgument[] = L"--elevated-restart";
inline constexpr wchar_t ElevatedTargetArgumentPrefix[] = L"--restore-target=";
inline constexpr wchar_t ElevatedPageArgumentPrefix[] = L"--restore-page=";
inline constexpr wchar_t ElevatedSettingsArgumentPrefix[] = L"--restore-settings=";
inline constexpr wchar_t ElevatedProfileArgumentPrefix[] = L"--restore-profile=";

} // namespace vectorclick::win

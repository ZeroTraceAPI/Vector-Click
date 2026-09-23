#include "Windows/target_window.h"

#include "Windows/app_identity.h"
#include "Windows/win32_raii.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <utility>
#include <vector>

namespace vectorclick::win {
namespace {

constexpr std::size_t MaximumWindowTitleCharacters = 512;
constexpr DWORD MaximumExecutablePathCharacters = 32'768;

// Low-level target inspection helpers. Candidate filtering intentionally uses
// observable window / process identity rather than titles alone so targeting and
// recovery do not silently drift to an unrelated window.
std::wstring ReadWindowClass(const HWND window) {
    std::array<wchar_t, 256> buffer{};
    const int copied = GetClassNameW(window, buffer.data(), static_cast<int>(buffer.size()));
    return copied > 0 ? std::wstring(buffer.data(), static_cast<std::size_t>(copied)) : std::wstring{};
}

bool IsRejectedShellClass(const std::wstring& class_name) noexcept {
    return class_name == L"Shell_TrayWnd" ||
           class_name == L"Shell_SecondaryTrayWnd" ||
           class_name == L"Progman" ||
           class_name == L"WorkerW";
}

bool WindowClassMatches(const HWND window, const std::wstring& expected) noexcept {
    std::array<wchar_t, 256> buffer{};
    const int copied = GetClassNameW(window, buffer.data(), static_cast<int>(buffer.size()));
    if (copied <= 0 || static_cast<std::size_t>(copied) != expected.size()) {
        return false;
    }
    return std::char_traits<wchar_t>::compare(buffer.data(), expected.data(), expected.size()) == 0;
}

std::wstring ReadWindowTitle(const HWND window) {
    const int reported_length = GetWindowTextLengthW(window);
    const std::size_t capacity = std::min<std::size_t>(
        MaximumWindowTitleCharacters + 1U,
        static_cast<std::size_t>(std::max(0, reported_length)) + 1U);
    std::vector<wchar_t> buffer(std::max<std::size_t>(capacity, 2U), L'\0');
    const int copied = GetWindowTextW(window, buffer.data(), static_cast<int>(buffer.size()));
    if (copied <= 0) {
        return {};
    }
    return std::wstring(buffer.data(), static_cast<std::size_t>(copied));
}


bool IsCloakedWindow(const HWND window) noexcept {
    DWORD cloaked{};
    return SUCCEEDED(DwmGetWindowAttribute(
               window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked != 0;
}

bool IsUsefulPickerCandidate(const HWND window) {
    if (window == nullptr || IsWindowVisible(window) == FALSE || IsCloakedWindow(window)) {
        return false;
    }

    const LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended_style & WS_EX_TOOLWINDOW) != 0 &&
        (extended_style & WS_EX_APPWINDOW) == 0) {
        return false;
    }

    RECT rectangle{};
    if (GetWindowRect(window, &rectangle) == FALSE ||
        rectangle.right <= rectangle.left ||
        rectangle.bottom <= rectangle.top) {
        return false;
    }

    // Empty-title shell and framework helper windows are not useful exact
    // targets and would otherwise create indistinguishable picker rows.
    return !ReadWindowTitle(window).empty();
}

std::wstring BaseNameFromPath(const std::wstring& path) {
    const std::size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1U);
}

std::wstring ReadProcessPath(const HANDLE process) {
    std::vector<wchar_t> path(MaximumExecutablePathCharacters, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &length) == FALSE || length == 0) {
        return {};
    }
    return std::wstring(path.data(), length);
}

bool PathsMatch(const std::wstring& left, const std::wstring& right) noexcept {
    return !left.empty() && !right.empty() &&
           CompareStringOrdinal(left.c_str(),
                                static_cast<int>(left.size()),
                                right.c_str(),
                                static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

bool RecoveryElevationMatches(const TargetElevation expected,
                              const TargetElevation candidate) noexcept {
    return expected != TargetElevation::Unknown && candidate == expected;
}

TargetElevation ReadElevation(const HANDLE process) noexcept {
    HANDLE raw_token{};
    if (OpenProcessToken(process, TOKEN_QUERY, &raw_token) == FALSE) {
        return TargetElevation::Unknown;
    }
    UniqueHandle token(raw_token);

    TOKEN_ELEVATION elevation{};
    DWORD returned_size{};
    if (GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &returned_size) == FALSE) {
        return TargetElevation::Unknown;
    }
    return elevation.TokenIsElevated != 0 ? TargetElevation::Elevated : TargetElevation::Standard;
}

const wchar_t* ElevationTextInternal(const TargetElevation elevation) noexcept {
    switch (elevation) {
    case TargetElevation::Standard: return L"Standard privilege";
    case TargetElevation::Elevated: return L"Administrator privilege";
    case TargetElevation::Unknown: return L"Privilege unavailable";
    }
    return L"Privilege unavailable";
}

int KeyboardRecipientScore(const HWND window) noexcept {
    if (window == nullptr || IsWindow(window) == FALSE ||
        IsWindowEnabled(window) == FALSE || IsWindowVisible(window) == FALSE) {
        return 0;
    }

    std::array<wchar_t, 256> class_name{};
    const int copied = GetClassNameW(
        window, class_name.data(), static_cast<int>(class_name.size()));
    if (copied <= 0) {
        return 0;
    }

    if (wcscmp(class_name.data(), L"Edit") == 0) {
        return 100;
    }
    if (wcsstr(class_name.data(), L"RichEdit") != nullptr ||
        wcsstr(class_name.data(), L"RICHEDIT") != nullptr) {
        return 95;
    }
    if (wcsstr(class_name.data(), L"Scintilla") != nullptr) {
        return 90;
    }
    return 0;
}

} // namespace

// Normalize any selected descendant to its usable top-level target and capture
// the identity fields needed for later validation, routing, and safe recovery.
bool InspectTargetWindow(const HWND candidate,
                         const DWORD excluded_process_id,
                         TargetWindowInfo& result,
                         std::wstring& error) {
    error.clear();
    result = {};

    if (candidate == nullptr || IsWindow(candidate) == FALSE) {
        error = L"No valid foreground window was available to select.";
        return false;
    }

    HWND target = GetAncestor(candidate, GA_ROOTOWNER);
    if (target == nullptr) {
        target = GetAncestor(candidate, GA_ROOT);
    }
    if (target == nullptr) {
        target = candidate;
    }

    const std::wstring target_class = ReadWindowClass(target);
    if (target_class.empty()) {
        error = L"The selected window's class could not be identified.";
        return false;
    }
    if (target == GetDesktopWindow() || target == GetShellWindow() || IsRejectedShellClass(target_class)) {
        error = L"The Windows desktop and taskbar cannot be selected as a target.";
        return false;
    }
    if (IsWindow(target) == FALSE || IsWindowVisible(target) == FALSE) {
        error = L"The selected window is no longer available or visible.";
        return false;
    }

    DWORD process_id{};
    const DWORD thread_id = GetWindowThreadProcessId(target, &process_id);
    if (process_id == 0 || thread_id == 0) {
        error = L"The selected window's process could not be identified.";
        return false;
    }
    if (process_id == excluded_process_id || target_class == MainWindowClassName) {
        error = L"Select another application's window. Vector Click cannot target itself.";
        return false;
    }

    TargetWindowInfo inspected{};
    inspected.window = target;
    inspected.process_id = process_id;
    inspected.thread_id = thread_id;
    inspected.title = ReadWindowTitle(target);
    inspected.window_class = target_class;

    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    if (process) {
        inspected.process_path = ReadProcessPath(process.get());
        inspected.process_name = BaseNameFromPath(inspected.process_path);
        inspected.elevation = ReadElevation(process.get());
    }

    if (inspected.title.empty()) {
        inspected.title = L"Untitled window";
    }
    if (inspected.process_name.empty()) {
        inspected.process_name = L"Process " + std::to_wstring(process_id);
    }

    result = std::move(inspected);
    return true;
}

bool IsTargetWindowValid(const TargetWindowInfo& target) noexcept {
    if (target.window == nullptr || target.process_id == 0 || IsWindow(target.window) == FALSE) {
        return false;
    }
    DWORD current_process_id{};
    const DWORD current_thread_id = GetWindowThreadProcessId(target.window, &current_process_id);
    if (current_process_id != target.process_id || current_thread_id != target.thread_id) {
        return false;
    }
    return WindowClassMatches(target.window, target.window_class);
}

TargetWindowIdentity CaptureTargetWindowIdentity(const TargetWindowInfo& target) {
    TargetWindowIdentity identity{};
    identity.window_value = reinterpret_cast<std::uintptr_t>(target.window);
    identity.process_id = target.process_id;
    identity.thread_id = target.thread_id;
    identity.window_class = target.window_class;
    identity.recovery_policy = target.recovery_policy;
    return identity;
}

bool TargetWindowMatchesIdentity(const TargetWindowInfo& target,
                                 const TargetWindowIdentity& identity) noexcept {
    return reinterpret_cast<std::uintptr_t>(target.window) == identity.window_value &&
           target.process_id == identity.process_id &&
           target.thread_id == identity.thread_id &&
           target.window_class == identity.window_class;
}

bool RestoreTargetWindow(const TargetWindowIdentity& identity,
                         const DWORD excluded_process_id,
                         TargetWindowInfo& result,
                         std::wstring& error) {
    result = {};
    error.clear();

    if (identity.window_value == 0 || identity.process_id == 0 ||
        identity.thread_id == 0 || identity.window_class.empty()) {
        error = L"The saved target identity is incomplete.";
        return false;
    }

    TargetWindowInfo inspected{};
    if (!InspectTargetWindow(reinterpret_cast<HWND>(identity.window_value),
                             excluded_process_id,
                             inspected,
                             error)) {
        return false;
    }
    if (!TargetWindowMatchesIdentity(inspected, identity)) {
        error = L"The target window changed before the administrator restart completed.";
        return false;
    }

    inspected.recovery_policy = identity.recovery_policy;
    result = std::move(inspected);
    return true;
}

bool IsTargetWindowForeground(const TargetWindowInfo& target) noexcept {
    if (!IsTargetWindowValid(target)) {
        return false;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    HWND root_owner = GetAncestor(foreground, GA_ROOTOWNER);
    if (root_owner == nullptr) {
        root_owner = GetAncestor(foreground, GA_ROOT);
    }
    if (root_owner == nullptr) {
        root_owner = foreground;
    }
    return root_owner == target.window;
}

bool IsWindowWithinTarget(const TargetWindowInfo& target,
                          const HWND recipient) noexcept {
    if (!IsTargetWindowValid(target) || recipient == nullptr ||
        IsWindow(recipient) == FALSE) {
        return false;
    }

    DWORD process_id{};
    if (GetWindowThreadProcessId(recipient, &process_id) == 0 ||
        process_id != target.process_id) {
        return false;
    }

    HWND root_owner = GetAncestor(recipient, GA_ROOTOWNER);
    if (root_owner == nullptr) {
        root_owner = GetAncestor(recipient, GA_ROOT);
    }
    if (root_owner == nullptr) {
        root_owner = recipient;
    }
    return root_owner == target.window;
}

// Targeted keyboard input prefers the thread's current focus, then scores child
// controls that are plausible text / key recipients. Every fallback is still
// required to remain inside the selected target process and root hierarchy.
HWND ResolveTargetKeyboardRecipient(const TargetWindowInfo& target) noexcept {
    if (!IsTargetWindowValid(target)) {
        return nullptr;
    }

    GUITHREADINFO information{};
    information.cbSize = sizeof(information);
    if (GetGUIThreadInfo(target.thread_id, &information) != FALSE &&
        information.hwndFocus != nullptr &&
        IsWindowWithinTarget(target, information.hwndFocus)) {
        return information.hwndFocus;
    }

    struct SearchState {
        const TargetWindowInfo* target{};
        HWND best{};
        int best_score{};
    } search{&target, nullptr, 0};

    EnumChildWindows(
        target.window,
        [](const HWND candidate, const LPARAM parameter) -> BOOL {
            auto& state = *reinterpret_cast<SearchState*>(parameter);
            if (!IsWindowWithinTarget(*state.target, candidate)) {
                return TRUE;
            }

            const int score = KeyboardRecipientScore(candidate);
            if (score > state.best_score) {
                state.best = candidate;
                state.best_score = score;
            }
            return score < 100 ? TRUE : FALSE;
        },
        reinterpret_cast<LPARAM>(&search));

    if (search.best != nullptr && IsWindowWithinTarget(target, search.best)) {
        return search.best;
    }

    if (information.cbSize == sizeof(information)) {
        for (const HWND candidate : {information.hwndActive, information.hwndCapture}) {
            if (IsWindowWithinTarget(target, candidate)) {
                return candidate;
            }
        }
    }
    return IsWindowWithinTarget(target, target.window) ? target.window : nullptr;
}


// Automatic recovery is intentionally conservative: the configured policy
// must identify exactly one replacement with the same application path and
// compatible elevation. Ambiguity is reported instead of guessed through.
bool TryRecoverTargetWindow(TargetWindowInfo& target,
                            const DWORD excluded_process_id,
                            std::wstring& error) {
    error.clear();

    if (IsTargetWindowValid(target)) {
        return true;
    }
    if (target.window == nullptr) {
        error = L"No target is selected.";
        return false;
    }
    if (target.recovery_policy == TargetRecoveryPolicy::ExactWindowOnly) {
        error = L"The exact selected window is no longer available.";
        return false;
    }
    if (target.process_path.empty()) {
        error = L"The selected application's executable path was unavailable, so Vector Click cannot recover it safely.";
        return false;
    }
    if (target.elevation == TargetElevation::Unknown) {
        error = L"The selected application's privilege level was unavailable, so Vector Click cannot recover it safely.";
        return false;
    }

    std::vector<TargetWindowInfo> matches;
    for (auto& candidate : EnumerateTargetWindows(excluded_process_id)) {
        if (!PathsMatch(candidate.process_path, target.process_path) ||
            !RecoveryElevationMatches(target.elevation, candidate.elevation)) {
            continue;
        }

        const bool identity_matches =
            target.recovery_policy == TargetRecoveryPolicy::SameApplicationAndClass
                ? candidate.window_class == target.window_class
                : target.recovery_policy == TargetRecoveryPolicy::SameApplicationAndTitle
                      ? candidate.title == target.title
                      : false;
        if (identity_matches) {
            matches.push_back(std::move(candidate));
        }
    }

    if (matches.empty()) {
        error = L"No replacement window matched the selected target recovery rule.";
        return false;
    }
    if (matches.size() != 1U) {
        error = L"More than one replacement window matched the selected target recovery rule. Vector Click will not choose between ambiguous targets.";
        return false;
    }

    const TargetRecoveryPolicy policy = target.recovery_policy;
    target = std::move(matches.front());
    target.recovery_policy = policy;
    return true;
}

// Build the picker list from usable top-level windows, remove duplicate hosted
// app shells when a direct application window is available, then sort only for
// stable presentation. Enumeration order never decides recovery identity.
std::vector<TargetWindowInfo> EnumerateTargetWindows(const DWORD excluded_process_id) {
    struct EnumerationContext {
        DWORD excluded_process_id{};
        std::vector<TargetWindowInfo> windows;
    };

    EnumerationContext context{};
    context.excluded_process_id = excluded_process_id;

    EnumWindows(
        [](const HWND candidate, const LPARAM parameter) -> BOOL {
            auto& state = *reinterpret_cast<EnumerationContext*>(parameter);
            if (!IsUsefulPickerCandidate(candidate)) {
                return TRUE;
            }

            TargetWindowInfo inspected{};
            std::wstring error;
            if (!InspectTargetWindow(candidate, state.excluded_process_id, inspected, error)) {
                return TRUE;
            }

            const auto duplicate = std::ranges::find_if(
                state.windows,
                [window = inspected.window](const TargetWindowInfo& existing) {
                    return existing.window == window;
                });
            if (duplicate == state.windows.end()) {
                state.windows.push_back(std::move(inspected));
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));

    std::vector<std::wstring> direct_application_titles;
    for (const auto& candidate : context.windows) {
        if (candidate.process_name != L"ApplicationFrameHost.exe") {
            direct_application_titles.push_back(candidate.title);
        }
    }
    context.windows.erase(
        std::remove_if(
            context.windows.begin(),
            context.windows.end(),
            [&direct_application_titles](const TargetWindowInfo& candidate) {
                return candidate.process_name == L"ApplicationFrameHost.exe" &&
                       std::ranges::find(direct_application_titles, candidate.title) !=
                           direct_application_titles.end();
            }),
        context.windows.end());

    std::ranges::sort(
        context.windows,
        [](const TargetWindowInfo& left, const TargetWindowInfo& right) {
            if (left.process_name != right.process_name) {
                return left.process_name < right.process_name;
            }
            if (left.title != right.title) {
                return left.title < right.title;
            }
            return left.process_id < right.process_id;
        });
    return context.windows;
}

const wchar_t* TargetElevationText(const TargetElevation elevation) noexcept {
    return ElevationTextInternal(elevation);
}

const wchar_t* TargetRecoveryPolicyText(const TargetRecoveryPolicy policy) noexcept {
    switch (policy) {
    case TargetRecoveryPolicy::ExactWindowOnly:
        return L"Exact selected window only";
    case TargetRecoveryPolicy::SameApplicationAndClass:
        return L"Same application and window type";
    case TargetRecoveryPolicy::SameApplicationAndTitle:
        return L"Same application and exact title";
    }
    return L"Exact selected window only";
}

const wchar_t* TargetRecoveryPolicyDescription(const TargetRecoveryPolicy policy) noexcept {
    switch (policy) {
    case TargetRecoveryPolicy::ExactWindowOnly:
        return L"Vector Click will keep using only the window you selected. If that window closes or the application restarts, the target becomes unavailable until you choose it again. Best when preventing an incorrect replacement is more important than automatic recovery.";
    case TargetRecoveryPolicy::SameApplicationAndClass:
        return L"If the selected window closes or is recreated, Vector Click can reconnect to one window from the same application and Windows permission level that is the same kind of window, such as its main window rather than a settings dialog. Best when the window title changes often.";
    case TargetRecoveryPolicy::SameApplicationAndTitle:
        return L"If the selected window closes or is recreated, Vector Click can reconnect to one window from the same application and Windows permission level only when its title text is exactly the same. Best when the desired window has a stable, unique title.";
    }
    return L"Vector Click will keep using only the window you selected.";
}

std::wstring TargetWindowSummary(const TargetWindowInfo& target) {
    if (target.window == nullptr) {
        return L"No target selected (optional)";
    }
    const std::wstring validity = IsTargetWindowValid(target) ? L"Ready" : L"Unavailable";
    return validity + L": " + target.title + L" | " + target.process_name + L" | " + TargetElevationText(target.elevation);
}

std::wstring TargetWindowDetails(const TargetWindowInfo& target) {
    if (target.window == nullptr) {
        return L"No target selected. Automatic uses Standard input until you choose one. Target selections and recovery choices are not saved between launches.";
    }

    std::wstring details = IsTargetWindowValid(target) ? L"Target available" : L"Target unavailable";
    details += L". Title: " + target.title;
    details += L". Process: " + target.process_name;
    details += L". PID: " + std::to_wstring(target.process_id);
    details += L". " + std::wstring(TargetElevationText(target.elevation));
    details += L". Target recovery: " + std::wstring(TargetRecoveryPolicyText(target.recovery_policy));
    details += L". Automatic uses Foreground target input while this target is selected and Allow background input is off. Enabling background input makes Automatic use Targeted window messages instead. Standard input and Unicode text input send to the currently active window rather than this target. Foreground target input, Targeted window messages, and Targeted Unicode text use the selected target. Targeted message methods may not work with every application.";
    return details;
}

} // namespace vectorclick::win

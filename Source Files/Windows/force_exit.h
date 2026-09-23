#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <thread>

namespace vectorclick::win::force_exit {

inline constexpr std::size_t CleanupPasses = 4;
inline constexpr DWORD InterpassDelayMilliseconds = 500;
inline constexpr DWORD WatchdogDeadlineMilliseconds = 2'000;
inline constexpr UINT ProcessExitCode = 0xE106U;

static_assert(CleanupPasses >= 2);
static_assert(WatchdogDeadlineMilliseconds >
              (CleanupPasses - 1U) * InterpassDelayMilliseconds);

[[noreturn]] inline void TerminateCurrentProcess() noexcept {
    if (TerminateProcess(GetCurrentProcess(), ProcessExitCode) == FALSE) {
        ExitProcess(ProcessExitCode);
    }
    for (;;) {
        Sleep(INFINITE);
    }
}

inline void StartWatchdogOrTerminate() noexcept {
    try {
        std::thread([] {
            Sleep(WatchdogDeadlineMilliseconds);
            TerminateCurrentProcess();
        }).detach();
    } catch (...) {
        // A force-exit request is useful only if it has a hard bounded end.
        // If the watchdog cannot be created, terminate rather than entering
        // cleanup that could block indefinitely.
        TerminateCurrentProcess();
    }
}

} // namespace vectorclick::win::force_exit

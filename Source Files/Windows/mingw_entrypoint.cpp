// Some MinGW-compatible compiler drivers do not honor -municode consistently.
// Keep the application implementation in wWinMain while providing a narrow
// GUI entry-point bridge only for those toolchains. MSVC builds compile this
// translation unit without defining an additional entry point.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if defined(__MINGW32__)
extern "C" int WINAPI wWinMain(HINSTANCE instance,
                                HINSTANCE previous_instance,
                                PWSTR command_line,
                                int show_command);

extern "C" int WINAPI WinMain(HINSTANCE instance,
                               HINSTANCE previous_instance,
                               LPSTR,
                               int show_command) {
    return wWinMain(instance, previous_instance, GetCommandLineW(), show_command);
}
#endif

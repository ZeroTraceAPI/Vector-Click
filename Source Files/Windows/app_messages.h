#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win {

inline constexpr UINT WM_APP_REQUEST_START = WM_APP + 1;
inline constexpr UINT WM_APP_EMERGENCY_UI = WM_APP + 2;
inline constexpr UINT WM_APP_ENGINE_STATE = WM_APP + 3;
inline constexpr UINT WM_APP_HOTKEY_ERROR = WM_APP + 4;
inline constexpr UINT WM_APP_CLEAR_TRANSIENT_FOCUS = WM_APP + 5;
inline constexpr UINT WM_APP_SAFETY_SHIELD_CLOSED = WM_APP + 6;
inline constexpr UINT WM_APP_CLICK_INDICATOR = WM_APP + 7;
inline constexpr UINT WM_APP_APPLY_INPUT_TYPE = WM_APP + 8;
inline constexpr UINT WM_APP_FINISH_STARTUP = WM_APP + 9;
inline constexpr UINT WM_APP_REPAIR_SURFACES = WM_APP + 10;
inline constexpr UINT WM_APP_CAPTURE_KEY = WM_APP + 11;
inline constexpr UINT WM_APP_REFRESH_NUMERIC_PRESENTATION = WM_APP + 12;
inline constexpr UINT WM_APP_FORCE_STOP_EXIT = WM_APP + 13;
inline constexpr UINT WM_APP_HOTKEY_REGISTRATION_RESULT = WM_APP + 14;
inline constexpr UINT WM_APP_RETRY_CLEANUP = WM_APP + 15;
inline constexpr UINT WM_APP_REFRESH_MOVE_COVER = WM_APP + 16;
inline constexpr UINT WM_APP_FINISH_RESIZE_OVERLAY = WM_APP + 17;
inline constexpr UINT WM_APP_RUN_SESSION_READY = WM_APP + 18;
inline constexpr UINT WM_APP_RECONCILE_ENGINE_STATE = WM_APP + 19;
inline constexpr UINT WM_APP_HOTKEY_NORMAL_STOP = WM_APP + 20;
inline constexpr UINT WM_APP_SINGLE_INSTANCE_NOTICE_REQUEST = WM_APP + 21;
inline constexpr UINT WM_APP_PRESENT_SINGLE_INSTANCE_NOTICE = WM_APP + 22;

// The request is accepted only when both values match. This lets a newer
// secondary process distinguish a compatible Vector Click instance from an
// older build that happens to use the same main-window class.
inline constexpr WPARAM SingleInstanceNoticeRequestTag = 0x56434E31U;
inline constexpr LPARAM SingleInstanceNoticeRequestCheck = 0x4E4F5449L;
inline constexpr LRESULT SingleInstanceNoticeAck = 0x56434F4BL;

} // namespace vectorclick::win

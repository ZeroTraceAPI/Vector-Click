#pragma once

#include "Windows/input_backend.h"
#include "Windows/target_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace vectorclick::win {

class WindowMessageBackend final : public InputBackend {
public:
    [[nodiscard]] bool BeginSession(const core::RunSettings& settings,
                                    const TargetWindowInfo& target) noexcept;
    [[nodiscard]] bool Press(const core::RunSettings& settings,
                             const InputSessionCancellation& cancellation) noexcept override;
    [[nodiscard]] bool Release(const core::RunSettings& settings) noexcept override;
    [[nodiscard]] bool ReleaseAll() noexcept override;
    [[nodiscard]] bool HasTrackedInput() const noexcept override;
    [[nodiscard]] bool LastMousePressScreenPoint(ScreenPoint& point) const noexcept override;

private:
    struct PressedMouseState {
        bool pressed{};
        HWND recipient{};
        LPARAM client_position{};
        ScreenPoint screen_point{};
    };

    struct PressedKeyboardState {
        bool pressed{};
        bool shift_pressed{};
        HWND recipient{};
        std::uint16_t virtual_key{};
        LPARAM key_down_lparam{};
        LPARAM shift_down_lparam{};
    };

    [[nodiscard]] static std::size_t MouseIndex(core::MouseButton button) noexcept;
    [[nodiscard]] bool DeliveryIsAllowed(const core::RunSettings& settings) const noexcept;
    [[nodiscard]] bool ResolveMouseRecipient(const core::RunSettings& settings,
                                             HWND& recipient,
                                             LPARAM& client_position,
                                             ScreenPoint& screen_point) const noexcept;
    [[nodiscard]] HWND ResolveKeyboardRecipient() const noexcept;
    [[nodiscard]] bool SendButtonMessage(HWND recipient,
                                         core::MouseButton button,
                                         bool pressed,
                                         LPARAM client_position,
                                         DWORD* windows_error = nullptr) noexcept;
    [[nodiscard]] bool SendKeyMessage(HWND recipient,
                                      std::uint16_t virtual_key,
                                      bool pressed,
                                      LPARAM key_down_lparam = 0,
                                      DWORD* windows_error = nullptr) noexcept;
    [[nodiscard]] bool SendCharacterMessage(HWND recipient,
                                            std::uint16_t virtual_key,
                                            std::uint16_t modifiers,
                                            LPARAM key_down_lparam) noexcept;
    [[nodiscard]] bool ReleaseKeyboardState() noexcept;
    [[nodiscard]] bool IsRecipientWithinTarget(HWND recipient) const noexcept;
    void ClearSession() noexcept;

    TargetWindowInfo target_{};
    std::array<PressedMouseState, 5> mouse_pressed_{};
    PressedKeyboardState keyboard_pressed_{};
    ScreenPoint last_mouse_press_point_{};
    bool has_last_mouse_press_point_{};
};

} // namespace vectorclick::win

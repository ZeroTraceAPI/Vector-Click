#pragma once

#include "Windows/input_backend.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace vectorclick::win {

class SendInputBackend final : public InputBackend {
public:
    [[nodiscard]] bool Press(const core::RunSettings& settings,
                             const InputSessionCancellation& cancellation) noexcept override;
    [[nodiscard]] bool Release(const core::RunSettings& settings) noexcept override;
    [[nodiscard]] bool ReleaseAll() noexcept override;
    [[nodiscard]] bool HasTrackedInput() const noexcept override;
    [[nodiscard]] bool LastMousePressScreenPoint(ScreenPoint& point) const noexcept override;

private:
    struct PressedMouseState {
        bool pressed{};
        bool fixed_position{};
        bool has_screen_point{};
        std::int32_t normalized_x{};
        std::int32_t normalized_y{};
        ScreenPoint screen_point{};
    };

    [[nodiscard]] static std::size_t MouseIndex(core::MouseButton button) noexcept;
    [[nodiscard]] bool PressMouse(const core::RunSettings& settings) noexcept;
    [[nodiscard]] bool ReleaseMouse(core::MouseButton button) noexcept;
    [[nodiscard]] bool PressKeyboard(const core::RunSettings& settings,
                                      const InputSessionCancellation& cancellation) noexcept;
    [[nodiscard]] bool ReleaseKeyboard(const core::RunSettings& settings) noexcept;
    struct KeyboardTransition {
        std::uint16_t virtual_key{};
        bool key_up{};
        bool generated_shift_transition{};
    };

    [[nodiscard]] bool SubmitKeyboardSequence(const KeyboardTransition* transitions,
                                               std::size_t count) noexcept;
    [[nodiscard]] bool PressKey(std::uint16_t virtual_key) noexcept;
    [[nodiscard]] bool ReleaseKey(std::uint16_t virtual_key) noexcept;

    std::array<PressedMouseState, 5> mouse_pressed_{};
    std::array<bool, 256> key_pressed_{};
    bool generated_shift_injected_{};
    ScreenPoint last_mouse_press_point_{};
    bool has_last_mouse_press_point_{};
};

} // namespace vectorclick::win

#pragma once

#include "Windows/input_backend.h"

#include <optional>

namespace vectorclick::win {

class UnicodeInputBackend final : public InputBackend {
public:
    [[nodiscard]] bool Press(const core::RunSettings& settings,
                             const InputSessionCancellation& cancellation) noexcept override;
    [[nodiscard]] bool Release(const core::RunSettings& settings) noexcept override;
    [[nodiscard]] bool ReleaseAll() noexcept override;
    [[nodiscard]] bool HasTrackedInput() const noexcept override;
    [[nodiscard]] bool LastMousePressScreenPoint(ScreenPoint& point) const noexcept override;

private:
    [[nodiscard]] bool SubmitCharacter(wchar_t character, bool key_up) noexcept;
    [[nodiscard]] bool SubmitPulse(wchar_t character) noexcept;

    std::optional<wchar_t> pressed_character_{};
};

} // namespace vectorclick::win

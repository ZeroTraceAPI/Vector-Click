#pragma once

#include "Windows/input_backend.h"
#include "Windows/target_window.h"

namespace vectorclick::win {

class TargetedUnicodeInputBackend final : public InputBackend {
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
    [[nodiscard]] bool DeliveryIsAllowed(const core::RunSettings& settings) const noexcept;
    [[nodiscard]] bool SendCharacter(HWND recipient,
                                     wchar_t character,
                                     std::uint16_t virtual_key) noexcept;
    void ClearSession() noexcept;

    TargetWindowInfo target_{};
};

} // namespace vectorclick::win

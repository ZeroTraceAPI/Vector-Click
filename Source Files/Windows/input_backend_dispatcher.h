#pragma once

#include "Core/settings.h"
#include "Windows/send_input_backend.h"
#include "Windows/target_window.h"
#include "Windows/targeted_unicode_input_backend.h"
#include "Windows/unicode_input_backend.h"
#include "Windows/window_message_backend.h"

namespace vectorclick::win {

class InputBackendDispatcher {
public:
    [[nodiscard]] bool BeginSession(const core::RunSettings& settings,
                                    const TargetWindowInfo& target) noexcept;
    [[nodiscard]] bool Press(const core::RunSettings& settings,
                             const InputSessionCancellation& cancellation) noexcept;
    [[nodiscard]] bool Release(const core::RunSettings& settings) noexcept;
    [[nodiscard]] bool ReleaseAll() noexcept;
    [[nodiscard]] bool HasTrackedInput() const noexcept;
    [[nodiscard]] bool RetryCleanup() noexcept;
    [[nodiscard]] bool LastMousePressScreenPoint(ScreenPoint& point) const noexcept;

    [[nodiscard]] static core::InputBackend EffectiveBackend(
        core::InputBackend requested,
        const TargetWindowInfo& target,
        bool allow_background_input) noexcept;

private:
    [[nodiscard]] InputBackend* Resolve(core::InputBackend effective) noexcept;
    [[nodiscard]] bool ForegroundTargetDeliveryIsAllowed(
        const core::RunSettings& settings) const noexcept;
    void ClearForegroundTargetSession() noexcept;

    SendInputBackend standard_input_;
    SendInputBackend foreground_target_input_;
    WindowMessageBackend targeted_window_messages_;
    UnicodeInputBackend unicode_text_input_;
    TargetedUnicodeInputBackend targeted_unicode_text_;
    TargetWindowInfo foreground_target_{};
    core::InputBackend active_backend_{core::InputBackend::Automatic};
    InputBackend* active_{nullptr};
};

} // namespace vectorclick::win

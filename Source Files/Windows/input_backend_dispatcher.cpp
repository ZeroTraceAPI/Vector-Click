#include "Windows/input_backend_dispatcher.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vectorclick::win {
namespace {

bool ScreenPointBelongsToTarget(const POINT point,
                                const TargetWindowInfo& target) noexcept {
    HWND recipient = WindowFromPoint(point);
    if (recipient == nullptr || IsWindow(recipient) == FALSE) {
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

} // namespace

bool InputBackendDispatcher::BeginSession(const core::RunSettings& settings,
                                          const TargetWindowInfo& target) noexcept {
    // A new run must never replace or clear any backend's unresolved release
    // ledger. This remains authoritative even if a stale UI request reaches the
    // worker after Start has already been disabled.
    if (HasTrackedInput()) {
        return false;
    }

    active_ = nullptr;
    active_backend_ = core::InputBackend::Automatic;
    ClearForegroundTargetSession();

    const core::InputBackend effective = EffectiveBackend(
        settings.backend, target, settings.allow_background_input);
    InputBackend* const candidate = Resolve(effective);
    if (candidate == nullptr) {
        return false;
    }

    if (effective == core::InputBackend::ForegroundTargetInput) {
        if (!IsTargetWindowValid(target) || !IsTargetWindowForeground(target)) {
            return false;
        }
        try {
            foreground_target_ = target;
        } catch (...) {
            ClearForegroundTargetSession();
            return false;
        }
    } else if (effective == core::InputBackend::TargetedWindowMessages &&
               !targeted_window_messages_.BeginSession(settings, target)) {
        return false;
    } else if (effective == core::InputBackend::TargetedUnicodeText &&
               !targeted_unicode_text_.BeginSession(settings, target)) {
        return false;
    }

    active_backend_ = effective;
    active_ = candidate;
    return true;
}

core::InputBackend InputBackendDispatcher::EffectiveBackend(
    const core::InputBackend requested,
    const TargetWindowInfo& target,
    const bool allow_background_input) noexcept {
    return core::ResolveInputBackend(
        requested, target.window != nullptr, allow_background_input);
}

bool InputBackendDispatcher::Press(
    const core::RunSettings& settings,
    const InputSessionCancellation& cancellation) noexcept {
    if (active_ == nullptr) {
        return false;
    }
    if (active_backend_ == core::InputBackend::ForegroundTargetInput &&
        !ForegroundTargetDeliveryIsAllowed(settings)) {
        return false;
    }
    return active_->Press(settings, cancellation);
}

bool InputBackendDispatcher::Release(const core::RunSettings& settings) noexcept {
    // Once VectorClick has accepted a press, release submission must not depend
    // on the selected target still being foreground. The backend ledger remains
    // authoritative until Windows accepts the matching release.
    return active_ != nullptr && active_->Release(settings);
}

bool InputBackendDispatcher::LastMousePressScreenPoint(ScreenPoint& point) const noexcept {
    return active_ != nullptr && active_->LastMousePressScreenPoint(point);
}

bool InputBackendDispatcher::ReleaseAll() noexcept {
    // Release every backend ledger, not only the active one, so cleanup remains
    // complete even after a run selected a different input method. Failed
    // releases intentionally leave their ledger entries intact for RetryCleanup.
    const bool standard_released = standard_input_.ReleaseAll();
    const bool foreground_released = foreground_target_input_.ReleaseAll();
    const bool targeted_released = targeted_window_messages_.ReleaseAll();
    const bool unicode_released = unicode_text_input_.ReleaseAll();
    const bool targeted_unicode_released = targeted_unicode_text_.ReleaseAll();
    const bool cleared = !HasTrackedInput();
    if (cleared) {
        active_ = nullptr;
        active_backend_ = core::InputBackend::Automatic;
        ClearForegroundTargetSession();
    }
    return standard_released && foreground_released && targeted_released &&
           unicode_released && targeted_unicode_released && cleared;
}

bool InputBackendDispatcher::HasTrackedInput() const noexcept {
    return standard_input_.HasTrackedInput() ||
           foreground_target_input_.HasTrackedInput() ||
           targeted_window_messages_.HasTrackedInput() ||
           unicode_text_input_.HasTrackedInput() ||
           targeted_unicode_text_.HasTrackedInput();
}

bool InputBackendDispatcher::RetryCleanup() noexcept {
    return ReleaseAll();
}

InputBackend* InputBackendDispatcher::Resolve(const core::InputBackend effective) noexcept {
    switch (effective) {
    case core::InputBackend::Automatic:
        return nullptr;
    case core::InputBackend::StandardInput:
        return &standard_input_;
    case core::InputBackend::ForegroundTargetInput:
        return &foreground_target_input_;
    case core::InputBackend::TargetedWindowMessages:
        return &targeted_window_messages_;
    case core::InputBackend::UnicodeTextInput:
        return &unicode_text_input_;
    case core::InputBackend::TargetedUnicodeText:
        return &targeted_unicode_text_;
    }
    return nullptr;
}

bool InputBackendDispatcher::ForegroundTargetDeliveryIsAllowed(
    const core::RunSettings& settings) const noexcept {
    if (!IsTargetWindowValid(foreground_target_) ||
        !IsTargetWindowForeground(foreground_target_)) {
        return false;
    }

    if (settings.action_type != core::ActionType::MouseClick) {
        return true;
    }
    if (IsIconic(foreground_target_.window) != FALSE) {
        return false;
    }

    POINT point{};
    if (settings.position_mode == core::PositionMode::FixedScreen) {
        point.x = settings.fixed_x;
        point.y = settings.fixed_y;
    } else if (GetCursorPos(&point) == FALSE) {
        return false;
    }
    return ScreenPointBelongsToTarget(point, foreground_target_);
}

void InputBackendDispatcher::ClearForegroundTargetSession() noexcept {
    foreground_target_ = {};
}

} // namespace vectorclick::win

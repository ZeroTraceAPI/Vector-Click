#include "Windows/controller.h"

#include <utility>

namespace vectorclick::win {

Controller::Controller(StateCallback callback,
                       MousePointCallback mouse_point_callback,
                       SessionReadyCallback session_ready_callback)
    : worker_(std::move(callback),
              std::move(mouse_point_callback),
              std::move(session_ready_callback)) {}

bool Controller::Start(const core::RunSettings& settings,
                       const TargetWindowInfo& target) {
    return worker_.Start(settings, target);
}

bool Controller::RequestStop() noexcept {
    return worker_.RequestStop();
}

bool Controller::EmergencyStop() noexcept {
    return worker_.EmergencyStop();
}

bool Controller::RequestEmergencyStop() noexcept {
    return worker_.RequestEmergencyStop();
}

bool Controller::CompleteEmergencyStop(const bool was_running) noexcept {
    return worker_.CompleteEmergencyStop(was_running);
}

bool Controller::HasTrackedInput() const noexcept {
    return worker_.HasTrackedInput();
}

bool Controller::RetryCleanup() noexcept {
    return worker_.RetryCleanup();
}

} // namespace vectorclick::win

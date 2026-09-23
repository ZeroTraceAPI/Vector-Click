#pragma once

#include "Core/settings.h"
#include "Windows/click_worker.h"


namespace vectorclick::win {

class Controller {
public:
    using StateCallback = ClickWorker::StateCallback;
    using MousePointCallback = ClickWorker::MousePointCallback;
    using SessionReadyCallback = ClickWorker::SessionReadyCallback;

    Controller(StateCallback callback,
               MousePointCallback mouse_point_callback,
               SessionReadyCallback session_ready_callback);

    [[nodiscard]] bool Start(const core::RunSettings& settings,
                             const TargetWindowInfo& target);
    [[nodiscard]] bool RequestStop() noexcept;
    [[nodiscard]] bool EmergencyStop() noexcept;
    [[nodiscard]] bool RequestEmergencyStop() noexcept;
    [[nodiscard]] bool CompleteEmergencyStop(bool was_running) noexcept;
    [[nodiscard]] bool HasTrackedInput() const noexcept;
    [[nodiscard]] bool RetryCleanup() noexcept;

    [[nodiscard]] bool IsRunning() const noexcept { return worker_.IsRunning(); }
    [[nodiscard]] EngineState State() const noexcept { return worker_.State(); }
    [[nodiscard]] std::uint64_t CompletedActions() const noexcept { return worker_.CompletedActions(); }
    [[nodiscard]] DiagnosticsSnapshot Diagnostics() const noexcept { return worker_.Diagnostics(); }
    [[nodiscard]] bool LastRunHadBackendFailure() const noexcept {
        return worker_.LastRunHadBackendFailure();
    }
    [[nodiscard]] RunLimitStopReason LastRunLimitStopReason() const noexcept {
        return worker_.LastRunLimitStopReason();
    }

private:
    ClickWorker worker_;
};

} // namespace vectorclick::win

#pragma once

#include "Core/settings.h"

#include <atomic>
#include <cstdint>

namespace vectorclick::win {

struct ScreenPoint {
    std::int32_t x{};
    std::int32_t y{};

    friend bool operator==(const ScreenPoint&, const ScreenPoint&) = default;
};

struct InputSessionCancellation {
    const std::atomic<bool>* running{};
    const std::atomic<std::uint64_t>* session_id{};
    std::uint64_t expected_session{};

    [[nodiscard]] bool Requested() const noexcept {
        return running == nullptr || session_id == nullptr ||
               !running->load(std::memory_order_acquire) ||
               session_id->load(std::memory_order_acquire) != expected_session;
    }
};

class InputBackend {
public:
    virtual ~InputBackend() = default;

    InputBackend(const InputBackend&) = delete;
    InputBackend& operator=(const InputBackend&) = delete;
    InputBackend(InputBackend&&) = delete;
    InputBackend& operator=(InputBackend&&) = delete;

    [[nodiscard]] virtual bool Press(const core::RunSettings& settings,
                                     const InputSessionCancellation& cancellation) noexcept = 0;
    [[nodiscard]] virtual bool Release(const core::RunSettings& settings) noexcept = 0;
    [[nodiscard]] virtual bool ReleaseAll() noexcept = 0;
    [[nodiscard]] virtual bool HasTrackedInput() const noexcept = 0;
    [[nodiscard]] virtual bool LastMousePressScreenPoint(ScreenPoint& point) const noexcept = 0;

protected:
    InputBackend() = default;
};

} // namespace vectorclick::win

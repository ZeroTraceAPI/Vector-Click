#pragma once

namespace vectorclick::win {

[[nodiscard]] constexpr bool ShutdownCleanupSucceeded(
    const bool releases_submitted,
    const bool has_tracked_input) noexcept {
    return releases_submitted && !has_tracked_input;
}

} // namespace vectorclick::win

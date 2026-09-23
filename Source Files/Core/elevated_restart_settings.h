#pragma once

#include "Core/settings.h"

#include <optional>
#include <string>
#include <string_view>

namespace vectorclick::core {

// Encodes only RunSettings scalar state into a bounded, versioned hexadecimal
// payload suitable for the one-process elevated-restart handoff. The payload
// contains no paths, target identity, window titles, or other arbitrary text.
[[nodiscard]] std::wstring EncodeElevatedRestartSettings(
    const RunSettings& settings);

// Strictly decodes the current handoff format and revalidates the resulting
// RunSettings. Unknown versions, malformed hex, trailing data, invalid booleans,
// inconsistent duration components, or unsafe settings are rejected.
[[nodiscard]] std::optional<RunSettings> DecodeElevatedRestartSettings(
    std::wstring_view payload) noexcept;

} // namespace vectorclick::core

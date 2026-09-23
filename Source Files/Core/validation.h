#pragma once

#include "Core/settings.h"

#include <optional>
#include <string>
#include <vector>

namespace vectorclick::core {

struct ValidationIssue {
    std::string message;
};

[[nodiscard]] std::vector<ValidationIssue> ValidateRunSettings(const RunSettings& settings);

[[nodiscard]] std::optional<HotkeyBinding> FindNextAvailableGeneratedKey(
    const std::vector<HotkeyBinding>& ordered_keys,
    const HotkeyBinding& current_key,
    const HotkeyBinding& start_stop_hotkey,
    const HotkeyBinding& emergency_hotkey) noexcept;

[[nodiscard]] bool HasErrors(const std::vector<ValidationIssue>& issues) noexcept;

} // namespace vectorclick::core

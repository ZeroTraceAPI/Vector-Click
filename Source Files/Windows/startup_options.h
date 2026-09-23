#pragma once

#include "Core/settings.h"
#include "Windows/target_window.h"

#include <optional>
#include <string>

namespace vectorclick::win {

struct StartupOptions {
    bool elevated_restart{};
    std::optional<TargetWindowIdentity> target_to_restore;
    std::optional<int> page_to_restore;
    std::optional<core::RunSettings> settings_to_restore;
    std::optional<std::wstring> profile_id_to_restore;
    bool settings_restore_invalid{};
};

[[nodiscard]] StartupOptions ParseStartupOptions();
[[nodiscard]] std::wstring BuildElevatedRestartArguments(
    const TargetWindowInfo* target,
    int selected_page_index,
    const core::RunSettings& settings,
    std::wstring_view active_profile_id = {});

} // namespace vectorclick::win

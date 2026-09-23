#pragma once

#include "Core/settings.h"

#include <optional>
#include <string>
#include <string_view>

namespace vectorclick::win {

class SettingsStore {
public:
    explicit SettingsStore(std::wstring product_name);

    [[nodiscard]] bool Exists() const;
    [[nodiscard]] bool Load(core::RunSettings& settings, std::wstring& error) const;
    [[nodiscard]] bool LoadActiveProfileId(std::optional<std::wstring>& profile_id) const;
    [[nodiscard]] bool LoadFromPath(const std::wstring& path,
                                    core::RunSettings& settings,
                                    std::wstring& error) const;
    [[nodiscard]] bool IsCanonicalPath(const std::wstring& path) const noexcept;
    [[nodiscard]] bool Save(const core::RunSettings& settings,
                            std::wstring& error,
                            std::wstring_view active_profile_id = {}) const;
    [[nodiscard]] bool SaveToPath(const std::wstring& path,
                                  const core::RunSettings& settings,
                                  std::wstring& error,
                                  std::string_view leading_json_members = {}) const;
    [[nodiscard]] bool Remove(std::wstring& error) const;

private:
    std::wstring path_;
};

} // namespace vectorclick::win

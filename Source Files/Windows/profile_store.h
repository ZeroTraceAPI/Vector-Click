#pragma once

#include "Core/settings.h"
#include "Windows/settings_store.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vectorclick::win {

inline constexpr std::size_t MaximumProfileNameCharacters = 40;
inline constexpr wchar_t ProfileFileExtension[] = L".VectorClickProfile";

struct ProfileInfo final {
    std::wstring id;
    std::wstring name;
    std::wstring path;

    friend bool operator==(const ProfileInfo&, const ProfileInfo&) = default;
};

class ProfileStore {
public:
    explicit ProfileStore(std::wstring product_name);

    [[nodiscard]] const std::wstring& DirectoryPath() const noexcept {
        return directory_path_;
    }

    [[nodiscard]] bool DirectoryExists(bool& exists, std::wstring& error) const;
    [[nodiscard]] bool EnsureDirectory(std::wstring& error) const;
    [[nodiscard]] bool List(std::vector<ProfileInfo>& profiles,
                            std::vector<std::wstring>* warnings,
                            std::wstring& error) const;
    [[nodiscard]] bool Load(const std::wstring& id,
                            core::RunSettings& settings,
                            ProfileInfo& profile,
                            std::wstring& error) const;
    [[nodiscard]] bool Create(std::wstring_view name,
                              const core::RunSettings& settings,
                              ProfileInfo& profile,
                              std::wstring& error) const;
    [[nodiscard]] bool Save(const ProfileInfo& profile,
                            const core::RunSettings& settings,
                            std::wstring& error) const;
    [[nodiscard]] bool Rename(const ProfileInfo& profile,
                              std::wstring_view new_name,
                              ProfileInfo& renamed,
                              std::wstring& error) const;
    [[nodiscard]] bool Duplicate(const ProfileInfo& profile,
                                 std::wstring_view new_name,
                                 ProfileInfo& duplicated,
                                 std::wstring& error) const;
    [[nodiscard]] bool Remove(const ProfileInfo& profile,
                              std::wstring& error) const;
    [[nodiscard]] bool ImportFromPath(const std::wstring& source_path,
                                      ProfileInfo& imported,
                                      std::wstring& error) const;
    [[nodiscard]] bool ExportToPath(const ProfileInfo& profile,
                                    const std::wstring& destination_path,
                                    std::wstring& error) const;

    [[nodiscard]] static bool NormalizeAndValidateName(std::wstring_view requested,
                                                       std::wstring& normalized,
                                                       std::wstring& error);
    [[nodiscard]] static bool CountNameCharacters(std::wstring_view input,
                                                  std::size_t& count) noexcept;
    [[nodiscard]] static std::wstring SuggestedFileName(std::wstring_view profile_name,
                                                        std::wstring_view id);
    [[nodiscard]] static core::RunSettings NormalizeSettingsForProfile(
        core::RunSettings settings) noexcept;

private:
    [[nodiscard]] bool ReadMetadata(const std::wstring& path,
                                    ProfileInfo& profile,
                                    std::wstring& error) const;
    [[nodiscard]] bool CollectValidatedProfiles(
        std::vector<ProfileInfo>& profiles,
        std::vector<std::wstring>* warnings,
        std::wstring& error) const;
    [[nodiscard]] bool FindById(const std::wstring& id,
                                ProfileInfo& profile,
                                std::wstring& error) const;
    [[nodiscard]] bool NameAvailable(std::wstring_view name,
                                     std::wstring_view except_id,
                                     std::wstring& error) const;
    [[nodiscard]] bool IdAvailable(std::wstring_view id,
                                   std::wstring& error) const;
    [[nodiscard]] std::wstring GenerateId(std::wstring& error) const;
    [[nodiscard]] bool WriteProfile(const ProfileInfo& profile,
                                    const core::RunSettings& settings,
                                    const std::wstring& destination_path,
                                    std::wstring& error) const;

    SettingsStore settings_store_;
    std::wstring directory_path_;
};

} // namespace vectorclick::win

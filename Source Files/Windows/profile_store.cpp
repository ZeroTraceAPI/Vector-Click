#include "Windows/profile_store.h"

#include "Core/json_object.h"
#include "Windows/admin_utils.h"
#include "Windows/win32_raii.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cwctype>
#include <limits>
#include <string_view>

namespace vectorclick::win {
namespace {

constexpr std::uintmax_t MaxProfileBytes = 64 * 1024;
constexpr std::string_view ProfileFormatToken = "Vector Click Profile";
constexpr std::uint32_t ProfileFormatVersion = 1;
constexpr std::size_t MaximumInternalFileBaseCharacters = 120;
constexpr std::size_t MaximumLocalProfiles = 256;
constexpr std::size_t MaximumProfileFilesInspected = 512;

bool IsWhitespace(const wchar_t value) noexcept {
    return std::iswspace(static_cast<wint_t>(value)) != 0;
}

bool IsControl(const wchar_t value) noexcept {
    return value < 0x20 || value == 0x7F;
}

bool IsHighSurrogate(const wchar_t value) noexcept {
    return value >= 0xD800 && value <= 0xDBFF;
}

bool IsLowSurrogate(const wchar_t value) noexcept {
    return value >= 0xDC00 && value <= 0xDFFF;
}

bool CountUnicodeScalars(const std::wstring_view text,
                         std::size_t& count) noexcept {
    count = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const wchar_t value = text[index];
        if (IsHighSurrogate(value)) {
            if (index + 1 >= text.size() || !IsLowSurrogate(text[index + 1])) {
                return false;
            }
            ++index;
        } else if (IsLowSurrogate(value)) {
            return false;
        }
        ++count;
    }
    return true;
}

std::wstring Trimmed(std::wstring_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && IsWhitespace(value[begin])) {
        ++begin;
    }
    while (end > begin && IsWhitespace(value[end - 1])) {
        --end;
    }
    return std::wstring(value.substr(begin, end - begin));
}

bool EqualInsensitive(const std::wstring_view left,
                      const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) ==
           CSTR_EQUAL;
}

std::optional<std::string> WideToUtf8(const std::wstring_view input) {
    if (input.empty()) {
        return std::string{};
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int required = WideCharToMultiByte(CP_UTF8,
                                             WC_ERR_INVALID_CHARS,
                                             input.data(),
                                             static_cast<int>(input.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            input.data(),
                            static_cast<int>(input.size()),
                            output.data(),
                            required,
                            nullptr,
                            nullptr) != required) {
        return std::nullopt;
    }
    return output;
}

std::optional<std::wstring> Utf8ToWide(const std::string_view input) {
    if (input.empty()) {
        return std::wstring{};
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int required = MultiByteToWideChar(CP_UTF8,
                                             MB_ERR_INVALID_CHARS,
                                             input.data(),
                                             static_cast<int>(input.size()),
                                             nullptr,
                                             0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            input.data(),
                            static_cast<int>(input.size()),
                            output.data(),
                            required) != required) {
        return std::nullopt;
    }
    return output;
}

void AppendJsonEscaped(std::string& output, const std::string_view value) {
    constexpr char Hex[] = "0123456789abcdef";
    for (const char raw : value) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        switch (ch) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (ch < 0x20) {
                output += "\\u00";
                output.push_back(Hex[(ch >> 4U) & 0x0FU]);
                output.push_back(Hex[ch & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
}

bool ReadAll(HANDLE file, std::string& data) noexcept {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const DWORD request = static_cast<DWORD>(data.size() - offset);
        DWORD read = 0;
        if (ReadFile(file, data.data() + offset, request, &read, nullptr) == FALSE ||
            read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

std::optional<std::string> ReadProfileText(const std::wstring& path,
                                           std::wstring& error) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        error = L"The profile file could not be opened.";
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > MaxProfileBytes) {
        error = L"The profile file is unavailable or exceeds the 64 KB safety limit.";
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    if (!text.empty() && !ReadAll(file.get(), text)) {
        error = L"Reading the profile file failed.";
        return std::nullopt;
    }
    return text;
}

bool IsReservedFileCharacter(const wchar_t ch) noexcept {
    switch (ch) {
    case L'<': case L'>': case L':': case L'"': case L'/':
    case L'\\': case L'|': case L'?': case L'*':
        return true;
    default:
        return IsControl(ch);
    }
}

std::wstring SanitizeFileBase(const std::wstring_view display_name) {
    std::wstring result;
    result.reserve(std::min(display_name.size(), MaximumInternalFileBaseCharacters));
    bool pending_space = false;
    for (const wchar_t ch : display_name) {
        if (IsReservedFileCharacter(ch) || IsWhitespace(ch)) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space && result.size() < MaximumInternalFileBaseCharacters) {
            result.push_back(L' ');
        }
        pending_space = false;
        if (result.size() >= MaximumInternalFileBaseCharacters) {
            break;
        }
        result.push_back(ch);
    }
    while (!result.empty() && (result.back() == L' ' || result.back() == L'.')) {
        result.pop_back();
    }
    while (!result.empty() && (result.front() == L' ' || result.front() == L'.')) {
        result.erase(result.begin());
    }
    if (result.empty()) {
        result = L"Profile";
    }

    // Device-name reservations apply even when an extension is present. The
    // stable [ID] suffix already makes the final component non-reserved, but
    // prefixing the rare exact device base keeps manually copied / edited names
    // unsurprising too.
    constexpr std::array<std::wstring_view, 4> reserved = {
        L"CON", L"PRN", L"AUX", L"NUL"
    };
    bool reserved_name = std::any_of(reserved.begin(), reserved.end(),
        [&](const std::wstring_view candidate) { return EqualInsensitive(result, candidate); });
    if (!reserved_name && result.size() == 4) {
        const std::wstring_view prefix(result.data(), 3);
        const wchar_t digit = result[3];
        reserved_name = (EqualInsensitive(prefix, L"COM") ||
                         EqualInsensitive(prefix, L"LPT")) &&
                        digit >= L'1' && digit <= L'9';
    }
    if (reserved_name) {
        result.insert(0, L"Profile ");
    }
    return result;
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& leaf) {
    std::wstring path = directory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path += leaf;
    return path;
}

std::wstring Hex8(const std::uint32_t value) {
    constexpr wchar_t Digits[] = L"0123456789ABCDEF";
    std::wstring result(8, L'0');
    for (unsigned int i = 0; i < 8; ++i) {
        const unsigned int shift = (7U - i) * 4U;
        result[i] = Digits[(value >> shift) & 0x0FU];
    }
    return result;
}

std::uint32_t Mix32(std::uint64_t value) noexcept {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return static_cast<std::uint32_t>(value ^ (value >> 32U));
}

} // namespace

ProfileStore::ProfileStore(std::wstring product_name)
    : settings_store_(product_name) {
    std::wstring directory = ExecutableDirectory();
    if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/') {
        directory.push_back(L'\\');
    }
    directory_path_ = std::move(directory);
    directory_path_ += L"Vector Click Profiles";
}

bool ProfileStore::DirectoryExists(bool& exists, std::wstring& error) const {
    exists = false;
    const DWORD attributes = GetFileAttributesW(directory_path_.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            exists = true;
            return true;
        }
        error = L"The profile storage path exists but is not a folder.";
        return false;
    }

    const DWORD attribute_error = GetLastError();
    if (attribute_error == ERROR_FILE_NOT_FOUND ||
        attribute_error == ERROR_PATH_NOT_FOUND) {
        return true;
    }
    error = L"The local Profiles folder could not be inspected.";
    return false;
}

bool ProfileStore::EnsureDirectory(std::wstring& error) const {
    bool exists = false;
    if (!DirectoryExists(exists, error)) {
        return false;
    }
    if (exists) {
        return true;
    }
    if (CreateDirectoryW(directory_path_.c_str(), nullptr) != FALSE) {
        return true;
    }

    const DWORD create_error = GetLastError();
    if (create_error == ERROR_ALREADY_EXISTS) {
        // Another actor may have created the path after the existence check.
        // Re-validate it through the checked path rather than treating raw
        // INVALID_FILE_ATTRIBUTES bits as a directory.
        error.clear();
        if (!DirectoryExists(exists, error)) {
            return false;
        }
        if (exists) {
            return true;
        }
    }

    error = L"Vector Click could not create the local Profiles folder beside the application.";
    return false;
}

bool ProfileStore::CountNameCharacters(const std::wstring_view input,
                                       std::size_t& count) noexcept {
    return CountUnicodeScalars(input, count);
}

bool ProfileStore::NormalizeAndValidateName(const std::wstring_view requested,
                                            std::wstring& normalized,
                                            std::wstring& error) {
    normalized = Trimmed(requested);
    if (normalized.empty()) {
        error = L"Profile names cannot be empty.";
        return false;
    }
    for (const wchar_t ch : normalized) {
        if (IsControl(ch)) {
            error = L"Profile names cannot contain control characters.";
            return false;
        }
    }
    std::size_t scalar_count = 0;
    if (!CountUnicodeScalars(normalized, scalar_count)) {
        error = L"The profile name contains invalid Unicode text.";
        return false;
    }
    if (scalar_count > MaximumProfileNameCharacters) {
        error = L"Profile names are limited to 40 characters.";
        return false;
    }
    return true;
}

std::wstring ProfileStore::SuggestedFileName(const std::wstring_view profile_name,
                                              const std::wstring_view id) {
    std::wstring name = SanitizeFileBase(profile_name);
    name += L" [";
    name.append(id);
    name += L"]";
    name += ProfileFileExtension;
    return name;
}

core::RunSettings ProfileStore::NormalizeSettingsForProfile(
    core::RunSettings settings) noexcept {
    settings.remember_settings = false;
    settings.timing_worker_priority_mode = core::TimingWorkerPriorityMode::SystemDefault;
    settings.hotkey_control_priority_mode = core::HotkeyControlPriorityMode::SystemDefault;
    settings.timing_worker_qos_mode = core::TimingWorkerQosMode::SystemManaged;
    return settings;
}

bool ProfileStore::ReadMetadata(const std::wstring& path,
                                ProfileInfo& profile,
                                std::wstring& error) const {
    const auto text = ReadProfileText(path, error);
    if (!text) {
        return false;
    }
    core::JsonObject json;
    if (!json.Parse(*text)) {
        error = L"The profile file is not a valid structural JSON object.";
        return false;
    }
    const auto* format = json.Find("profile_format");
    const auto* version = json.Find("profile_format_version");
    const auto* id = json.Find("profile_id");
    const auto* name = json.Find("profile_name");
    if (format == nullptr || format->kind != core::JsonValueKind::String ||
        format->text != ProfileFormatToken ||
        version == nullptr || version->kind != core::JsonValueKind::Number ||
        id == nullptr || id->kind != core::JsonValueKind::String ||
        name == nullptr || name->kind != core::JsonValueKind::String) {
        error = L"The file does not contain valid Vector Click profile metadata.";
        return false;
    }
    std::uint32_t parsed_version = 0;
    const auto parsed = std::from_chars(version->text.data(),
                                        version->text.data() + version->text.size(),
                                        parsed_version);
    if (parsed.ec != std::errc{} || parsed.ptr != version->text.data() + version->text.size() ||
        parsed_version != ProfileFormatVersion) {
        error = L"The profile file uses an unsupported profile format version.";
        return false;
    }
    const auto wide_id = Utf8ToWide(id->text);
    const auto wide_name = Utf8ToWide(name->text);
    if (!wide_id || !wide_name || wide_id->size() != 8 ||
        !std::all_of(wide_id->begin(), wide_id->end(), [](const wchar_t ch) {
            return (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F');
        })) {
        error = L"The profile file contains invalid profile identity metadata.";
        return false;
    }
    std::wstring normalized_name;
    if (!NormalizeAndValidateName(*wide_name, normalized_name, error) ||
        normalized_name != *wide_name) {
        if (error.empty()) {
            error = L"The profile file contains a non-canonical profile name.";
        }
        return false;
    }
    profile.id = *wide_id;
    profile.name = std::move(normalized_name);
    profile.path = path;
    return true;
}

bool ProfileStore::CollectValidatedProfiles(
    std::vector<ProfileInfo>& profiles,
    std::vector<std::wstring>* warnings,
    std::wstring& error) const {
    profiles.clear();
    if (warnings != nullptr) {
        warnings->clear();
    }

    const DWORD attributes = GetFileAttributesW(directory_path_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        error = L"The local Profiles folder could not be inspected.";
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        error = L"The profile storage path exists but is not a folder.";
        return false;
    }

    const std::wstring pattern = JoinPath(directory_path_, L"*" + std::wstring(ProfileFileExtension));
    WIN32_FIND_DATAW data{};
    UniqueFindHandle find(FindFirstFileW(pattern.c_str(), &data));
    if (!find) {
        const DWORD find_error = GetLastError();
        if (find_error == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        error = L"Vector Click could not enumerate the local profile files.";
        return false;
    }

    profiles.reserve(std::min<std::size_t>(MaximumProfileFilesInspected, 64U));
    std::size_t inspected = 0;
    bool inspection_limit_reached = false;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (inspected >= MaximumProfileFilesInspected) {
            inspection_limit_reached = true;
            break;
        }
        ++inspected;

        const std::wstring path = JoinPath(directory_path_, data.cFileName);
        ProfileInfo info;
        std::wstring profile_error;
        if (!ReadMetadata(path, info, profile_error)) {
            if (warnings != nullptr) {
                warnings->push_back(std::wstring(data.cFileName) + L": " + profile_error);
            }
            continue;
        }

        core::RunSettings validated = core::DefaultRunSettings();
        if (!settings_store_.LoadFromPath(path, validated, profile_error)) {
            if (warnings != nullptr) {
                warnings->push_back(std::wstring(data.cFileName) + L": " + profile_error);
            }
            continue;
        }
        profiles.push_back(std::move(info));
    } while (FindNextFileW(find.get(), &data) != FALSE);

    if (!inspection_limit_reached && GetLastError() != ERROR_NO_MORE_FILES) {
        error = L"Vector Click could not finish enumerating the local profile files.";
        return false;
    }
    if (inspection_limit_reached && warnings != nullptr) {
        warnings->push_back(
            L"The Profiles folder contains more than 512 profile files. Additional files were not inspected.");
    }
    return true;
}

bool ProfileStore::List(std::vector<ProfileInfo>& profiles,
                        std::vector<std::wstring>* warnings,
                        std::wstring& error) const {
    std::vector<ProfileInfo> candidates;
    if (!CollectValidatedProfiles(candidates, warnings, error)) {
        profiles.clear();
        return false;
    }

    std::sort(candidates.begin(), candidates.end(), [](const ProfileInfo& left,
                                                        const ProfileInfo& right) {
        const int name_order = CompareStringOrdinal(
            left.name.c_str(), -1, right.name.c_str(), -1, TRUE);
        if (name_order == CSTR_LESS_THAN) return true;
        if (name_order == CSTR_GREATER_THAN) return false;
        return CompareStringOrdinal(
                   left.path.c_str(), -1, right.path.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });

    // If two independently stored files claim the same stable ID or the same
    // case-insensitive display name, neither is authoritative. Ignore every
    // conflicting copy instead of choosing based on filesystem enumeration.
    std::vector<bool> ambiguous(candidates.size(), false);
    for (std::size_t left = 0; left < candidates.size(); ++left) {
        for (std::size_t right = left + 1U; right < candidates.size(); ++right) {
            if (EqualInsensitive(candidates[left].id, candidates[right].id) ||
                EqualInsensitive(candidates[left].name, candidates[right].name)) {
                ambiguous[left] = true;
                ambiguous[right] = true;
            }
        }
    }

    profiles.clear();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (ambiguous[index]) {
            if (warnings != nullptr) {
                warnings->push_back(candidates[index].path +
                                    L": duplicate profile identity or name was ignored.");
            }
            continue;
        }
        if (profiles.size() >= MaximumLocalProfiles) {
            if (warnings != nullptr) {
                warnings->push_back(
                    L"Only the first 256 valid local profiles are shown. Additional valid profiles were ignored.");
            }
            break;
        }
        profiles.push_back(std::move(candidates[index]));
    }
    return true;
}

bool ProfileStore::NameAvailable(const std::wstring_view name,
                                 const std::wstring_view except_id,
                                 std::wstring& error) const {
    std::vector<ProfileInfo> profiles;
    if (!CollectValidatedProfiles(profiles, nullptr, error)) {
        return false;
    }
    for (const auto& profile : profiles) {
        if (!except_id.empty() && EqualInsensitive(profile.id, except_id)) {
            continue;
        }
        if (EqualInsensitive(profile.name, name)) {
            error = L"A local profile with that name already exists.";
            return false;
        }
    }
    return true;
}

bool ProfileStore::IdAvailable(const std::wstring_view id,
                               std::wstring& error) const {
    std::vector<ProfileInfo> profiles;
    if (!CollectValidatedProfiles(profiles, nullptr, error)) {
        return false;
    }
    for (const auto& profile : profiles) {
        if (EqualInsensitive(profile.id, id)) {
            return false;
        }
    }
    return true;
}

std::wstring ProfileStore::GenerateId(std::wstring& error) const {
    static std::atomic<std::uint64_t> counter{1};
    for (unsigned int attempt = 0; attempt < 64; ++attempt) {
        FILETIME file_time{};
        GetSystemTimeAsFileTime(&file_time);
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const std::uint64_t time = (static_cast<std::uint64_t>(file_time.dwHighDateTime) << 32U) |
                                   file_time.dwLowDateTime;
        const std::uint64_t seed = time ^ static_cast<std::uint64_t>(qpc.QuadPart) ^
            (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U) ^
            static_cast<std::uint64_t>(GetCurrentThreadId()) ^
            counter.fetch_add(1U, std::memory_order_relaxed) ^ attempt;
        const std::wstring id = Hex8(Mix32(seed));
        std::wstring probe_error;
        if (IdAvailable(id, probe_error)) {
            return id;
        }
        if (!probe_error.empty()) {
            error = std::move(probe_error);
            return {};
        }
    }
    error = L"Vector Click could not generate a unique local profile identity.";
    return {};
}

bool ProfileStore::WriteProfile(const ProfileInfo& profile,
                                const core::RunSettings& settings,
                                const std::wstring& destination_path,
                                std::wstring& error) const {
    const auto utf8_id = WideToUtf8(profile.id);
    const auto utf8_name = WideToUtf8(profile.name);
    if (!utf8_id || !utf8_name) {
        error = L"The profile name could not be encoded safely.";
        return false;
    }

    std::string metadata;
    metadata.reserve(256 + utf8_name->size());
    metadata += "  \"profile_format\": \"Vector Click Profile\",\n";
    metadata += "  \"profile_format_version\": 1,\n";
    metadata += "  \"profile_id\": \"";
    AppendJsonEscaped(metadata, *utf8_id);
    metadata += "\",\n  \"profile_name\": \"";
    AppendJsonEscaped(metadata, *utf8_name);
    metadata += "\",\n";

    core::RunSettings portable = NormalizeSettingsForProfile(settings);
    if (!settings_store_.SaveToPath(destination_path, portable, error, metadata)) {
        if (error.empty()) {
            error = L"The profile file could not be written safely.";
        } else {
            error = L"The profile file could not be written safely.\n\n" + error;
        }
        return false;
    }
    return true;
}

bool ProfileStore::Create(const std::wstring_view name,
                          const core::RunSettings& settings,
                          ProfileInfo& profile,
                          std::wstring& error) const {
    std::wstring normalized;
    if (!NormalizeAndValidateName(name, normalized, error) ||
        !NameAvailable(normalized, {}, error) || !EnsureDirectory(error)) {
        return false;
    }
    const std::wstring id = GenerateId(error);
    if (id.empty()) {
        return false;
    }
    profile = {id, normalized, JoinPath(directory_path_, SuggestedFileName(normalized, id))};
    if (!WriteProfile(profile, settings, profile.path, error)) {
        profile = {};
        return false;
    }
    return true;
}

bool ProfileStore::FindById(const std::wstring& id,
                            ProfileInfo& profile,
                            std::wstring& error) const {
    std::vector<ProfileInfo> profiles;
    if (!List(profiles, nullptr, error)) {
        return false;
    }
    for (const auto& candidate : profiles) {
        if (EqualInsensitive(candidate.id, id)) {
            profile = candidate;
            return true;
        }
    }
    error = L"The selected local profile no longer exists.";
    return false;
}

bool ProfileStore::Load(const std::wstring& id,
                        core::RunSettings& settings,
                        ProfileInfo& profile,
                        std::wstring& error) const {
    if (!FindById(id, profile, error)) {
        return false;
    }
    core::RunSettings loaded = core::DefaultRunSettings();
    if (!settings_store_.LoadFromPath(profile.path, loaded, error)) {
        if (error.empty()) {
            error = L"The selected local profile could not be loaded.";
        }
        return false;
    }
    loaded = NormalizeSettingsForProfile(loaded);
    settings = loaded;
    return true;
}

bool ProfileStore::Save(const ProfileInfo& requested_profile,
                        const core::RunSettings& settings,
                        std::wstring& error) const {
    ProfileInfo profile;
    if (!FindById(requested_profile.id, profile, error)) {
        return false;
    }
    return WriteProfile(profile, settings, profile.path, error);
}

bool ProfileStore::Rename(const ProfileInfo& requested_profile,
                          const std::wstring_view new_name,
                          ProfileInfo& renamed,
                          std::wstring& error) const {
    ProfileInfo profile;
    if (!FindById(requested_profile.id, profile, error)) {
        return false;
    }
    std::wstring normalized;
    if (!NormalizeAndValidateName(new_name, normalized, error) ||
        !NameAvailable(normalized, profile.id, error)) {
        return false;
    }
    if (profile.name == normalized) {
        renamed = profile;
        return true;
    }

    core::RunSettings settings;
    ProfileInfo loaded;
    if (!Load(profile.id, settings, loaded, error)) {
        return false;
    }
    ProfileInfo updated{profile.id, normalized, profile.path};
    if (!WriteProfile(updated, settings, profile.path, error)) {
        return false;
    }

    const std::wstring desired_path = JoinPath(directory_path_, SuggestedFileName(normalized, profile.id));
    if (!EqualInsensitive(desired_path, profile.path) &&
        MoveFileExW(profile.path.c_str(), desired_path.c_str(),
                    MOVEFILE_WRITE_THROUGH) != FALSE) {
        updated.path = desired_path;
    }
    // A filename refresh is cosmetic. The exact display name is authoritative
    // metadata, so a blocked rename does not roll back an otherwise successful
    // transactional profile-content update.
    renamed = std::move(updated);
    return true;
}

bool ProfileStore::Duplicate(const ProfileInfo& requested_profile,
                             const std::wstring_view new_name,
                             ProfileInfo& duplicated,
                             std::wstring& error) const {
    core::RunSettings settings;
    ProfileInfo profile;
    if (!Load(requested_profile.id, settings, profile, error)) {
        return false;
    }
    return Create(new_name, settings, duplicated, error);
}

bool ProfileStore::Remove(const ProfileInfo& requested_profile,
                          std::wstring& error) const {
    ProfileInfo profile;
    if (!FindById(requested_profile.id, profile, error)) {
        return false;
    }
    if (DeleteFileW(profile.path.c_str()) == FALSE) {
        error = L"The selected local profile could not be deleted.";
        return false;
    }

    // The Profiles folder is owned by Vector Click. Remove it when the profile
    // deletion leaves it completely empty. RemoveDirectoryW refuses to remove
    // a non-empty directory, so unrelated files are never deleted here.
    (void)RemoveDirectoryW(directory_path_.c_str());
    return true;
}

bool ProfileStore::ImportFromPath(const std::wstring& source_path,
                                  ProfileInfo& imported,
                                  std::wstring& error) const {
    ProfileInfo source;
    if (!ReadMetadata(source_path, source, error)) {
        return false;
    }
    core::RunSettings settings = core::DefaultRunSettings();
    if (!settings_store_.LoadFromPath(source_path, settings, error)) {
        return false;
    }
    if (!NameAvailable(source.name, {}, error)) {
        return false;
    }
    std::wstring id_error;
    std::wstring id = source.id;
    if (!IdAvailable(id, id_error)) {
        if (!id_error.empty()) {
            error = std::move(id_error);
            return false;
        }
        id = GenerateId(error);
        if (id.empty()) {
            return false;
        }
    }
    if (!EnsureDirectory(error)) {
        return false;
    }
    imported = {id, source.name, JoinPath(directory_path_, SuggestedFileName(source.name, id))};
    return WriteProfile(imported, settings, imported.path, error);
}

bool ProfileStore::ExportToPath(const ProfileInfo& requested_profile,
                                const std::wstring& destination_path,
                                std::wstring& error) const {
    ProfileInfo profile;
    if (!FindById(requested_profile.id, profile, error)) {
        return false;
    }
    if (CompareStringOrdinal(profile.path.c_str(), -1,
                             destination_path.c_str(), -1, TRUE) == CSTR_EQUAL) {
        return true;
    }
    if (CopyFileW(profile.path.c_str(), destination_path.c_str(), FALSE) == FALSE) {
        error = L"The selected profile could not be exported to that location.";
        return false;
    }
    return true;
}

} // namespace vectorclick::win

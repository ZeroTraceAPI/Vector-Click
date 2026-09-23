#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vectorclick::core {

enum class JsonValueKind : std::uint8_t {
    String,
    Number,
    Boolean,
    Null,
    Object,
    Array,
};

struct JsonObjectMember {
    std::string key;
    JsonValueKind kind{JsonValueKind::Null};
    std::string text;
};

class JsonObject {
public:
    [[nodiscard]] bool Parse(std::string_view text);
    [[nodiscard]] const JsonObjectMember* Find(std::string_view key) const noexcept;

private:
    std::vector<JsonObjectMember> members_;
};

} // namespace vectorclick::core

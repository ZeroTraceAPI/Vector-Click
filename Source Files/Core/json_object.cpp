#include "Core/json_object.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace vectorclick::core {
namespace {

// Settings and profile JSON only need top-level member values, but nested
// objects and arrays are still parsed structurally so malformed input cannot be
// accepted merely because an unknown value is ignored. Bound nesting to keep
// hostile or corrupted files from driving unbounded recursion.
constexpr std::size_t MaximumJsonDepth = 64;

bool AppendUtf8(std::string& output, const std::uint32_t code_point) {
    if (code_point <= 0x7FU) {
        output.push_back(static_cast<char>(code_point));
        return true;
    }
    if (code_point <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        return true;
    }
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
        return false;
    }
    if (code_point <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        return true;
    }
    if (code_point <= 0x10FFFFU) {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        return true;
    }
    return false;
}

int HexDigit(const char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return 10 + (character - 'a');
    }
    if (character >= 'A' && character <= 'F') {
        return 10 + (character - 'A');
    }
    return -1;
}

class JsonParser {
public:
    explicit JsonParser(const std::string_view text) : text_(text) {}

    bool Parse(std::vector<JsonObjectMember>& members) {
        members.clear();
        if (text_.size() >= 3 &&
            static_cast<unsigned char>(text_[0]) == 0xEFU &&
            static_cast<unsigned char>(text_[1]) == 0xBBU &&
            static_cast<unsigned char>(text_[2]) == 0xBFU) {
            position_ = 3;
        }
        SkipWhitespace();
        if (!ParseObject(&members, 0)) {
            members.clear();
            return false;
        }
        SkipWhitespace();
        if (position_ != text_.size()) {
            members.clear();
            return false;
        }
        return true;
    }

private:
    bool ParseObject(std::vector<JsonObjectMember>* members,
                     const std::size_t depth) {
        if (depth >= MaximumJsonDepth || !Consume('{')) {
            return false;
        }
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }

        while (position_ < text_.size()) {
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            if (members != nullptr) {
                for (const auto& member : *members) {
                    if (member.key == key) {
                        return false;
                    }
                }
            }

            SkipWhitespace();
            if (!Consume(':')) {
                return false;
            }
            SkipWhitespace();

            JsonObjectMember member;
            if (members != nullptr) {
                member.key = std::move(key);
                if (!ParseValue(&member, depth + 1U)) {
                    return false;
                }
                members->push_back(std::move(member));
            } else if (!ParseValue(nullptr, depth + 1U)) {
                return false;
            }

            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
        return false;
    }

    bool ParseArray(const std::size_t depth) {
        if (depth >= MaximumJsonDepth || !Consume('[')) {
            return false;
        }
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }

        while (position_ < text_.size()) {
            if (!ParseValue(nullptr, depth + 1U)) {
                return false;
            }
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
        return false;
    }

    bool ParseValue(JsonObjectMember* member, const std::size_t depth) {
        if (position_ >= text_.size() || depth > MaximumJsonDepth) {
            return false;
        }

        if (text_[position_] == '"') {
            std::string decoded;
            if (!ParseString(decoded)) {
                return false;
            }
            if (member != nullptr) {
                member->kind = JsonValueKind::String;
                member->text = std::move(decoded);
            }
            return true;
        }

        if (text_[position_] == '{') {
            if (!ParseObject(nullptr, depth)) {
                return false;
            }
            if (member != nullptr) {
                member->kind = JsonValueKind::Object;
                member->text.clear();
            }
            return true;
        }

        if (text_[position_] == '[') {
            if (!ParseArray(depth)) {
                return false;
            }
            if (member != nullptr) {
                member->kind = JsonValueKind::Array;
                member->text.clear();
            }
            return true;
        }

        if (MatchLiteral("true")) {
            if (member != nullptr) {
                member->kind = JsonValueKind::Boolean;
                member->text = "true";
            }
            return true;
        }
        if (MatchLiteral("false")) {
            if (member != nullptr) {
                member->kind = JsonValueKind::Boolean;
                member->text = "false";
            }
            return true;
        }
        if (MatchLiteral("null")) {
            if (member != nullptr) {
                member->kind = JsonValueKind::Null;
                member->text.clear();
            }
            return true;
        }

        const std::size_t start = position_;
        if (!ParseNumber()) {
            return false;
        }
        if (member != nullptr) {
            member->kind = JsonValueKind::Number;
            member->text.assign(text_.substr(start, position_ - start));
        }
        return true;
    }

    // Decode JSON escapes while also validating any UTF-8 bytes that appear
    // directly in the string. Invalid scalar values and malformed surrogate
    // pairs fail the complete object parse rather than being repaired silently.
    bool ParseString(std::string& output) {
        output.clear();
        if (!Consume('"')) {
            return false;
        }

        while (position_ < text_.size()) {
            const unsigned char byte =
                static_cast<unsigned char>(text_[position_++]);
            if (byte == static_cast<unsigned char>('"')) {
                return true;
            }
            if (byte < 0x20U) {
                return false;
            }
            if (byte == static_cast<unsigned char>('\\')) {
                if (position_ >= text_.size()) {
                    return false;
                }
                const char escape = text_[position_++];
                switch (escape) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t code_point = 0;
                    if (!ParseHexQuad(code_point)) {
                        return false;
                    }
                    if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                        if (position_ + 2U > text_.size() ||
                            text_[position_] != '\\' ||
                            text_[position_ + 1U] != 'u') {
                            return false;
                        }
                        position_ += 2U;
                        std::uint32_t low = 0;
                        if (!ParseHexQuad(low) ||
                            low < 0xDC00U || low > 0xDFFFU) {
                            return false;
                        }
                        code_point = 0x10000U +
                            ((code_point - 0xD800U) << 10U) +
                            (low - 0xDC00U);
                    } else if (code_point >= 0xDC00U &&
                               code_point <= 0xDFFFU) {
                        return false;
                    }
                    if (!AppendUtf8(output, code_point)) {
                        return false;
                    }
                    break;
                }
                default:
                    return false;
                }
                continue;
            }

            if (byte < 0x80U) {
                output.push_back(static_cast<char>(byte));
                continue;
            }

            const std::size_t sequence_start = position_ - 1U;
            std::size_t sequence_length = 0;
            std::uint32_t code_point = 0;
            if ((byte & 0xE0U) == 0xC0U) {
                sequence_length = 2;
                code_point = byte & 0x1FU;
            } else if ((byte & 0xF0U) == 0xE0U) {
                sequence_length = 3;
                code_point = byte & 0x0FU;
            } else if ((byte & 0xF8U) == 0xF0U) {
                sequence_length = 4;
                code_point = byte & 0x07U;
            } else {
                return false;
            }
            if (sequence_start + sequence_length > text_.size()) {
                return false;
            }
            for (std::size_t index = 1; index < sequence_length; ++index) {
                const unsigned char continuation =
                    static_cast<unsigned char>(text_[sequence_start + index]);
                if ((continuation & 0xC0U) != 0x80U) {
                    return false;
                }
                code_point = (code_point << 6U) | (continuation & 0x3FU);
            }
            const std::uint32_t minimum = sequence_length == 2 ? 0x80U
                : sequence_length == 3 ? 0x800U : 0x10000U;
            if (code_point < minimum || code_point > 0x10FFFFU ||
                (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
                return false;
            }
            output.append(text_.substr(sequence_start, sequence_length));
            position_ = sequence_start + sequence_length;
        }
        return false;
    }

    bool ParseHexQuad(std::uint32_t& value) {
        if (position_ + 4U > text_.size()) {
            return false;
        }
        value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const int digit = HexDigit(text_[position_++]);
            if (digit < 0) {
                return false;
            }
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        return true;
    }

    // Validate the JSON number grammar without converting the value here.
    // Callers retain the original token text and apply their own bounded type
    // conversion when they recognize a particular settings key.
    bool ParseNumber() {
        const std::size_t start = position_;
        if (Consume('-') && position_ >= text_.size()) {
            position_ = start;
            return false;
        }

        if (position_ >= text_.size()) {
            position_ = start;
            return false;
        }
        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() &&
                text_[position_] >= '0' && text_[position_] <= '9') {
                position_ = start;
                return false;
            }
        } else if (text_[position_] >= '1' && text_[position_] <= '9') {
            do {
                ++position_;
            } while (position_ < text_.size() &&
                     text_[position_] >= '0' && text_[position_] <= '9');
        } else {
            position_ = start;
            return false;
        }

        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const std::size_t fraction_start = position_;
            while (position_ < text_.size() &&
                   text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            if (position_ == fraction_start) {
                position_ = start;
                return false;
            }
        }

        if (position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() &&
                (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_start = position_;
            while (position_ < text_.size() &&
                   text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponent_start) {
                position_ = start;
                return false;
            }
        }
        return true;
    }

    bool MatchLiteral(const std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    bool Consume(const char expected) noexcept {
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void SkipWhitespace() noexcept {
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character != ' ' && character != '\t' &&
                character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    std::string_view text_;
    std::size_t position_{};
};

} // namespace

bool JsonObject::Parse(const std::string_view text) {
    JsonParser parser(text);
    return parser.Parse(members_);
}

const JsonObjectMember* JsonObject::Find(const std::string_view key) const noexcept {
    for (const auto& member : members_) {
        if (member.key == key) {
            return &member;
        }
    }
    return nullptr;
}

} // namespace vectorclick::core

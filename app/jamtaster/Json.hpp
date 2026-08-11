#pragma once

#include <cmath>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace jamtaster::native {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    Json(int value) : value_(static_cast<double>(value)) {}
    Json(std::size_t value) : value_(static_cast<double>(value)) {}
    Json(double value) : value_(value) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    static Json array() { return Array{}; }
    static Json object() { return Object{}; }

    static Json parse(std::string_view text)
    {
        Parser parser(text);
        Json result = parser.value();
        parser.whitespace();
        if (!parser.finished()) throw std::runtime_error("unexpected trailing JSON data");
        return result;
    }

    Json& operator[](const std::string& key)
    {
        if (!std::holds_alternative<Object>(value_)) value_ = Object{};
        return std::get<Object>(value_)[key];
    }

    void push(Json value)
    {
        if (!std::holds_alternative<Array>(value_)) value_ = Array{};
        std::get<Array>(value_).push_back(std::move(value));
    }

    [[nodiscard]] bool isObject() const noexcept { return std::holds_alternative<Object>(value_); }
    [[nodiscard]] bool isArray() const noexcept { return std::holds_alternative<Array>(value_); }
    [[nodiscard]] bool isString() const noexcept { return std::holds_alternative<std::string>(value_); }

    [[nodiscard]] bool contains(const std::string& key) const
    {
        const auto* values = std::get_if<Object>(&value_);
        return values && values->contains(key);
    }

    [[nodiscard]] const Json& get(const std::string& key) const
    {
        static const Json empty;
        const auto* values = std::get_if<Object>(&value_);
        if (!values) return empty;
        const auto found = values->find(key);
        return found == values->end() ? empty : found->second;
    }

    [[nodiscard]] std::string stringValue(std::string fallback = {}) const
    {
        const auto* value = std::get_if<std::string>(&value_);
        return value ? *value : std::move(fallback);
    }

    [[nodiscard]] double numberValue(double fallback = 0.0) const noexcept
    {
        const auto* value = std::get_if<double>(&value_);
        return value ? *value : fallback;
    }

    [[nodiscard]] int integerValue(int fallback = 0) const noexcept
    {
        const auto* value = std::get_if<double>(&value_);
        return value ? static_cast<int>(*value) : fallback;
    }

    [[nodiscard]] bool boolValue(bool fallback = false) const noexcept
    {
        const auto* value = std::get_if<bool>(&value_);
        return value ? *value : fallback;
    }

    [[nodiscard]] std::string dump(int indent = 2) const
    {
        std::ostringstream output;
        write(output, *this, indent, 0);
        return output.str();
    }

private:
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Value value_ = nullptr;

    class Parser {
    public:
        explicit Parser(std::string_view text) : text_(text) {}

        Json value()
        {
            whitespace();
            if (finished()) fail("expected a JSON value");
            switch (text_[position_]) {
            case '{': return objectValue();
            case '[': return arrayValue();
            case '"': return Json(stringValue());
            case 't': literal("true"); return Json(true);
            case 'f': literal("false"); return Json(false);
            case 'n': literal("null"); return Json();
            default: return Json(numberValue());
            }
        }

        void whitespace()
        {
            while (!finished() && std::isspace(static_cast<unsigned char>(text_[position_]))) {
                ++position_;
            }
        }

        [[nodiscard]] bool finished() const noexcept { return position_ == text_.size(); }

    private:
        Json objectValue()
        {
            ++position_;
            Json result = Json::object();
            whitespace();
            if (consume('}')) return result;
            while (true) {
                whitespace();
                if (finished() || text_[position_] != '"') fail("expected an object key");
                const std::string key = stringValue();
                whitespace();
                if (!consume(':')) fail("expected ':' after an object key");
                result[key] = value();
                whitespace();
                if (consume('}')) return result;
                if (!consume(',')) fail("expected ',' between object members");
            }
        }

        Json arrayValue()
        {
            ++position_;
            Json result = Json::array();
            whitespace();
            if (consume(']')) return result;
            while (true) {
                result.push(value());
                whitespace();
                if (consume(']')) return result;
                if (!consume(',')) fail("expected ',' between array members");
            }
        }

        std::string stringValue()
        {
            if (!consume('"')) fail("expected a string");
            std::string result;
            while (!finished()) {
                const char character = text_[position_++];
                if (character == '"') return result;
                if (static_cast<unsigned char>(character) < 0x20U) fail("control character in string");
                if (character != '\\') {
                    result.push_back(character);
                    continue;
                }
                if (finished()) fail("unfinished string escape");
                switch (text_[position_++]) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': appendUnicode(result); break;
                default: fail("invalid string escape");
                }
            }
            fail("unterminated string");
        }

        double numberValue()
        {
            const std::size_t start = position_;
            if (!finished() && text_[position_] == '-') ++position_;
            if (finished()) fail("invalid number");
            if (text_[position_] == '0') ++position_;
            else {
                if (!std::isdigit(static_cast<unsigned char>(text_[position_]))) fail("invalid number");
                while (!finished() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
            }
            if (!finished() && text_[position_] == '.') {
                ++position_;
                if (finished() || !std::isdigit(static_cast<unsigned char>(text_[position_]))) fail("invalid number fraction");
                while (!finished() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
            }
            if (!finished() && (text_[position_] == 'e' || text_[position_] == 'E')) {
                ++position_;
                if (!finished() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
                if (finished() || !std::isdigit(static_cast<unsigned char>(text_[position_]))) fail("invalid number exponent");
                while (!finished() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
            }
            const std::string token(text_.substr(start, position_ - start));
            char* end = nullptr;
            const double result = std::strtod(token.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(result)) fail("invalid JSON number");
            return result;
        }

        void appendUnicode(std::string& output)
        {
            unsigned int codepoint = hexCodepoint();
            if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                if (position_ + 2 > text_.size() || text_.substr(position_, 2) != "\\u") {
                    fail("missing low Unicode surrogate");
                }
                position_ += 2;
                const unsigned int low = hexCodepoint();
                if (low < 0xDC00U || low > 0xDFFFU) fail("invalid low Unicode surrogate");
                codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
            } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                fail("unexpected low Unicode surrogate");
            }
            if (codepoint <= 0x7FU) output.push_back(static_cast<char>(codepoint));
            else if (codepoint <= 0x7FFU) {
                output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
                output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            } else if (codepoint <= 0xFFFFU) {
                output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
                output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            } else {
                output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
                output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
                output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
            }
        }

        unsigned int hexCodepoint()
        {
            if (position_ + 4 > text_.size()) fail("unfinished Unicode escape");
            unsigned int result = 0;
            for (int index = 0; index < 4; ++index) {
                const char character = text_[position_++];
                result <<= 4U;
                if (character >= '0' && character <= '9') result += character - '0';
                else if (character >= 'a' && character <= 'f') result += 10U + character - 'a';
                else if (character >= 'A' && character <= 'F') result += 10U + character - 'A';
                else fail("invalid Unicode escape");
            }
            return result;
        }

        void literal(std::string_view expected)
        {
            if (text_.substr(position_, expected.size()) != expected) fail("invalid JSON literal");
            position_ += expected.size();
        }

        bool consume(char expected)
        {
            if (finished() || text_[position_] != expected) return false;
            ++position_;
            return true;
        }

        [[noreturn]] void fail(const char* message) const
        {
            throw std::runtime_error(std::string(message) + " at byte " + std::to_string(position_));
        }

        std::string_view text_;
        std::size_t position_ = 0;
    };

    static void string(std::ostream& output, std::string_view value)
    {
        output << '"';
        for (unsigned char character : value) {
            switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20U) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(character) << std::dec << std::setfill(' ');
                } else output << static_cast<char>(character);
            }
        }
        output << '"';
    }

    static void padding(std::ostream& output, int count)
    {
        for (int index = 0; index < count; ++index) output.put(' ');
    }

    static void write(std::ostream& output, const Json& json, int indent, int depth)
    {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) output << "null";
            else if constexpr (std::is_same_v<T, bool>) output << (value ? "true" : "false");
            else if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(value)) output << "null";
                else if (std::floor(value) == value && std::abs(value) < 9.0e15)
                    output << std::fixed << std::setprecision(0) << value << std::defaultfloat;
                else output << std::setprecision(12) << value;
            } else if constexpr (std::is_same_v<T, std::string>) string(output, value);
            else if constexpr (std::is_same_v<T, Array>) {
                output << '[';
                for (std::size_t index = 0; index < value.size(); ++index) {
                    if (index) output << ',';
                    if (indent >= 0) { output << '\n'; padding(output, depth + indent); }
                    write(output, value[index], indent, depth + indent);
                }
                if (!value.empty() && indent >= 0) { output << '\n'; padding(output, depth); }
                output << ']';
            } else {
                output << '{';
                std::size_t index = 0;
                for (const auto& [key, item] : value) {
                    if (index++) output << ',';
                    if (indent >= 0) { output << '\n'; padding(output, depth + indent); }
                    string(output, key);
                    output << (indent >= 0 ? ": " : ":");
                    write(output, item, indent, depth + indent);
                }
                if (!value.empty() && indent >= 0) { output << '\n'; padding(output, depth); }
                output << '}';
            }
        }, json.value_);
    }
};

template <typename T>
Json jsonNumbers(const std::vector<T>& values)
{
    Json result = Json::array();
    for (const auto value : values) result.push(Json(static_cast<double>(value)));
    return result;
}

} // namespace jamtaster::native

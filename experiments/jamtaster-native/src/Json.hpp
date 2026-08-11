#pragma once

#include <cmath>
#include <iomanip>
#include <map>
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

    [[nodiscard]] std::string dump(int indent = 2) const
    {
        std::ostringstream output;
        write(output, *this, indent, 0);
        return output.str();
    }

private:
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Value value_ = nullptr;

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

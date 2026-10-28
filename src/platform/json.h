#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mscharged::platform
{
// A parsed JSON document (RFC 8259) for small inputs such as GitHub API
// answers and package manifests. Accessors return an empty value instead of
// throwing when a field is missing or has another type, so readers check
// only what they need. Numbers keep their text; Integer() reads whole ones.
class JsonValue
{
public:
    enum class Type { Null, Boolean, Number, String, Array, Object };

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsObject() const { return type_ == Type::Object; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsString() const { return type_ == Type::String; }

    bool Bool(bool fallback = false) const { return type_ == Type::Boolean ? boolean_ : fallback; }
    // A whole number that fits 64 bits, or nothing.
    std::optional<std::int64_t> Integer() const;
    // String text, or the source text of a number; empty for other types.
    const std::string& Text() const { return text_; }
    // Array items or object member values, in document order.
    const std::vector<JsonValue>& Items() const { return items_; }
    // Object member names, parallel to Items().
    const std::vector<std::string>& Names() const { return names_; }
    // The first member with this name; a null value when absent.
    const JsonValue& operator[](std::string_view name) const;
    std::size_t size() const { return items_.size(); }

private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool boolean_ = false;
    std::string text_;
    std::vector<JsonValue> items_;
    std::vector<std::string> names_;
};

// Throws std::runtime_error (naming the byte offset) for malformed input,
// nesting deeper than 64 levels or documents over 32 MiB.
JsonValue ParseJson(std::string_view text);

// Appends `text` (UTF-8) as a JSON string, quoted, with quotes, backslashes
// and control characters escaped.
void AppendJsonString(std::string& out, std::string_view text);
} // namespace mscharged::platform

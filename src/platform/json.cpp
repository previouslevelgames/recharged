#include "platform/json.h"

#include <charconv>
#include <stdexcept>

namespace mscharged::platform
{
namespace
{
constexpr std::size_t kMaxDocument = 32u << 20;
constexpr int kMaxDepth = 64;

const JsonValue& NullValue()
{
    static const JsonValue null;
    return null;
}

void AppendUtf8(std::string& out, std::uint32_t code)
{
    if (code < 0x80) out += char(code);
    else if (code < 0x800)
    {
        out += char(0xC0 | (code >> 6));
        out += char(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        out += char(0xE0 | (code >> 12));
        out += char(0x80 | ((code >> 6) & 0x3F));
        out += char(0x80 | (code & 0x3F));
    }
    else
    {
        out += char(0xF0 | (code >> 18));
        out += char(0x80 | ((code >> 12) & 0x3F));
        out += char(0x80 | ((code >> 6) & 0x3F));
        out += char(0x80 | (code & 0x3F));
    }
}
} // namespace

class JsonParser
{
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue Document()
    {
        if (text_.size() > kMaxDocument) Fail("document too large");
        // A UTF-8 byte order mark is tolerated before the document.
        if (text_.substr(0, 3) == "\xEF\xBB\xBF") position_ = 3;
        JsonValue value = Value(0);
        SkipSpace();
        if (position_ != text_.size()) Fail("unexpected data after the document");
        return value;
    }

private:
    [[noreturn]] void Fail(const char* what) const
    {
        throw std::runtime_error(std::string("Invalid JSON at byte ") + std::to_string(position_) + ": " + what);
    }

    void SkipSpace()
    {
        while (position_ < text_.size())
        {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++position_;
        }
    }

    bool Consume(std::string_view literal)
    {
        if (text_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    JsonValue Value(int depth)
    {
        if (depth > kMaxDepth) Fail("nesting too deep");
        SkipSpace();
        if (position_ >= text_.size()) Fail("unexpected end");
        JsonValue value;
        const char c = text_[position_];
        if (c == '{')
        {
            value.type_ = JsonValue::Type::Object;
            ++position_;
            SkipSpace();
            if (Consume("}")) return value;
            for (;;)
            {
                SkipSpace();
                if (position_ >= text_.size() || text_[position_] != '"') Fail("expected a member name");
                value.names_.push_back(String());
                SkipSpace();
                if (!Consume(":")) Fail("expected ':'");
                value.items_.push_back(Value(depth + 1));
                SkipSpace();
                if (Consume("}")) return value;
                if (!Consume(",")) Fail("expected ',' or '}'");
            }
        }
        if (c == '[')
        {
            value.type_ = JsonValue::Type::Array;
            ++position_;
            SkipSpace();
            if (Consume("]")) return value;
            for (;;)
            {
                value.items_.push_back(Value(depth + 1));
                SkipSpace();
                if (Consume("]")) return value;
                if (!Consume(",")) Fail("expected ',' or ']'");
            }
        }
        if (c == '"')
        {
            value.type_ = JsonValue::Type::String;
            value.text_ = String();
            return value;
        }
        if (Consume("true")) { value.type_ = JsonValue::Type::Boolean; value.boolean_ = true; return value; }
        if (Consume("false")) { value.type_ = JsonValue::Type::Boolean; return value; }
        if (Consume("null")) return value;
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            value.type_ = JsonValue::Type::Number;
            value.text_ = Number();
            return value;
        }
        Fail("unexpected character");
    }

    std::string Number()
    {
        const std::size_t start = position_;
        auto digits = [&] {
            const std::size_t first = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            return position_ - first;
        };
        if (text_[position_] == '-') ++position_;
        if (position_ < text_.size() && text_[position_] == '0') ++position_;
        else if (!digits()) Fail("invalid number");
        if (position_ < text_.size() && text_[position_] == '.')
        {
            ++position_;
            if (!digits()) Fail("invalid number");
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E'))
        {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            if (!digits()) Fail("invalid number");
        }
        return std::string(text_.substr(start, position_ - start));
    }

    std::uint32_t Hex4()
    {
        if (position_ + 4 > text_.size()) Fail("truncated escape");
        std::uint32_t code = 0;
        for (int n = 0; n < 4; ++n)
        {
            const char c = text_[position_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= std::uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') code |= std::uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= std::uint32_t(c - 'A' + 10);
            else Fail("invalid escape");
        }
        return code;
    }

    std::string String()
    {
        ++position_; // opening quote
        std::string out;
        for (;;)
        {
            if (position_ >= text_.size()) Fail("unterminated string");
            const char c = text_[position_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) Fail("control character in a string");
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (position_ >= text_.size()) Fail("unterminated string");
            switch (text_[position_++])
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                std::uint32_t code = Hex4();
                if (code >= 0xD800 && code <= 0xDBFF && Consume("\\u"))
                {
                    const std::uint32_t low = Hex4();
                    if (low >= 0xDC00 && low <= 0xDFFF) code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    else
                    {
                        AppendUtf8(out, 0xFFFD); // unpaired high surrogate
                        code = low >= 0xD800 && low <= 0xDFFF ? 0xFFFD : low;
                    }
                }
                else if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
                AppendUtf8(out, code);
                break;
            }
            default: Fail("invalid escape");
            }
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

std::optional<std::int64_t> JsonValue::Integer() const
{
    if (type_ != Type::Number) return std::nullopt;
    std::int64_t value = 0;
    const auto result = std::from_chars(text_.data(), text_.data() + text_.size(), value);
    if (result.ec != std::errc{} || result.ptr != text_.data() + text_.size()) return std::nullopt;
    return value;
}

const JsonValue& JsonValue::operator[](std::string_view name) const
{
    if (type_ == Type::Object)
        for (std::size_t n = 0; n < names_.size(); ++n)
            if (names_[n] == name) return items_[n];
    return NullValue();
}

JsonValue ParseJson(std::string_view text) { return JsonParser(text).Document(); }
} // namespace mscharged::platform

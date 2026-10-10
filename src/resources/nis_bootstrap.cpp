#include "resources/nis_bootstrap.h"
#include "platform/parse_float.h"
#include <charconv>
#include <cmath>
#include <set>
#include <string_view>

namespace mscharged::resources
{
namespace
{
bool Space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
std::string_view Trim(std::string_view text)
{
    while (!text.empty() && Space(text.front())) text.remove_prefix(1);
    while (!text.empty() && Space(text.back())) text.remove_suffix(1);
    return text;
}
class Lines
{
    std::string_view remaining_;
public:
    explicit Lines(Bytes bytes)
    {
        Require(bytes.size() <= MaximumAssetBytes, "NIS text exceeds the asset limit");
        if (!bytes.empty()) remaining_ = {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        Require(remaining_.find('\0') == std::string_view::npos, "NIS text contains an embedded NUL");
    }
    std::string_view Next()
    {
        while (!remaining_.empty())
        {
            const auto end = remaining_.find('\n');
            auto line = remaining_.substr(0, end);
            remaining_.remove_prefix(end == std::string_view::npos ? remaining_.size() : end + 1);
            Require(line.size() <= 1024, "NIS text line exceeds its bound");
            line = Trim(line);
            if (!line.empty() && line.front() != '#') return line;
        }
        return {};
    }
    std::string_view Field(std::string_view key)
    {
        auto line = Next();
        Require(line.size() > key.size() && line.substr(0, key.size()) == key
                && Space(line[key.size()]), "Missing or out-of-order NIS dictionary field");
        line.remove_prefix(key.size());
        return Trim(line);
    }
};
std::string Name(std::string_view text, std::size_t maximum, bool lower)
{
    Require(!text.empty() && text.size() <= maximum, "Invalid NIS name length");
    std::string name(text);
    for (auto& c : name)
    {
        Require(c > 32 && c < 127 && c != '/' && c != '\\' && c != ':', "Invalid NIS asset name");
        if (lower && c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    Require(name != "." && name != "..", "Invalid NIS asset name");
    return name;
}
std::uint32_t Count(std::string_view text, std::uint32_t maximum)
{
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && result <= maximum,
            "Invalid NIS dictionary count");
    return result;
}
float Number(std::string_view& text)
{
    text = Trim(text);
    float value = 0;
    const auto result = mscharged::FromCharsFloat(text.data(), text.data() + text.size(), value);
    Require(result.ec == std::errc{} && result.ptr != text.data() && std::isfinite(value),
            "Invalid NIS dictionary coordinate");
    text.remove_prefix(result.ptr - text.data());
    return value;
}
template<std::size_t N> std::array<float, N> Vector(std::string_view text, bool comma)
{
    std::array<float, N> result{};
    for (std::size_t i = 0; i < N; ++i)
    {
        result[i] = Number(text);
        if (i + 1 != N)
        {
            if (comma)
            {
                text = Trim(text);
                Require(!text.empty() && text.front() == ',', "NIS vector lacks a comma");
                text.remove_prefix(1);
            }
            else Require(!text.empty() && Space(text.front()), "NIS vector lacks a separator");
        }
    }
    Require(Trim(text).empty(), "Unexpected NIS vector suffix");
    return result;
}
}
std::vector<std::string> ReadNisNameList(Bytes bytes)
{
    Lines lines(bytes);
    std::vector<std::string> names;
    for (auto line = lines.Next(); !line.empty(); line = lines.Next())
    {
        Require(names.size() < 512, "Too many NIS list entries");
        // The original stores spelling verbatim and compares case-insensitively.
        names.push_back(Name(line, 63, false));
    }
    return names;
}
std::vector<NisDictionaryEntry> ReadNisDictionary(Bytes bytes)
{
    Lines lines(bytes);
    std::vector<NisDictionaryEntry> entries;
    std::set<std::string> names;
    for (auto line = lines.Next(); !line.empty(); line = lines.Next())
    {
        Require(entries.size() < 512, "NIS dictionary exceeds 512 entries");
        Require(line.starts_with("name ") || line.starts_with("name\t"), "NIS dictionary lacks a name");
        NisDictionaryEntry entry;
        entry.name = Name(Trim(line.substr(5)), 63, true);
        Require(names.insert(entry.name).second, "Duplicate NIS dictionary name");
        entry.bytes = Count(lines.Field("size"), MaximumAssetBytes);
        Require(entry.bytes != 0, "NIS dictionary declares an empty asset");
        entry.balls = Count(lines.Field("has_ball"), 10);
        const auto animations = Count(lines.Field("num_animations"), 10);
        entry.cameras = Count(lines.Field("num_cameras"), 10);
        entry.center = Vector<3>(lines.Field("center"), true);
        entry.minimum = Vector<3>(lines.Field("min_bounds"), true);
        entry.maximum = Vector<3>(lines.Field("max_bounds"), true);
        for (std::size_t i = 0; i < 3; ++i)
            Require(entry.minimum[i] <= entry.maximum[i], "NIS dictionary has inverted bounds");
        for (std::uint32_t i = 0; i < animations; ++i)
            entry.animation_starts.push_back(Vector<3>(lines.Field("begin_pos"), true));
        const auto proxies = Count(lines.Field("num_anim_proxies"), 8);
        for (std::uint32_t i = 0; i < proxies; ++i)
        {
            NisAnimationProxy proxy;
            proxy.name = Name(lines.Field("anim_proxy_name"), 15, false);
            proxy.position = Vector<2>(lines.Field("anim_proxy_position"), false);
            proxy.direction = static_cast<std::uint16_t>(Count(lines.Field("anim_proxy_direction"), 65535));
            entry.proxies.push_back(std::move(proxy));
        }
        entries.push_back(std::move(entry));
    }
    Require(!entries.empty(), "NIS dictionary is empty");
    return entries;
}
}

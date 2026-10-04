#include "resources/nis_bootstrap.h"
#include <iostream>
#include <stdexcept>

using namespace mscharged::resources;
namespace
{
unsigned checks = 0;
Bytes View(const std::string& text) { return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}; }
void Check(bool result, const char* message) { ++checks; if (!result) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid NIS text accepted"); }
std::string Record(unsigned actors = 8)
{
    std::string text = "name OPENING\nsize 1024\nhas_ball 1\nnum_animations " + std::to_string(actors)
        + "\nnum_cameras 2\ncenter 0, -0, 0\nmin_bounds -1, -2, -3\nmax_bounds 1, 2, 3\n";
    for (unsigned i = 0; i < actors; ++i) text += "begin_pos 1, 2, 3\n";
    return text + "num_anim_proxies 1\nanim_proxy_name crowd\nanim_proxy_position 2 3\nanim_proxy_direction 65535\n";
}
void Replace(std::string& text, const std::string& from, const std::string& to)
{ const auto p = text.find(from); Check(p != text.npos, "Fixture replacement missing"); text.replace(p, from.size(), to); }
}
int main()
{
    try
    {
        for (unsigned actors : {0u, 4u, 8u, 10u})
        {
            auto text = Record(actors); auto entries = ReadNisDictionary(View(text));
            Check(entries.size() == 1 && entries[0].name == "opening" && entries[0].animation_starts.size() == actors
                && entries[0].proxies[0].direction == 65535, "Dictionary lost names, positions or direction");
            text.clear(); Check(entries[0].proxies[0].name == "crowd", "Dictionary borrowed source text");
        }
        auto valid = Record();
        for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
            {"name OPENING", "name ../bad"}, {"name OPENING", "name " + std::string(64, 'x')},
            {"size 1024", "size 0"}, {"size 1024", "size 16777217"}, {"size 1024", "size -1"},
            {"has_ball 1", "has_ball 11"}, {"num_animations 8", "num_animations 11"},
            {"num_cameras 2", "num_cameras 11"}, {"center 0, -0, 0", "center nan, 0, 0"},
            {"center 0, -0, 0", "center 0 0 0"}, {"center 0, -0, 0", "center 0, 0, 0junk"},
            {"max_bounds 1, 2, 3", "max_bounds -2, 2, 3"}, {"begin_pos 1, 2, 3", "begin_pos 1, 2, inf"},
            {"num_anim_proxies 1", "num_anim_proxies 9"}, {"anim_proxy_name crowd", "anim_proxy_name " + std::string(16, 'a')},
            {"anim_proxy_position 2 3", "anim_proxy_position 2, 3"},
            {"anim_proxy_direction 65535", "anim_proxy_direction 65536"}})
        { auto bad = valid; Replace(bad, from, to); Reject([&] { ReadNisDictionary(View(bad)); }); }
        Reject([&] { ReadNisDictionary(View(valid + valid)); });
        for (std::size_t end = 0; end < valid.rfind("anim_proxy_direction"); ++end)
            Reject([&] { ReadNisDictionary(View(valid.substr(0, end))); });
        auto crlf = std::string("# header\r\n\r\n");
        for (char c : valid) { if (c == '\n') crlf += '\r'; crlf += c; }
        Check(ReadNisDictionary(View(crlf)).size() == 1, "CRLF/comments rejected");
        valid.pop_back(); Check(ReadNisDictionary(View(valid)).size() == 1, "Final newline required");
        auto names = ReadNisNameList(View("# comment\r\nOpening\r\n\r\nOther\t"));
        Check(names.size() == 2 && names[0] == "Opening" && names[1] == "Other", "Name list spelling changed");
        Check(ReadNisNameList({}).empty(), "Empty exception list rejected");
        Reject([] { ReadNisNameList(View(std::string("name\0hidden", 11))); });
        Reject([] { ReadNisNameList(View(std::string(1025, ' '))); });
        std::string many;
        for (unsigned i = 0; i < 512; ++i) many += "name\n";
        Check(ReadNisNameList(View(many)).size() == 512, "512-entry list rejected");
        many += "overflow\n"; Reject([&] { ReadNisNameList(View(many)); });
        std::cout << checks << " bounded NIS text checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}

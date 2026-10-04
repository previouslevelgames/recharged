#include "resources/frontend_text_catalog.h"
#include "frontend_font_fixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace mscharged::resources;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
template<class F> void Reject(F action) { ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid catalog accepted"); }
std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{ std::ifstream file(path, std::ios::binary); if (!file) throw std::runtime_error("Cannot read " + path.string()); return {std::istreambuf_iterator<char>(file), {}}; }
void Generated()
{
    auto font = ReadFrontendFont(font_fixture::Font(), "fe/fonts/fixture", "fixture");
    const std::array fonts{font};
    auto loc = ReadLocalization(font_fixture::Localization(), 0x7a947b29);
    FrontendScene scene{};
    scene.resources.push_back({32, 1, font->alias, 0, false});
    FrontendLibraryObject library{}; library.offset = 64; library.type = 2; library.resource = 32;
    scene.library.push_back(library);
    FrontendInstance instance{}; instance.offset = 128; instance.type = 3; instance.library = 64;
    instance.text_overload_flags = 8; instance.localization_hash = 1; instance.name = "Label";
    scene.instances.push_back(instance);
    auto catalog = InspectFrontendText(scene, *loc, fonts);
    Check(catalog.entries.size() == 1 && catalog.entries[0].text == u"A\u00e9" && catalog.entries[0].layout.quads.size() == 2, "Localized component was not resolved");
    scene.instances[0].text = u"AB";
    Check(InspectFrontendText(scene, *loc, fonts).entries[0].text == u"A\u00e9", "Localization override lost precedence over stored literal");
    scene.instances[0].text_overload_flags = 0;
    Check(InspectFrontendText(scene, *loc, fonts).entries[0].text == u"AB", "Stored literal was not retained");
    for (auto text : {u"A{icon}", u"A\nB", u"\U0001f600"})
    { scene.instances[0].text = text; auto unsupported = InspectFrontendText(scene, *loc, fonts); Check(unsupported.entries.empty() && unsupported.unavailable.size() == 1, "Unsupported formatting was silently rendered"); }
    scene.instances[0].text.clear(); scene.instances[0].text_overload_flags = 8; scene.instances[0].localization_hash = 999;
    Reject([&] { InspectFrontendText(scene, *loc, fonts); });
    scene.instances[0].localization_hash = 1; scene.resources[0].hash ^= 1;
    Check(InspectFrontendText(scene, *loc, fonts).entries.empty(), "Unknown font silently replaced");
    scene.resources[0].hash = font->alias; scene.resources[0].type = 0;
    Reject([&] { InspectFrontendText(scene, *loc, fonts); });
    scene.resources[0].type = 1; scene.library[0].resource = 999;
    Reject([&] { InspectFrontendText(scene, *loc, fonts); });
    scene.library.clear(); scene.resources.clear(); scene.instances.clear(); font.reset(); loc.reset();
    Check(catalog.entries[0].layout.font && catalog.entries[0].text == u"A\u00e9", "Catalog lost its retained font/text");
}
void Owned(const std::filesystem::path& root)
{
    const std::array fonts{
        ReadFrontendFont(Read(root / "fonts/eurfonttext18.res"), "fe/fonts/eurfonttext18", "fot-rodinprob18"),
        ReadFrontendFont(Read(root / "fonts/eurfontheading36.res"), "fe/fonts/eurfontheading36", "Scratchy36")};
    const auto scene = ReadFrontendScene(Read(root / "main_menu_v3.fen"));
    for (const auto& [name, hash] : {std::pair{"english", 0x7a947b29u}, {"nafrench", 0x30d469c4u}, {"naspanish", 0x2f242024u}})
    {
        auto loc = ReadLocalization(Read(root / (std::string(name) + ".loc")), hash);
        auto catalog = InspectFrontendText(scene, *loc, fonts);
        Check(!catalog.entries.empty(), "Owned main menu has no selected text");
        unsigned omitted = 0; for (const auto& [reason, count] : catalog.unavailable) omitted += count;
        std::cout << name << ": " << catalog.entries.size() << " text components, " << omitted << " unavailable\n";
    }
}
}
int main(int argc, char** argv)
{
    try { Generated(); if (argc == 2) Owned(argv[1]); std::cout << checks << " frontend text catalog checks passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

// recharged:// links on Linux (platform/watch_link_handler.h): the desktop
// entry and the default in mimeapps.list, written once, the rest of the list
// kept, a moved program registered again.
#include "platform/watch_link_handler.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace
{
int sChecks = 0;
void Check(bool value, const std::string& message)
{
    ++sChecks;
    if (!value) throw std::runtime_error(message);
}
std::string Read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

int main()
{
#if defined(_WIN32) || defined(__APPLE__)
    std::cout << "watch links: registered by the system's own means here\n";
    return 0;
#else
    const auto root = fs::temp_directory_path()
        / ("recharged-watch-links-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        fs::create_directories(root / "config");
        setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        std::ofstream(root / "config/mimeapps.list") << "[Added Associations]\ntext/plain=gedit.desktop;\n\n"
                                                        "[Default Applications]\ntext/html=firefox.desktop\n";
        const auto program = root / "My Games/Recharged $1/recharged";
        const std::string line = mscharged::platform::RegisterWatchLinks(program);
        Check(line.find("recharged://") != std::string::npos, "the first start registers the links: " + line);
        const std::string entry = Read(root / "data/applications/recharged-watch.desktop");
        Check(entry.find("Exec=\"" + (root / "My Games/Recharged \\\\$1/recharged").string() + "\" %u\n")
                  != std::string::npos,
              "the entry starts this program, quoted: " + entry);
        Check(entry.find("MimeType=x-scheme-handler/recharged;\n") != std::string::npos
                  && entry.find("NoDisplay=true\n") != std::string::npos,
              "a hidden handler of the scheme");
        const std::string list = Read(root / "config/mimeapps.list");
        Check(list == "[Added Associations]\ntext/plain=gedit.desktop;\n\n[Default Applications]\n"
                      "text/html=firefox.desktop\nx-scheme-handler/recharged=recharged-watch.desktop\n",
              "the default, the rest of the list kept: " + list);
        Check(mscharged::platform::RegisterWatchLinks(program).empty(), "nothing to do on the next start");
        const auto moved = root / "elsewhere/recharged";
        Check(!mscharged::platform::RegisterWatchLinks(moved).empty()
                  && Read(root / "data/applications/recharged-watch.desktop").find(moved.string()) != std::string::npos
                  && Read(root / "config/mimeapps.list") == list,
              "a moved program registered again, the list unchanged");
        fs::remove(root / "config/mimeapps.list");
        Check(!mscharged::platform::RegisterWatchLinks(moved).empty()
                  && Read(root / "config/mimeapps.list")
                      == "[Default Applications]\nx-scheme-handler/recharged=recharged-watch.desktop\n",
              "a list made when there is none");
        fs::remove_all(root);
    }
    catch (const std::exception& error)
    {
        fs::remove_all(root);
        std::cerr << "watch links: " << error.what() << '\n';
        return 1;
    }
    std::cout << "watch links: " << sChecks << " checks passed\n";
    return 0;
#endif
}

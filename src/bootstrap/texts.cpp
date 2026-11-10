#include "bootstrap/texts.h"

#include "platform/json.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <system_error>

namespace mscharged::texts {
// The built-in English (tools/embed_texts.py), ended by a null ID.
struct EnglishText {
    const char* id;
    const char* text;
};
extern const EnglishText kEnglishTexts[];

namespace {
// One language's texts with English's under them, by ID.
struct Table {
    std::string language = "en";
    std::map<std::string, std::string, std::less<>> texts;
};

// The built-in English alone: the texts before any Load.
const Table& BuiltIn()
{
    static const Table table = [] {
        Table english;
        for (const EnglishText* text = kEnglishTexts; text->id; ++text) english.texts.emplace(text->id, text->text);
        return english;
    }();
    return table;
}

std::mutex sMutex;
// Every table loaded stays (the texts handed out point into them); the
// current one is read without the lock.
std::vector<std::unique_ptr<Table>> sTables;
std::atomic<const Table*> sCurrent{nullptr};
// IDs without a text: their own copies, which stay as well.
std::set<std::string, std::less<>> sMissing;

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// A component's texts: its nested objects' strings, by their dotted path
// after `prefix`.
void Collect(const platform::JsonValue& value, const std::string& prefix,
             std::map<std::string, std::string, std::less<>>& out)
{
    if (value.IsString()) {
        out[prefix] = value.Text();
        return;
    }
    if (!value.IsObject()) return;
    const auto& names = value.Names();
    const auto& items = value.Items();
    for (std::size_t n = 0; n < items.size(); ++n) Collect(items[n], prefix + "." + names[n], out);
}

// The components of one language's folder (its *.json files), over `out`.
void ReadLanguage(const std::filesystem::path& folder, std::map<std::string, std::string, std::less<>>& out)
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".json") continue;
        try {
            std::map<std::string, std::string, std::less<>> texts;
            Collect(platform::ParseJson(ReadFile(entry.path())), entry.path().stem().string(), texts);
            for (auto& [id, text] : texts)
                if (!text.empty()) out[id] = std::move(text);
        } catch (const std::exception&) {
            // A broken file: its texts stay English.
        }
    }
}

bool HasLanguage(const std::filesystem::path& folder, const std::string& code)
{
    std::error_code error;
    return !code.empty() && code.find_first_of("/\\.") == std::string::npos
        && std::filesystem::is_directory(folder / code, error);
}

std::string Lower(std::string_view text)
{
    std::string out(text);
    for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The folder for a PC language ("pt"): its own, else the first regional one
// ("pt-BR").
std::string FolderFor(const std::filesystem::path& folder, const std::string& language)
{
    if (HasLanguage(folder, language)) return language;
    std::error_code error;
    std::vector<std::string> regional;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(error) && Lower(name).rfind(language + "-", 0) == 0) regional.push_back(name);
    }
    std::sort(regional.begin(), regional.end());
    return regional.empty() ? std::string() : regional.front();
}

const Table* Current()
{
    const Table* table = sCurrent.load(std::memory_order_acquire);
    return table ? table : &BuiltIn();
}

const std::string* Find(const Table* table, std::string_view id)
{
    const auto found = table->texts.find(id);
    return found == table->texts.end() ? nullptr : &found->second;
}

std::string Fill(std::string text, std::initializer_list<Arg> args)
{
    for (const Arg& arg : args) {
        const std::string mark = "{{" + std::string(arg.name) + "}}";
        for (std::size_t at = text.find(mark); at != std::string::npos; at = text.find(mark, at + arg.value.size()))
            text.replace(at, mark.size(), arg.value);
    }
    return text;
}

// One UTF-8 character at `at`: its code point and length (a broken byte: as
// it is, length 1).
char32_t Decode(std::string_view text, std::size_t at, std::size_t& length)
{
    const auto byte = [&](std::size_t n) { return static_cast<unsigned char>(text[at + n]); };
    const unsigned char lead = byte(0);
    const auto follows = [&](std::size_t count) {
        if (at + count >= text.size() + 1) return false;
        for (std::size_t n = 1; n < count; ++n)
            if ((byte(n) & 0xC0) != 0x80) return false;
        return true;
    };
    if (lead >= 0xC2 && lead < 0xE0 && follows(2)) {
        length = 2;
        return char32_t(lead & 0x1F) << 6 | (byte(1) & 0x3F);
    }
    if (lead >= 0xE0 && lead < 0xF0 && follows(3)) {
        length = 3;
        return char32_t(lead & 0x0F) << 12 | char32_t(byte(1) & 0x3F) << 6 | (byte(2) & 0x3F);
    }
    if (lead >= 0xF0 && lead < 0xF5 && follows(4)) {
        length = 4;
        return char32_t(lead & 0x07) << 18 | char32_t(byte(1) & 0x3F) << 12 | char32_t(byte(2) & 0x3F) << 6
            | (byte(3) & 0x3F);
    }
    length = 1;
    return lead;
}

void Encode(char32_t c, std::string& out)
{
    if (c < 0x80) out += char(c);
    else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

// A letter's capital (Latin, Greek, Cyrillic), else the letter.
char32_t Capital(char32_t c)
{
    if (c >= 'a' && c <= 'z') return c - 0x20;
    if (c < 0x80) return c;
    // Latin-1: à to þ (not ÷), and ÿ.
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    if (c == 0xFF) return 0x178;
    if (c == 0xB5) return 0x39C;
    // Latin Extended-A: pairs, upper first; ı's capital is I, ſ's S.
    if (c == 0x131) return 'I';
    if (c == 0x17F) return 'S';
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
    if (c >= 0x100 && c <= 0x177 && c != 0x138 && c != 0x149) return (c & 1) ? c - 1 : c;
    // Greek: final sigma, the accented vowels, then the plain letters.
    if (c == 0x3C2) return 0x3A3;
    switch (c) {
    case 0x3AC: return 0x386;
    case 0x3AD: return 0x388;
    case 0x3AE: return 0x389;
    case 0x3AF: return 0x38A;
    case 0x3CC: return 0x38C;
    case 0x3CD: return 0x38E;
    case 0x3CE: return 0x38F;
    case 0x3CA: return 0x3AA;
    case 0x3CB: return 0x3AB;
    default: break;
    }
    if (c >= 0x3B1 && c <= 0x3C9) return c - 0x20;
    // Cyrillic: а to я, ѐ to џ, then the pairs (upper first; Ӂ to ӎ the other way).
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c : c - 1;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
        return (c & 1) ? c - 1 : c;
    if (c == 0x4CF) return 0x4C0;
    return c;
}
} // namespace

std::string Load(const std::filesystem::path& folder, std::string_view language,
                 const std::vector<std::string>& pc_languages)
{
    std::string code;
    if (language == "auto") {
        for (const auto& pc : pc_languages) {
            if (pc == "en") break; // English is what there is anyway
            code = FolderFor(folder, Lower(pc));
            if (!code.empty()) break;
        }
    } else if (language != "en" && HasLanguage(folder, std::string(language))) code = std::string(language);
    // English: built in, then the folder's own (edits show without a new
    // build), then the language over it.
    auto table = std::make_unique<Table>(BuiltIn());
    ReadLanguage(folder / "en", table->texts);
    if (!code.empty()) {
        ReadLanguage(folder / code, table->texts);
        table->language = code;
    }
    const std::lock_guard lock(sMutex);
    sCurrent.store(table.get(), std::memory_order_release);
    sTables.push_back(std::move(table));
    return sTables.back()->language;
}

const char* T(std::string_view id)
{
    if (const std::string* text = Find(Current(), id)) return text->c_str();
    const std::lock_guard lock(sMutex);
    return sMissing.emplace(id).first->c_str();
}

std::string T(std::string_view id, std::initializer_list<Arg> args) { return Fill(T(id), args); }

std::string Count(std::string_view id, long long count, std::initializer_list<Arg> args)
{
    const Table* table = Current();
    const std::string base(id);
    const std::string* text = Find(table, base + "_" + std::string(PluralForm(Language(), count)));
    if (!text) text = Find(table, base + "_other");
    std::string out = text ? *text : base;
    out = Fill(std::move(out), {{"count", std::to_string(count)}});
    return Fill(std::move(out), args);
}

std::string Language() { return Current()->language; }

std::vector<LanguageChoice> Languages(const std::filesystem::path& folder)
{
    std::vector<LanguageChoice> choices;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_directory(error)) continue;
        std::map<std::string, std::string, std::less<>> texts;
        ReadLanguage(entry.path(), texts);
        const auto name = texts.find("common.language.name");
        if (name == texts.end()) continue;
        choices.push_back({entry.path().filename().string(), name->second});
    }
    std::sort(choices.begin(), choices.end(), [](const LanguageChoice& a, const LanguageChoice& b) {
        if ((a.code == "en") != (b.code == "en")) return a.code == "en";
        return a.code < b.code;
    });
    return choices;
}

std::string AllTexts()
{
    std::string out;
    for (const auto& [id, text] : Current()->texts) out += text;
    return out;
}

std::string_view PluralForm(std::string_view language, long long count)
{
    const std::string_view code = language.substr(0, language.find('-'));
    const long long n = count < 0 ? -count : count;
    const long long tens = n % 10, hundreds = n % 100;
    const bool teen = hundreds >= 11 && hundreds <= 14;
    // No forms by number.
    for (const char* other : {"ja", "ko", "zh", "vi", "th", "id", "ms"})
        if (code == other) return "other";
    // 0 and 1 are one.
    if (code == "fr" || (code == "pt" && language != "pt-PT")) return n <= 1 ? "one" : "other";
    // one, few (2-4), many.
    if (code == "ru" || code == "uk" || code == "be") {
        if (tens == 1 && hundreds != 11) return "one";
        if (tens >= 2 && tens <= 4 && !teen) return "few";
        return "many";
    }
    if (code == "pl") {
        if (n == 1) return "one";
        if (tens >= 2 && tens <= 4 && !teen) return "few";
        return "many";
    }
    if (code == "cs" || code == "sk") {
        if (n == 1) return "one";
        return n >= 2 && n <= 4 ? "few" : "other";
    }
    if (code == "hr" || code == "sr" || code == "bs") {
        if (tens == 1 && hundreds != 11) return "one";
        if (tens >= 2 && tens <= 4 && !teen) return "few";
        return "other";
    }
    if (code == "ro") {
        if (n == 1) return "one";
        return n == 0 || (hundreds >= 2 && hundreds <= 19) ? "few" : "other";
    }
    if (code == "lt") {
        if (tens == 1 && !(hundreds >= 11 && hundreds <= 19)) return "one";
        if (tens >= 2 && !(hundreds >= 11 && hundreds <= 19)) return "few";
        return "other";
    }
    if (code == "lv") {
        if (tens == 0 || (hundreds >= 11 && hundreds <= 19)) return "zero";
        return tens == 1 && hundreds != 11 ? "one" : "other";
    }
    if (code == "ar") {
        if (n == 0) return "zero";
        if (n == 1) return "one";
        if (n == 2) return "two";
        if (hundreds >= 3 && hundreds <= 10) return "few";
        if (hundreds >= 11) return "many";
        return "other";
    }
    if (code == "he") {
        if (n == 1) return "one";
        return n == 2 ? "two" : "other";
    }
    return n == 1 ? "one" : "other";
}

std::string Upper(std::string_view utf8)
{
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t at = 0; at < utf8.size();) {
        std::size_t length = 1;
        const char32_t c = Decode(utf8, at, length);
        if (length == 1 && c >= 0x80) out += utf8[at]; // a broken byte stays
        else if (c == 0xDF) out += "SS";
        else Encode(Capital(c), out);
        at += length;
    }
    return out;
}

} // namespace mscharged::texts

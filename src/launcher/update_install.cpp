#include "launcher/update_install.h"

#include "platform/http_client.h"
#include "platform/json.h"
#include "platform/path.h"
#include "platform/sha256.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mscharged::launcher
{
namespace
{
namespace fs = std::filesystem;

// Limits against damaged or hostile archives (the largest package is ~100 MB unpacked).
constexpr std::uint64_t kMaxUnpacked = 2ull << 30;
constexpr std::uint64_t kMaxEntries = 20000;
constexpr const char* kReplacingMarker = "replacing";

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

// A package path as stored in an archive or manifest: relative, '/'
// separated, without "..", "." or empty parts, drive letters, backslashes
// or control characters.
fs::path SafePath(std::string_view name)
{
    if (name.empty() || name.size() > 1024 || name.front() == '/') Fail("Unsafe path in the package: " + std::string(name));
    std::string_view rest = name;
    while (!rest.empty())
    {
        const auto slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part.empty() || part == "." || part == ".."
            || std::any_of(part.begin(), part.end(), [](char c) {
                   return static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?'
                       || c == '"' || c == '<' || c == '>' || c == '|';
               }))
            Fail("Unsafe path in the package: " + std::string(name));
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    }
    return PathFromUtf8(name);
}

// A relative path as text with '/' separators.
std::string Generic(const fs::path& path)
{
    std::string text = PathUtf8(path);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

bool Exists(const fs::path& path)
{
    std::error_code error;
    return fs::symlink_status(path, error).type() != fs::file_type::not_found && !error;
}

// Renames one file. Windows can rename (not replace) a running program or
// loaded library; a virus scanner may hold a fresh file for a moment.
void MovePath(const fs::path& from, const fs::path& to)
{
#ifdef _WIN32
    for (int attempt = 0;; ++attempt)
    {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return;
        const DWORD error = GetLastError();
        if ((error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION)
            && attempt < 30)
        {
            Sleep(100);
            continue;
        }
        throw std::system_error(int(error), std::system_category(), "Cannot move " + PathUtf8(from));
    }
#else
    if (Exists(to)) Fail("Cannot move " + PathUtf8(from) + ": " + PathUtf8(to) + " exists");
    if (std::rename(from.c_str(), to.c_str()) != 0)
        throw std::system_error(errno, std::generic_category(), "Cannot move " + PathUtf8(from));
#endif
}

unsigned ParseMode(const std::string& text)
{
    // tools/package_build.py writes Python's oct(): "0o755".
    std::string_view digits = text;
    if (digits.substr(0, 2) == "0o") digits.remove_prefix(2);
    unsigned mode = 0;
    if (digits.empty() || digits.size() > 6) Fail("Invalid file mode in the package manifest: " + text);
    for (const char c : digits)
    {
        if (c < '0' || c > '7') Fail("Invalid file mode in the package manifest: " + text);
        mode = mode * 8 + unsigned(c - '0');
    }
    return mode & 07777;
}

// Writes one unpacked file, checking the limits as it grows.
class OutputFile
{
public:
    OutputFile(const fs::path& path, std::uint64_t& total) : path_(path), total_(total)
    {
        fs::create_directories(path.parent_path());
        if (Exists(path)) Fail("The package contains " + PathUtf8(path.filename()) + " twice");
        stream_.open(path, std::ios::binary | std::ios::trunc);
        if (!stream_) Fail("Cannot create " + PathUtf8(path));
    }
    void Write(const char* data, std::size_t size)
    {
        total_ += size;
        if (total_ > kMaxUnpacked) Fail("The package unpacks to more than 2 GB");
        if (!stream_.write(data, std::streamsize(size))) Fail("Cannot write " + PathUtf8(path_));
    }
    void Close()
    {
        stream_.close();
        if (!stream_) Fail("Cannot write " + PathUtf8(path_));
    }

private:
    fs::path path_;
    std::uint64_t& total_;
    std::ofstream stream_;
};

// Sequential reads of a gzip file (one or more members).
class GzipReader
{
public:
    explicit GzipReader(const fs::path& file) : file_(file, std::ios::binary)
    {
        if (!file_) Fail("Cannot read " + PathUtf8(file));
        if (inflateInit2(&stream_, 16 + MAX_WBITS) != Z_OK) Fail("Cannot start decompression");
    }
    ~GzipReader() { inflateEnd(&stream_); }
    GzipReader(const GzipReader&) = delete;
    GzipReader& operator=(const GzipReader&) = delete;

    // Fills `size` bytes; returns fewer only at the end of the data.
    std::size_t Read(char* out, std::size_t size)
    {
        stream_.next_out = reinterpret_cast<Bytef*>(out);
        stream_.avail_out = uInt(size);
        while (stream_.avail_out && !finished_)
        {
            if (!stream_.avail_in)
            {
                file_.read(reinterpret_cast<char*>(input_.data()), std::streamsize(input_.size()));
                stream_.next_in = input_.data();
                stream_.avail_in = uInt(file_.gcount());
                if (!stream_.avail_in)
                {
                    if (!member_started_) { finished_ = true; break; } // clean end after a member
                    Fail("The package is truncated");
                }
            }
            member_started_ = true;
            const int result = inflate(&stream_, Z_NO_FLUSH);
            if (result == Z_STREAM_END)
            {
                // Another gzip member may follow.
                member_started_ = false;
                if (inflateReset(&stream_) != Z_OK) Fail("Cannot continue decompression");
                continue;
            }
            if (result != Z_OK && result != Z_BUF_ERROR) Fail("The package is damaged (gzip)");
        }
        return size - stream_.avail_out;
    }

    void ReadExactly(char* out, std::size_t size)
    {
        if (Read(out, size) != size) Fail("The package is truncated");
    }

    void Skip(std::uint64_t size)
    {
        char buffer[4096];
        while (size)
        {
            const std::size_t step = std::size_t(std::min<std::uint64_t>(size, sizeof buffer));
            ReadExactly(buffer, step);
            size -= step;
        }
    }

private:
    std::ifstream file_;
    z_stream stream_{};
    std::array<Bytef, 1 << 16> input_{};
    bool finished_ = false, member_started_ = false;
};

std::uint64_t TarNumber(const char* field, std::size_t size)
{
    // Large values use base-256 with the high bit set.
    if (static_cast<unsigned char>(field[0]) & 0x80)
    {
        std::uint64_t value = static_cast<unsigned char>(field[0]) & 0x7F;
        for (std::size_t n = 1; n < size; ++n) value = (value << 8) | static_cast<unsigned char>(field[n]);
        return value;
    }
    std::uint64_t value = 0;
    std::size_t n = 0;
    while (n < size && field[n] == ' ') ++n;
    for (; n < size && field[n] >= '0' && field[n] <= '7'; ++n) value = value * 8 + std::uint64_t(field[n] - '0');
    return value;
}

std::string TarText(const char* field, std::size_t size)
{
    return std::string(field, std::find(field, field + size, '\0'));
}

// tar.gz (ustar, pax or GNU long names), as Python's tarfile writes them.
void UnpackTar(const fs::path& archive, const fs::path& destination, const std::function<bool()>& cancelled)
{
    GzipReader input(archive);
    std::uint64_t total = 0, entries = 0;
    std::string long_name;
    char header[512];
    for (;;)
    {
        if (cancelled && cancelled()) Fail("Cancelled");
        if (input.Read(header, sizeof header) != sizeof header) Fail("The package is truncated");
        if (std::all_of(header, header + sizeof header, [](char c) { return c == 0; })) return; // end
        unsigned sum = 0;
        for (std::size_t n = 0; n < sizeof header; ++n)
            sum += (n >= 148 && n < 156) ? unsigned(' ') : static_cast<unsigned char>(header[n]);
        if (sum != TarNumber(header + 148, 8)) Fail("The package is damaged (tar)");
        if (++entries > kMaxEntries) Fail("The package has too many entries");
        const std::uint64_t size = TarNumber(header + 124, 12);
        const std::uint64_t padded = (size + 511) / 512 * 512;
        const char type = header[156];
        if (type == 'x' || type == 'L' || type == 'g')
        {
            // pax records ("<length> path=<name>\n") or a GNU long name.
            if (size > (1u << 20)) Fail("The package is damaged (tar)");
            std::string data(std::size_t(size), '\0');
            input.ReadExactly(data.data(), data.size());
            input.Skip(padded - size);
            if (type == 'L') long_name = std::string(data.c_str());
            else if (type == 'x')
            {
                std::size_t at = 0;
                while (at < data.size())
                {
                    const auto space = data.find(' ', at);
                    std::size_t length = 0;
                    if (space == std::string::npos) break;
                    for (std::size_t n = at; n < space; ++n)
                    {
                        if (data[n] < '0' || data[n] > '9' || length > data.size()) Fail("The package is damaged (pax)");
                        length = length * 10 + std::size_t(data[n] - '0');
                    }
                    if (length <= space - at + 1 || at + length > data.size()) Fail("The package is damaged (pax)");
                    const std::string record = data.substr(space + 1, at + length - space - 2);
                    if (record.rfind("path=", 0) == 0) long_name = record.substr(5);
                    at += length;
                }
            }
            continue;
        }
        std::string name = long_name;
        long_name.clear();
        if (name.empty())
        {
            name = TarText(header, 100);
            const std::string prefix = std::string(header + 257, 5) == "ustar" ? TarText(header + 345, 155) : "";
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        while (!name.empty() && name.back() == '/') name.pop_back();
        if (name.rfind("./", 0) == 0) name.erase(0, 2);
        if (type == '5')
        {
            if (!name.empty()) fs::create_directories(destination / SafePath(name));
            input.Skip(padded);
            continue;
        }
        if (type != '0' && type != '\0' && type != '7')
            Fail("The package contains a link or special file: " + name);
        OutputFile file(destination / SafePath(name), total);
        char buffer[1 << 15];
        for (std::uint64_t left = size; left;)
        {
            const std::size_t step = std::size_t(std::min<std::uint64_t>(left, sizeof buffer));
            input.ReadExactly(buffer, step);
            file.Write(buffer, step);
            left -= step;
        }
        file.Close();
        input.Skip(padded - size);
    }
}

std::uint32_t Read16(const unsigned char* data) { return std::uint32_t(data[0]) | std::uint32_t(data[1]) << 8; }
std::uint32_t Read32(const unsigned char* data) { return Read16(data) | Read16(data + 2) << 16; }

// zip (stored or deflated entries, no zip64), as Python's zipfile writes them.
void UnpackZip(const fs::path& archive, const fs::path& destination, const std::function<bool()>& cancelled)
{
    std::ifstream file(archive, std::ios::binary);
    if (!file) Fail("Cannot read " + PathUtf8(archive));
    file.seekg(0, std::ios::end);
    const std::uint64_t length = std::uint64_t(file.tellg());
    // The end-of-central-directory record: 22 bytes plus up to 64 KiB of comment.
    const std::uint64_t tail = std::min<std::uint64_t>(length, 22 + 65535);
    std::vector<unsigned char> end(tail);
    file.seekg(std::streamoff(length - tail));
    if (!file.read(reinterpret_cast<char*>(end.data()), std::streamsize(tail))) Fail("The package is truncated");
    std::optional<std::size_t> record;
    for (std::size_t at = tail >= 22 ? tail - 22 : 0; tail >= 22; --at)
    {
        if (Read32(end.data() + at) == 0x06054b50) { record = at; break; }
        if (at == 0) break;
    }
    if (!record) Fail("The package is damaged (zip)");
    const unsigned char* eocd = end.data() + *record;
    const std::uint32_t count = Read16(eocd + 10), directory_size = Read32(eocd + 12), directory_at = Read32(eocd + 16);
    if (count == 0xFFFF || directory_at == 0xFFFFFFFFu || Read16(eocd + 4) || Read16(eocd + 6))
        Fail("Unsupported zip package (zip64 or multiple parts)");
    if (count > kMaxEntries || std::uint64_t(directory_at) + directory_size > length)
        Fail("The package is damaged (zip)");
    std::vector<unsigned char> directory(directory_size);
    file.seekg(directory_at);
    if (!file.read(reinterpret_cast<char*>(directory.data()), std::streamsize(directory.size())))
        Fail("The package is truncated");
    std::uint64_t total = 0;
    std::size_t at = 0;
    for (std::uint32_t n = 0; n < count; ++n)
    {
        if (cancelled && cancelled()) Fail("Cancelled");
        if (at + 46 > directory.size() || Read32(directory.data() + at) != 0x02014b50) Fail("The package is damaged (zip)");
        const unsigned char* entry = directory.data() + at;
        const std::uint32_t flags = Read16(entry + 8), method = Read16(entry + 10), crc = Read32(entry + 16);
        const std::uint32_t packed = Read32(entry + 20), size = Read32(entry + 24);
        const std::uint32_t name_length = Read16(entry + 28), extra = Read16(entry + 30), comment = Read16(entry + 32);
        const std::uint32_t attributes = Read32(entry + 38), local = Read32(entry + 42);
        if (at + 46 + name_length > directory.size()) Fail("The package is damaged (zip)");
        std::string name(reinterpret_cast<const char*>(entry + 46), name_length);
        at += 46 + name_length + extra + comment;
        if (flags & 1) Fail("The package is encrypted");
        // Unix modes in the upper half: links are refused.
        if (Read16(entry + 4) >> 8 == 3 && ((attributes >> 16) & 0170000) == 0120000)
            Fail("The package contains a link: " + name);
        if (!name.empty() && name.back() == '/')
        {
            name.pop_back();
            if (!name.empty()) fs::create_directories(destination / SafePath(name));
            continue;
        }
        if (method != 0 && method != 8) Fail("Unsupported compression in the package: " + name);
        unsigned char local_header[30];
        file.seekg(local);
        if (!file.read(reinterpret_cast<char*>(local_header), sizeof local_header) || Read32(local_header) != 0x04034b50)
            Fail("The package is damaged (zip)");
        const std::uint64_t data_at = std::uint64_t(local) + 30 + Read16(local_header + 26) + Read16(local_header + 28);
        if (data_at + packed > length) Fail("The package is truncated");
        file.seekg(std::streamoff(data_at));
        OutputFile output(destination / SafePath(name), total);
        uLong check = crc32(0, nullptr, 0);
        std::uint64_t written = 0;
        std::vector<char> in(1 << 16), out(1 << 16);
        z_stream inflater{};
        if (method == 8 && inflateInit2(&inflater, -MAX_WBITS) != Z_OK) Fail("Cannot start decompression");
        struct End
        {
            z_stream& stream;
            bool active;
            ~End() { if (active) inflateEnd(&stream); }
        } end_inflater{inflater, method == 8};
        std::uint64_t left = packed;
        bool done = method == 0 && packed == 0;
        while (!done)
        {
            const std::size_t step = std::size_t(std::min<std::uint64_t>(left, in.size()));
            if (step && !file.read(in.data(), std::streamsize(step))) Fail("The package is truncated");
            left -= step;
            if (method == 0)
            {
                output.Write(in.data(), step);
                check = crc32(check, reinterpret_cast<const Bytef*>(in.data()), uInt(step));
                written += step;
                done = left == 0;
                continue;
            }
            inflater.next_in = reinterpret_cast<Bytef*>(in.data());
            inflater.avail_in = uInt(step);
            do
            {
                inflater.next_out = reinterpret_cast<Bytef*>(out.data());
                inflater.avail_out = uInt(out.size());
                const int result = inflate(&inflater, Z_NO_FLUSH);
                if (result != Z_OK && result != Z_STREAM_END && !(result == Z_BUF_ERROR && left))
                    Fail("The package is damaged (deflate): " + name);
                const std::size_t produced = out.size() - inflater.avail_out;
                output.Write(out.data(), produced);
                check = crc32(check, reinterpret_cast<const Bytef*>(out.data()), uInt(produced));
                written += produced;
                if (result == Z_STREAM_END) done = true;
            } while (!done && inflater.avail_out == 0);
            if (!done && (inflater.avail_in != 0 || left == 0)) Fail("The package is damaged (deflate): " + name);
        }
        output.Close();
        if (written != size || std::uint32_t(check) != crc) Fail("The package is damaged (checksum): " + name);
    }
}

// The folder holding SOURCE-MANIFEST.json: the destination (zip) or its only
// subfolder (tar.gz packages wrap their files in one).
fs::path PackageRoot(const fs::path& destination)
{
    if (Exists(destination / kPackageManifest)) return destination;
    std::optional<fs::path> only;
    for (const auto& entry : fs::directory_iterator(destination))
    {
        if (only || !entry.is_directory()) Fail("The package has an unexpected layout");
        only = entry.path();
    }
    if (!only || !Exists(*only / kPackageManifest)) Fail("The package has no SOURCE-MANIFEST.json");
    return *only;
}

void VerifyPackage(const fs::path& root, const std::function<bool()>& cancelled)
{
    const auto manifest = ReadPackageManifest(root / kPackageManifest);
    std::set<std::string> listed;
    for (const auto& file : manifest.files)
    {
        if (cancelled && cancelled()) Fail("Cancelled");
        const fs::path path = root / SafePath(file.path);
        std::error_code error;
        if (!fs::is_regular_file(fs::symlink_status(path, error)) || fs::file_size(path, error) != file.bytes)
            Fail("The package is incomplete: " + file.path);
        if (platform::Sha256File(path) != file.sha256) Fail("The package is damaged: " + file.path);
#ifndef _WIN32
        fs::permissions(path, fs::perms(file.mode & 0777), fs::perm_options::replace);
#endif
        listed.insert(file.path);
    }
    listed.insert(kPackageManifest);
    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        if (entry.is_directory()) continue;
        const std::string relative = Generic(fs::relative(entry.path(), root));
        if (!entry.is_regular_file() || !listed.count(relative)) Fail("The package contains an unlisted file: " + relative);
    }
}

// Moves the old files out and the new ones in. Steps that are already done
// (a replacement an interrupted launcher left half done) are skipped, so the
// same pass also completes such a replacement. `log` records the moves.
void ApplyReplacement(const fs::path& base, const fs::path& staged, const fs::path& backup,
                      std::vector<std::pair<fs::path, fs::path>>* log,
                      const std::function<void(const std::string&)>& before_move)
{
    const fs::path incoming_manifest = staged / kPackageManifest;
    if (!Exists(incoming_manifest)) return; // the manifest moves last: nothing left to do
    const auto incoming = ReadPackageManifest(incoming_manifest);
    const fs::path installed_manifest = Exists(base / kPackageManifest) ? base / kPackageManifest
                                                                         : backup / kPackageManifest;
    const auto installed = ReadPackageManifest(installed_manifest);
    if (incoming.Package() != installed.Package())
        Fail("The download is the " + incoming.Package() + " package; this folder holds " + installed.Package());
    std::set<std::string> paths, fresh;
    for (const auto& file : installed.files) paths.insert(file.path);
    for (const auto& file : incoming.files)
    {
        paths.insert(file.path);
        fresh.insert(file.path);
    }
    std::vector<std::string> order(paths.begin(), paths.end());
    order.push_back(kPackageManifest);
    fresh.insert(kPackageManifest);
    auto move = [&](const fs::path& from, const fs::path& to) {
        fs::create_directories(to.parent_path());
        MovePath(from, to);
        if (log) log->emplace_back(from, to);
    };
    for (const auto& relative : order)
    {
        const fs::path path = SafePath(relative);
        const fs::path target = base / path, previous = backup / path, source = staged / path;
        const bool replace = fresh.count(relative) && Exists(source);
        if (!replace && fresh.count(relative)) continue; // already moved in
        if (before_move) before_move(relative);
        if (Exists(target))
        {
            if (fs::is_directory(fs::symlink_status(target))) Fail("A folder is in the way of " + relative);
            if (Exists(previous)) Fail("Cannot keep the previous " + relative);
            move(target, previous);
        }
        if (replace) move(source, target);
    }
}

fs::path UpdatePath(const fs::path& base, const std::string& name) { return UpdateFolder(base) / PathFromUtf8(name); }
} // namespace

PackageManifest ReadPackageManifest(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) Fail("Cannot read " + PathUtf8(file));
    std::stringstream text;
    text << stream.rdbuf();
    const auto json = platform::ParseJson(text.str());
    PackageManifest manifest;
    if (json["format"].Integer().value_or(0) != 1) Fail("Unknown package manifest format in " + PathUtf8(file));
    manifest.version = json["version"].Text();
    manifest.platform = json["platform"].Text();
    manifest.architecture = json["architecture"].Text();
    const auto& files = json["files"];
    if (manifest.version.empty() || manifest.platform.empty() || manifest.architecture.empty() || !files.IsObject()
        || files.size() == 0)
        Fail("Incomplete package manifest in " + PathUtf8(file));
    for (std::size_t n = 0; n < files.size(); ++n)
    {
        const auto& entry = files.Items()[n];
        PackageFile item;
        item.path = files.Names()[n];
        SafePath(item.path);
        if (item.path == kPackageManifest || item.path.rfind(".update/", 0) == 0)
            Fail("Invalid file in the package manifest: " + item.path);
        item.sha256 = entry["sha256"].Text();
        item.bytes = std::uint64_t(std::max<std::int64_t>(entry["bytes"].Integer().value_or(-1), 0));
        if (item.sha256.size() != 64 || !entry["bytes"].Integer()) Fail("Invalid entry in the package manifest: " + item.path);
        item.mode = entry["mode"].IsString() ? ParseMode(entry["mode"].Text()) : 0644;
        manifest.files.push_back(std::move(item));
    }
    return manifest;
}

fs::path UnpackPackage(const fs::path& archive, const fs::path& destination, const std::function<bool()>& cancelled)
{
    if (Exists(destination)) Fail("The unpack folder exists already: " + PathUtf8(destination));
    fs::create_directories(destination);
    const std::string name = PathUtf8(archive.filename());
    if (name.size() > 4 && name.substr(name.size() - 4) == ".zip") UnpackZip(archive, destination, cancelled);
    else if (name.size() > 7 && name.substr(name.size() - 7) == ".tar.gz") UnpackTar(archive, destination, cancelled);
    else Fail("Unknown package type: " + name);
    const fs::path root = PackageRoot(destination);
    VerifyPackage(root, cancelled);
    return root;
}

void ReplacePackage(const fs::path& base, const fs::path& staged, const fs::path& backup,
                    const std::function<void(const std::string&)>& before_move)
{
    std::vector<std::pair<fs::path, fs::path>> moves;
    try
    {
        ApplyReplacement(base, staged, backup, &moves, before_move);
    }
    catch (...)
    {
        // Every move back, newest first; one that fails does not stop the rest.
        for (auto it = moves.rbegin(); it != moves.rend(); ++it)
        {
            try { MovePath(it->second, it->first); }
            catch (...) {}
        }
        throw;
    }
}

fs::path UpdateFolder(const fs::path& base) { return base / ".update"; }

std::string InstallBlocker(const fs::path& base)
{
    if (!Exists(base / kPackageManifest)) return "This copy was not unpacked from a release package, so it cannot update itself.";
    try { ReadPackageManifest(base / kPackageManifest); }
    catch (const std::exception& error) { return std::string("The package's file list cannot be read: ") + error.what() + "."; }
    std::error_code error;
    const fs::path folder = UpdateFolder(base);
    fs::create_directories(folder, error);
    const fs::path probe = folder / "write-test";
    if (error || !std::ofstream(probe).good()) return "The game folder is read-only: " + PathUtf8(base) + ".";
    fs::remove(probe, error);
    if (fs::is_empty(folder, error)) fs::remove(folder, error);
    if (const auto reason = platform::HttpUnavailable(); !reason.empty())
        return "Downloads are not available here: " + reason + ".";
    return {};
}

void InstallStagedPackage(const fs::path& base, const fs::path& staged, const fs::path& backup)
{
    const fs::path marker = UpdatePath(base, kReplacingMarker);
    {
        std::ofstream stream(marker, std::ios::binary | std::ios::trunc);
        stream << Generic(fs::relative(staged, UpdateFolder(base))) << '\n'
               << Generic(fs::relative(backup, UpdateFolder(base))) << '\n';
        if (!stream.flush()) Fail("Cannot write " + PathUtf8(marker));
    }
    try
    {
        ReplacePackage(base, staged, backup);
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove(marker, ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove(marker, ignored);
}

std::string FinishUpdates(const fs::path& base)
{
    const fs::path folder = UpdateFolder(base);
    std::error_code error;
    if (!fs::is_directory(folder, error)) return {};
    // A replacement that stopped half way (the launcher was killed) is completed.
    const fs::path marker = folder / kReplacingMarker;
    if (Exists(marker))
    {
        std::string staged, backup;
        {
            // Closed before the marker is removed (Windows cannot delete open files).
            std::ifstream stream(marker);
            std::getline(stream, staged);
            std::getline(stream, backup);
        }
        try
        {
            ApplyReplacement(base, UpdatePath(base, PathUtf8(SafePath(staged))), UpdatePath(base, PathUtf8(SafePath(backup))),
                             nullptr, {});
            fs::remove(marker, error);
        }
        catch (const std::exception&)
        {
            return {}; // kept for the next start; the folder stays as it is
        }
    }
    std::string replaced;
    for (const auto& entry : fs::directory_iterator(folder, error))
    {
        const std::string name = PathUtf8(entry.path().filename());
        if (name.rfind("previous-", 0) == 0)
        {
            try { replaced = ReadPackageManifest(entry.path() / kPackageManifest).version; }
            catch (const std::exception&) {}
            fs::remove_all(entry.path(), error);
        }
        else if (name.rfind("staged-", 0) == 0 || name.rfind("download-", 0) == 0)
            fs::remove_all(entry.path(), error);
    }
    if (fs::is_empty(folder, error)) fs::remove(folder, error);
    return replaced;
}

fs::path InstalledLauncher(const fs::path& base)
{
#ifdef _WIN32
    const char* const names[] = {"recharged.exe", "mscharged.exe"};
#else
    const char* const names[] = {"recharged", "mscharged"};
#endif
    for (const char* name : names)
        if (Exists(base / name)) return base / name;
    return base / names[0];
}

std::string RestartLauncher(const fs::path& executable, int argc, char** argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::wstring command = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process))
        return std::system_category().message(int(GetLastError()));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return {};
#else
    std::vector<char*> arguments(argv, argv + argc);
    arguments.push_back(nullptr);
    execv(executable.c_str(), arguments.data());
    return std::strerror(errno);
#endif
}
} // namespace mscharged::launcher

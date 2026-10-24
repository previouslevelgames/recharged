// The Wii disc reader that stands in for nod where Rust is not available (the
// Switch): synthetic encrypted Wii discs, made here with a separate AES-128
// encryption, as ISO and WBFS images (split ones too) and as RVZ (every codec)
// and WIA images, written here from the format description with junk packed
// as generator seeds, read back through nod's C API. Checks FST iteration and
// lookup, file, partition and disc reads and seeks across cluster, block, group
// and file boundaries, the cache probe, metadata, concurrent readers and the
// error paths. Contains no game data.
#include <nod.h>

#include "platform/wii_disc/aes128.h"
#include "platform/wii_disc/lfg.h"

#include <bzlib.h>
#include <lzma.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string Message()
{
    const char* message = nod_error_message();
    return message ? message : "";
}

void RequireError(const std::string& expected, const std::string& context)
{
    Require(Message().find(expected) != std::string::npos,
            context + ": message \"" + Message() + "\" lacks \"" + expected + "\"");
}

Bytes Hex(const char* text)
{
    Bytes out;
    for (; text[0] && text[1]; text += 2) out.push_back(std::uint8_t(std::stoi(std::string(text, 2), nullptr, 16)));
    return out;
}

std::uint64_t Next(std::uint64_t& state)
{
    // splitmix64
    std::uint64_t z = (state += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

Bytes Random(std::size_t size, std::uint64_t seed)
{
    Bytes out(size);
    for (auto& byte : out) byte = std::uint8_t(Next(seed) >> 56);
    return out;
}

void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value)
{
    if (bytes.size() < offset + 4) bytes.resize(offset + 4);
    for (int i = 0; i < 4; ++i) bytes[offset + i] = std::uint8_t(value >> (24 - 8 * i));
}

void Put16(Bytes& bytes, std::size_t offset, std::uint16_t value)
{
    if (bytes.size() < offset + 2) bytes.resize(offset + 2);
    bytes[offset] = std::uint8_t(value >> 8);
    bytes[offset + 1] = std::uint8_t(value);
}

void Put64(Bytes& bytes, std::size_t offset, std::uint64_t value)
{
    Put32(bytes, offset, std::uint32_t(value >> 32));
    Put32(bytes, offset + 4, std::uint32_t(value));
}

void Append32(Bytes& bytes, std::uint32_t value) { Put32(bytes, bytes.size(), value); }

Bytes Slice(const Bytes& bytes, std::uint64_t begin, std::uint64_t size)
{
    begin = std::min<std::uint64_t>(begin, bytes.size());
    size = std::min<std::uint64_t>(size, bytes.size() - begin);
    return Bytes(bytes.begin() + std::ptrdiff_t(begin), bytes.begin() + std::ptrdiff_t(begin + size));
}

void Put(Bytes& bytes, std::size_t offset, const Bytes& part)
{
    if (bytes.size() < offset + part.size()) bytes.resize(offset + part.size());
    std::copy(part.begin(), part.end(), bytes.begin() + std::ptrdiff_t(offset));
}

// AES-128 encryption, byte by byte as FIPS-197 describes it, independent of
// the reader's table-based decryption (both checked against known answers).
class Aes128Encryptor
{
public:
    explicit Aes128Encryptor(const std::uint8_t* key)
    {
        std::copy(key, key + 16, round_keys_);
        std::uint8_t rcon = 1;
        for (int i = 16; i < 176; i += 4)
        {
            std::uint8_t t[4] = {round_keys_[i - 4], round_keys_[i - 3], round_keys_[i - 2], round_keys_[i - 1]};
            if (i % 16 == 0)
            {
                const std::uint8_t first = t[0];
                t[0] = SBox(t[1]) ^ rcon;
                t[1] = SBox(t[2]);
                t[2] = SBox(t[3]);
                t[3] = SBox(first);
                rcon = Times2(rcon);
            }
            for (int j = 0; j < 4; ++j) round_keys_[i + j] = round_keys_[i - 16 + j] ^ t[j];
        }
    }

    void EncryptBlock(std::uint8_t* block) const
    {
        AddRoundKey(block, 0);
        for (int round = 1; round <= 10; ++round)
        {
            for (int i = 0; i < 16; ++i) block[i] = SBox(block[i]);
            std::uint8_t shifted[16];
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    shifted[row + 4 * column] = block[row + 4 * ((column + row) % 4)];
            std::copy(shifted, shifted + 16, block);
            if (round != 10)
                for (int column = 0; column < 4; ++column)
                {
                    std::uint8_t* a = block + 4 * column;
                    const std::uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                    a[0] = Times2(a0) ^ Times3(a1) ^ a2 ^ a3;
                    a[1] = a0 ^ Times2(a1) ^ Times3(a2) ^ a3;
                    a[2] = a0 ^ a1 ^ Times2(a2) ^ Times3(a3);
                    a[3] = Times3(a0) ^ a1 ^ a2 ^ Times2(a3);
                }
            AddRoundKey(block, round);
        }
    }

    void EncryptCbc(const std::uint8_t* iv, std::uint8_t* data, std::size_t size) const
    {
        std::uint8_t previous[16];
        std::copy(iv, iv + 16, previous);
        for (std::size_t offset = 0; offset < size; offset += 16)
        {
            for (int i = 0; i < 16; ++i) data[offset + i] ^= previous[i];
            EncryptBlock(data + offset);
            std::copy(data + offset, data + offset + 16, previous);
        }
    }

private:
    static std::uint8_t Times2(std::uint8_t v) { return std::uint8_t(v << 1) ^ (v & 0x80 ? 0x1b : 0); }
    static std::uint8_t Times3(std::uint8_t v) { return Times2(v) ^ v; }
    static std::uint8_t Multiply(std::uint8_t a, std::uint8_t b)
    {
        std::uint8_t product = 0;
        for (int i = 0; i < 8; ++i, b >>= 1, a = Times2(a))
            if (b & 1) product ^= a;
        return product;
    }
    // The inverse as v^254, then the affine transform; tabulated once.
    static std::uint8_t SBox(std::uint8_t v)
    {
        static const auto table = [] {
            std::array<std::uint8_t, 256> sbox{};
            for (int value = 0; value < 256; ++value)
            {
                std::uint8_t inverse = 1, base = std::uint8_t(value);
                for (int e = 254; e; e >>= 1, base = Multiply(base, base))
                    if (e & 1) inverse = Multiply(inverse, base);
                std::uint8_t s = 0x63, x = inverse;
                for (int i = 0; i < 5; ++i, x = std::uint8_t(x << 1 | x >> 7)) s ^= x;
                sbox[std::size_t(value)] = s;
            }
            return sbox;
        }();
        return table[v];
    }
    void AddRoundKey(std::uint8_t* block, int round) const
    {
        for (int i = 0; i < 16; ++i) block[i] ^= round_keys_[16 * round + i];
    }

    std::uint8_t round_keys_[176];
};

void CheckAes()
{
    using mscharged::platform::wii_disc::Aes128Decryptor;
    // FIPS-197 appendix C.1.
    const Bytes key = Hex("000102030405060708090a0b0c0d0e0f");
    Bytes block = Hex("00112233445566778899aabbccddeeff");
    Aes128Encryptor(key.data()).EncryptBlock(block.data());
    Require(block == Hex("69c4e0d86a7b0430d8cdb78070b4c55a"), "Test AES encryption fails FIPS-197 C.1");
    const std::uint8_t zero[16] = {};
    Aes128Decryptor(key.data()).DecryptCbc(zero, block.data(), block.data(), 16);
    Require(block == Hex("00112233445566778899aabbccddeeff"), "Reader AES decryption fails FIPS-197 C.1");
    // NIST SP 800-38A F.2.1/F.2.2, CBC-AES128.
    const Bytes cbc_key = Hex("2b7e151628aed2a6abf7158809cf4f3c"), iv = Hex("000102030405060708090a0b0c0d0e0f");
    const Bytes plain = Hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                            "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    const Bytes cipher = Hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                             "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    Bytes data = plain;
    Aes128Encryptor(cbc_key.data()).EncryptCbc(iv.data(), data.data(), data.size());
    Require(data == cipher, "Test AES-CBC encryption fails SP 800-38A F.2.1");
    Bytes out(cipher.size());
    Aes128Decryptor(cbc_key.data()).DecryptCbc(iv.data(), cipher.data(), out.data(), out.size());
    Require(out == plain, "Reader AES-CBC decryption fails SP 800-38A F.2.2");
    // Lengths around the reader's four-block groups, in and out of place.
    for (const std::size_t blocks : {1, 3, 4, 5, 8, 9, 64, 67, 1984})
    {
        const Bytes original = Random(blocks * 16, blocks), round_key = Random(16, blocks + 99),
                    vector = Random(16, blocks + 7);
        Bytes encrypted = original;
        Aes128Encryptor(round_key.data()).EncryptCbc(vector.data(), encrypted.data(), encrypted.size());
        Bytes separate(encrypted.size()), in_place = encrypted;
        const Aes128Decryptor decryptor(round_key.data());
        decryptor.DecryptCbc(vector.data(), encrypted.data(), separate.data(), separate.size());
        decryptor.DecryptCbc(vector.data(), in_place.data(), in_place.data(), in_place.size());
        Require(separate == original && in_place == original, "AES-CBC round trip failed at " + std::to_string(blocks));
    }
}

// SHA-1 for the hashes WIA and RVZ keep of their headers.
Bytes Sha1(const Bytes& message)
{
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    Bytes padded = message;
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) padded.push_back(0);
    Put64(padded, padded.size(), std::uint64_t(message.size()) * 8);
    const auto rol = [](std::uint32_t x, int n) { return x << n | x >> (32 - n); };
    for (std::size_t block = 0; block < padded.size(); block += 64)
    {
        std::uint32_t w[80];
        for (int i = 0; i < 80; ++i)
            w[i] = i < 16 ? std::uint32_t(padded[block + 4 * i]) << 24 | std::uint32_t(padded[block + 4 * i + 1]) << 16
                    | std::uint32_t(padded[block + 4 * i + 2]) << 8 | padded[block + 4 * i + 3]
                          : rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t v[5] = {h[0], h[1], h[2], h[3], h[4]};
        for (int i = 0; i < 80; ++i)
        {
            const int round = i / 20;
            const std::uint32_t f = round == 0 ? (v[1] & v[2]) | (~v[1] & v[3])
                : round == 2              ? (v[1] & v[2]) | (v[1] & v[3]) | (v[2] & v[3])
                                          : v[1] ^ v[2] ^ v[3];
            const std::uint32_t k[4] = {0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6};
            const std::uint32_t t = rol(v[0], 5) + f + v[4] + k[round] + w[i];
            v[4] = v[3];
            v[3] = v[2];
            v[2] = rol(v[1], 30);
            v[1] = v[0];
            v[0] = t;
        }
        for (int i = 0; i < 5; ++i) h[i] += v[i];
    }
    Bytes digest;
    for (const std::uint32_t word : h) Append32(digest, word);
    return digest;
}

// The junk generator again, written as Dolphin describes it (raw words, the
// second output byte taken from bits 18-25 when output) rather than as the
// reader keeps it.
std::array<std::uint32_t, 17> JunkSeed(const char* disc_id, std::uint8_t disc_num, std::uint32_t sector)
{
    const auto byte = [disc_id](int i) { return std::uint8_t(disc_id[i]); };
    const std::uint32_t key = (std::uint32_t(byte(2)) << 24 | std::uint32_t(byte(1)) << 16
                               | std::uint32_t(std::uint8_t(byte(3) + byte(2))) << 8 | std::uint8_t(byte(0) + byte(1)))
        ^ disc_num;
    std::uint32_t n = key * 0x260BCD5u ^ sector * 0x1EF29123u;
    std::array<std::uint32_t, 17> seed{};
    for (std::uint32_t& word : seed)
        for (int bit = 0; bit < 32; ++bit)
        {
            n = n * 0x5D588B65u + 1;
            word = word >> 1 | (n & 0x80000000u);
        }
    seed[16] ^= seed[0] >> 9 ^ seed[16] << 23;
    return seed;
}

Bytes JunkStream(const std::array<std::uint32_t, 17>& seed, std::size_t skip, std::size_t size)
{
    std::vector<std::uint32_t> words(521);
    std::copy(seed.begin(), seed.end(), words.begin());
    for (std::size_t i = 17; i < 521; ++i) words[i] = words[i - 17] << 23 ^ words[i - 16] >> 9 ^ words[i - 1];
    const auto step = [&words] {
        for (std::size_t i = 0; i < 521; ++i) words[i] ^= words[i < 32 ? i + 489 : i - 32];
    };
    for (int i = 0; i < 4; ++i) step();
    Bytes out;
    for (std::size_t position = 0; out.size() < size; ++position)
    {
        if (position && position % 2084 == 0) step();
        const std::uint32_t word = words[position % 2084 / 4];
        const std::uint8_t value[4] = {std::uint8_t(word >> 24), std::uint8_t(word >> 18), std::uint8_t(word >> 8),
                                       std::uint8_t(word)};
        if (position >= skip) out.push_back(value[position % 4]);
    }
    return out;
}

// Junk as a disc has it at `offset`: restarted every 0x8000 bytes.
Bytes Junk(const char* disc_id, std::uint8_t disc_num, std::uint64_t offset, std::size_t size)
{
    Bytes out;
    while (out.size() < size)
    {
        const std::uint64_t at = offset + out.size();
        const std::size_t count = std::min<std::size_t>(size - out.size(), 0x8000u - at % 0x8000u);
        const Bytes part = JunkStream(JunkSeed(disc_id, disc_num, std::uint32_t(at / 0x8000u)), at % 0x8000u, count);
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

void CheckSha1AndJunk()
{
    const auto text = [](const char* s) { return Bytes(s, s + std::strlen(s)); };
    Require(Sha1({}) == Hex("da39a3ee5e6b4b0d3255bfef95601890afd80709") && Sha1(text("abc")) == Hex("a9993e364706816aba3e25717850c26c9cd0d89d")
                && Sha1(text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
                       == Hex("84983e441c3bd26ebaae4aa1f95129e5e54670f1"),
            "Test SHA-1 fails its known answers");
    // nod's own expected junk (util/lfg.rs tests), the last across a 0x8000 edge.
    struct Answer
    {
        const char* id;
        std::uint64_t offset;
        const char* bytes;
    };
    for (const Answer& answer : {Answer{"GALE", 0x600000, "e94767bd41504d5d6148b199a0120cba"},
                                 Answer{"GALE", 0x608000, "e2bbbd77dab222421c0c0bfcac06ead0"},
                                 Answer{"GPIE", 0x322904, "97d8230b12aa2045c2bd718c3032c52f"},
                                 Answer{"GM8E", 0x27FF0,
                                        "ad6f21be055710edeab08efd9158a20edc0d59c00298a500395b68a65d532db6"}})
    {
        const Bytes expected = Hex(answer.bytes);
        Require(Junk(answer.id, 0, answer.offset, expected.size()) == expected,
                std::string("Test junk generator differs from nod's for ") + answer.id);
        // The reader's generator, seeded as RVZ seeds it.
        using mscharged::platform::wii_disc::LaggedFibonacci;
        Bytes reader;
        while (reader.size() < expected.size())
        {
            const std::uint64_t at = answer.offset + reader.size();
            std::uint32_t seed[LaggedFibonacci::kSeedWords];
            LaggedFibonacci::GenerateSeed(reinterpret_cast<const std::uint8_t*>(answer.id), 0,
                                          std::uint32_t(at / 0x8000u), seed);
            LaggedFibonacci lfg;
            lfg.Init(seed);
            lfg.Skip(at % 0x8000u);
            Bytes part(std::min<std::size_t>(expected.size() - reader.size(), 0x8000u - at % 0x8000u));
            lfg.Fill(part.data(), part.size());
            reader.insert(reader.end(), part.begin(), part.end());
        }
        Require(reader == expected, std::string("Reader junk generator differs from nod's for ") + answer.id);
    }
    // Long runs (many generator steps) agree between the two formulations.
    using mscharged::platform::wii_disc::LaggedFibonacci;
    const auto seed = JunkSeed("RWDE", 1, 77);
    LaggedFibonacci lfg;
    lfg.Init(seed.data());
    lfg.Skip(1234);
    Bytes reader(0x8000u - 1234);
    lfg.Fill(reader.data(), 3000);
    lfg.Fill(reader.data() + 3000, reader.size() - 3000);
    Require(reader == JunkStream(seed, 1234, reader.size()), "The two junk generators differ over a sector");
}

// ---- Synthetic discs ----

// The common keys nod knows (disc/wii.rs): retail, Korean; debug.
const Bytes kRetailKey = Hex("ebe42a225e8593e448d9c5457381aaf7");
const Bytes kKoreanKey = Hex("63b82bb4f4614e2e13f2fefbba4c9b7e");
const Bytes kDebugKey = Hex("a1604a6a7123b529ae8bec32c816fcaa");
constexpr const char* kRetailIssuer = "Root-CA00000001-XS00000003";
constexpr const char* kDebugIssuer = "Root-CA00000002-XS00000006";
constexpr std::uint32_t kSector = 0x8000;
constexpr std::uint32_t kSectorData = 0x7C00;

struct Tree
{
    std::string name;
    bool directory = false;
    Bytes data;
    std::uint64_t offset = 0; // a file's place in the partition data; 0: next free
    std::vector<Tree> children;
};

Tree File(std::string name, Bytes data, std::uint64_t offset = 0)
{
    Tree tree;
    tree.name = std::move(name);
    tree.data = std::move(data);
    tree.offset = offset;
    return tree;
}

Tree Directory(std::string name, std::vector<Tree> children)
{
    Tree tree;
    tree.name = std::move(name);
    tree.directory = true;
    tree.children = std::move(children);
    return tree;
}

// One FST node as the fixture wrote it.
struct Node
{
    std::string path, name;
    bool directory;
    std::uint32_t parent_or_offset, length; // as stored: offset >> 2 / parent; size / end
    Bytes data; // a file's contents
    std::uint64_t offset = 0;
};

struct PartitionSpec
{
    std::uint32_t kind = 0;
    std::string issuer = kRetailIssuer;
    std::uint8_t key_index = 0;
    const Bytes* common_key = &kRetailKey;
    std::uint64_t disc_offset = 0;
    Tree root;
    std::uint32_t extra_clusters = 0; // unused data after the files
    std::uint64_t seed = 1;
};

struct BuiltPartition
{
    PartitionSpec spec;
    Bytes data; // decrypted partition data, whole clusters
    Bytes header, tmd, cert, h3, boot, bi2, apploader, dol, fst;
    std::vector<Node> nodes;
    std::uint32_t clusters = 0, cluster_data = kSectorData;
    std::uint64_t data_offset = 0; // on the disc
    std::uint32_t used_clusters = 0;
    std::uint64_t junk_begin = 0; // the unused data after the files is junk
    Bytes title_key;
};

struct BuiltDisc
{
    Bytes iso;
    std::vector<BuiltPartition> partitions;
    std::uint64_t junk_tail = 0; // junk from here to the disc's end
};

void Flatten(const Tree& tree, std::uint32_t parent, const std::string& prefix, std::vector<Node>& nodes)
{
    const std::uint32_t index = std::uint32_t(nodes.size());
    const std::string path = prefix.empty() ? tree.name : prefix + "/" + tree.name;
    Node node{path, tree.name, tree.directory, tree.directory ? parent : 0, 0, {}, tree.offset};
    if (!tree.directory)
    {
        node.length = std::uint32_t(tree.data.size());
        node.data = tree.data;
    }
    nodes.push_back(node);
    if (tree.directory)
    {
        for (const Tree& child : tree.children) Flatten(child, index, path, nodes);
        nodes[index].length = std::uint32_t(nodes.size());
    }
}

BuiltPartition BuildPartition(const PartitionSpec& spec, const Bytes& disc_header, bool hashed)
{
    BuiltPartition built;
    built.spec = spec;
    built.cluster_data = hashed ? kSectorData : kSector;
    Bytes& data = built.data;
    // boot.bin: a copy of the disc header and the boot block (offsets >> 2).
    built.boot.assign(disc_header.begin(), disc_header.begin() + 0x400);
    built.boot.resize(0x440);
    built.bi2 = Random(0x2000, spec.seed * 11);
    built.apploader = Random(0x20 + 0x1000 + 0x100, spec.seed * 13);
    std::memcpy(built.apploader.data(), "2026/10/23", 10);
    Put32(built.apploader, 0x14, 0x1000);
    Put32(built.apploader, 0x18, 0x100);
    // main.dol: a text section and a data section that ends last.
    built.dol = Random(0xC34, spec.seed * 17);
    std::fill(built.dol.begin(), built.dol.begin() + 0x100, 0);
    Put32(built.dol, 0x00, 0x100);
    Put32(built.dol, 0x90, 0x900);
    Put32(built.dol, 0x1C, 0xA00);
    Put32(built.dol, 0xAC, 0x234);
    const std::uint64_t dol_offset = 0x3580;
    // The FST, then the files.
    built.nodes.clear();
    Flatten(spec.root, 0, "", built.nodes);
    built.nodes[0].path = "";
    Bytes strings;
    std::vector<std::uint32_t> name_offsets(built.nodes.size(), 0);
    for (std::size_t i = 1; i < built.nodes.size(); ++i)
    {
        name_offsets[i] = std::uint32_t(strings.size());
        strings.insert(strings.end(), built.nodes[i].name.begin(), built.nodes[i].name.end());
        strings.push_back(0);
    }
    const std::uint64_t fst_offset = (dol_offset + built.dol.size() + 31) & ~std::uint64_t(31);
    const std::size_t fst_size = (built.nodes.size() * 12 + strings.size() + 3) & ~std::size_t(3);
    std::uint64_t cursor = (fst_offset + fst_size + 31) & ~std::uint64_t(31);
    for (Node& node : built.nodes)
    {
        if (node.directory) continue;
        if (node.offset)
        {
            Require(node.offset >= cursor && node.offset % 4 == 0, "Fixture file offset overlaps: " + node.path);
            cursor = node.offset;
        }
        node.offset = cursor;
        node.parent_or_offset = std::uint32_t(cursor >> 2);
        cursor = (cursor + node.data.size() + 3) & ~std::uint64_t(3);
    }
    built.fst.assign(fst_size, 0);
    for (std::size_t i = 0; i < built.nodes.size(); ++i)
    {
        const Node& node = built.nodes[i];
        Put32(built.fst, i * 12, (node.directory ? 0x01000000u : 0u) | name_offsets[i]);
        Put32(built.fst, i * 12 + 4, node.parent_or_offset);
        Put32(built.fst, i * 12 + 8, node.length);
    }
    std::copy(strings.begin(), strings.end(), built.fst.begin() + std::ptrdiff_t(built.nodes.size() * 12));
    Put32(built.boot, 0x420, std::uint32_t(dol_offset >> 2));
    Put32(built.boot, 0x424, std::uint32_t(fst_offset >> 2));
    Put32(built.boot, 0x428, std::uint32_t(fst_size >> 2));
    Put32(built.boot, 0x42C, std::uint32_t(fst_size >> 2));
    Put(data, 0, built.boot);
    Put(data, 0x440, built.bi2);
    Put(data, 0x2440, built.apploader);
    Put(data, dol_offset, built.dol);
    Put(data, fst_offset, built.fst);
    for (const Node& node : built.nodes)
        if (!node.directory) Put(data, node.offset, node.data);
    built.used_clusters = std::uint32_t((data.size() + built.cluster_data - 1) / built.cluster_data);
    built.clusters = built.used_clusters + spec.extra_clusters;
    const std::size_t used = data.size();
    data.resize(std::size_t(built.clusters) * built.cluster_data);
    // Unused space holds junk, as on discs; RVZ stores it as generator seeds.
    built.junk_begin = used;
    const Bytes junk = Junk("RWDE", 0, used, data.size() - used);
    std::copy(junk.begin(), junk.end(), data.begin() + std::ptrdiff_t(used));

    // The partition header: ticket, then the TMD, certificates, H3 and data.
    const Bytes title_key = Random(16, spec.seed * 23), title_id = Random(8, spec.seed * 29);
    built.title_key = title_key;
    built.header.assign(0x2C0, 0);
    Put32(built.header, 0, 0x00010001);
    std::memcpy(built.header.data() + 0x140, spec.issuer.data(), std::min<std::size_t>(spec.issuer.size(), 64));
    Bytes encrypted_key = title_key;
    std::uint8_t iv[16] = {};
    std::copy(title_id.begin(), title_id.end(), iv);
    Aes128Encryptor(spec.common_key->data()).EncryptCbc(iv, encrypted_key.data(), 16);
    Put(built.header, 0x1BF, encrypted_key);
    Put(built.header, 0x1DC, title_id);
    built.header[0x1F1] = spec.key_index;
    built.tmd = Random(0x208, spec.seed * 31);
    Put32(built.tmd, 0, 0x00010001);
    Put(built.tmd, 0x18C, title_id);
    built.cert = Random(0xA00, spec.seed * 37);
    built.h3 = Random(0x18000, spec.seed * 41);
    Put32(built.header, 0x2A4, std::uint32_t(built.tmd.size()));
    Put32(built.header, 0x2A8, 0x2C0 >> 2);
    Put32(built.header, 0x2AC, std::uint32_t(built.cert.size()));
    Put32(built.header, 0x2B0, (0x2C0 + 0x208) >> 2);
    Put32(built.header, 0x2B4, 0x8000 >> 2);
    Put32(built.header, 0x2B8, 0x20000 >> 2);
    Put32(built.header, 0x2BC, std::uint32_t(std::uint64_t(built.clusters) * kSector >> 2));
    built.data_offset = spec.disc_offset + 0x20000;
    return built;
}

// Encrypts the partition's clusters as a Wii disc stores them.
Bytes PartitionSectors(const BuiltPartition& built, bool encrypted, bool hashed)
{
    Bytes raw(std::size_t(built.clusters) * kSector);
    const Bytes title_key = Random(16, built.spec.seed * 23);
    const Aes128Encryptor aes(title_key.data());
    for (std::uint32_t cluster = 0; cluster < built.clusters; ++cluster)
    {
        std::uint8_t* sector = raw.data() + std::size_t(cluster) * kSector;
        const std::uint8_t* plain = built.data.data() + std::size_t(cluster) * built.cluster_data;
        if (!hashed)
        {
            std::memcpy(sector, plain, kSector);
            continue;
        }
        const Bytes hashes = Random(0x400, built.spec.seed * 1000 + cluster); // stands in for the H0-H2 hashes
        std::memcpy(sector, hashes.data(), 0x400);
        std::memcpy(sector + 0x400, plain, kSectorData);
        if (!encrypted) continue;
        const std::uint8_t zero[16] = {};
        aes.EncryptCbc(zero, sector, 0x400);
        aes.EncryptCbc(sector + 0x3D0, sector + 0x400, kSectorData);
    }
    return raw;
}

BuiltDisc BuildDisc(const std::vector<PartitionSpec>& specs, bool encrypted = true, bool hashed = true)
{
    BuiltDisc disc;
    Bytes header(0x440, 0);
    std::memcpy(header.data(), "RWDE01", 6);
    header[6] = 0;
    header[7] = 3;
    header[8] = 1;
    header[9] = 2;
    Put(header, 0x18, Hex("5d1c9ea3"));
    const std::string title = "Wii disc reader fixture";
    std::memcpy(header.data() + 0x20, title.data(), title.size());
    header[0x60] = hashed ? 0 : 1;
    header[0x61] = encrypted ? 0 : 1;
    Bytes& iso = disc.iso;
    Put(iso, 0, header);
    Put32(iso, 0x40000, std::uint32_t(specs.size()));
    Put32(iso, 0x40004, 0x40020 >> 2);
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        Put32(iso, 0x40020 + i * 8, std::uint32_t(specs[i].disc_offset >> 2));
        Put32(iso, 0x40024 + i * 8, specs[i].kind);
    }
    Put(iso, 0x4E000, Random(0x20, 5));
    for (const PartitionSpec& spec : specs)
    {
        disc.partitions.push_back(BuildPartition(spec, header, hashed));
        const BuiltPartition& built = disc.partitions.back();
        Put(iso, spec.disc_offset, built.header);
        Put(iso, spec.disc_offset + 0x2C0, built.tmd);
        Put(iso, spec.disc_offset + 0x2C0 + 0x208, built.cert);
        Put(iso, spec.disc_offset + 0x8000, built.h3);
        Put(iso, built.data_offset, PartitionSectors(built, encrypted, hashed));
    }
    // Junk after the partitions, ending inside a sector.
    disc.junk_tail = iso.size();
    const Bytes tail = Junk("RWDE", 0, iso.size(), 3 * kSector + 0x123);
    iso.insert(iso.end(), tail.begin(), tail.end());
    return disc;
}

// A WBFS with 512-byte sectors; disc blocks in `left_out` (and empty ones past
// the disc) get no image block. Image blocks are assigned out of order.
Bytes MakeWbfs(const Bytes& iso, unsigned block_shift, const std::set<std::uint32_t>& left_out)
{
    const std::uint32_t block = 1u << block_shift;
    const std::uint32_t max_blocks = (143432 * 2) >> (block_shift - 15);
    const std::uint32_t disc_blocks = std::uint32_t((iso.size() + block - 1) / block);
    std::vector<std::uint32_t> kept;
    for (std::uint32_t b = 0; b < disc_blocks; ++b)
        if (!left_out.count(b)) kept.push_back(b);
    // Reverse the stored order, block 0 stays first: no two neighbours adjacent.
    std::reverse(kept.begin() + 1, kept.end());
    Bytes image(std::size_t(1 + kept.size()) * block, 0);
    std::memcpy(image.data(), "WBFS", 4);
    Put32(image, 4, std::uint32_t(image.size() / 512));
    image[8] = 9;
    image[9] = std::uint8_t(block_shift);
    image[10] = 1;
    image[12] = 1; // one disc in the first slot
    std::copy(iso.begin(), iso.begin() + 0x100, image.begin() + 512);
    Require(512 + 0x100 + max_blocks * 2 <= block, "WBFS fixture block map does not fit its first block");
    for (std::size_t i = 0; i < kept.size(); ++i)
    {
        const std::uint32_t disc_block = kept[i];
        const std::uint32_t stored = std::uint32_t(i + 1);
        image[512 + 0x100 + disc_block * 2] = std::uint8_t(stored >> 8);
        image[512 + 0x100 + disc_block * 2 + 1] = std::uint8_t(stored);
        const std::size_t begin = std::size_t(disc_block) * block;
        const std::size_t count = std::min<std::size_t>(block, iso.size() - begin);
        std::copy(iso.begin() + std::ptrdiff_t(begin), iso.begin() + std::ptrdiff_t(begin + count),
                  image.begin() + std::ptrdiff_t(std::size_t(stored) * block));
    }
    return image;
}

// What nod_disc_meta and nod_disc_size must say of an image.
struct ExpectedMeta
{
    NodDiscMeta meta{};
    std::uint64_t disc_size = 0;
};

ExpectedMeta IsoMeta(const Bytes& iso)
{
    ExpectedMeta expected;
    expected.meta.format = NOD_FORMAT_ISO;
    expected.meta.lossless = true;
    expected.meta.disc_size = expected.disc_size = iso.size();
    return expected;
}

// As nod: the block size, no disc size, a single-layer DVD guess.
ExpectedMeta WbfsMeta(std::uint32_t block_size)
{
    ExpectedMeta expected;
    expected.meta.format = NOD_FORMAT_WBFS;
    expected.meta.block_size = block_size;
    expected.disc_size = 4699979776ull;
    return expected;
}

// ---- RVZ and WIA, written here from Dolphin's format description ----

enum Codec : std::uint32_t
{
    kNone = 0,
    kPurge = 1,
    kBzip2 = 2,
    kLzma = 3,
    kLzma2 = 4,
    kZstd = 5,
};

// A group as written, for tests that damage one.
struct GroupRecord
{
    std::uint64_t offset;
    std::uint32_t size;
    bool compressed;
    std::uint32_t packed;
    bool partition;
};

struct RvzOptions
{
    bool rvz = true;
    std::uint32_t codec = kZstd;
    int level = 1;
    std::uint32_t chunk = 0x20000;
    std::uint32_t first_segment = 0; // sectors of each partition's first data segment; 0: a third
    bool nkit = false;
    std::function<void(std::vector<GroupRecord>&)> tamper; // changes group headers before they are written
};

Bytes Compress(std::uint32_t codec, int level, const Bytes& data)
{
    switch (codec)
    {
    case kBzip2:
    {
        unsigned size = unsigned(data.size() + data.size() / 100 + 1024);
        Bytes out(size);
        Require(BZ2_bzBuffToBuffCompress(reinterpret_cast<char*>(out.data()), &size,
                                         const_cast<char*>(reinterpret_cast<const char*>(data.data())),
                                         unsigned(data.size()), level, 0, 30)
                    == BZ_OK,
                "bzip2 compression failed");
        out.resize(size);
        return out;
    }
    case kLzma:
    case kLzma2:
    {
        lzma_options_lzma options{};
        Require(!lzma_lzma_preset(&options, std::uint32_t(level)), "LZMA preset");
        const lzma_filter filters[2] = {{codec == kLzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2, &options},
                                        {LZMA_VLI_UNKNOWN, nullptr}};
        Bytes out(data.size() + data.size() / 2 + 4096);
        std::size_t size = 0;
        Require(lzma_raw_buffer_encode(filters, nullptr, data.data(), data.size(), out.data(), &size, out.size())
                    == LZMA_OK,
                "LZMA compression failed");
        out.resize(size);
        return out;
    }
    case kZstd:
    {
        Bytes out(ZSTD_compressBound(data.size()));
        const std::size_t size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), level);
        Require(!ZSTD_isError(size), "zstd compression failed");
        out.resize(size);
        return out;
    }
    default: return data;
    }
}

// The 7-Zip property bytes of the encoder's settings, from liblzma itself.
Bytes LzmaProperties(std::uint32_t codec, int level)
{
    lzma_options_lzma options{};
    Require(!lzma_lzma_preset(&options, std::uint32_t(level)), "LZMA preset");
    const lzma_filter filter{codec == kLzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2, &options};
    std::uint32_t size = 0;
    Require(lzma_properties_size(&size, &filter) == LZMA_OK, "LZMA properties size");
    Bytes props(size);
    Require(lzma_properties_encode(&filter, props.data()) == LZMA_OK, "LZMA properties");
    return props;
}

// The partitions' data sectors: a raw read there fails (WIA/RVZ store it decrypted).
std::set<std::uint32_t> PartitionDataSectors(const BuiltDisc& disc)
{
    std::set<std::uint32_t> sectors;
    for (const BuiltPartition& partition : disc.partitions)
        for (std::uint32_t i = 0; i < partition.clusters; ++i)
            sectors.insert(std::uint32_t(partition.data_offset / kSector) + i);
    return sectors;
}

Bytes MakeRvz(const BuiltDisc& disc, const RvzOptions& options, std::vector<GroupRecord>* written = nullptr)
{
    const Bytes& iso = disc.iso;
    const std::uint32_t chunk = options.chunk, per_group = chunk / kSector;
    struct Stored
    {
        Bytes bytes;
        bool compressed = true;
        std::uint32_t packed = 0;
        bool partition = false;
    };
    std::vector<Stored> groups;
    std::uint64_t counter = 0;
    // One group: exception lists for partition data (which the reader skips),
    // then the data, with RVZ junk runs as seeds; zeroes take no data at all.
    const auto add_group = [&](const Bytes& data, bool partition, std::uint64_t section_offset, std::uint64_t junk_begin,
                               std::uint64_t junk_end) {
        ++counter;
        Bytes lists;
        std::size_t exceptions = 0;
        if (partition)
            for (std::uint32_t list = 0; list < (chunk + 0x1FFFFF) / 0x200000; ++list)
            {
                const std::uint16_t count = std::uint16_t((counter + list) % 3);
                Put16(lists, lists.size(), count);
                const Bytes hashes = Random(count * 22, counter * 7 + list);
                lists.insert(lists.end(), hashes.begin(), hashes.end());
                exceptions += count;
            }
        Bytes body;
        bool packed = false;
        const std::uint64_t data_end = section_offset + data.size();
        if (options.rvz && junk_begin < data_end && junk_end > section_offset)
        {
            packed = true;
            std::uint64_t at = section_offset;
            while (at < data_end)
            {
                const bool junk = at >= junk_begin && at < junk_end;
                std::uint64_t end = junk ? std::min({junk_end, data_end, (at / kSector + 1) * kSector})
                                         : std::min(data_end, at < junk_begin ? junk_begin : data_end);
                if (junk)
                {
                    Append32(body, 0x80000000u | std::uint32_t(end - at));
                    for (const std::uint32_t word : JunkSeed("RWDE", 0, std::uint32_t(at / kSector))) Append32(body, word);
                }
                else
                {
                    Append32(body, std::uint32_t(end - at));
                    body.insert(body.end(), data.begin() + std::ptrdiff_t(at - section_offset),
                                data.begin() + std::ptrdiff_t(end - section_offset));
                }
                at = end;
            }
        }
        else
            body = data;
        Stored stored;
        stored.packed = packed ? std::uint32_t(body.size()) : 0;
        stored.partition = partition;
        if (!exceptions && !packed && std::all_of(data.begin(), data.end(), [](std::uint8_t b) { return b == 0; }))
        {
            groups.push_back(stored); // size 0: zeroes
            return;
        }
        // RVZ may keep a group as is: then (as with no codec) its lists come
        // first, padded to four bytes.
        const bool as_is = options.rvz && counter % 4 == 1;
        if (options.codec == kNone || as_is)
        {
            stored.bytes = lists;
            while (partition && stored.bytes.size() % 4) stored.bytes.push_back(0);
            stored.bytes.insert(stored.bytes.end(), body.begin(), body.end());
            stored.compressed = !as_is;
        }
        else
        {
            Bytes all = lists;
            all.insert(all.end(), body.begin(), body.end());
            stored.bytes = Compress(options.codec, options.level, all);
        }
        groups.push_back(stored);
    };

    // Partition data, in two segments each, decrypted and without hashes.
    Bytes partition_structs;
    for (const BuiltPartition& partition : disc.partitions)
    {
        const std::uint32_t first = std::uint32_t(partition.data_offset / kSector);
        const std::uint32_t split = options.first_segment ? std::min(options.first_segment, partition.clusters)
                                                          : std::max<std::uint32_t>(1, partition.clusters / 3);
        Bytes entry = partition.title_key;
        for (const auto& [begin, end] : {std::pair<std::uint32_t, std::uint32_t>{0, split}, {split, partition.clusters}})
        {
            const std::uint32_t group_index = std::uint32_t(groups.size());
            for (std::uint32_t sector = begin; sector < end; sector += per_group)
            {
                const std::uint32_t last = std::min(end, sector + per_group);
                const Bytes data = Slice(partition.data, std::uint64_t(sector) * kSectorData,
                                         std::uint64_t(last - sector) * kSectorData);
                add_group(data, true, std::uint64_t(sector) * kSectorData, partition.junk_begin,
                          std::uint64_t(partition.clusters) * kSectorData);
            }
            Append32(entry, first + begin);
            Append32(entry, end - begin);
            Append32(entry, group_index);
            Append32(entry, std::uint32_t(groups.size()) - group_index);
        }
        partition_structs.insert(partition_structs.end(), entry.begin(), entry.end());
    }
    // Everything else as raw data, split where partitions start.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> raw;
    std::uint64_t cursor = 0x80;
    for (const BuiltPartition& partition : disc.partitions)
    {
        if (partition.spec.disc_offset > cursor) raw.push_back({cursor, partition.spec.disc_offset});
        raw.push_back({partition.spec.disc_offset, partition.data_offset});
        cursor = partition.data_offset + std::uint64_t(partition.clusters) * kSector;
    }
    if (iso.size() > cursor) raw.push_back({cursor, iso.size()});
    Bytes raw_structs;
    for (const auto& [begin, end] : raw)
    {
        const std::uint32_t group_index = std::uint32_t(groups.size());
        for (std::uint64_t at = begin & ~std::uint64_t(kSector - 1); at < end; at += chunk)
        {
            Bytes data = Slice(iso, at, std::min<std::uint64_t>(chunk, end - at));
            // The reader takes the first 0x80 bytes from the disc struct.
            if (at == 0) std::fill(data.begin(), data.begin() + 0x80, 0xEE);
            add_group(data, false, at, disc.junk_tail, iso.size());
        }
        Put64(raw_structs, raw_structs.size(), begin);
        Put64(raw_structs, raw_structs.size(), end - begin);
        Append32(raw_structs, group_index);
        Append32(raw_structs, std::uint32_t(groups.size()) - group_index);
    }

    // Header, disc struct, an NKit header, partition structs, group data, then
    // the compressed raw data and group headers.
    Bytes file(0x48 + 0xDC, 0);
    if (options.nkit)
    {
        Bytes nkit;
        for (const char c : std::string("NKIT  v1")) nkit.push_back(std::uint8_t(c));
        Append32(nkit, 0x12345678);
        const Bytes md5 = Random(16, 61), sha1 = Random(20, 62);
        nkit.insert(nkit.end(), md5.begin(), md5.end());
        nkit.insert(nkit.end(), sha1.begin(), sha1.end());
        Put64(nkit, nkit.size(), 0x1122334455667788ull);
        file.insert(file.end(), nkit.begin(), nkit.end());
    }
    const std::uint64_t partitions_at = file.size();
    file.insert(file.end(), partition_structs.begin(), partition_structs.end());
    std::vector<GroupRecord> records;
    for (const Stored& group : groups)
    {
        while (file.size() % 4) file.push_back(0);
        records.push_back({file.size(), std::uint32_t(group.bytes.size()), group.compressed && !group.bytes.empty(),
                           group.packed, group.partition});
        file.insert(file.end(), group.bytes.begin(), group.bytes.end());
    }
    if (written) *written = records;
    if (options.tamper) options.tamper(records);
    Bytes group_headers;
    for (const GroupRecord& group : records)
    {
        Append32(group_headers, std::uint32_t(group.offset / 4));
        if (options.rvz)
        {
            Append32(group_headers, group.size | (group.compressed ? 0x80000000u : 0));
            Append32(group_headers, group.packed);
        }
        else
            Append32(group_headers, group.size);
    }
    const Bytes raw_stored = Compress(options.codec, options.level, raw_structs);
    const Bytes groups_stored = Compress(options.codec, options.level, group_headers);
    const std::uint64_t raw_at = file.size();
    file.insert(file.end(), raw_stored.begin(), raw_stored.end());
    const std::uint64_t groups_at = file.size();
    file.insert(file.end(), groups_stored.begin(), groups_stored.end());

    Bytes discs(0xDC, 0);
    Put32(discs, 0, 2);
    Put32(discs, 4, options.codec);
    Put32(discs, 8, std::uint32_t(options.level));
    Put32(discs, 12, chunk);
    std::copy(iso.begin(), iso.begin() + 0x80, discs.begin() + 16);
    Put32(discs, 0x90, std::uint32_t(disc.partitions.size()));
    Put32(discs, 0x94, 0x30);
    Put64(discs, 0x98, partitions_at);
    Put(discs, 0xA0, Sha1(partition_structs));
    Put32(discs, 0xB4, std::uint32_t(raw.size()));
    Put64(discs, 0xB8, raw_at);
    Put32(discs, 0xC0, std::uint32_t(raw_stored.size()));
    Put32(discs, 0xC4, std::uint32_t(groups.size()));
    Put64(discs, 0xC8, groups_at);
    Put32(discs, 0xD0, std::uint32_t(groups_stored.size()));
    if (options.codec == kLzma || options.codec == kLzma2)
    {
        const Bytes props = LzmaProperties(options.codec, options.level);
        discs[0xD4] = std::uint8_t(props.size());
        Put(discs, 0xD5, props);
    }
    Bytes header;
    for (const char c : std::string(options.rvz ? "RVZ\x01" : "WIA\x01", 4)) header.push_back(std::uint8_t(c));
    Append32(header, 0x01000000);
    Append32(header, options.rvz ? 0x00030000 : 0x01000000);
    Append32(header, 0xDC);
    const Bytes disc_hash = Sha1(discs);
    header.insert(header.end(), disc_hash.begin(), disc_hash.end());
    Put64(header, header.size(), iso.size());
    Put64(header, header.size(), file.size());
    const Bytes head_hash = Sha1(header);
    header.insert(header.end(), head_hash.begin(), head_hash.end());
    std::copy(header.begin(), header.end(), file.begin());
    std::copy(discs.begin(), discs.end(), file.begin() + 0x48);
    return file;
}

ExpectedMeta RvzMeta(const Bytes& iso, const RvzOptions& options)
{
    ExpectedMeta expected;
    NodDiscMeta& meta = expected.meta;
    meta.format = options.rvz ? NOD_FORMAT_RVZ : NOD_FORMAT_WIA;
    const NodCompressionKind kinds[] = {NOD_COMPRESSION_KIND_NONE,  NOD_COMPRESSION_KIND_NONE,
                                        NOD_COMPRESSION_KIND_BZIP2, NOD_COMPRESSION_KIND_LZMA,
                                        NOD_COMPRESSION_KIND_LZMA2, NOD_COMPRESSION_KIND_ZSTANDARD};
    meta.compression = {kinds[options.codec], std::int16_t(options.codec == kNone ? 0 : options.level)};
    meta.block_size = options.chunk;
    meta.decrypted = meta.needs_hash_recovery = meta.lossless = true;
    meta.disc_size = expected.disc_size = iso.size();
    if (options.nkit)
    {
        meta.crc32 = 0x12345678;
        const Bytes md5 = Random(16, 61), sha1 = Random(20, 62);
        std::copy(md5.begin(), md5.end(), meta.md5);
        std::copy(sha1.begin(), sha1.end(), meta.sha1);
        meta.xxh64 = 0x1122334455667788ull;
    }
    return expected;
}

void WriteFile(const fs::path& path, const Bytes& bytes, std::size_t begin = 0, std::size_t end = SIZE_MAX)
{
    end = std::min(end, bytes.size());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data() + begin), std::streamsize(end - begin));
    Require(bool(out), "Cannot write " + path.string());
}

// Writes `bytes` as parts named by `name(index)`, cut at `cuts`.
void WriteSplit(const Bytes& bytes, const std::vector<std::size_t>& cuts, const std::function<fs::path(int)>& name)
{
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= cuts.size(); ++i)
    {
        const std::size_t end = i < cuts.size() ? cuts[i] : bytes.size();
        WriteFile(name(int(i)), bytes, begin, end);
        begin = end;
    }
}

// ---- Reading back ----

struct Owned
{
    NodHandle* handle = nullptr;
    Owned() = default;
    explicit Owned(NodHandle* h) : handle(h) {}
    Owned(Owned&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
    ~Owned() { nod_free(handle); }
    NodHandle* get() const { return handle; }
};

Owned OpenDisc(const fs::path& path)
{
    NodHandle* handle = nullptr;
    const NodResult result = nod_disc_open(path.c_str(), nullptr, &handle);
    Require(result == NOD_RESULT_OK && handle, "Cannot open " + path.string() + ": " + Message());
    Require(nod_error_message() == nullptr, "A successful open left an error message");
    return Owned(handle);
}

Owned OpenData(const Owned& disc)
{
    NodHandle* handle = nullptr;
    Require(nod_disc_open_partition_kind(disc.get(), NOD_PARTITION_KIND_DATA, nullptr, &handle) == NOD_RESULT_OK,
            "Cannot open the data partition: " + Message());
    return Owned(handle);
}

Owned OpenFile(const Owned& partition, std::uint32_t index)
{
    NodHandle* handle = nullptr;
    Require(nod_partition_open_file(partition.get(), index, &handle) == NOD_RESULT_OK,
            "Cannot open FST node " + std::to_string(index) + ": " + Message());
    return Owned(handle);
}

Bytes ReadAll(NodHandle* handle, std::size_t chunk)
{
    Bytes out;
    Bytes buffer(chunk);
    for (;;)
    {
        const std::int64_t got = nod_read(handle, buffer.data(), buffer.size());
        Require(got >= 0, "nod_read failed: " + Message());
        if (got == 0) return out;
        Require(std::size_t(got) <= chunk, "nod_read returned more than asked");
        out.insert(out.end(), buffer.begin(), buffer.begin() + got);
    }
}

struct Visit
{
    std::uint32_t index;
    NodNodeKind kind;
    std::string name;
    std::uint32_t size;
};

std::vector<Visit> Iterate(const Owned& partition, std::function<std::uint32_t(const Visit&)> next)
{
    struct Context
    {
        std::vector<Visit> visits;
        std::function<std::uint32_t(const Visit&)> next;
    } context{{}, std::move(next)};
    nod_partition_iterate_fst(
        partition.get(),
        [](std::uint32_t index, NodNodeKind kind, const char* name, std::uint32_t size, void* data) {
            auto& c = *static_cast<Context*>(data);
            c.visits.push_back({index, kind, name, size});
            return c.next(c.visits.back());
        },
        &context);
    return context.visits;
}

// Everything nod's API shows of one data partition, against the fixture.
void CheckPartition(const Owned& disc, const BuiltPartition& expected, const Bytes& expected_data,
                    const std::string& label)
{
    Owned partition = OpenData(disc);
    Require(nod_partition_is_wii(partition.get()) && !nod_partition_is_wii(disc.get()), label + ": is_wii");
    NodPartitionMeta meta{};
    Require(nod_partition_meta(partition.get(), &meta) == NOD_RESULT_OK, label + ": partition meta");
    const auto same = [](const NodBlob& blob, const Bytes& bytes) {
        return blob.data && blob.size == bytes.size() && std::equal(bytes.begin(), bytes.end(), blob.data);
    };
    Require(same(meta.raw_boot, expected.boot) && same(meta.raw_bi2, expected.bi2)
                && same(meta.raw_apploader, expected.apploader) && same(meta.raw_dol, expected.dol)
                && same(meta.raw_fst, expected.fst) && same(meta.raw_ticket, Slice(expected.header, 0, 0x2A4))
                && same(meta.raw_tmd, expected.tmd) && same(meta.raw_cert_chain, expected.cert),
            label + ": partition metadata blobs differ");
    const bool hashed = expected.cluster_data == kSectorData;
    Require(hashed ? same(meta.raw_h3_table, expected.h3) : meta.raw_h3_table.data == nullptr,
            label + ": H3 table");

    // Iteration from node 1 in FST order, with nod's names, kinds and sizes.
    const auto visits = Iterate(partition, [](const Visit& v) { return v.index + 1; });
    Require(visits.size() == expected.nodes.size() - 1 && nod_error_message() == nullptr, label + ": FST node count");
    for (const Visit& visit : visits)
    {
        const Node& node = expected.nodes[visit.index];
        Require(visit.name == node.name && (visit.kind == NOD_NODE_KIND_DIRECTORY) == node.directory
                    && visit.size == node.length,
                label + ": FST node " + std::to_string(visit.index) + " differs");
    }
    // Skipping directories by their end index, and stopping.
    std::size_t files_at_top = 0;
    for (const Visit& visit :
         Iterate(partition, [](const Visit& v) { return v.kind == NOD_NODE_KIND_DIRECTORY ? v.size : v.index + 1; }))
        if (visit.kind == NOD_NODE_KIND_FILE) ++files_at_top;
    std::size_t expected_top = 0;
    for (std::uint32_t i = 1; i < expected.nodes.size();)
    {
        expected_top += !expected.nodes[i].directory;
        i = expected.nodes[i].directory ? expected.nodes[i].length : i + 1;
    }
    Require(files_at_top == expected_top, label + ": FST directory skipping");
    Require(Iterate(partition, [](const Visit&) { return NOD_FST_STOP; }).size() == 1, label + ": FST stop");

    // Every file through find_file and a full read, in several chunk sizes.
    std::size_t files = 0;
    for (std::uint32_t index = 1; index < expected.nodes.size(); ++index)
    {
        const Node& node = expected.nodes[index];
        NodNodeKind kind{};
        std::uint32_t length = 0;
        std::string upper = node.path;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        for (const std::string& path : {node.path, "/" + upper, "//" + node.path + "/"})
            Require(nod_partition_find_file(partition.get(), path.c_str(), &kind, &length) == index
                        && (kind == NOD_NODE_KIND_DIRECTORY) == node.directory && length == node.length,
                    label + ": find_file(" + path + ")");
        if (node.directory) continue;
        ++files;
        const Bytes& data = node.data;
        for (const std::size_t chunk : {std::size_t(1) << 20, std::size_t(kSectorData + 1), std::size_t(4093)})
        {
            if (data.size() > 600000 && chunk < 4096) continue; // keep the test fast
            Owned file = OpenFile(partition, index);
            Require(ReadAll(file.get(), chunk) == data, label + ": " + node.path + " read in chunks of "
                                                            + std::to_string(chunk) + " differs");
        }
        Require(Slice(expected_data, node.offset, data.size()) == data, label + ": fixture places " + node.path);
    }
    Require(files > 0, label + ": no files");
    std::uint32_t root = 1;
    NodNodeKind root_kind{};
    Require(nod_partition_find_file(partition.get(), "/", &root_kind, &root) == 0 && root_kind == NOD_NODE_KIND_DIRECTORY
                && root == expected.nodes.size(),
            label + ": find_file(/) is the root");

    // The partition handle reads the decrypted data, skipping hash blocks.
    Require(ReadAll(partition.get(), 300000) == expected_data, label + ": whole partition data differs");
    const std::uint64_t cds = expected.cluster_data;
    std::uint64_t seed = 77;
    for (int i = 0; i < 200; ++i)
    {
        const std::uint64_t offset = Next(seed) % expected_data.size();
        const std::size_t size = std::size_t(Next(seed) % (3 * cds));
        Bytes out(size + 16, 0xEE);
        Require(nod_seek(partition.get(), std::int64_t(offset), 0) == std::int64_t(offset), label + ": partition seek");
        const std::int64_t got = nod_read(partition.get(), out.data(), size);
        const Bytes want = Slice(expected_data, offset, size);
        Require(got == std::int64_t(want.size()) && std::equal(want.begin(), want.end(), out.begin())
                    && std::all_of(out.begin() + got, out.end(), [](std::uint8_t b) { return b == 0xEE; }),
                label + ": partition read at " + std::to_string(offset) + "+" + std::to_string(size));
    }
    // nod's partition end counts the hash blocks.
    Require(nod_seek(partition.get(), -3, 2) == std::int64_t(std::uint64_t(expected.clusters) * kSector - 3),
            label + ": partition seek from end");
    Require(nod_seek(partition.get(), std::int64_t(expected_data.size()), 0) == std::int64_t(expected_data.size()),
            label + ": partition seek to its data end");
    std::uint8_t byte = 0;
    Require(nod_read(partition.get(), &byte, 1) == 0, label + ": partition read past its data");
}

std::uint32_t FindIndex(const BuiltPartition& built, const std::string& path)
{
    for (std::uint32_t i = 0; i < built.nodes.size(); ++i)
        if (built.nodes[i].path == path) return i;
    throw std::runtime_error("Fixture has no " + path);
}

// On a newly opened disc, so the cache holds only what opening it read (with
// big WIA/RVZ chunks that can include the "cold" file).
void CheckSeeksAndProbes(const fs::path& path, const BuiltPartition& built, bool cold = true)
{
    Owned disc = OpenDisc(path);
    Owned partition = OpenData(disc);
    const std::uint32_t index = FindIndex(built, "files/edges.bin");
    const Bytes& data = built.nodes[index].data;
    const std::int64_t length = std::int64_t(data.size());
    Owned file = OpenFile(partition, index);
    // Seeks clamp to the file; Start, Current and End.
    Require(nod_seek(file.get(), 0, 2) == length && nod_seek(file.get(), -10, 2) == length - 10
                && nod_seek(file.get(), 5, 1) == length - 5 && nod_seek(file.get(), length + 1000, 0) == length
                && nod_seek(file.get(), -length - 1000, 1) == 0 && nod_seek(file.get(), 1000, 2) == length,
            "File seeks do not clamp like nod's windowed reader");
    std::uint8_t byte = 0;
    Require(nod_read(file.get(), &byte, 1) == 0, "Read past a file's end");
    Require(nod_seek(file.get(), 3, 3) == -1, "Invalid whence was accepted");
    RequireError("invalid whence value: 3", "nod_seek(whence 3)");
    Require(nod_seek(file.get(), 7, 0) == 7 && nod_read(file.get(), &byte, 1) == 1 && byte == data[7],
            "Read after seek");
    Require(nod_error_message() == nullptr, "A successful call left the previous error message");

    // The resident-bytes probe: a cold file misses, changing nothing.
    const std::uint32_t cold_index = FindIndex(built, "files/later.bin");
    Owned cold_file = OpenFile(partition, cold_index);
    Bytes out(256 * 1024 + 1, 0xA5);
    nod_seek(cold_file.get(), 17, 0);
    nod_seek(cold_file.get(), 0, 9); // leaves an error message that a probe must keep
    const std::string kept = Message();
    const std::int64_t cold_probe = nod_try_read_cached_at(cold_file.get(), 5, out.data(), 32);
    Require(Message() == kept && (cold_probe == -2 || (!cold && cold_probe == 32)), "A cold probe hit or changed the error message");
    if (cold_probe == -2)
        Require(nod_seek(cold_file.get(), 0, 1) == 17 && std::all_of(out.begin(), out.end(), [](std::uint8_t b) { return b == 0xA5; }),
                "A missed probe moved the cursor or wrote the destination");
    else
        Require(nod_seek(cold_file.get(), 0, 1) == 37 && Slice(out, 0, 32) == Slice(built.nodes[cold_index].data, 5, 32),
                "A probe of a group decoded while opening was wrong");
    // Reading a file's start decrypts what follows: probes then hit, exactly.
    Owned warm = OpenFile(partition, index);
    Require(nod_read(warm.get(), out.data(), 100) == 100, "Warm-up read");
    std::fill(out.begin(), out.end(), 0xA5);
    const std::int64_t count = std::min<std::int64_t>(length - 19, 64 * 1024);
    Require(nod_try_read_cached_at(warm.get(), 19, out.data(), std::size_t(count)) == count,
            "A probe of decrypted bytes missed");
    Require(std::equal(data.begin() + 19, data.begin() + 19 + count, out.begin()) && out[std::size_t(count)] == 0xA5,
            "A probe hit copied wrong bytes or past its end");
    Require(nod_read(warm.get(), &byte, 1) == (19 + count < length ? 1 : 0)
                && (19 + count >= length || byte == data[std::size_t(19 + count)]),
            "A probe hit did not move the file cursor");
    // EOF clips the count but not the tail; oversized and empty requests miss.
    std::fill(out.begin(), out.end(), 0xA5);
    Require(nod_try_read_cached_at(warm.get(), std::uint64_t(length - 5), out.data(), 32) == 5
                && std::equal(data.end() - 5, data.end(), out.begin())
                && std::all_of(out.begin() + 5, out.end(), [](std::uint8_t b) { return b == 0xA5; }),
            "A probe at the end did not clip to the file");
    Require(nod_read(warm.get(), &byte, 1) == 0, "The clipped probe left the cursor before the end");
    nod_seek(warm.get(), 23, 0);
    for (const auto& [offset, size] : std::vector<std::pair<std::uint64_t, std::size_t>>{
             {0, 0}, {0, 256 * 1024 + 1}, {std::uint64_t(length), 1}, {UINT64_MAX, 32}})
        Require(nod_try_read_cached_at(warm.get(), offset, out.data(), size) == -2 && nod_seek(warm.get(), 0, 1) == 23,
                "A guarded probe hit or moved the cursor");
    Require(nod_try_read_cached_at(warm.get(), 0, nullptr, 4) == -2 && nod_try_read_cached_at(nullptr, 0, out.data(), 4) == -2
                && nod_try_read_cached_at(partition.get(), 0, out.data(), 4) == -2
                && nod_try_read_cached_at(disc.get(), 0, out.data(), 4) == -2,
            "Probes of null or non-file handles must miss");
    // An empty file reads nothing and never hits.
    Owned empty = OpenFile(partition, FindIndex(built, "files/empty.bin"));
    Require(nod_read(empty.get(), out.data(), 10) == 0 && nod_seek(empty.get(), 0, 2) == 0
                && nod_try_read_cached_at(empty.get(), 0, out.data(), 1) == -2,
            "Empty file");
}

void CheckDiscReads(const Owned& disc, const Bytes& iso, const BuiltDisc& built, const ExpectedMeta& expected,
                    const std::set<std::uint32_t>& left_out_sectors)
{
    NodDiscHeader header{};
    Require(nod_disc_header(disc.get(), &header) == NOD_RESULT_OK && std::memcmp(&header, iso.data(), 0x400) == 0,
            "Disc header differs");
    Require(std::string(header.game_id, 6) == "RWDE01" && header.disc_version == 3 && header.audio_streaming == 1
                && header.audio_stream_buf_size == 2 && std::memcmp(header.wii_magic, "\x5d\x1c\x9e\xa3", 4) == 0,
            "Disc header fields");
    NodDiscMeta meta{};
    Require(nod_disc_meta(disc.get(), &meta) == NOD_RESULT_OK, "Disc meta");
    const NodDiscMeta& stated = expected.meta;
    Require(meta.format == stated.format && meta.compression.kind == stated.compression.kind
                && meta.compression.level == stated.compression.level && meta.block_size == stated.block_size
                && meta.decrypted == stated.decrypted && meta.needs_hash_recovery == stated.needs_hash_recovery
                && meta.lossless == stated.lossless && meta.disc_size == stated.disc_size && meta.crc32 == stated.crc32
                && std::memcmp(meta.md5, stated.md5, 16) == 0 && std::memcmp(meta.sha1, stated.sha1, 20) == 0
                && meta.xxh64 == stated.xxh64,
            "Disc meta of format " + std::to_string(int(stated.format)) + " differs");
    Require(nod_disc_size(disc.get()) == expected.disc_size, "Disc size");
    NodPartitionInfo infos[4]{};
    Require(nod_disc_partitions(disc.get(), infos, 4) == built.partitions.size(), "Partition count");
    for (std::size_t i = 0; i < built.partitions.size(); ++i)
        Require(infos[i].index == i && infos[i].kind == built.partitions[i].spec.kind
                    && infos[i].data_size == std::uint64_t(built.partitions[i].clusters) * kSector,
                "Partition info " + std::to_string(i));

    // Raw disc bytes, as stored, sector by sector boundaries crossed.
    std::uint64_t seed = 99;
    for (int i = 0; i < 120; ++i)
    {
        const std::uint64_t offset = Next(seed) % iso.size();
        const std::size_t size = std::size_t(1 + Next(seed) % (5 * kSector));
        const std::uint32_t first = std::uint32_t(offset / kSector);
        const std::uint32_t last = std::uint32_t((offset + size - 1) / kSector);
        bool hole = false;
        for (std::uint32_t s = first; s <= last; ++s) hole = hole || left_out_sectors.count(s);
        Bytes out(size);
        Require(nod_seek(disc.get(), std::int64_t(offset), 0) == std::int64_t(offset), "Disc seek");
        const std::int64_t got = nod_read(disc.get(), out.data(), size);
        if (hole)
        {
            // Left-out partition data: a short read up to it, or an error at it.
            if (left_out_sectors.count(first))
            {
                Require(got == -1, "Raw read of left-out partition data did not fail");
                RequireError("cannot rebuild", "Raw read of left-out partition data");
            }
            else
                Require(got > 0 && std::size_t(got) < size
                            && std::equal(out.begin(), out.begin() + got, iso.begin() + std::ptrdiff_t(offset)),
                        "Raw read did not stop before left-out partition data");
            continue;
        }
        const Bytes want = Slice(iso, offset, size);
        Require(got == std::int64_t(want.size()) && std::equal(want.begin(), want.end(), out.begin()),
                "Raw disc read at " + std::to_string(offset) + "+" + std::to_string(size) + " differs");
    }
    if (!left_out_sectors.empty())
    {
        // At left-out partition data a raw read fails; one that reaches it stops there.
        const std::int64_t hole = std::int64_t(*left_out_sectors.begin()) * kSector;
        Bytes out(100);
        Require(nod_seek(disc.get(), hole, 0) == hole && nod_read(disc.get(), out.data(), out.size()) == -1,
                "Raw read at left-out partition data");
        RequireError("cannot rebuild its hashes and encryption", "Raw read at left-out partition data");
        Require(nod_seek(disc.get(), hole - 40, 0) == hole - 40 && nod_read(disc.get(), out.data(), out.size()) == 40
                    && std::equal(out.begin(), out.begin() + 40, iso.begin() + std::ptrdiff_t(hole - 40)),
                "Raw read into left-out partition data");
    }
    if (expected.meta.format == NOD_FORMAT_ISO)
        Require(nod_seek(disc.get(), 0, 0) == 0 && ReadAll(disc.get(), 1 << 20) == iso, "Whole raw ISO differs");
    if (expected.meta.format != NOD_FORMAT_WBFS)
    {
        // The disc ends where the image says.
        std::uint8_t byte = 0;
        Require(nod_seek(disc.get(), std::int64_t(iso.size()), 0) == std::int64_t(iso.size())
                    && nod_read(disc.get(), &byte, 1) == 0,
                "Raw read past the disc's end");
        Bytes tail(50);
        Require(nod_seek(disc.get(), std::int64_t(iso.size() - 30), 0) > 0 && nod_read(disc.get(), tail.data(), 50) == 30
                    && std::equal(tail.begin(), tail.begin() + 30, iso.end() - 30),
                "Raw read of the disc's last bytes");
    }
    else
    {
        // Past the stored blocks: zeroes up to the guessed disc size.
        Bytes out(100, 0xFF);
        Require(nod_seek(disc.get(), std::int64_t(iso.size()) + 12345, 0) > 0
                    && nod_read(disc.get(), out.data(), out.size()) == 100
                    && std::all_of(out.begin(), out.end(), [](std::uint8_t b) { return b == 0; }),
                "WBFS raw read past the stored blocks");
    }
    Require(nod_seek(disc.get(), 0, 2) == -1, "Disc seek from the end");
    RequireError("SeekFrom::End is not supported", "Disc seek from the end");
    Require(nod_seek(disc.get(), 5, 0) == 5 && nod_seek(disc.get(), -10, 1) == 0, "Disc seek saturates at zero");
}

void CheckConcurrentReads(const Owned& disc, const BuiltPartition& built)
{
    Owned partition = OpenData(disc);
    std::vector<std::uint32_t> files;
    for (std::uint32_t i = 1; i < built.nodes.size(); ++i)
        if (!built.nodes[i].directory && !built.nodes[i].data.empty()) files.push_back(i);
    std::atomic<bool> failed{false};
    std::atomic<int> hits{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 5; ++t)
        threads.emplace_back([&, t] {
            std::uint64_t seed = 1000 + t;
            for (int i = 0; i < 150 && !failed; ++i)
            {
                const std::uint32_t index = files[Next(seed) % files.size()];
                const Bytes& data = built.nodes[index].data;
                NodHandle* raw = nullptr;
                if (nod_partition_open_file(partition.get(), index, &raw) != NOD_RESULT_OK)
                {
                    failed = true;
                    break;
                }
                Owned file(raw);
                const std::uint64_t offset = Next(seed) % data.size();
                const std::size_t size = std::size_t(std::min<std::uint64_t>(data.size() - offset, Next(seed) % 200000 + 1));
                Bytes out(size);
                // One thread only probes; the others read.
                const std::int64_t got = t == 0 && size <= 256 * 1024
                    ? nod_try_read_cached_at(file.get(), offset, out.data(), size)
                    : (nod_seek(file.get(), std::int64_t(offset), 0), nod_read(file.get(), out.data(), size));
                if (got == -2 && t == 0) continue;
                if (t == 0) ++hits;
                if (got <= 0 || !std::equal(out.begin(), out.begin() + got, data.begin() + std::ptrdiff_t(offset)))
                    failed = true;
            }
        });
    for (auto& thread : threads) thread.join();
    Require(!failed, "Concurrent reads returned wrong bytes or failed");
}

NodResult TryOpen(const fs::path& path, const NodDiscOptions* options = nullptr)
{
    NodHandle* handle = nullptr;
    const NodResult result = nod_disc_open(path.c_str(), options, &handle);
    nod_free(handle);
    return result;
}

void RequireOpenFails(const fs::path& path, NodResult result, const std::string& message)
{
    Require(TryOpen(path) == result, path.filename().string() + ": wrong result for " + message);
    RequireError(message, path.filename().string());
}

PartitionSpec UpdatePartition()
{
    PartitionSpec spec;
    spec.kind = NOD_PARTITION_KIND_UPDATE;
    spec.key_index = 1;
    spec.common_key = &kKoreanKey;
    spec.disc_offset = 0x50000;
    spec.root = Directory("", {File("update.bin", Random(5000, 3))});
    spec.seed = 2;
    return spec;
}

// Files at cluster edges, across several clusters, a large one that does
// not fit the reader's cache, an empty one and nested directories.
PartitionSpec DataPartition(std::uint64_t disc_offset, bool large)
{
    PartitionSpec spec;
    spec.disc_offset = disc_offset;
    std::vector<Tree> files = {
        File("edges.bin", Random(3 * kSectorData + 77, 10), 4 * kSectorData - 64),
        File("empty.bin", {}),
        File("one.txt", Random(1, 11)),
        File("exact.bin", Random(kSectorData, 12), 8 * kSectorData),
        Directory("deep", {Directory("deeper", {File("Mixed.Case", Random(1234, 13))}), File("after.dat", Random(99, 14))}),
        File("later.bin", Random(40000, 15), 40 * kSectorData + 8), // past what opening decrypts
    };
    if (large) files.push_back(File("large.bin", Random(4500000, 16)));
    spec.root = Directory("", {Directory("files", std::move(files)), File("top.bin", Random(777, 17))});
    spec.extra_clusters = 72; // whole WBFS blocks of 256 KiB and 1 MiB to leave out
    spec.seed = 9;
    return spec;
}

// The data a WBFS without some disc blocks returns: zeroes there.
Bytes DataWithoutBlocks(const BuiltPartition& built, const std::set<std::uint32_t>& left_out_sectors)
{
    Bytes data = built.data;
    for (std::uint32_t cluster = 0; cluster < built.clusters; ++cluster)
        if (left_out_sectors.count(std::uint32_t(built.data_offset / kSector) + cluster))
            std::fill(data.begin() + std::ptrdiff_t(std::size_t(cluster) * built.cluster_data),
                      data.begin() + std::ptrdiff_t(std::size_t(cluster + 1) * built.cluster_data), 0);
    return data;
}

void CheckErrors(const fs::path& work, const BuiltDisc& retail)
{
    // Paths and formats.
    RequireOpenFails(work / "missing.iso", NOD_RESULT_ERR_IO, "Failed to open");
    fs::create_directories(work / "folder.iso");
    RequireOpenFails(work / "folder.iso", NOD_RESULT_ERR_FORMAT, "Input is not a file");
    WriteFile(work / "tiny.iso", Bytes(31, 0));
    RequireOpenFails(work / "tiny.iso", NOD_RESULT_ERR_FORMAT, "Unknown disc format");
    WriteFile(work / "noise.iso", Random(0x10000, 4));
    RequireOpenFails(work / "noise.iso", NOD_RESULT_ERR_FORMAT, "Unknown disc format");
    const std::vector<std::pair<std::string, Bytes>> formats = {
        {"CISO", Hex("4349534f")}, {"GCZ", Hex("01c00bb1")}, {"TGC", Hex("ae0f38a2")}, {"NFS", Hex("45474753")}};
    for (const auto& [name, magic] : formats)
    {
        Bytes image(0x1000, 0);
        Put(image, 0, magic);
        WriteFile(work / ("image." + name), image);
        RequireOpenFails(work / ("image." + name), NOD_RESULT_ERR_FORMAT,
                         name + " images are not supported on this build; use ISO, WBFS or RVZ");
    }
    Bytes gamecube(0x10000, 0);
    std::memcpy(gamecube.data(), "GZZE01", 6);
    Put(gamecube, 0x1C, Hex("c2339f3d"));
    WriteFile(work / "gamecube.iso", gamecube);
    RequireOpenFails(work / "gamecube.iso", NOD_RESULT_ERR_FORMAT, "GameCube discs are not supported on this build");

    // Wii headers nod rejects.
    const auto variant = [&](const std::string& name, const std::function<void(Bytes&)>& change) {
        Bytes iso = retail.iso;
        change(iso);
        WriteFile(work / name, iso);
        return work / name;
    };
    const std::uint64_t data_partition = retail.partitions[1].spec.disc_offset;
    RequireOpenFails(variant("unknown-issuer.iso",
                             [&](Bytes& iso) { std::memcpy(iso.data() + data_partition + 0x140, "Root-CA00000003", 15); }),
                     NOD_RESULT_ERR_FORMAT, "unknown certificate issuer \"Root-CA00000003-XS00000003\"");
    RequireOpenFails(variant("unterminated-issuer.iso",
                             [&](Bytes& iso) { std::fill_n(iso.begin() + std::ptrdiff_t(data_partition + 0x140), 64, 'A'); }),
                     NOD_RESULT_ERR_FORMAT, "failed to parse certificate issuer");
    RequireOpenFails(variant("key-index.iso", [&](Bytes& iso) { iso[data_partition + 0x1F1] = 3; }),
                     NOD_RESULT_ERR_FORMAT, "unknown common key index 3");
    RequireOpenFails(variant("misaligned.iso",
                             [&](Bytes& iso) {
                                 // A valid ticket, at a place that is no sector start.
                                 std::copy_n(iso.begin() + std::ptrdiff_t(data_partition), 0x2C0, iso.begin() + 0xC0004);
                                 Put32(iso, 0x40028, 0xC0004 >> 2);
                             }),
                     NOD_RESULT_ERR_FORMAT, "Partition 0:1 offset is not sector aligned");
    RequireOpenFails(variant("data-misaligned.iso", [&](Bytes& iso) { Put32(iso, data_partition + 0x2BC, 0x100); }),
                     NOD_RESULT_ERR_FORMAT, "Partition 0:1 data is not sector aligned");
    RequireOpenFails(variant("fst-past-data.iso", [&](Bytes& iso) { Put32(iso, data_partition + 0x2BC, 0x8000 >> 2); }),
                     NOD_RESULT_ERR_FORMAT, "Partition 0:1 FST exceeds data size");
    RequireOpenFails(variant("encrypted-unhashed.iso", [&](Bytes& iso) { iso[0x60] = 1; }), NOD_RESULT_ERR_FORMAT,
                     "Wii disc is encrypted but has no partition hashes");
    // Cut before the header's sector (inside it, the rest would read as zeroes, as nod's do).
    RequireOpenFails(variant("truncated-header.iso", [&](Bytes& iso) { iso.resize(data_partition); }),
                     NOD_RESULT_ERR_IO, "Reading partition header 0:1");

    // WBFS headers nod rejects.
    const Bytes wbfs = MakeWbfs(retail.iso, 18, {});
    RequireOpenFails(variant("short.wbfs", [&](Bytes& iso) { iso = wbfs; iso.resize(iso.size() - 512); }),
                     NOD_RESULT_ERR_FORMAT, "Invalid WBFS file size");
    RequireOpenFails(variant("empty-slot.wbfs", [&](Bytes& iso) { iso = wbfs; iso[12] = 0; }), NOD_RESULT_ERR_FORMAT,
                     "WBFS doesn't contain a disc");
    RequireOpenFails(variant("two-discs.wbfs", [&](Bytes& iso) { iso = wbfs; iso[13] = 1; }), NOD_RESULT_ERR_FORMAT,
                     "Only single WBFS discs are supported");
    RequireOpenFails(variant("small-blocks.wbfs", [&](Bytes& iso) { iso = wbfs; iso[9] = 14; }), NOD_RESULT_ERR_FORMAT,
                     "Unsupported WBFS block size");
    // A split WBFS missing its second part is too short for its header.
    WriteSplit(wbfs, {300000}, [&](int i) { return work / (i ? "missing-part.wbf1" : "missing-part.wbfs"); });
    fs::remove(work / "missing-part.wbf1");
    RequireOpenFails(work / "missing-part.wbfs", NOD_RESULT_ERR_FORMAT, "Invalid WBFS file size");

    // Options this build does not offer.
    WriteFile(work / "retail.iso", retail.iso);
    NodDiscOptions decrypting{};
    decrypting.partition_encryption = NOD_PARTITION_ENCRYPTION_FORCE_DECRYPTED;
    Require(TryOpen(work / "retail.iso", &decrypting) == NOD_RESULT_ERR_OTHER, "Forced decryption was accepted");
    RequireError("NOD_PARTITION_ENCRYPTION_ORIGINAL", "Forced decryption");
    NodDiscOptions preloading{};
    preloading.preloader_threads = 4;
    Require(TryOpen(work / "retail.iso", &preloading) == NOD_RESULT_OK && nod_error_message() == nullptr,
            "Preloader threads must be accepted");
    Owned disc = OpenDisc(work / "retail.iso");
    NodHandle* handle = nullptr;
    const NodPartitionOptions validating{true};
    Require(nod_disc_open_partition_kind(disc.get(), 0, &validating, &handle) == NOD_RESULT_ERR_OTHER && !handle,
            "Hash validation was accepted");
    Require(nod_disc_open_partition_kind(disc.get(), NOD_PARTITION_KIND_CHANNEL, nullptr, &handle) == NOD_RESULT_ERR_FORMAT,
            "A missing partition kind opened");
    RequireError("Partition type Channel not found", "Channel partition");
    Require(nod_disc_open_partition(disc.get(), 5, nullptr, &handle) == NOD_RESULT_ERR_FORMAT, "Partition 5 opened");
    RequireError("Partition 5 not found", "Partition index 5");
    Require(nod_disc_open_partition(disc.get(), 0, nullptr, &handle) == NOD_RESULT_OK, "Partition 0 by index");
    Owned update(handle);
    Require(nod_partition_find_file(update.get(), "update.bin", nullptr, nullptr) == 1, "Update partition FST");

    // Handles of the wrong kind, null handles and arguments.
    Owned partition = OpenData(disc);
    NodDiscHeader header{};
    NodPartitionMeta meta{};
    Require(nod_disc_header(partition.get(), &header) == NOD_RESULT_ERR_INVALID_HANDLE, "Header of a partition");
    RequireError("handle is not a disc", "Header of a partition");
    Require(nod_partition_meta(disc.get(), &meta) == NOD_RESULT_ERR_INVALID_HANDLE, "Meta of a disc");
    RequireError("handle is not a partition", "Meta of a disc");
    Require(nod_partition_open_file(disc.get(), 1, &handle) == NOD_RESULT_ERR_INVALID_HANDLE, "File of a disc");
    Require(nod_disc_open_partition_kind(partition.get(), 0, nullptr, &handle) == NOD_RESULT_ERR_INVALID_HANDLE,
            "Partition of a partition");
    Require(nod_disc_header(nullptr, &header) == NOD_RESULT_ERR_INVALID_HANDLE, "Null handle");
    RequireError("null handle", "Null handle");
    Require(nod_disc_header(disc.get(), nullptr) == NOD_RESULT_ERR_INVALID_HANDLE, "Null output");
    RequireError("null pointer argument", "Null output");
    Require(nod_read(disc.get(), nullptr, 4) == -1, "Read into null");
    Require(nod_disc_open(nullptr, nullptr, &handle) == NOD_RESULT_ERR_INVALID_HANDLE, "Open null path");
    Require(nod_disc_open("\xff\xfe.iso", nullptr, &handle) == NOD_RESULT_ERR_OTHER, "Open invalid UTF-8");
    RequireError("invalid UTF-8 in path", "Open invalid UTF-8");
    nod_free(nullptr);

    // FST nodes that are no file, out of range, and lookups that miss.
    const BuiltPartition& data = retail.partitions[1];
    Require(nod_partition_open_file(partition.get(), FindIndex(data, "files"), &handle) == NOD_RESULT_ERR_FORMAT,
            "A directory opened as a file");
    RequireError("node is not a file", "Directory as a file");
    Require(nod_partition_open_file(partition.get(), std::uint32_t(data.nodes.size()), &handle) == NOD_RESULT_ERR_NOT_FOUND,
            "An FST index past the end opened");
    RequireError("FST index " + std::to_string(data.nodes.size()) + " out of range (total: "
                     + std::to_string(data.nodes.size()) + ")",
                 "FST index past the end");
    for (const char* path : {"files/missing.bin", "files/one.txt/inside", "missing/one.txt", "files/top.bin",
                             "deep/after.dat", "files/deep/after", "files/deep/deeper/mixed.case/x"})
        Require(nod_partition_find_file(partition.get(), path, nullptr, nullptr) == NOD_FST_STOP && nod_error_message() == nullptr,
                std::string("find_file(") + path + ") found a node or set an error");
}

RvzOptions Rvz(bool rvz, std::uint32_t codec, int level, std::uint32_t chunk)
{
    RvzOptions options;
    options.rvz = rvz;
    options.codec = codec;
    options.level = level;
    options.chunk = chunk;
    return options;
}

// Every codec, chunk sizes from RVZ's least to WIA's two exception lists a
// group, partition data in two segments, junk packed as seeds, groups kept
// as is, zero groups, and an RVZ split into files. WIA decodes with the same
// codecs as RVZ: its big chunks take one slow codec only, to keep the test fast.
void CheckRvzImages(const fs::path& work, const BuiltDisc& disc, const BuiltDisc& retail)
{
    struct Image
    {
        std::string name;
        RvzOptions options;
        const BuiltDisc* source;
    };
    std::vector<Image> images = {
        {"zstd.rvz", Rvz(true, kZstd, 3, 0x20000), &disc},   {"none.rvz", Rvz(true, kNone, 0, 0x20000), &disc},
        {"bzip2.rvz", Rvz(true, kBzip2, 1, 0x20000), &disc}, {"lzma.rvz", Rvz(true, kLzma, 0, 0x20000), &disc},
        {"lzma2.rvz", Rvz(true, kLzma2, 0, 0x20000), &disc}, {"small.rvz", Rvz(true, kZstd, 1, 0x8000), &disc},
        {"big.rvz", Rvz(true, kZstd, 1, 0x200000), &disc},   {"none.wia", Rvz(false, kNone, 0, 0x200000), &disc},
        {"zstd.wia", Rvz(false, kZstd, 1, 0x200000), &disc}, {"lzma2.wia", Rvz(false, kLzma2, 0, 0x400000), &disc},
        {"retail.rvz", Rvz(true, kZstd, 1, 0x20000), &retail},
    };
    images[0].options.nkit = true;
    images[5].options.first_segment = 7;
    images[6].options.first_segment = 70;
    for (const Image& image : images)
    {
        const BuiltPartition& partition = image.source->partitions[1];
        WriteFile(work / image.name, MakeRvz(*image.source, image.options));
        {
            Owned opened = OpenDisc(work / image.name);
            CheckDiscReads(opened, image.source->iso, *image.source, RvzMeta(image.source->iso, image.options),
                           PartitionDataSectors(*image.source));
            CheckPartition(opened, partition, partition.data, image.name);
            NodHandle* handle = nullptr;
            Require(nod_disc_open_partition_kind(opened.get(), NOD_PARTITION_KIND_UPDATE, nullptr, &handle) == NOD_RESULT_OK,
                    image.name + ": update partition");
            Owned update(handle);
            Require(ReadAll(update.get(), 1 << 20) == image.source->partitions[0].data, image.name + ": update partition data");
        }
        CheckSeeksAndProbes(work / image.name, partition, image.options.chunk < 0x200000);
    }
    {
        Owned opened = OpenDisc(work / "retail.rvz");
        CheckConcurrentReads(opened, retail.partitions[1]);
    }
    // nod's first split scheme: game.rvz, game.rvz.1 ...
    const Bytes split = MakeRvz(disc, Rvz(true, kZstd, 1, 0x20000));
    WriteSplit(split, {split.size() / 3 + 1, split.size() * 2 / 3 + 3}, [&](int i) {
        return work / (i ? "split.rvz." + std::to_string(i) : std::string("split.rvz"));
    });
    Owned opened = OpenDisc(work / "split.rvz");
    CheckPartition(opened, disc.partitions[1], disc.partitions[1].data, "split.rvz");
}

void CheckRvzErrors(const fs::path& work, const BuiltDisc& disc)
{
    const BuiltPartition& data = disc.partitions[1];
    // Damaged headers, made consistent again where the damage is not the point.
    const auto rehash = [](Bytes& file) {
        Put(file, 0x10, Sha1(Slice(file, 0x48, 0xDC)));
        Put(file, 0x34, Sha1(Slice(file, 0, 0x34)));
    };
    const auto write = [&](const std::string& name, Bytes file, const std::function<void(Bytes&)>& change) {
        change(file);
        WriteFile(work / name, file);
        return work / name;
    };
    const Bytes rvz = MakeRvz(disc, Rvz(true, kZstd, 1, 0x20000));
    const Bytes wia = MakeRvz(disc, Rvz(false, kNone, 0, 0x200000));
    RequireOpenFails(write("head-hash.rvz", rvz, [](Bytes& f) { f[0x24] ^= 1; }), NOD_RESULT_ERR_FORMAT,
                     "disc format error: WIA/RVZ hash mismatch: ");
    RequireOpenFails(write("disc-hash.rvz", rvz, [](Bytes& f) { f[0x48 + 0x30] ^= 1; }), NOD_RESULT_ERR_FORMAT,
                     "WIA/RVZ hash mismatch");
    RequireOpenFails(write("partition-hash.rvz", rvz, [](Bytes& f) { f[0x48 + 0xDC] ^= 1; }), NOD_RESULT_ERR_FORMAT,
                     "WIA/RVZ hash mismatch");
    RequireOpenFails(write("version.rvz", rvz, [&](Bytes& f) { Put32(f, 8, 0x02000000); rehash(f); }),
                     NOD_RESULT_ERR_FORMAT, "Unsupported WIA/RVZ version: 0x1000000");
    RequireOpenFails(write("old.rvz", rvz, [&](Bytes& f) { Put32(f, 4, 0x00020000); rehash(f); }), NOD_RESULT_ERR_FORMAT,
                     "Unsupported WIA/RVZ version: 0x20000");
    RequireOpenFails(write("chunk.rvz", rvz, [&](Bytes& f) { Put32(f, 0x48 + 12, 0x18000); rehash(f); }),
                     NOD_RESULT_ERR_FORMAT, "Invalid RVZ chunk size: 0x18000");
    RequireOpenFails(write("chunk.wia", wia, [&](Bytes& f) { Put32(f, 0x48 + 12, 0x100000); rehash(f); }),
                     NOD_RESULT_ERR_FORMAT, "Invalid WIA chunk size: 0x100000");
    RequireOpenFails(write("huge.rvz", rvz, [&](Bytes& f) { Put32(f, 0x48 + 12, 0x4000000); rehash(f); }),
                     NOD_RESULT_ERR_FORMAT, "WIA/RVZ chunk size 0x4000000 is larger than this build reads");
    RequireOpenFails(write("purge.wia", wia, [&](Bytes& f) { Put32(f, 0x48 + 4, kPurge); rehash(f); }),
                     NOD_RESULT_ERR_FORMAT, "WIA images compressed with purge are not supported on this build");
    RequireOpenFails(write("codec.rvz", rvz, [&](Bytes& f) { Put32(f, 0x48 + 4, 9); rehash(f); }), NOD_RESULT_ERR_FORMAT,
                     "Invalid compression type 9");
    RequireOpenFails(write("type.rvz", rvz, [&](Bytes& f) { Put32(f, 0x48, 3); rehash(f); }), NOD_RESULT_ERR_FORMAT,
                     "Invalid disc type 3");
    RequireOpenFails(write("structs.rvz", rvz, [&](Bytes& f) { Put32(f, 0x48 + 0x94, 0x2C); rehash(f); }),
                     NOD_RESULT_ERR_FORMAT, "WIA/RVZ partition type size is 44, expected 48");
    RequireOpenFails(write("props.rvz", MakeRvz(disc, Rvz(true, kLzma, 0, 0x20000)),
                           [&](Bytes& f) {
                               f[0x48 + 0xD4] = 4;
                               rehash(f);
                           }),
                     NOD_RESULT_ERR_FORMAT, "Invalid LZMA props length: 4");
    RequireOpenFails(write("short.rvz", rvz, [](Bytes& f) { f.resize(0x100); }), NOD_RESULT_ERR_IO,
                     "WIA/RVZ disc header");

    // Damaged groups: the image opens, reading their data fails.
    const auto failing_read = [&](const std::string& name, const std::string& message) {
        Owned opened = OpenDisc(work / name);
        Owned partition = OpenData(opened);
        Bytes out(data.data.size());
        std::int64_t got = 0;
        std::size_t done = 0;
        while ((got = nod_read(partition.get(), out.data() + done, out.size() - done)) > 0) done += std::size_t(got);
        Require(got == -1, name + ": a damaged group read without error");
        RequireError(message, name);
        RequireError("Reading WIA/RVZ group", name);
    };
    std::vector<GroupRecord> groups;
    Bytes damaged = MakeRvz(disc, Rvz(true, kZstd, 1, 0x20000), &groups);
    const auto first = [&](const std::function<bool(const GroupRecord&)>& match) {
        // Group 1 holds the data partition's boot block and FST, read while opening.
        const auto found = std::find_if(groups.begin() + 2, groups.end(), match);
        Require(found != groups.end(), "No group to damage");
        return std::size_t(found - groups.begin());
    };
    const std::size_t compressed =
        first([](const GroupRecord& g) { return g.partition && g.compressed && !g.packed && g.size; });
    damaged[groups[compressed].offset] ^= 0xFF; // the zstd frame's magic
    WriteFile(work / "frame.rvz", damaged);
    failing_read("frame.rvz", "Invalid Zstandard frame header");
    RvzOptions unpacked = Rvz(true, kZstd, 1, 0x20000);
    unpacked.tamper = [&](std::vector<GroupRecord>& records) {
        for (std::size_t i = 2; i < records.size(); ++i)
            if (records[i].partition && records[i].packed)
            {
                records[i].packed = 0; // its packed data is then the wrong size
                return;
            }
    };
    WriteFile(work / "unpacked.rvz", MakeRvz(disc, unpacked));
    failing_read("unpacked.rvz", "data size mismatch");
    RvzOptions moved = Rvz(true, kZstd, 1, 0x20000);
    moved.tamper = [&](std::vector<GroupRecord>& records) {
        for (std::size_t i = 2; i < records.size(); ++i)
            if (records[i].partition && records[i].size)
            {
                records[i].offset = 0x7FFFFFF0ull * 4; // past the end of the file
                return;
            }
    };
    WriteFile(work / "moved.rvz", MakeRvz(disc, moved));
    failing_read("moved.rvz", "unexpected end of the file");
    // As stored, partition data is hashed and encrypted: a raw read of it fails.
    Owned opened = OpenDisc(work / "split.rvz");
    Bytes out(16);
    Require(nod_seek(opened.get(), std::int64_t(data.data_offset), 0) > 0 && nod_read(opened.get(), out.data(), 16) == -1,
            "A raw read of RVZ partition data did not fail");
    RequireError("the image stores this partition data decrypted", "Raw RVZ partition read");
}

void CheckHeaderCopy(const char* copy, const char* prepared)
{
    const auto read = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        Require(bool(in), std::string("Cannot read ") + path);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string ours = read(copy), theirs = read(prepared);
    const std::string marker = "/* ---- prepared nod-ffi/include/nod.h follows ---- */\n";
    const std::size_t at = ours.find(marker);
    Require(at != std::string::npos, "The nod.h copy lacks its marker line");
    Require(ours.compare(at + marker.size(), std::string::npos, theirs) == 0,
            "src/platform/wii_disc/include/nod.h no longer matches the prepared nod.h; copy it again");
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        // WORK_DIR [NOD_H_COPY PREPARED_NOD_H] [--keep]: --keep leaves the images.
        std::vector<std::string> args(argv + 1, argv + argc);
        const bool keep = !args.empty() && args.back() == "--keep";
        if (keep) args.pop_back();
        if (args.size() != 1 && args.size() != 3)
        {
            std::cerr << "usage: wii_disc_reader_tests WORK_DIR [NOD_H_COPY PREPARED_NOD_H] [--keep]\n";
            return 2;
        }
        if (args.size() == 3) CheckHeaderCopy(args[1].c_str(), args[2].c_str());
        CheckAes();
        CheckSha1AndJunk();
        const fs::path work = args[0];
        fs::remove_all(work);
        fs::create_directories(work);

        // An update partition (Korean key) before the data partition (retail
        // key), with an unused gap between them.
        const BuiltDisc retail = BuildDisc({UpdatePartition(), DataPartition(0x100000, true)});
        const BuiltPartition& data = retail.partitions[1];
        WriteFile(work / "retail.iso", retail.iso);
        {
            Owned disc = OpenDisc(work / "retail.iso");
            CheckDiscReads(disc, retail.iso, retail, IsoMeta(retail.iso), {});
            CheckPartition(disc, data, data.data, "ISO");
            CheckConcurrentReads(disc, data);
        }
        CheckSeeksAndProbes(work / "retail.iso", data);
        // Split ISOs, cut inside sectors: game.iso + .1 + .2, and game.part0.iso ...
        WriteSplit(retail.iso, {1000001, 3333333}, [&](int i) {
            return work / (i ? "split.iso." + std::to_string(i) : std::string("split.iso"));
        });
        WriteSplit(retail.iso, {0x8000 * 3 + 5, 2000000}, [&](int i) {
            return work / ("parts.part" + std::to_string(i) + ".iso");
        });
        for (const char* name : {"split.iso", "parts.part0.iso"})
        {
            Owned disc = OpenDisc(work / name);
            CheckDiscReads(disc, retail.iso, retail, IsoMeta(retail.iso), {});
            CheckPartition(disc, data, data.data, name);
        }

        // WBFS: without the gap's whole blocks and the whole blocks of unused
        // data-partition clusters; with stored blocks out of disc order.
        for (const unsigned shift : {18u, 20u})
        {
            const std::uint32_t block = 1u << shift, per_block = block / kSector;
            std::set<std::uint32_t> left_out, left_out_sectors;
            const std::uint64_t unused = data.data_offset + std::uint64_t(data.used_clusters) * kSector;
            const std::uint64_t end = data.data_offset + std::uint64_t(data.clusters) * kSector;
            for (std::uint64_t b = (unused + block - 1) / block; (b + 1) * block <= end; ++b)
            {
                left_out.insert(std::uint32_t(b));
                for (std::uint32_t s = 0; s < per_block; ++s) left_out_sectors.insert(std::uint32_t(b * per_block + s));
            }
            const std::uint64_t gap_begin = 0x70000 + std::uint64_t(retail.partitions[0].clusters) * kSector;
            for (std::uint64_t b = (gap_begin + block - 1) / block; (b + 1) * block <= 0x100000; ++b)
                left_out.insert(std::uint32_t(b));
            Require(!left_out_sectors.empty(), "WBFS fixture leaves out no partition data");
            const Bytes wbfs = MakeWbfs(retail.iso, shift, left_out);
            const std::string name = "retail-" + std::to_string(shift);
            WriteFile(work / (name + ".wbfs"), wbfs);
            const Bytes holed = DataWithoutBlocks(data, left_out_sectors);
            {
                Owned disc = OpenDisc(work / (name + ".wbfs"));
                CheckDiscReads(disc, retail.iso, retail, WbfsMeta(block), left_out_sectors);
                CheckPartition(disc, data, holed, name + ".wbfs");
            }
            CheckSeeksAndProbes(work / (name + ".wbfs"), data);
            // The same WBFS in three files, cut inside sectors: .wbfs, .wbf1, .wbf2.
            WriteSplit(wbfs, {block + 4097, block * 5 + 33}, [&](int i) {
                return work / (name + "-split.wbf" + (i ? std::to_string(i) : std::string("s")));
            });
            Owned disc = OpenDisc(work / (name + "-split.wbfs"));
            CheckDiscReads(disc, retail.iso, retail, WbfsMeta(block), left_out_sectors);
            CheckPartition(disc, data, holed, name + "-split.wbfs");
            CheckConcurrentReads(disc, data);
        }

        // Debug-signed tickets, decrypted images with and without hashes.
        PartitionSpec debug = DataPartition(0x50000, false);
        debug.issuer = kDebugIssuer;
        debug.common_key = &kDebugKey;
        debug.key_index = 0;
        struct Variant
        {
            const char* name;
            PartitionSpec spec;
            bool encrypted, hashed;
        };
        for (const Variant& variant : {Variant{"debug.iso", debug, true, true},
                                       Variant{"decrypted.iso", DataPartition(0x50000, false), false, true},
                                       Variant{"unhashed.iso", DataPartition(0x50000, false), false, false}})
        {
            const BuiltDisc built = BuildDisc({variant.spec}, variant.encrypted, variant.hashed);
            WriteFile(work / variant.name, built.iso);
            Owned disc = OpenDisc(work / variant.name);
            CheckPartition(disc, built.partitions[0], built.partitions[0].data, variant.name);
            CheckSeeksAndProbes(work / variant.name, built.partitions[0]);
        }

        // An ISO that ends inside a file: reads end with its last cluster, as nod's do.
        {
            const std::uint32_t index = FindIndex(data, "files/large.bin");
            const std::uint64_t cut = data.data_offset + (data.nodes[index].offset / kSectorData + 10) * kSector;
            WriteFile(work / "cut.iso", retail.iso, 0, std::size_t(cut));
            Owned disc = OpenDisc(work / "cut.iso");
            Owned partition = OpenData(disc);
            Owned file = OpenFile(partition, index);
            const Bytes got = ReadAll(file.get(), 1 << 20);
            const std::size_t whole = std::size_t((cut - data.data_offset) / kSector * kSectorData - data.nodes[index].offset);
            Require(got.size() == whole && std::equal(got.begin(), got.end(), data.nodes[index].data.begin()),
                    "A cut ISO did not end the file at its last whole cluster");
        }

        // RVZ and WIA, mostly of a disc without the large file.
        const BuiltDisc medium = BuildDisc({UpdatePartition(), DataPartition(0x100000, false)});
        CheckRvzImages(work, medium, retail);
        CheckRvzErrors(work, medium);
        CheckErrors(work, retail);
        if (!keep) fs::remove_all(work);
        std::cout << "Wii disc reader: AES, SHA-1, junk, ISO, split ISO, WBFS, split WBFS, RVZ in every codec, WIA, "
                     "decrypted images, seeks, cache probes, concurrent reads and error paths passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}

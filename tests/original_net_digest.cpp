#include <revolution/net/NETDigest.h>
#include <cstdio>

int main()
{
    static_assert(sizeof(u32) == 4);
    struct Storage { u32 before; u32 word; u32 after; } storage{};
    const u32 values[] = {0, 1, 0x01020304u, 0x12345678u, 0x7fffffffu,
        0x80000000u, 0xae597f5eu, 0xff0000ffu, 0xffffffffu};
    unsigned checks = 0;
    for (u32 value : values)
    {
        storage.before = 0xa5a5a5a5u;
        storage.after = 0x5a5a5a5au;
        NETWriteSwappedBytes32(&storage.word, value);
        const u8* bytes = reinterpret_cast<const u8*>(&storage.word);
        const u32 oracle = (u32)bytes[0] + (u32)bytes[1] * 256u +
            (u32)bytes[2] * 65536u + (u32)bytes[3] * 16777216u;
        if (oracle != value || NETReadSwappedBytes32(&storage.word) != value ||
            storage.before != 0xa5a5a5a5u || storage.after != 0x5a5a5a5au)
            return 1;
        checks += 4;
    }
    // Independent authored bytes: little-endian memory is the contract even
    // when the host scalar itself uses another byte order.
    u8* bytes = reinterpret_cast<u8*>(&storage.word);
    bytes[0] = 0x78; bytes[1] = 0x56; bytes[2] = 0x34; bytes[3] = 0x12;
    if (NETReadSwappedBytes32(&storage.word) != 0x12345678u ||
        NETSwapBytes32(0x12345678u) != 0x78563412u)
        return 2;
    checks += 2;
    std::printf("Original NET byte-reverse memory: %u checks passed\n", checks);
}

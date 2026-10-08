#include "platform/string_format.h"
#include <array>
#include <charconv>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
unsigned checks = 0;
unsigned char pointerStorage = 0;

void Check(bool condition, const char* description)
{
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

int Format(unsigned short* output, std::size_t count, const unsigned short* format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = mscharged_format_wii16(output, count, format, args);
    va_end(args);
    return result;
}

template<class... Args>
void Observe(std::u16string_view expected, const char16_t* format, Args... args)
{
    static_assert(sizeof(char16_t) == sizeof(unsigned short));
    std::array<unsigned short, 130> output;
    output.fill(0x55aa);
    Check(expected.size() < 128, "Fixture output exceeds its owned buffer");
    const int actual = Format(output.data() + 1, 128,
        reinterpret_cast<const unsigned short*>(format), args...);
    if (actual != static_cast<int>(expected.size())) {
        std::cerr << "Format ";
        for (const char16_t* c = format; *c; ++c) std::cerr << static_cast<char>(*c);
        std::cerr << " returned " << actual << ", expected " << expected.size() << '\n';
    }
    Check(actual == static_cast<int>(expected.size()), "Original MSL varargs output length differs");
    for (std::size_t i = 0; i < output.size(); ++i) {
        const unsigned short word = (i == 0 || i > expected.size() + 1) ? 0x55aa
            : i == expected.size() + 1 ? 0 : static_cast<unsigned short>(expected[i - 1]);
        Check(output[i] == word, "Original MSL varargs output or buffer guard differs");
    }
}

void CursorChecks()
{
    Observe(u"   007:9", u"%*.*d:%d", 6, 3, 7, 9);
    Observe(u"007   :9", u"%*.*d:%d", -6, 3, 7, 9);
    Observe(u"[     7]:9", u"[%*.*d]:%d", 6, -1, 7, 9);
    Observe(u"    3.50|17", u"%*.*f|%d", 8, 2, 3.5, 17);
    unsigned short wide[] = {'A', 0x03a9, 'B', 0};
    Observe(u"     A\u03a9|17", u"%*.*ls|%d", 7, 2, wide, 17);
    Observe(u"32  /   5/71", u"%*u/%*u/%d", -4, 32u, 4, 5u, 71);
    // The actual %lc conversion establishes the original temporary before
    // zero-length narrow %s. No source field or temporary is installed here.
    Observe(u"#[    ][    ]", u"%lc[%*.*s][%*.*ls]", static_cast<int>('#'),
        4, 0, static_cast<char*>(nullptr), 4, 0, static_cast<unsigned short*>(nullptr));
}

void CarrierChecks()
{
    static_assert(sizeof(std::size_t) == 8 && sizeof(std::ptrdiff_t) == 8);
    const std::size_t largeSize = (std::size_t{1} << 40) + 17;
    const std::ptrdiff_t largeDifference = (std::ptrdiff_t{1} << 40) + 17;
    // These match the original reads: %zd takes size_t, and %tx takes
    // ptrdiff_t. This gate does not redesign the source's signedness choices.
    Observe(u"1099511627793/1099511627793/-1099511627793/123456789abcdef",
        u"%zu/%zd/%td/%tx", largeSize, largeSize, -largeDifference,
        static_cast<std::ptrdiff_t>(0x123456789abcdefLL));
    Observe(u"18446744073709551615/ffffffffffffffff", u"%zu/%zx",
        std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max());
    Observe(u"-2147483648/2147483647/4294967295", u"%d/%d/%u",
        std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
        std::numeric_limits<unsigned int>::max());
    if constexpr (sizeof(long) == 4) {
        Observe(u"-2147483648/2147483647/4294967295", u"%ld/%ld/%lu",
            std::numeric_limits<long>::min(), std::numeric_limits<long>::max(),
            std::numeric_limits<unsigned long>::max());
    } else {
        Observe(u"-1099511627793/1099511627793/18446744073709551615", u"%ld/%ld/%lu",
            static_cast<long>(-largeDifference), static_cast<long>(largeDifference),
            std::numeric_limits<unsigned long>::max());
    }
    Observe(u"-1099511627793/18446744073709551615", u"%lld/%llu",
        static_cast<long long>(-largeDifference), std::numeric_limits<unsigned long long>::max());
    struct Counts {
        std::uint64_t before = 0x123456789abcdef0ULL;
        std::size_t size = 0;
        std::ptrdiff_t difference = 0;
        std::uint64_t after = 0xfedcba9876543210ULL;
    } counts;
    Observe(u"abc|", u"abc%zn|%tn", &counts.size, &counts.difference);
    Check(counts.size == 3 && counts.difference == 4, "Original %n pointer types differ");
    Check(counts.before == 0x123456789abcdef0ULL && counts.after == 0xfedcba9876543210ULL,
        "Original %n crossed its owned fields");
}

void PointerChecks()
{
    // A real live native image object supplies the pointer. No synthetic
    // address is dereferenced or forced by mmap or platform-specific APIs.
    void* pointer = &pointerStorage;
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    Check(address > std::numeric_limits<std::uint32_t>::max(),
        "Pointer regression needs the actual image object above 4 GiB");
    std::array<char, 2 * sizeof(address)> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), address, 16);
    Check(converted.ec == std::errc{}, "Host pointer oracle conversion failed");
    const std::size_t length = converted.ptr - digits.data();
    std::u16string expected = u"0x";
    if (length < 8) expected.append(8 - length, u'0');
    for (std::size_t i = 0; i < length; ++i) expected += static_cast<char16_t>(digits[i]);
    Observe(expected, u"%p", pointer);
    Observe(u"0x00000000", u"%p", static_cast<void*>(nullptr));
    Check(expected.size() <= 20, "Pointer width oracle exceeds field width");
    std::u16string right(20 - expected.size(), u' ');
    right += expected;
    Observe(right, u"%20p", pointer);
    std::u16string left = expected;
    left.append(20 - expected.size(), u' ');
    Observe(left, u"%-20p", pointer);
}

void MinimumChecks()
{
    static_assert(sizeof(std::ptrdiff_t) == 8);
    const auto minimum = std::numeric_limits<std::ptrdiff_t>::min();
    // %zi still reads size_t in the original source. Its unsigned carrier
    // supplies the same minimum-word bits without a mismatched va_arg type.
    Observe(u"-9223372036854775808/-9223372036854775808", u"%td/%zi",
        minimum, static_cast<std::size_t>(minimum));
    if constexpr (sizeof(long) == 8) {
        Observe(u"-9223372036854775808", u"%ld", std::numeric_limits<long>::min());
    } else {
        Observe(u"-2147483648", u"%ld", std::numeric_limits<long>::min());
    }
    // The separate original long-long helper already has its 64-bit sentinel.
    Observe(u"-9223372036854775808", u"%lld", std::numeric_limits<long long>::min());
}
}

int main(int argc, char** argv)
{
    try {
        const std::string_view mode = argc == 1 ? "all" : argc == 2 ? argv[1] : "invalid";
        if (mode == "all" || mode == "--cursor") CursorChecks();
        if (mode == "all" || mode == "--carriers") CarrierChecks();
        if (mode == "all" || mode == "--pointer") PointerChecks();
        if (mode == "all" || mode == "--minimum") MinimumChecks();
        Check(mode == "all" || mode == "--cursor" || mode == "--carriers" || mode == "--pointer"
            || mode == "--minimum", "Use --cursor, --carriers, --pointer, --minimum, or no arguments");
        std::cout << checks << " original MSL varargs checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

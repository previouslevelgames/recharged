#include "platform/string_format.h"
#include <array>
#include <cstdarg>
#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Check(bool condition)
{
    ++checks;
    if (!condition) throw std::runtime_error("Original MSL literal formatting changed");
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
void Observe(const char16_t* expected, const unsigned short* format, Args... args)
{
    std::array<unsigned short, 66> output;
    output.fill(0x55aa);
    std::size_t length = 0;
    while (expected[length]) ++length;
    Check(length < 64);
    Check(Format(output.data() + 1, 64, format, args...) == static_cast<int>(length));
    for (std::size_t i = 0; i < output.size(); ++i) {
        const unsigned short word = (i == 0 || i > length + 1) ? 0x55aa
            : i == length + 1 ? 0 : static_cast<unsigned short>(expected[i - 1]);
        Check(output[i] == word);
    }
}
}

int main()
{
    try {
        static_assert(sizeof(unsigned short) == 2);
        // The original %lc conversion establishes its temporary before the
        // zero-length narrow conversion; no source buffer or flag is seeded.
        const unsigned short nulls[] = {'%', 'l', 'c', '[', '%', 's', ']', '[', '%', 'l', 's', ']', 0};
        Observe(u"#[][]", nulls, static_cast<int>('#'), static_cast<char*>(nullptr),
            static_cast<unsigned short*>(nullptr));
        const unsigned short widths[] = {'%', 'l', 'c', '[', '%', '6', '.', '0', 's', ']',
            '[', '%', '-', '6', '.', '0', 'l', 's', ']', 0};
        Observe(u"#[      ][      ]", widths, static_cast<int>('#'), static_cast<char*>(nullptr),
            static_cast<unsigned short*>(nullptr));
        const unsigned short values[] = {'%', '.', '2', 'f', '/', '%', 's', '/', '%', 'l', 's', 0};
        char narrow[] = "font";
        unsigned short wide[] = {'M', 0x03a9, 0x6f22, 0};
        Observe(u"3.50/font/M\u03a9\u6f22", values, 3.5, narrow, wide);
        std::cout << checks << " original MSL literal checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#pragma once
#include <cstddef>
#include <cstdarg>
namespace mscharged
{
int FormatString(char* output, std::size_t count, const char* format, va_list arguments);
int FormatString(unsigned short* output, std::size_t count, const unsigned short* format, va_list arguments);
}

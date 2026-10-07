#include "platform/ax_studio_depop.h"
#include <cstring>
namespace mscharged::platform {
namespace {
std::int64_t Floor(std::int64_t value) {
    return value>=0?value/65536:-((-value+65535)/65536);
}
std::int32_t Store(std::int64_t accumulator) {
    const auto raw=std::uint64_t(accumulator)&0xffffffffffULL;
    const auto value=raw&0x8000000000ULL?std::int64_t(raw)-0x10000000000LL:std::int64_t(raw);
    auto middle=Floor(value);if(middle>32767)middle=32767;if(middle< -32768)middle= -32768;
    const std::uint32_t word=(std::uint32_t(std::uint16_t(middle))<<16)|std::uint32_t(raw&65535);
    std::int32_t result;std::memcpy(&result,&word,4);return result;
}
template<std::size_t Frames> std::array<std::int32_t,Frames> Ramp(std::int32_t value,std::int16_t delta) {
    std::array<std::int32_t,Frames> result{};
    // Owned TST/NZ selects the zero-fill path before any delta arithmetic.
    if(!value)return result;
    std::int64_t accumulator=value;
    for(std::size_t i=0;i<Frames;++i) {
        result[i]=Store(accumulator);accumulator+=delta;
    }
    return result;
}
}
std::array<std::int32_t,96> NativeAXStudioDepop96(std::int32_t value,std::int16_t delta) {return Ramp<96>(value,delta);}
std::array<std::int32_t,18> NativeAXStudioDepop18(std::int32_t value,std::int16_t delta) {return Ramp<18>(value,delta);}
}

#include "revolution/hbm/nw4hbm/math/arithmetic.h"
#include "revolution/hbm/nw4hbm/ut/Rect.h"
#include "revolution/hbm/nw4hbm/lyt/types.h"
#include <algorithm>
#include <array>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
unsigned checks{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
f32 Float(u32 bits) { f32 value; std::memcpy(&value, &bits, sizeof(value)); return value; }
u32 Bits(f32 value) { u32 bits; std::memcpy(&bits, &value, sizeof(bits)); return bits; }
struct Condition { u32 bits; bool positive; };
}

int main() {
    try {
        // Primary fsel contract: signed zeros choose ifPos, all NaNs choose
        // ifNeg. Output operands are copied, not compared or arithmetically used.
        const std::array<Condition, 18> conditions{{
            {0x00000000,true}, {0x80000000,true},
            {0x00000001,true}, {0x80000001,false},
            {0x00800000,true}, {0x80800000,false},
            {0x3f800000,true}, {0xbf800000,false},
            {0x7f7fffff,true}, {0xff7fffff,false},
            {0x7f800000,true}, {0xff800000,false},
            {0x7fc00000,false}, {0xffc00000,false},
            {0x7fc12345,false}, {0xffc12345,false},
            {0x7f812345,false}, {0xff812345,false}
        }};
        const std::array<u32,8> operands{{0x00000000,0x80000000,0x3f800000,0xbf800000,
            0x7f800000,0xff800000,0x7fc12345,0xff812345}};
        for (const auto cond : conditions) {
            for (unsigned index=0; index<operands.size(); ++index) {
                const auto positive=operands[index], negative=operands[(index+3)%operands.size()];
                std::feclearexcept(FE_ALL_EXCEPT);
                const auto value=nw4hbm::math::FSelect(Float(cond.bits),Float(positive),Float(negative));
                Check(Bits(value)==(cond.positive?positive:negative),"Native fsel changed the selected input bits");
                Check(std::fetestexcept(FE_ALL_EXCEPT)==0,"Native fsel introduced an FP exception");
            }
            std::feclearexcept(FE_ALL_EXCEPT);
            const auto absolute=nw4hbm::math::FAbs(Float(cond.bits));
            Check(Bits(absolute)==(cond.bits&0x7fffffff),"Native fabs changed magnitude/NaN payload bits");
            Check(std::fetestexcept(FE_ALL_EXCEPT)==0,"Native fabs introduced an FP exception");
        }
        // Execute unchanged original Rect::Normalize for every finite ordering.
        for (f32 left : {-8.f,0.f,5.f}) for (f32 right : {-8.f,0.f,5.f})
        for (f32 top : {-8.f,0.f,5.f}) for (f32 bottom : {-8.f,0.f,5.f}) {
            nw4hbm::ut::Rect rect(left,top,right,bottom);
            rect.Normalize();
            Check(rect.left==std::min(left,right) && rect.right==std::max(left,right)
                &&rect.top==std::min(top,bottom) && rect.bottom==std::max(top,bottom),
                "Original Rect normalization did not preserve its four source bounds");
        }
        for (u16 value : {u16{0},u16{255},u16{32768},u16{65535}})
            Check(nw4hbm::math::F32ToU16(nw4hbm::math::U16ToF32(value))==value,
                "Representable native fast-cast roundtrip changed a source u16");
        // These are actual CPU byte-array addresses, not completed game assets.
        // No resource parsing, typed record access or owner lifetime is implied.
        std::array<u8,192> byteArray{};
        Check(reinterpret_cast<std::uintptr_t>(byteArray.data()) > UINT32_MAX,
            "Native offset transport probe did not exercise a high host address");
        for (unsigned offset : {0u,1u,44u,191u}) {
            Check(nw4hbm::lyt::detail::ConvertOffsToPtr<u8>(byteArray.data(),offset)
                == byteArray.data()+offset,"Mutable native byte offset truncated its source base");
            Check(nw4hbm::lyt::detail::ConvertOffsToPtr<u8>(static_cast<const void*>(byteArray.data()),offset)
                == byteArray.data()+offset,"Const native byte offset truncated its source base");
        }
        std::printf("Native HBM arithmetic: %u checks; fsel/fabs bit/exception contract and original Rect orders; high-pointer byte offsets; extreme fast-cast/raw resource/whole UI HOLD\n",checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"Native HBM arithmetic failed: %s\n",error.what());
        return 1;
    }
}

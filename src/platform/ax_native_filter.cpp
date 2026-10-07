#include "platform/ax_native_filter.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace mscharged::platform {
namespace {
constexpr double Pi=3.141592653589793238462643383279502884;
double Sinc(double value) { return value==0 ? 1.0 : std::sin(Pi*value)/(Pi*value); }
}
NativeAXNativeFilter::NativeAXNativeFilter() {
    for(unsigned bank=0;bank<3;++bank)for(unsigned phase=0;phase<128;++phase) {
        const double cutoff=double(bank+2)/8; // explicit native .25/.375/.5 policy
        const double center=1.0+double(phase)/128;
        std::array<double,4> weights{};double total=0;
        for(unsigned tap=0;tap<4;++tap) {
            const double distance=double(tap)-center;
            weights[tap]=2*cutoff*Sinc(2*cutoff*distance)*Sinc(distance/2);
            total+=weights[tap];
        }
        if(!std::isfinite(total)||total<=0)
            throw std::runtime_error("native AX filter design has invalid normalization");
        auto& row=rows_[bank][phase];int sum=0;
        for(unsigned tap=0;tap<4;++tap) {
            const auto rounded=std::llround(weights[tap]*32768/total);
            row[tap]=static_cast<std::int16_t>(std::clamp<long long>(rounded,-32768,32767));
            sum+=row[tap];
        }
        // Quantize DC exactly to1 inQ15. Apply the small signed residual to
        // the closest coefficient with available signed16 headroom; ties
        // prefer the higher index. This also represents the phase0 Nyquist
        // unit impulse as32767+1 rather than overflowing one signed16 tap.
        int residual=32768-sum;
        while(residual) {
            unsigned selected=4;double closest=std::numeric_limits<double>::infinity();
            for(unsigned tap=0;tap<4;++tap) {
                if((residual>0&&row[tap]==32767)||(residual<0&&row[tap]==-32768))continue;
                const double distance=std::abs(double(tap)-center);
                if(distance<=closest){selected=tap;closest=distance;}
            }
            if(selected==4)throw std::runtime_error("native AX filter residual exceeds signedQ15 support");
            const int adjustment=residual>0 ? 1 : -1;
            row[selected]=static_cast<std::int16_t>(int(row[selected])+adjustment);
            residual-=adjustment;
        }
    }
}
NativeAXCoefficientView NativeAXNativeFilter::View() const noexcept {
    return {NativeAXCoefficientPolicy::NativeWindowedSinc4TapV1,this,
        [](const void* context,std::uint16_t bank,std::uint16_t fraction) {
            return static_cast<const NativeAXNativeFilter*>(context)->Row(bank,fraction);
        }};
}
std::array<std::int16_t,4> NativeAXNativeFilter::Row(
    std::uint16_t bank,std::uint16_t fraction) const {
    if(bank>2)throw NativeAXVoiceError(NativeAXVoiceFailure::UnknownCoefficientBank,
                                     "native AX filter has only the source-selected three banks");
    return rows_[bank][fraction>>9];
}
} // namespace mscharged::platform

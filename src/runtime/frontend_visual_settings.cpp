#include "runtime/frontend_visual_settings.h"
#include <cmath>
#include <limits>
#include <stdexcept>
namespace mscharged
{
namespace
{
void Check(bool value,const char* message){if(!value)throw std::logic_error(message);}
void Validate(float zoom){Check(std::isfinite(zoom)&&zoom>=0&&zoom<=1,"Visual zoom must be finite and in 0..1");}
}
FrontendVisualSettings::FrontendVisualSettings(bool automatic,float zoom):value_{automatic,zoom,0}{Validate(zoom);}
void FrontendVisualSettings::Ready()const{Check(thread_==std::this_thread::get_id(),"Visual settings require their owning thread");}
FrontendVisualSettingsSnapshot FrontendVisualSettings::Snapshot()const{Ready();return value_;}
void FrontendVisualSettings::Set(const FrontendVisualSettingsSnapshot& expected,bool automatic,float zoom)
{
    Ready();Check(value_==expected,"Visual settings were changed by another authority");Validate(zoom);
    Check(value_.revision!=std::numeric_limits<std::uint64_t>::max(),"Visual settings revision exhausted");
    value_={automatic,zoom,value_.revision+1};
}
}

#include "runtime/frontend_title_dimming.h"
#include <stdexcept>
#include <thread>
namespace mscharged
{
struct FrontendTitleDimming::Implementation
{
    std::function<bool(unsigned)> service;
    const std::thread::id thread=std::this_thread::get_id();
    FrontendTitleDimmingStatus status;
    bool busy=false;
    void Ready()const{if(thread!=std::this_thread::get_id())throw std::logic_error("Title dimming requires its owner thread");}
};
FrontendTitleDimming::FrontendTitleDimming(std::function<bool(unsigned)> service):impl_(std::make_unique<Implementation>())
{impl_->service=std::move(service);}
FrontendTitleDimming::~FrontendTitleDimming()=default;
bool FrontendTitleDimming::Admit(unsigned request)
{
    auto& s=*impl_;s.Ready();
    if(s.busy||(request!=0&&request!=2))throw std::logic_error("Title dimming requires a nonrecursive original0/2 request");
    if(s.status.pending&&*s.status.pending!=request)throw std::logic_error("Pending Title dimming must complete before a different request");
    if(request==2&&s.status.armed){s.status.pending.reset();return true;}
    s.status.pending=request;
    if(!s.service)return false;
    s.busy=true;struct Reset{bool& busy;~Reset(){busy=false;}}reset{s.busy};
    if(!s.service(request))return false;
    s.status.armed=request==2;s.status.pending.reset();return true;
}
FrontendTitleDimmingStatus FrontendTitleDimming::Status()const{impl_->Ready();return impl_->status;}
}

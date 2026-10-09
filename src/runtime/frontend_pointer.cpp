#include "runtime/frontend_pointer.h"
#include "Game/FE/FrontendPointerSteps.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace mscharged
{
namespace
{
void Check(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
void Scalar(float value)
{ Check(std::isfinite(value) && std::abs(value) <= 1.0e12f, "Frontend pointer coordinate exceeds supported range"); }
void Point(std::array<float,2> p) { Scalar(p[0]); Scalar(p[1]); }
nlVector2 Vector(std::array<float,2> p) { nlVector2 v; v.x=p[0]; v.y=p[1]; return v; }
void BoundsValid(const FrontendPointerBounds& b)
{
    for(float v:{b.min_x,b.max_x,b.min_y,b.max_y})Scalar(v);
    Point(b.pivot);
    const float angle=10430.378f*b.rotation;
    Check(std::isfinite(angle) && double(angle)>=-2147483648.0 && double(angle)<2147483648.0,
          "Frontend pointer rotation exceeds original angle conversion");
}
struct Position { struct { float x,y,z; } f; };
Position PositionOf(const std::array<float,3>& p) { Point({p[0],p[1]}); return {{p[0],p[1],p[2]}}; }
struct InstanceView
{
    const resources::FrontendInstance& instance;
    Position GetAssetPosition() const { return PositionOf(instance.attributes.position); }
    Position GetAssetRotation() const { return {{0,0,instance.attributes.rotation[2]}}; }
};
class Measurement
{
    unsigned height_, work_=0;
    std::map<std::uint32_t,const resources::FrontendInstance*> instances_;
    std::map<std::uint32_t,const resources::FrontendLibraryObject*> library_;
    std::map<std::uint32_t,const resources::FrontendSlide*> slides_;
    std::map<std::uint32_t,const resources::FrontendLayoutText*> texts_;
    std::set<std::uint32_t> path_;
    template<class T> static const T& Get(const std::map<std::uint32_t,const T*>& map, std::uint32_t id)
    { auto it=map.find(id); Check(it!=map.end(),"Frontend pointer has an unresolved graph reference"); return *it->second; }
    template<class T> static void Index(std::map<std::uint32_t,const T*>& map,const std::vector<T>& values)
    {
        Check(values.size()<=65536,"Frontend pointer graph exceeds node limit");
        for(const auto& value:values)Check(map.emplace(value.offset,&value).second,"Frontend pointer graph has duplicate IDs");
    }
    nlVector2 List(const std::vector<std::uint32_t>& ids)
    {
        if(ids.empty())return Vector({0,0});
        Check(ids.size()<=16384,"Frontend pointer ring exceeds work limit");
        std::set<std::uint32_t> ring;
        float minX=854/2, minY=float(height_/2), maxX=-minX, maxY=-minY;
        for(auto id:ids)
        {
            Check(ring.insert(id).second,"Frontend pointer ring contains duplicate nodes");
            const auto size=Size(id);
            const auto position=PositionOf(Get(instances_,id).attributes.position);
            FrontendPointerExpand(size,position,minX,minY,maxX,maxY);
        }
        return Vector({maxX-minX,maxY-minY});
    }
public:
    Measurement(const FrontendSessionFrame& frame,unsigned height):height_(height)
    {
        Index(instances_,frame.graph.instances);Index(library_,frame.graph.library);Index(slides_,frame.graph.slides);
        Check(frame.layout.entries.size()<=65536,"Frontend pointer layout exceeds entry limit");
        for(const auto& entry:frame.layout.entries)
            if(const auto* text=std::get_if<resources::FrontendLayoutText>(&entry))
            {
                const auto [it,inserted]=texts_.emplace(text->instance,text);
                if(!inserted)it->second=nullptr; // Ambiguous only if that text is measured.
            }
    }
    const resources::FrontendInstance& Instance(std::uint32_t id) const { return Get(instances_,id); }
    nlVector2 Size(std::uint32_t id)
    {
        Check(++work_<=16384&&path_.size()<64,"Frontend pointer measurement exceeds work/depth limit");
        Check(path_.insert(id).second,"Frontend pointer graph contains a cycle");
        struct Pop { std::set<std::uint32_t>& path; std::uint32_t id; ~Pop(){path.erase(id);} } pop{path_,id};
        const auto& instance=Instance(id);
        switch(instance.type)
        {
        case 1: case 5: return List(instance.children);
        case 2:
        {
            auto scale=instance.attributes.scale;
            if(!(instance.overload_flags&4))
            {
                Check(bool(instance.library),"Frontend pointer image has no library object");
                const auto& library=Get(library_,*instance.library);
                Check(library.type==1,"Frontend pointer image has a wrong library type");
                scale=library.attributes.scale;
            }
            Point({scale[0],scale[1]});
            const float height=scale[1]*100.0f;
            return Vector({scale[0]*100.0f,height});
        }
        case 3:
        {
            const auto found=texts_.find(id);
            const auto* text=found==texts_.end()?nullptr:found->second;
            Check(text&&text->layout.font,"Frontend pointer text requires a published original textbox layout");
            const auto& font=*text->layout.font;
            Check(font.height&&std::isfinite(text->layout.height)&&text->layout.height>=0,
                  "Frontend pointer text has invalid published row count");
            const float rows=text->layout.height/font.height;
            Check(rows<=16&&rows==std::floor(rows),"Frontend pointer text has invalid published row count");
            // Published height is exactly original ProcessString RowCount*Height.
            // Width is the separate original pointer query, not glyph extents.
            const auto width=resources::FrontendStringWidth(font,text->text,false,640,true);
            return Vector({float(width),float(font.height*unsigned(rows))});
        }
        case 4:
        {
            Check(bool(instance.library),"Frontend pointer component has no library object");
            const auto& library=Get(library_,*instance.library);
            Check(library.type==3&&bool(library.active_slide),"Frontend pointer component has no active slide");
            Check(std::find(library.slides.begin(),library.slides.end(),*library.active_slide)!=library.slides.end(),
                  "Frontend pointer component active slide is outside its ring");
            return List(Get(slides_,*library.active_slide).children);
        }
        default: return Vector({0,0}); // Original default type branch.
        }
    }
};
struct Event
{
    int mIndex=-1;
    nlVector2 mPosition=Vector({-9999.9f,-9999.9f});
    bool mPressed=false,mReleased=false,mAuxiliaryTriggered=false;
};
}
FrontendPointerBounds MeasureFrontendPointerBounds(const FrontendSession::Handle& frame,const FrontendPointerBinding& binding)
{
    Check(bool(frame),"Frontend pointer requires a retained scene");
    Check(binding.screen_height>0&&binding.screen_height<=65535,"Frontend pointer screen height is invalid");
    for(float v:{binding.offset_x,binding.offset_y,binding.scale_x,binding.scale_y})Scalar(v);
    Measurement measurement(*frame,binding.screen_height);
    const auto size=measurement.Size(binding.instance);
    Point({size.x,size.y});
    InstanceView instance{measurement.Instance(binding.instance)};
    FrontendPointerBounds out;
    nlVector2 pivot=Vector({0,0});
    FrontendPointerSetInstanceBounds<InstanceView,Position>(&instance,size,binding.use_rotation,
        binding.offset_x,binding.offset_y,binding.scale_x,binding.scale_y,
        out.min_x,out.max_x,out.max_y,out.min_y,out.rotation,pivot);
    out.pivot={pivot.x,pivot.y}; BoundsValid(out);return out;
}
bool FrontendPointerContains(const FrontendPointerBounds& bounds,std::array<float,2> point)
{
    BoundsValid(bounds);Point(point);
    return FrontendPointerContainsPoint(Vector(point),bounds.min_x,bounds.max_x,bounds.max_y,bounds.min_y,
        bounds.rotation,Vector(bounds.pivot));
}
struct FrontendPointerRegion::Implementation
{
    FrontendInput& input;
    Frame frame;
    FrontendPointerBounds bounds;
    std::uint32_t measured_instance;
    Callback callback;
    std::thread::id thread=std::this_thread::get_id();
    bool busy=false,live=true,mDisabled=false,mIgnoreInputLock=false;
    Event mPreviousEvents[4];
    void* mContext=nullptr;
    Implementation(FrontendInput& in,Frame value,FrontendPointerBinding binding,Callback cb)
        :input(in),frame(std::move(value)),bounds(MeasureFrontendPointerBounds(frame,binding)),measured_instance(binding.instance),callback(std::move(cb))
    { (void)input.InputLocked(); }
    Implementation(FrontendInput& in,Frame value,std::uint32_t instance,FrontendPointerBounds authored,Callback cb)
        :input(in),frame(std::move(value)),bounds(authored),measured_instance(instance),callback(std::move(cb))
    {
        (void)input.InputLocked();BoundsValid(bounds);
        Check(frame&&instance&&std::count_if(frame->graph.instances.begin(),frame->graph.instances.end(),
            [&](const auto& v){return v.offset==instance;})==1,
            "Authored pointer bounds require one retained instance");
    }
    void Ready() const
    { Check(std::this_thread::get_id()==thread,"Frontend pointer used from a different thread");Check(live,"Frontend pointer has been released"); }
    void Mutable() const {Ready();Check(!busy,"Frontend pointer cannot mutate during a callback");}
    bool ContainsPoint(nlVector2 position) const {return FrontendPointerContains(bounds,{position.x,position.y});}
    void Emit(FrontendPointerCallback kind,int index) {if(callback)callback(kind,unsigned(index),frame);}
    void OnPointerEnter(int index,void*){Emit(FrontendPointerCallback::Enter,index);}
    void OnPointerUpdate(int index,void*){Emit(FrontendPointerCallback::Update,index);}
    void OnPointerInside(int index,void*){Emit(FrontendPointerCallback::Inside,index);}
    void OnPointerPress(int index,void*){Emit(FrontendPointerCallback::Press,index);}
    void OnPointerAuxiliaryAction(int index,void*){Emit(FrontendPointerCallback::Unidentified,index);}
    void OnPointerLeave(int index,void*){Emit(FrontendPointerCallback::Leave,index);}
    void OnPointerRelease(int index,void*){Emit(FrontendPointerCallback::Release,index);}
};
FrontendPointerRegion::FrontendPointerRegion(FrontendInput& input,Frame frame,FrontendPointerBinding binding,Callback callback)
    :impl_(std::make_unique<Implementation>(input,std::move(frame),binding,std::move(callback))){}
FrontendPointerRegion::FrontendPointerRegion(FrontendInput& input,Frame frame,std::uint32_t instance,FrontendPointerBounds bounds,Callback callback)
    :impl_(std::make_unique<Implementation>(input,std::move(frame),instance,bounds,std::move(callback))){}
FrontendPointerRegion::~FrontendPointerRegion()
{ if(impl_->busy||std::this_thread::get_id()!=impl_->thread)std::terminate(); }
void FrontendPointerRegion::Rebind(Frame frame,FrontendPointerBinding binding)
{
    impl_->Mutable();auto bounds=MeasureFrontendPointerBounds(frame,binding);
    // Original useRotation=false only clears rotation; it preserves old pivot.
    if(!binding.use_rotation)bounds.pivot=impl_->bounds.pivot;
    impl_->bounds=bounds;impl_->measured_instance=binding.instance;impl_->frame=std::move(frame);
}
void FrontendPointerRegion::RebindFrame(Frame frame)
{
    impl_->Mutable();
    Check(frame&&impl_->frame,"Frontend pointer frame rebinding requires retained frames");
    Check(frame->visuals==impl_->frame->visuals&&frame->images==impl_->frame->images
        &&frame->request.path==impl_->frame->request.path,
        "Frontend pointer frame rebinding requires the measured scene resources");
    Check(std::any_of(frame->graph.instances.begin(),frame->graph.instances.end(),[&](const auto& instance)
        {return instance.offset==impl_->measured_instance;}),"Frontend pointer measured instance is absent");
    impl_->frame=std::move(frame);
}
void FrontendPointerRegion::SetBounds(float min_x,float max_x,float max_y,float min_y)
{
    impl_->Mutable();auto b=impl_->bounds;b.min_x=min_x;b.max_x=max_x;b.max_y=max_y;b.min_y=min_y;
    BoundsValid(b);impl_->bounds=b;
}
void FrontendPointerRegion::Enable(){impl_->Mutable();impl_->mDisabled=false;}
void FrontendPointerRegion::Disable(){impl_->Mutable();FrontendPointerDisable<Implementation,Event>(*impl_);}
void FrontendPointerRegion::IgnoreInputLock(bool value){impl_->Mutable();impl_->mIgnoreInputLock=value;}
bool FrontendPointerRegion::Enabled()const{impl_->Ready();return !impl_->mDisabled;}
bool FrontendPointerRegion::Contains(std::array<float,2> p)const{impl_->Ready();return FrontendPointerContains(impl_->bounds,p);}
FrontendPointerBounds FrontendPointerRegion::Bounds()const{impl_->Ready();return impl_->bounds;}
FrontendPointerRegion::Frame FrontendPointerRegion::Current()const{impl_->Ready();return impl_->frame;}
void FrontendPointerRegion::Deliver(const FrontendPointerEvent& event)
{
    impl_->Mutable();Check(event.index<4,"Frontend pointer index exceeds four original pointers");Point(event.position);
    Event native;native.mIndex=int(event.index);native.mPosition=Vector(event.position);
    native.mPressed=event.pressed;native.mReleased=event.released;native.mAuxiliaryTriggered=event.unidentified;
    struct InputView { unsigned m_InputLockDepth; } input{unsigned(impl_->input.InputLocked())};
    impl_->busy=true;struct Guard{bool& busy;~Guard(){busy=false;}}guard{impl_->busy};
    FrontendPointerProcess(*impl_,&native,&input);
}
void FrontendPointerRegion::Deliver(const Frame& expected,const FrontendPointerEvent& event)
{
    impl_->Mutable();Check(expected&&expected==impl_->frame,"Frontend pointer event targets a stale scene");
    Deliver(event);
}
void FrontendPointerRegion::Release()
{
    Check(std::this_thread::get_id()==impl_->thread,"Frontend pointer released from a different thread");
    if(!impl_->live)return;
    impl_->Mutable();impl_->frame.reset();impl_->callback={};impl_->live=false;
}
}

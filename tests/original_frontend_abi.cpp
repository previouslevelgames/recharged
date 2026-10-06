#include "platform/frontend_package.h"
#include "Game/FE/feAnimation.h"
#include "Game/FE/feFontResource.h"
#include "Game/FE/feImage.h"
#include "Game/FE/fePackage.h"
#include "Game/FE/fePresentation.h"
#include "Game/FE/feText.h"
#include "Game/FE/feTextureResource.h"
#include "Game/FE/tlComponent.h"
#include "Game/FE/tlComponentInstance.h"
#include "Game/FE/tlImageInstance.h"
#include "Game/FE/tlSlide.h"
#include "generated_offsets.h"
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace
{
using Transport=mscharged::platform::PreparedFrontendPackage;
unsigned checks=0;
void Check(bool value,const char* message)
{
    ++checks;if (!value) throw std::runtime_error(message);
}
template<class F> void Reject(F function,const char* message)
{
    bool failed=false;try { function(); } catch (const std::exception&) { failed=true; }
    Check(failed,message);
}
std::uint32_t BE32(const unsigned char* raw)
{
    return (std::uint32_t(raw[0])<<24)|(std::uint32_t(raw[1])<<16)|(std::uint32_t(raw[2])<<8)|raw[3];
}
std::uint16_t BE16(const unsigned char* raw) { return std::uint16_t((unsigned(raw[0])<<8)|raw[1]); }
template<class T> T Host(const unsigned char* raw) { T out;std::memcpy(&out,raw,sizeof(out));return out; }
std::vector<unsigned char> Read(const std::filesystem::path& path)
{
    std::ifstream in(path,std::ios::binary);std::vector<unsigned char> raw{std::istreambuf_iterator<char>(in),{}};
    Check(!raw.empty() && bool(in),"Independent raw FEN input is missing");return raw;
}

class Oracle
{
    const unsigned char* raw_;
    unsigned char* data_;
    const Transport& file_;
    std::set<std::uint32_t> slots_;
    std::vector<std::uint32_t> order_;
    std::map<std::uint32_t,std::size_t> native_slots_;
public:
    Oracle(const std::vector<unsigned char>& raw,Transport& native):raw_(raw.data()+16),file_(native)
    {
        auto* bytes=static_cast<unsigned char*>(native.Data());data_=bytes+16;
        const auto raw_size=BE32(raw.data()+8),raw_table=BE32(raw.data()+12);
        for (unsigned at=0;at<raw_table;at+=4)
        {
            const auto slot=BE32(raw.data()+16+raw_size+at);slots_.insert(slot);order_.push_back(slot);
        }
        Check(Host<std::uint32_t>(bytes+12)==order_.size()*sizeof(std::uintptr_t),"Native table lost an original relocation entry");
        const auto size=Host<std::uint32_t>(bytes+8);
        for (std::size_t i=0;i<order_.size();++i)
        {
            const auto slot=Host<std::uintptr_t>(data_+size+i*sizeof(std::uintptr_t));
            auto value=Host<std::uintptr_t>(data_+slot);
            value=value==0xffffffffu?0:value+reinterpret_cast<std::uintptr_t>(data_);
            std::memcpy(data_+slot,&value,sizeof(value));
        }
        // Fixture applies source-equivalent transport relocation. The genuine
        // original FEScene entry/queue/callback path remains separately gated.
        std::vector<std::byte> before(size);
        std::memcpy(before.data(),data_,size);
        native.StartObjectLifetimes(data_);
        for (const auto& record:native.Records())
            if (record.kind==6)
            {
                Check(std::memcmp(before.data()+record.native_offset+sizeof(void*),data_+record.native_offset+sizeof(void*),record.native_size-sizeof(void*))==0,
                    "Actual native animation constructor changed copied original source fields");
                Check(Host<std::uintptr_t>(data_+record.native_offset)!=BE32(raw_+record.wii_offset),"Foreign Wii/editor vptr was retained as a native callable address");
            }
    }
    void* Address(std::uint32_t raw) const { return data_+file_.NativeOffset(raw); }
    template<class T> T* Object(std::uint32_t at) const { return static_cast<T*>(Address(at)); }
    void Pointer(std::uint32_t at,const void* actual,const void* native_slot)
    {
        const auto word=BE32(raw_+at);
        const void* expected=slots_.contains(at) && word!=0xffffffffu?Address(word):nullptr;
        Check(actual==expected,"Native typed pointer changed original alias/null/record identity");
        if (slots_.contains(at)) native_slots_[at]=static_cast<const unsigned char*>(native_slot)-data_;
    }
    template<class T> void Word(std::uint32_t at,T actual)
    {
        if constexpr (std::is_same_v<T,float>) Check(std::bit_cast<std::uint32_t>(actual)==BE32(raw_+at),"Authored f32 bit pattern changed during native conversion");
        else Check(std::uint64_t(actual)==BE32(raw_+at),"Original Wii32 scalar/signature bits changed");
    }
    template<class T> void Half(std::uint32_t at,T actual) { Check(std::uint16_t(actual)==BE16(raw_+at),"Original signed/unsigned Wii16 bits changed"); }
    void Bytes(std::uint32_t at,const void* actual,std::size_t size) { Check(std::memcmp(raw_+at,actual,size)==0,"Original authored name/color/padding/flag bytes changed"); }
    void Attributes(std::uint32_t at,const FELibObjectAttributes& a)
    {
        const feVector3* values[]={&a.v3Position,&a.v3Rotation,&a.v3Scale,&a.v3Pivot};
        for (unsigned v=0;v<4;++v) for(unsigned c=0;c<3;++c) Word(at+12*v+4*c,values[v]->e[c]);
        Bytes(at+48,&a.bVisible,1);Bytes(at+49,&a.colour,4);Bytes(at+53,a.pad_35,3);
        Word(at+56,a.fUVX);Word(at+60,a.fUVY);Word(at+64,a.fUVWidth);Word(at+68,a.fUVHeight);
    }
    void Library(std::uint32_t at,FELibObject& v)
    {
        Pointer(at,v.next,&v.next);Pointer(at+4,v.prev,&v.prev);Attributes(at+8,v.m_attributes);
        Word(at+0x50,v.m_hashID);Bytes(at+0x54,v.m_szName,32);Word(at+0x74,v.m_type);
        if (v.m_type==FEOT_IMAGE)
        {
            auto& t=static_cast<FEImage&>(v);Pointer(at+0x78,t.m_pFeTextureResource,&t.m_pFeTextureResource);
        }
        if (v.m_type==FEOT_TEXT)
        {
            auto& t=static_cast<FEText&>(v);Pointer(at+0x78,t.m_pFeFontResource,&t.m_pFeFontResource);
            Bytes(at+0x7c,&t.m_TextAttributes.EffectColour,4);Word(at+0x80,t.m_TextAttributes.BoxSize.x);Word(at+0x84,t.m_TextAttributes.BoxSize.y);
        }
        if (v.m_type==FEOT_COMPONENT)
        {
            auto& t=static_cast<TLComponent&>(v);Pointer(at+0x78,t.pChildren,&t.pChildren);Pointer(at+0x7c,t.m_pActiveSlide,&t.m_pActiveSlide);
        }
    }
    void Instance(std::uint32_t at,TLInstance& v)
    {
        Pointer(at,v.m_next,&v.m_next);Pointer(at+4,v.m_prev,&v.m_prev);Pointer(at+8,v.pChildren,&v.pChildren);Pointer(at+12,v.m_component,&v.m_component);
        Word(at+16,v.m_fStartTime);Word(at+20,v.m_fDuration);Bytes(at+24,v.m_szName,32);Word(at+56,v.m_hash);
        Attributes(at+60,v.m_overloadedAttributes);Word(at+132,v.m_overloadFlags);Word(at+136,v.m_type);Half(at+140,v.m_priority);Bytes(at+142,&v.m_bVisible,1);
        if (v.m_type==TLAT_IMAGE)
        {
            auto& t=static_cast<TLImageInstance&>(v);Pointer(at+144,t.m_pTextureResource,&t.m_pTextureResource);Word(at+148,t.field_0x94);
        }
        if (v.m_type==TLAT_TEXT)
        {
            auto& t=static_cast<TLTextInstance&>(v);Word(at+144,t.m_LocStrId);Bytes(at+148,&t.m_OverloadedAttributes.EffectColour,4);
            Word(at+152,t.m_OverloadedAttributes.BoxSize.x);Word(at+156,t.m_OverloadedAttributes.BoxSize.y);Word(at+160,t.m_OverloadFlags);
            Pointer(at+164,t.m_DrawInfo.pFont,&t.m_DrawInfo.pFont);Pointer(at+168,t.m_DrawInfo.String,&t.m_DrawInfo.String);Pointer(at+172,t.m_DrawInfo.pMatrix,&t.m_DrawInfo.pMatrix);
            Word(at+176,t.m_DrawInfo.DrawOptions);Half(at+180,t.m_DrawInfo.RowCount);Half(at+182,t.m_DrawInfo.YOffset);
            for (unsigned i=0;i<17;++i) { Half(at+184+4*i,t.m_DrawInfo.Rows[i].XOffset);Half(at+186+4*i,t.m_DrawInfo.Rows[i].FirstChar); }
            Pointer(at+252,t.m_pFontString,&t.m_pFontString);Word(at+256,t.m_DrawOptions);Pointer(at+260,t.m_wcUserString,&t.m_wcUserString);
            Bytes(at+264,&t.m_UseScissorRect,1);Bytes(at+265,&t.pad_109,1);
            Half(at+266,t.m_ScissorRect.X);Half(at+268,t.m_ScissorRect.Y);Half(at+270,t.m_ScissorRect.Width);Half(at+272,t.m_ScissorRect.Height);
        }
    }
    void Verify(const Transport::Record& record)
    {
        const auto at=record.wii_offset;
        switch(record.kind)
        {
        case 0:
        {
            auto& v=*Object<FEPackage>(at);Pointer(at,v.m_pComponentList,&v.m_pComponentList);Pointer(at+4,v.m_pFEPresentation,&v.m_pFEPresentation);
            Pointer(at+8,v.m_pResourceList,&v.m_pResourceList);Pointer(at+12,v.m_pFEObjectLibrary,&v.m_pFEObjectLibrary);Word(at+16,v.m_uUniqueID);Word(at+20,v.m_uResourceCount);break;
        }
        case 1:
        {
            auto& v=*Object<FEPresentation>(at);Pointer(at,v.m_slides,&v.m_slides);Pointer(at+4,v.m_currentSlide,&v.m_currentSlide);Word(at+8,v.m_fadeDuration);break;
        }
        case 2:Library(at,*Object<FELibObject>(at));break;
        case 3:
        {
            auto& v=*Object<FEResourceHandle>(at);Pointer(at,v.m_next,&v.m_next);Pointer(at+4,v.m_prev,&v.m_prev);Word(at+8,v.m_type);Word(at+12,v.m_hashID);Bytes(at+16,&v.m_bValid,1);Bytes(at+17,v.pad_0x11,3);Word(at+20,v.m_uFileBlock);
            if (v.m_type==FERT_FONT) { auto& t=static_cast<FEFontResource&>(v);Pointer(at+24,t.m_pFontReference,&t.m_pFontReference); }
            else {auto& t=static_cast<FETextureResource&>(v);Word(at+24,t.m_glTextureHandle);Half(at+28,t.m_uWidth);Half(at+30,t.m_uHeight);}break;
        }
        case 4:
        {
            auto& v=*Object<TLSlide>(at);Pointer(at,v.m_next,&v.m_next);
            Pointer(at+4,Host<TLSlide*>(reinterpret_cast<const unsigned char*>(v.pad0)),v.pad0);
            Pointer(at+8,v.pChildren,&v.pChildren);Pointer(at+12,v.m_animations,&v.m_animations);Word(at+16,v.m_start);Word(at+20,v.m_duration);Word(at+24,v.m_time);Word(at+28,v.m_uPlayMode);Bytes(at+32,v.m_szName,32);Word(at+64,v.m_hash);Bytes(at+68,&v.field_0x44,1);break;
        }
        case 5:Instance(at,*Object<TLInstance>(at));break;
        case 6:
        {
            auto& v=*Object<FEAnimation>(at);Pointer(at+4,v.m_next,&v.m_next);Pointer(at+8,v.m_prev,&v.m_prev);Pointer(at+12,v.m_pTLInstanceTarget,&v.m_pTLInstanceTarget);Half(at+16,v.m_cast_type);Bytes(at+18,v.pad12,2);Word(at+20,v.m_type);Pointer(at+24,v.m_DLRingHead,&v.m_DLRingHead);break;
        }
        case 7:
        {
            auto& v=*Object<fAnimationKeyframe>(at);Word(at,v.pKeyFrameData.GetPoint());Word(at+4,v.pKeyFrameData.GetControl1());Word(at+8,v.pKeyFrameData.GetControl2());Word(at+12,v.pKeyFrameData.GetTime());Pointer(at+16,v.m_next,&v.m_next);Pointer(at+20,v.m_prev,&v.m_prev);break;
        }
        case 8:
        {
            auto& v=*Object<v3AnimationKeyframe>(at);
            const FEAnimationKeyframe* keys[]={&v.pKeyFrameDataX,&v.pKeyFrameDataY,&v.pKeyFrameDataZ};
            for(unsigned c=0;c<3;++c) {Word(at+16*c,keys[c]->GetPoint());Word(at+16*c+4,keys[c]->GetControl1());Word(at+16*c+8,keys[c]->GetControl2());Word(at+16*c+12,keys[c]->GetTime());}
            Pointer(at+48,v.m_next,&v.m_next);Pointer(at+52,v.m_prev,&v.m_prev);break;
        }
        case 9:
            for (std::size_t i=0;i<record.native_size;i+=2) Half(at+i,Host<std::uint16_t>(static_cast<unsigned char*>(Address(at))+i));break;
        case 10:
            for (unsigned i=0;i<16;++i) Word(at+4*i,Host<float>(static_cast<unsigned char*>(Address(at))+4*i));break;
        default:throw std::runtime_error("Independent oracle saw an unknown exposed record kind");
        }
    }
    void Finish()
    {
        Check(native_slots_.size()==slots_.size(),"Independent typed member oracle did not account for every original slot");
        const auto size=Host<std::uint32_t>(static_cast<const unsigned char*>(file_.Data())+8);
        for (std::size_t i=0;i<order_.size();++i)
            Check(Host<std::uintptr_t>(data_+size+i*sizeof(std::uintptr_t))==native_slots_.at(order_[i]),"Native relocation changed original source table order or exact typed destination");
    }
};

void Generated(const std::filesystem::path& path)
{
    auto raw=Read(path);const auto pristine=raw;
    auto native=mscharged::platform::PrepareFrontendPackage(raw.data(),raw.size());Oracle oracle(raw,native);
    for (const auto& record:native.Records()) oracle.Verify(record);
    oracle.Finish();Check(raw==pristine,"Native ABI preparation wrote original raw game data");
    auto* package=oracle.Object<FEPackage>(gen_package);
    auto* presentation=package->GetPresentation();
    Check(presentation==oracle.Object<FEPresentation>(gen_presentation),"Actual source GetPresentation changed typed ownership");
    auto* text=oracle.Object<TLTextInstance>(gen_text);
    Check(text->m_wcUserString==text->m_DrawInfo.String+1,"Authored UTF16 suffix alias was flattened");
    Check(text->m_wcUserString[0]=='B'&&text->m_wcUserString[1]==0xd83d&&text->m_wcUserString[2]==0xde00,"Raw UTF16 units/surrogates were rewritten");
    // Whole actual original methods, preserving the retail two-step time quirk.
    package->Update(.5f);
    Check(presentation->m_fadeDuration==.5f && presentation->m_currentSlide->m_time==1.f,"Original presentation plus slide time update was repaired or replaced");
    auto* image=oracle.Object<TLImageInstance>(gen_image);
    Check(image->GetAssetPosition().f.x==11.f&&image->GetAssetPosition().f.y==12.f&&image->GetAssetPosition().f.z==13.f,"Actual original vector keyframe update did not own the image transform");
    Check(text->GetAssetColour().c[3]==128,"Actual original float keyframe update did not own opacity");
    package->Update(.125f);
    Check(presentation->m_fadeDuration==.625f&&presentation->m_currentSlide->m_time==.75f,
        "Original subsequent presentation/slide clock decisions changed");
    Check(image->GetAssetPosition().f.x==5.640625f&&image->GetAssetPosition().f.y==6.640625f&&image->GetAssetPosition().f.z==7.640625f,
        "Actual original Bezier interpolation did not consume the authored vector ring/control points");
    Check(text->GetAssetColour().c[3]==104,"Original interpolated opacity truncation was normalized or replaced");
    float endpoint_points[]={1.f,2.f,3.f,99.f};
    Check(nlBezier(endpoint_points,3,1.f)==3.f,"Original Bezier terminal control-index quirk was repaired");
    presentation->SetActiveSlide(0x22222222ul,true);
    Check(presentation->m_currentSlide==oracle.Object<TLSlide>(gen_slide_b)&&presentation->m_fadeDuration==0,"Actual original slide selection/reset changed");
    const unsigned short replacement[]={u'F',u'E',0};
    text->SetString(replacement);
    Check(text->m_wcUserString==replacement&&text->m_pFontString==nullptr&&!(text->m_OverloadFlags&8),"Actual original SetString lifecycle changed");
    Check(raw==pristine,"Original native FE methods rewrote the raw input domain");
    for (std::size_t size:{std::size_t(0),std::size_t(12),raw.size()-1}) Reject([&]{mscharged::platform::PrepareFrontendPackage(raw.data(),size);},"Truncated actual raw byte span was accepted");
    auto bad=raw;bad[4]=1;Reject([&]{mscharged::platform::PrepareFrontendPackage(bad.data(),bad.size());},"Unsupported raw/version profile was guessed");
    bad=raw;const auto raw_size=BE32(raw.data()+8);std::memcpy(bad.data()+16+raw_size+4,bad.data()+16+raw_size,4);
    Reject([&]{mscharged::platform::PrepareFrontendPackage(bad.data(),bad.size());},"Duplicate relocation slot was silently applied");
}
void Owned(const std::filesystem::path& root)
{
    for (const auto* name:{"credits.fen","movieplayer.fen","options_main_menu.fen"})
    {
        const auto before=checks;auto raw=Read(root/name);const auto pristine=raw;
        auto native=mscharged::platform::PrepareFrontendPackage(raw.data(),raw.size());Oracle oracle(raw,native);
        for (const auto& record:native.Records()) oracle.Verify(record);
        oracle.Finish();Check(raw==pristine,"Owned raw FEN source bytes were modified");
        std::cout<<name<<": "<<checks-before<<" independent authored/member/alias/vtable checks\n";
    }
}
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==2||argc==3,"Generated raw FEN and optional owned raw FEN folder required");
        Generated(argv[1]);if (argc==3) Owned(argv[2]);
        std::cout<<"Original FE native ABI: "<<checks<<" checks; full FEScene/game startup and visual gate pending\n";return 0;
    }
    catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

#include "runtime/frontend_font_load.h"
#include "Game/Font/FontLoadSteps.h"
#include "NL/nlFileGC.h"
#include "NL/MemAlloc.h"
#include <algorithm>
#include <array>
#include <exception>
#include <set>
#include <thread>

namespace mscharged
{
namespace
{
constexpr std::size_t BatchBytes=32*1024*1024;
void Name(std::string_view value,std::size_t limit)
{resources::Require(!value.empty()&&value.size()<limit&&value.find('\0')==std::string_view::npos,"Invalid font request name");}
struct FreeBuffer { void operator()(std::uint8_t* p)const noexcept {if(p)VirtualAllocator.Free(p);} };
}
struct FrontendFontLoad::Implementation
{
    struct Read
    {
        Implementation* owner=nullptr;
        nlFile* file=nullptr;
        std::unique_ptr<std::uint8_t,FreeBuffer> bytes;
        std::uint32_t size=0;
        bool complete=false,page=false;
        unsigned completion_order=0;
        resources::Bytes Bytes()const{return {bytes.get(),size};}
    };
    struct Entry {std::uint32_t offset=0,size=0;};
    enum class Stage { Header, Directory, Description, Pages, Done };
    struct Slot
    {
        bool bComplete=true; // The unused original slots start idle/complete.
        FrontendFontRequest request;
        std::unique_ptr<nlFile> file;
        std::uint32_t size=0,directory=0,count=0,data=0;
        Stage stage=Stage::Header;
        unsigned description_order=0;
        std::unique_ptr<Read> read;
        std::map<std::uint32_t,Entry> entries;
        resources::FrontendFontDescription description;
        std::vector<std::unique_ptr<Read>> pages;
        std::shared_ptr<const resources::FrontendFont> font;
    };
    std::array<Slot,16> slots;
    unsigned count=0;
    FrontendFontLoadProgress progress;
    ResultType result;
    std::vector<unsigned> registration_order;
    unsigned completion_sequence=0;
    FrontendFontLoadState state=FrontendFontLoadState::Loading;
    std::exception_ptr error;
    std::thread::id thread=std::this_thread::get_id();
    bool servicing=false;
    std::size_t requested_bytes=0;

    void Thread()const
    {if(thread!=std::this_thread::get_id())throw std::logic_error("Font batch requires its NL servicing thread");}
    void Mutable()const
    {
        Thread();
        if(servicing||nlGetCurrentAsyncRead())throw std::logic_error("Font batch mutation during an NL callback is unsupported");
    }
    static void Complete(nlFile* file,void* data,unsigned size,nlFileAsyncParam context)
    {
        auto& read=*reinterpret_cast<Read*>(context);auto& owner=*read.owner;
        try
        {
            owner.Thread();
            resources::Require(!read.complete&&read.file==file&&read.bytes.get()==data&&read.size==size,"Font callback identity or byte count differs");
            read.complete=true;read.completion_order=++owner.completion_sequence;++owner.progress.completed_reads;if(read.page)++owner.progress.completed_pages;
        }
        catch(...){if(!owner.error)owner.error=std::current_exception();}
    }
    void Issue(Slot& slot,std::unique_ptr<Read>& target,std::uint32_t offset,std::uint32_t size,bool page=false)
    {
        resources::Require(!target&&size&&offset<=slot.size&&size<=slot.size-offset,"Font range exceeds its bundle");
        resources::Require(size<=BatchBytes-requested_bytes,"Font staged read budget exceeded");
        auto read=std::make_unique<Read>();read->owner=this;read->file=slot.file.get();read->size=size;read->page=page;
        read->bytes.reset(static_cast<std::uint8_t*>(VirtualAllocator.Allocate(size,32,false)));
        // Stable callback ownership and counters precede submission, including
        // providers that complete inline. No next-stage work runs in callbacks.
        target=std::move(read);requested_bytes+=size;++progress.requested_reads;if(page)++progress.requested_pages;
        nlSeek(slot.file.get(),offset,0);
        auto* token=nlReadAsync(slot.file.get(),target->bytes.get(),size,Complete,reinterpret_cast<nlFileAsyncParam>(target.get()),size);
        if(!token&&!target->complete)throw std::runtime_error("Font range read was not queued");
    }
    void Drain()
    {
        // Join every worker before discarding callback contexts or arenas.
        for(auto& slot:slots)if(slot.file)nlCancelPendingAsyncReads(slot.file.get(),nullptr);
        for(auto& slot:slots){slot.read.reset();slot.pages.clear();slot.file.reset();}
    }
    void Fail(std::exception_ptr failure)
    {if(!error)error=failure;Drain();for(auto& slot:slots){slot.font.reset();slot.description={};}result.clear();state=FrontendFontLoadState::Failed;}
    explicit Implementation(std::span<const FrontendFontRequest> requests)
    {
        Mutable();
        if(!gMemoryInitialized||!nlFileSystemReady())throw std::logic_error("Font batch requires native memory and NL files");
        resources::Require(!requests.empty()&&requests.size()<=slots.size(),"Font batch requires one to sixteen slots");
        count=requests.size();progress.requested_fonts=count;
        std::set<std::uint32_t> aliases;std::size_t source_bytes=0;
        try
        {
            // Preflight the whole batch before the first asynchronous read.
            for(unsigned i=0;i<count;++i)
            {
                auto& slot=slots[i];slot.request=requests[i];Name(slot.request.path,255);Name(slot.request.texture_base,240);Name(slot.request.alias,255);
                auto alias=slot.request.alias;for(auto& c:alias)if(c>='A'&&c<='Z')c+='a'-'A';
                resources::Require(aliases.insert(resources::FrontendNameHash(alias)).second,"Duplicate font batch alias");
                slot.file.reset(nlOpen(slot.request.path.c_str()));if(!slot.file)throw std::runtime_error("Font bundle is missing: "+slot.request.path);
                slot.size=nlFileSize(slot.file.get(),nullptr);
                resources::Require(slot.size&&slot.size<=resources::MaximumAssetBytes&&slot.size<=BatchBytes-source_bytes,"Font bundle exceeds the batch size limit");source_bytes+=slot.size;
                slot.bComplete=false;
            }
            for(unsigned i=0;i<count;++i)Issue(slots[i],slots[i].read,0,std::min(16u,slots[i].size));
        }
        catch(...){Drain();throw;}
    }
    void Process(Slot& slot,unsigned index)
    {
        if(slot.bComplete)return;
        if(slot.stage!=Stage::Pages)
        {
            if(!slot.read->complete)return;
            const auto bytes=slot.read->Bytes();
            if(slot.stage==Stage::Header)
            {
                resources::Require(bytes.size()==16&&resources::U32(bytes,0)==32,"Unsupported font bundle header");
                slot.count=resources::U32(bytes,4);const auto directory=std::uint64_t(resources::U32(bytes,8))*32,data=std::uint64_t(resources::U32(bytes,12))*32;
                resources::Require(slot.count&&slot.count<=33&&directory>=16&&directory<=slot.size
                    &&data>=directory+std::uint64_t(slot.count)*12&&data<=slot.size,"Invalid font directory metadata");
                slot.directory=directory;slot.data=data;slot.read.reset();slot.stage=Stage::Directory;
                Issue(slot,slot.read,slot.directory,slot.count*12);return;
            }
            if(slot.stage==Stage::Directory)
            {
                std::vector<std::pair<std::uint32_t,std::uint32_t>> ranges;
                for(unsigned i=0;i<slot.count;++i)
                {
                    const auto hash=resources::U32(bytes,i*12),size=resources::U32(bytes,i*12+8);
                    const auto offset=std::uint64_t(resources::U32(bytes,i*12+4))*32;
                    resources::Require(offset>=slot.data&&offset<=slot.size&&size&&size<=slot.size-offset,"Invalid font bundle entry range");
                    resources::Require(slot.entries.emplace(hash,Entry{std::uint32_t(offset),size}).second,"Duplicate font bundle entry");ranges.emplace_back(offset,offset+size);
                }
                std::sort(ranges.begin(),ranges.end());for(unsigned i=1;i<ranges.size();++i)resources::Require(ranges[i].first>=ranges[i-1].second,"Overlapping font bundle entries");
                const auto found=slot.entries.find(resources::FrontendNameHash(slot.request.texture_base));
                resources::Require(found!=slot.entries.end()&&found->second.size<=1024*1024,"Font descriptor missing or excessive");
                const auto entry=found->second;slot.read.reset();slot.stage=Stage::Description;Issue(slot,slot.read,entry.offset,entry.size);return;
            }
            slot.description=resources::ReadFrontendFontDescription(bytes,slot.request.texture_base,slot.request.alias);
            resources::Require(slot.entries.size()==slot.description.page_hashes.size()+1,"Font bundle has unexplained records");
            for(auto hash:slot.description.page_hashes)resources::Require(slot.entries.contains(hash),"Font texture page is absent");
            slot.description_order=slot.read->completion_order;
            slot.read.reset();slot.stage=Stage::Pages;
            struct PageOrder
            {
                std::size_t m_PageCount;const std::vector<std::uint32_t>& m_TextureHandles;
                TextureType m_TextureType=Colour;std::array<std::uint32_t,16> m_EffectTextureHandles{};
            } order{slot.description.page_hashes.size(),slot.description.page_hashes};
            slot.pages.resize(order.m_PageCount);unsigned page=0;
            FontLoadTextureOrder(order,[&](unsigned long hash){const auto entry=slot.entries.at(hash);Issue(slot,slot.pages.at(page++),entry.offset,entry.size,true);});
        }
        if(slot.stage==Stage::Pages)
        {
            if(std::any_of(slot.pages.begin(),slot.pages.end(),[](auto& page){return !page->complete;}))return;
            std::vector<resources::Texture> pages;pages.reserve(slot.pages.size());
            for(unsigned i=0;i<slot.pages.size();++i)pages.push_back(resources::ReadTexture(slot.pages[i]->Bytes(),slot.description.page_hashes[i]));
            slot.font=resources::AssembleFrontendFont(std::move(slot.description),std::move(pages));slot.pages.clear();slot.file.reset();
            slot.bComplete=true;slot.stage=Stage::Done;++progress.completed_fonts;progress.completed_mask|=1u<<index;
        }
    }
    void Poll()
    {
        if(state!=FrontendFontLoadState::Loading)return;
        try
        {
            if(error)std::rethrow_exception(error);
            if(!nlFileSystemReady())throw std::runtime_error("Font file services stopped");
            for(unsigned i=0;i<count;++i)
            {
                auto& slot=slots[i];if(slot.bComplete)continue;
                const bool pending=slot.stage==Stage::Pages
                    ?std::any_of(slot.pages.begin(),slot.pages.end(),[](auto& page){return !page->complete;})
                    :!slot.read->complete;
                if(pending&&!nlAsyncReadsPending(slot.file.get()))throw std::runtime_error("Font range read failed or was cancelled");
                Process(slot,i);
            }
            if(FontLoadSlotsComplete(slots.data(),slots.size()))
            {
                ResultType next;next.reserve(count);for(unsigned i=0;i<count;++i)next.push_back(slots[i].font);
                std::vector<unsigned> order;for(unsigned i=0;i<count;++i)order.push_back(i);
                std::sort(order.begin(),order.end(),[&](unsigned a,unsigned b){return slots[a].description_order<slots[b].description_order;});
                result=std::move(next);registration_order=std::move(order);state=FrontendFontLoadState::Ready;
            }
        }
        catch(...){Fail(std::current_exception());}
    }
};
FrontendFontLoad::FrontendFontLoad(std::span<const FrontendFontRequest> requests):impl_(std::make_unique<Implementation>(requests)){}
FrontendFontLoad::~FrontendFontLoad(){try{Cancel();}catch(...){std::terminate();}}
void FrontendFontLoad::Poll(){impl_->Mutable();impl_->Poll();}
void FrontendFontLoad::Service()
{
    auto& s=*impl_;s.Mutable();s.Poll();if(s.state!=FrontendFontLoadState::Loading)return;
    s.servicing=true;
    try{nlServiceFileSystem();}catch(...){s.servicing=false;s.Fail(std::current_exception());throw;}
    s.servicing=false;s.Poll();
}
void FrontendFontLoad::Cancel()
{
    auto& s=*impl_;s.Mutable();if(s.state!=FrontendFontLoadState::Loading)return;
    s.Drain();for(auto& slot:s.slots){slot.font.reset();slot.description={};}s.state=FrontendFontLoadState::Cancelled;
}
FrontendFontLoadState FrontendFontLoad::State()const{impl_->Thread();return impl_->state;}
FrontendFontLoadProgress FrontendFontLoad::Progress()const{impl_->Thread();return impl_->progress;}
const FrontendFontLoad::ResultType& FrontendFontLoad::Result()const
{
    impl_->Thread();if(impl_->error)std::rethrow_exception(impl_->error);
    if(impl_->state!=FrontendFontLoadState::Ready)throw std::logic_error("Font batch is pending or cancelled");return impl_->result;
}
const std::vector<unsigned>& FrontendFontLoad::RegistrationOrder()const
{(void)Result();return impl_->registration_order;}

}

#include "runtime/movie_draw_observer.h"
#include <stdexcept>
#include <thread>
#include <dolphin/gx/GXAurora.h>
namespace mscharged::detail
{
namespace{const glModelPacket* pending=nullptr;bool drawn=false;u64 tag=0;std::thread::id owner;}
void BeginMovieDrawObservation(const glModelPacket* packet)
{
    if(!packet||pending)throw std::logic_error("Movie packet observation is already active or empty");
    pending=packet;drawn=false;tag=0;owner=std::this_thread::get_id();
}
bool FinishMovieDrawObservation(const glModelPacket* packet)
{
    if(!packet||pending!=packet||owner!=std::this_thread::get_id())throw std::logic_error("Movie packet observation identity changed");
    const bool result=drawn&&AuroraGXWasDrawEncoded(tag);pending=nullptr;drawn=false;tag=0;owner={};return result;
}
void BeginMoviePacketDraw(const glModelPacket* packet)
{
    if(pending==packet)
    {
        if(owner!=std::this_thread::get_id()||tag)throw std::logic_error("Movie packet draw ownership changed");
        tag=AuroraGXBeginDrawReceipt();
        if(!tag)throw std::runtime_error("GX movie receipt admission failed");
    }
}
void EndMoviePacketDraw(const glModelPacket* packet)
{
    if(pending==packet&&tag)
    {
        AuroraGXEndDrawReceipt();drawn=true;
    }
}
}
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTexture.h"
namespace mscharged::detail
{
namespace{unsigned loaded_width=0,loaded_height=0;}
bool LoadMovieTexture(unsigned long hash)
{
    auto* manager=glGetTextureManager();
    if(!manager)throw std::logic_error("Movie texture query requires initialized manager");
    const unsigned long index=manager->GetTextureIndex(hash);
    if(index==0xffff)return false;
    const auto* texture=manager->GetTextureAtIndex(&index);
    if(!texture){loaded_width=loaded_height=0;return false;}
    // Original glTextureLoad/glplatTextureLoad snapshot the metadata. Retain
    // only values used here, never copy any owning native texture pointers.
    loaded_width=texture->m_Width;loaded_height=texture->m_Height;return true;
}
unsigned MovieTextureWidth(){return loaded_width;}
unsigned MovieTextureHeight(){return loaded_height;}
}

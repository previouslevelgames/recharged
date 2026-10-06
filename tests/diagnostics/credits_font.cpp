#include "Game/Font/fontmanager.h"
#include "Game/FE/feResourceManager.h"
#include "Game/FE/feFontResource.h"
#include "NL/nlFont.h"
#include "NL/nlLocalization.h"
#include "NL/nlFileGC.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"
#include "NL/MemAlloc.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glTextureManager.h"
#include "Game/GL/GLInventory.h"
#include "platform/game_allocation_ownership.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

unsigned DrawActualCredits231(FontManager&,GLResourcePool*);

namespace {
unsigned checks,polls;
void Check(bool value,const char* reason);
struct Memory {
    unsigned standard=StandardAllocator.TotalFreeMemory(), virt=VirtualAllocator.TotalFreeMemory();
    unsigned slargest=StandardAllocator.LargestFreeBlock(), vlargest=VirtualAllocator.LargestFreeBlock();
    void Same() const {
        Check(standard==StandardAllocator.TotalFreeMemory() && virt==VirtualAllocator.TotalFreeMemory(),"Original font prerequisite retained allocation bytes");
        Check(slargest==StandardAllocator.LargestFreeBlock() && vlargest==VirtualAllocator.LargestFreeBlock(),"Original font prerequisite fragmented owning arenas");
    }
};
void Check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
}
extern "C" __attribute__((visibility("default"))) unsigned charged_font_qualify() {
    Check(gMemoryInitialized==1,"Actual source pre-main constructors did not initialize real arenas");
    Memory baseline;
    nlInitFileSystem();
    glInitTextureManager(1000);
    auto* textures=glGetTextureManager();
    glInitResourcePools();
    const GLMemoryRequirement sizes[]={{GLM_Header,2*1024*1024},{GLM_TextureData,8*1024*1024},{GLM_Matrix,65536}};
    auto* pool=glCreateResourcePool(sizes,3,"source-font-prerequisite201");
    Check(pool!=nullptr,"Actual original resource pool unavailable");
    glSetCurrentResourcePool(pool);
    const auto marker=pool->MarkResource();
    auto* manager=new FontManager();
    FontManager::s_pInstance=manager;
    manager->SetResourcePool(pool);
    nlLocalization::Initialize();
    Check(g_pLocalization && !g_pLocalization->m_pFile,"Original localization precondition changed");
    Check(g_pLocalization->Load(nlLocalization::LangEnglish,false,&VirtualAllocator),"Actual original localization load rejected its request");
    // Exactly the two source LoadFonts EUR requests. Full BeginFontLoading is
    // held on original main static-task providers; this is a bounded prerequisite.
    Check(manager->LoadFont("art/fe/fonts/eurfonttext18.res","fe/fonts/eurfonttext18","fot-rodinprob18"),"Original text font request failed");
    Check(manager->LoadFont("art/fe/fonts/eurfontheading36.res","fe/fonts/eurfontheading36","Scratchy36"),"Original heading font request failed");
    Check(!manager->IsLoadingComplete(),"Original font requests fabricated readiness before genuine read callbacks");
    Check(!g_pLocalization->m_pFile,"Original localized loading became ready before callbacks");
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!manager->IsLoadingComplete() || !g_pLocalization->m_pFile) {
        nlServiceFileSystem();++polls;
        if(std::chrono::steady_clock::now()>end)throw std::runtime_error("Original font/localization loading stalled");
        std::this_thread::yield();
    }
    Check(g_pLocalization->m_pFile && g_pLocalization->m_LookupTable && g_pLocalization->m_FirstString,"Original localization callback did not publish actual owned table");
    Check(mscharged::platform::FindGameAllocationOwner(g_pLocalization->m_pFile)==&VirtualAllocator,"Original localization allocator request was not honored");
    auto it=manager->m_fonts.Begin();
    Check(it.hasNext(),"Actual descriptor callbacks registered no source font");
    auto* first=*it;
    auto countIt=manager->m_fonts.Begin();unsigned count=0;
    while(countIt.hasNext() && count<9){++count;countIt.Step();}
    Check(count==2,"Actual descriptor callbacks registered extra or missing fonts");
    Check(first && (first->m_Metrics.FontName==0xf180b38d || first->m_Metrics.FontName==0xcd74f509),"First source registration is not an originally requested font");
    auto* text=manager->GetFontByHashID(0xf180b38d);
    auto* heading=manager->GetFontByHashID(0xcd74f509);
    Check(text && heading && text!=heading,"Actual original aliases did not resolve requested fonts");
    Check(text->m_Metrics.FontName==0xf180b38d && heading->m_Metrics.FontName==0xcd74f509,"Native lookup incorrectly used source fallback for real original aliases");
    unsigned pages=0;
    for(auto* font:{text,heading}) {
        Check(font->m_PageCount>0,"Original descriptor has no pages");
        for(unsigned i=0;i<font->m_PageCount;++i) {
            Check(pool->m_inventory->GetTexture(font->m_TextureHandles[i])!=nullptr,"Actual source font page did not reach GLInventory");++pages;
            if(font->m_TextureType==SplitFX){Check(pool->m_inventory->GetTexture(font->m_EffectTextureHandles[i])!=nullptr,"Source effect page was not registered");++pages;}
        }
    }
    Check(manager->GetFontByHashID(0x501e5791)==first,"Authored Credits arial18 miss did not select first actual registered source font");
    const auto firstHash=first->m_Metrics.FontName;
    std::printf("231 real original font/LOC prerequisite: %u checks, %u source pages, first0x%08x; texture slots1000 from original main config.\n",checks,pages,firstHash);
    return DrawActualCredits231(*manager,pool);
}

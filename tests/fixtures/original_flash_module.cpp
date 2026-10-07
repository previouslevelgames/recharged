#include "original_flash_api.h"
#include "NL/plat/nlFlash.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include "platform/game_allocation_ownership.h"
#include <revolution/nand.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace {
FlashGateObservation* observation;
unsigned callbacks;
s32 last_result;
bool reenter;
s32 reentrant_submission;

void Check(bool value,const char* reason) {
    ++observation->checks;
    FlashGateCheck(value,reason);
}
void UserCallback(s32 result) {
    Check(FlashGateOwnerContextRestored(),"Original delayed flash callback did not return to its source task context");
    Check(!nlFlashCallbackPending(),"Original task did not clear its callback delay before invoking the user");
    last_result=result;++callbacks;++observation->source_user_callbacks;
    if(reenter) {
        reenter=false;
        reentrant_submission=nlFlashCreate("cb-second",NAND_PERM_RUSR|NAND_PERM_WUSR,UserCallback);
    }
}
void TaskRun(FlashMemoryTask& task) {
    ++observation->original_task_runs;
    task.Run(1.0f/60.0f);
}
void Deliver(FlashMemoryTask& task,s32 expected,unsigned previous) {
    Check(callbacks==previous&&!nlFlashCallbackPending()&&FlashGateIOSPending(),
        "Flash submission changed its original not-yet-completed pending quirk or delivered early");
    TaskRun(task);
    Check(callbacks==previous&&!nlFlashCallbackPending(),"Source task manufactured a callback before genuine NAND completion");
    FlashGateServiceIOS(true);
    Check(callbacks==previous&&nlFlashCallbackPending(),"NAND completion bypassed the original source delay or user callback storage");
    TaskRun(task);
    if(callbacks!=previous+1||last_result!=expected||nlFlashCallbackPending())
        std::fprintf(stderr,"Actual original flash outcome: wanted=%d result=%d previous=%u callbacks=%u pending=%d taskRuns=%u\n",
            expected,last_result,previous,callbacks,nlFlashCallbackPending(),observation->original_task_runs);
    Check(callbacks==previous+1&&last_result==expected&&!nlFlashCallbackPending(),
        "Original FlashMemoryTask failed to deliver exactly the completed NAND result");
    TaskRun(task);
    Check(callbacks==previous+1&&!nlFlashCallbackPending(),"Original flash task delivered the same result again");
}
}

extern "C" __attribute__((visibility("default"))) void FlashGateCold(FlashGateObservation* result) {
    observation=result;
    Check(!nandIsInitialized(),"Cold original flash gate already had source NAND readiness");
    nlFlashInitialize();
    Check(!nandIsInitialized()&&!nlFlashCallbackPending(),"Absent real FS/ES device manufactured source flash readiness");
    Check(nlFlashCreate("absent",0x30,UserCallback)==NAND_RESULT_FATAL_ERROR,
        "Cold whole original flash create did not preserve genuine NAND failure");
    Check(callbacks==0&&!FlashGateIOSPending(),"Rejected cold flash request retained an invented callback");
}

extern "C" __attribute__((visibility("default"))) void FlashGateRun(FlashGateObservation* result) {
    observation=result;
    nlFlashInitialize(); // Explicit isolated-source test, not production-main hookup.
    Check(nandIsInitialized()&&!nlFlashCallbackPending(),"Original nlFlashInitialize failed actual NANDInit and source home selection");
    char home[64],current[64];
    Check(NANDGetHomeDir(home)==NAND_RESULT_OK&&NANDGetCurrentDir(current)==NAND_RESULT_OK&&std::strcmp(home,current)==0,
        "Original flash initialization did not select its genuine TMD-backed NAND home");
    auto* task=new FlashMemoryTask;
    Check(std::uintptr_t(task)>UINT32_MAX&&std::strcmp(task->GetName(),"Flash Memory")==0,
        "Actual original FlashMemoryTask backing/name did not survive native ABI");
    mscharged::platform::GameAllocationSpan task_storage{};
    Check(mscharged::platform::FindGameAllocationSpan(task,sizeof(*task),task_storage)&&task_storage.owner==&StandardAllocator,
        "Original ordinary task new did not use its true CurrentAllocator source request");
    task->StateTransition(0x10000,4); // Actual whole Team.cpp provider, original no-op.
    const auto task_only_free=StandardAllocator.TotalFreeMemory();
    const auto task_only_count=StandardAllocator.m_allocation_count;

    const auto immediate_before=callbacks;
    Check(nlFlashChangeDirectory(0,UserCallback)==NAND_RESULT_OK&&callbacks==immediate_before+1&&!nlFlashCallbackPending(),
        "Original equal-directory branch lost its immediate user callback quirk");
    Check(nlFlashCreateDirectory("nocopy",0x30,UserCallback)==NAND_RESULT_OK,"Original flash directory submission failed");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    Check(nlFlashChangeDirectory(1,UserCallback)==NAND_RESULT_OK,"Original nocopy directory request failed");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    Check(NANDGetCurrentDir(current)==NAND_RESULT_OK&&std::strlen(current)==std::strlen(home)+7&&std::strcmp(current+std::strlen(home),"/nocopy")==0,
        "Original nlStrNCat/current-dir source request was altered");

    Check(nlFlashCreate("payload",0x30,UserCallback)==NAND_RESULT_OK,"Original file create submission failed");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    Check(nlFlashOpen("payload",NAND_ACCESS_RW,UserCallback)==NAND_RESULT_OK,"Original file open submission failed");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    alignas(32) unsigned char payload[97];
    for(unsigned n=0;n<sizeof(payload);++n)payload[n]=(n*37+11)&255;
    Check(nlFlashWrite(payload,sizeof(payload),UserCallback)==NAND_RESULT_OK,"Original raw write submission failed");
    Deliver(*task,sizeof(payload),callbacks);
    u32 length=0xdeadbeef;
    Check(nlFlashGetLength(&length,UserCallback)==NAND_RESULT_OK,"Original source get-length request failed");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    Check(length==sizeof(payload),"Actual raw file size was confused with original read-all padding");
    Check(nlFlashClose(UserCallback)==NAND_RESULT_OK,"Original write close request failed");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    Check(nlFlashOpen("payload",NAND_ACCESS_READ,nullptr)==NAND_RESULT_OK,"Original synchronous reopen failed");
    void* owned{};u32 owned_bytes{};
    Check(nlFlashRead(&owned,&owned_bytes,UserCallback,false)==NAND_RESULT_OK,"Original owned read request failed");
    Check(owned_bytes==128&&owned&&std::uintptr_t(owned)>UINT32_MAX&&(std::uintptr_t(owned)&31)==0,
        "Original round-to32/from-end allocation request changed");
    mscharged::platform::GameAllocationSpan read_storage{};
    Check(mscharged::platform::FindGameAllocationSpan(owned,owned_bytes,read_storage)&&read_storage.owner==&StandardAllocator&&read_storage.bytes==128,
        "Read backing lost its actual original allocator/physical requested span");
    Deliver(*task,sizeof(payload),callbacks);
    observation->allocated_read_bytes=owned_bytes;observation->raw_read_result=last_result;
    Check(std::memcmp(owned,payload,sizeof(payload))==0,"Actual native NAND read changed the source raw bytes");
    for(unsigned n=sizeof(payload);n<owned_bytes;++n)
        Check(static_cast<unsigned char*>(owned)[n]==0xCD,"Native short read wrote the source padded capacity after real EOF");
    nlFree(owned);
    Check(!mscharged::platform::FindGameAllocationSpan(owned,1,read_storage)&&StandardAllocator.TotalFreeMemory()==task_only_free&&StandardAllocator.m_allocation_count==task_only_count+1,
        "Original read/free did not retire ownership and restore the allocator ring");
    Check(nlFlashClose(nullptr)==NAND_RESULT_OK,"Original synchronous owned-read close failed");

    Check(nlFlashOpen("payload",NAND_ACCESS_READ,UserCallback)==NAND_RESULT_OK,"Original supplied-buffer reopen request failed");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    alignas(32) unsigned char provided[160];std::memset(provided,0x6b,sizeof(provided));
    void* provided_pointer=provided;u32 provided_bytes=128;
    Check(nlFlashRead(&provided_pointer,&provided_bytes,UserCallback,true)==NAND_RESULT_OK,"Original supplied-buffer read request failed");
    Deliver(*task,sizeof(payload),callbacks);
    Check(provided_pointer==provided&&provided_bytes==128&&std::memcmp(provided,payload,sizeof(payload))==0,
        "Original supplied-buffer pointer/size/raw bytes were replaced");
    for(unsigned n=sizeof(payload);n<sizeof(provided);++n)
        Check(provided[n]==0x6b,"Actual native read crossed EOF or the caller-provided capacity");
    Check(StandardAllocator.TotalFreeMemory()==task_only_free&&StandardAllocator.m_allocation_count==task_only_count+1,
        "Original provided-buffer path allocated hidden game storage");
    Check(nlFlashClose(UserCallback)==NAND_RESULT_OK,"Original read close request failed");
    Deliver(*task,NAND_RESULT_OK,callbacks);

    Check(nlFlashOpen("missing",NAND_ACCESS_READ,UserCallback)==NAND_RESULT_OK,"Missing source async open did not queue its real request");
    Deliver(*task,NAND_RESULT_NOEXISTS,callbacks);
    const auto rejected_before=callbacks;
    Check(nlFlashOpen("/shared2/private",NAND_ACCESS_READ,UserCallback)==NAND_RESULT_ACCESS,
        "Original private-path branch did not reject source access immediately");
    TaskRun(*task);
    Check(callbacks==rejected_before&&!nlFlashCallbackPending()&&!FlashGateIOSPending(),
        "Rejected original request retained a stale callback or fabricated NAND completion");

    Check(nlFlashCreate("payload",0x30,UserCallback)==NAND_RESULT_OK,"Actual duplicate request submission failed");
    Deliver(*task,NAND_RESULT_EXISTS,callbacks);
    u32 answer=0xdeadbeef;
    Check(nlFlashCheck(0,0,&answer,UserCallback)==NAND_RESULT_OK,"Original multi-stage usage check did not submit");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    Check(answer==0,"Actual original NAND capacity rules fabricated an application quota failure");
    Check(nlFlashCheck(0x401,0,&answer,nullptr)==NAND_RESULT_OK&&(answer&NAND_CHECK_TOO_MANY_APP_BLOCKS),
        "Original source application block threshold was replaced by host disk capacity");

    reenter=true;
    Check(nlFlashCreate("cb-first",0x30,UserCallback)==NAND_RESULT_OK,"Original reentrant first request failed");
    const auto before_reentrant=callbacks;
    FlashGateServiceIOS(true);
    Check(nlFlashCallbackPending()&&callbacks==before_reentrant,"Original first completion bypassed task delay");
    TaskRun(*task);
    Check(callbacks==before_reentrant+1&&reentrant_submission==NAND_RESULT_OK&&!nlFlashCallbackPending()&&FlashGateIOSPending(),
        "Original callback-clear-before-call order lost a genuine reentrant submission");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    Check(nlFlashDelete("payload",UserCallback)==NAND_RESULT_OK,"Original async raw file deletion failed");
    Deliver(*task,NAND_RESULT_OK,callbacks);
    Check(nlFlashDelete("cb-first",nullptr)==NAND_RESULT_OK&&nlFlashDelete("cb-second",nullptr)==NAND_RESULT_OK,
        "Original synchronous callback-created files did not delete");
    Check(nlFlashChangeDirectory(0,nullptr)==NAND_RESULT_OK&&nlFlashDelete("nocopy",nullptr)==NAND_RESULT_OK,
        "Original synchronous home/delete did not retire the actual private fixture directory");
    Check(!FlashGateIOSPending()&&!nlFlashCallbackPending(),"Real source flash completion/task state remained pending at terminal scope");
    delete task;
    // This original counter counts allocations; Free does not decrement it.
    Check(StandardAllocator.TotalFreeMemory()==StandardAllocator.m_memory_size&&StandardAllocator.m_allocation_count==task_only_count+1&&!mscharged::platform::FindGameAllocationSpan(task,1,task_storage),
        "Original typed task delete did not retire its genuine ordinary allocation");
}

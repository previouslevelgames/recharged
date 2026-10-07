#include "original_save_data_api.h"
#include "platform/save_data.h"
#include "platform/game_allocation_ownership.h"
#include "Game/DB/GameProgress.h"
#include "Game/DB/SaveLoad.h"
#include "NL/plat/nlFlash.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include "NL/nlMain.h"
#include <revolution/nand.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace mscharged::platform;
SaveGateObservation* observation;
unsigned callbacks;
s32 callback_result;
void Check(bool okay,const char* reason) {++observation->checks;SaveGateCheck(okay,reason);}
void Callback(s32 result) {
    Check(SaveGateOwnerContextRestored(),"Actual source flash callback context changed");
    Check(!nlFlashCallbackPending(),"Original flash delay remained set inside its user callback");
    callback_result=result;++callbacks;++observation->source_user_callbacks;
}
void Run(FlashMemoryTask& task) {++observation->original_task_runs;task.Run(1.0f/60.0f);}
void Deliver(FlashMemoryTask& task,s32 expected) {
    const unsigned previous=callbacks;
    Check(SaveGateIOSPending()&&!nlFlashCallbackPending(),"Original save-byte operation did not remain queued");
    Run(task);Check(callbacks==previous,"Original flash callback ran before actual IO");
    SaveGateServiceIOS(true);
    Check(callbacks==previous&&nlFlashCallbackPending(),"Actual NAND completion bypassed source delay");
    Run(task);
    Check(callbacks==previous+1&&callback_result==expected&&!nlFlashCallbackPending(),"Source task did not deliver exact original result");
    Run(task);Check(callbacks==previous+1,"Source task duplicated an original save-byte callback");
}
std::uint32_t CRC(const unsigned char* p,std::size_t bytes) {
    std::uint32_t result=0xFFFFFFFFu;
    for(std::size_t i=0;i<bytes;++i){result^=p[i];for(unsigned j=0;j<8;++j)result=(result>>1)^((result&1)?0xEDB88320u:0);}
    return ~result;
}
std::uint32_t ReadBE(const void* bytes,unsigned count) {
    const auto* p=static_cast<const unsigned char*>(bytes);std::uint32_t value=0;
    for(unsigned n=0;n<count;++n)value=(value<<8)|p[n];return value;
}
std::uint32_t ReadNative(const void* bytes,unsigned count) {
    std::uint32_t result=0;std::memcpy(&result,bytes,count);return result;
}
void Scalars(const unsigned char* wire,const void* object,unsigned offset,unsigned count,unsigned width) {
    const auto* native=static_cast<const unsigned char*>(object);
    for(unsigned n=0;n<count;++n)Check(ReadBE(wire+offset+n*width,width)==ReadNative(native+offset+n*width,width),
        "Stored Wii scalar differs from its original native source field");
}
void Player(const unsigned char* p,const PlayerStats& v) {
    Scalars(p,&v,0,29,2);Scalars(p,&v,0x3C,2,4);Scalars(p,&v,0x44,4,2);Scalars(p,&v,0x4C,2,4);
    Check(std::memcmp(p+0x3A,reinterpret_cast<const unsigned char*>(&v)+0x3A,2)==0,"Original player record padding changed");
}
void Team(const unsigned char* p,const TeamStats& v) {
    Scalars(p,&v,0,4,4);Scalars(p,&v,0x10,4,2);Scalars(p,&v,0x18,1,4);Player(p+0x1C,v.mPlayerTotalStats);
}
void Match(const unsigned char* p,const BasicGameInfo& v) {
    Scalars(p,&v,0,9,4);Team(p+0x24,v.mSides[0]);Team(p+0x94,v.mSides[1]);Scalars(p,&v,0x104,18,2);
}
template<unsigned short Teams,unsigned short Rounds>
void Fill(Cup<Teams,Rounds>& v,unsigned seed) {
    // Explicit fixture record contents only; constructors/reset/serialization
    // remain the genuine original template and whole source providers.
    v.mUserSelectedTeam=int(seed+1);for(unsigned n=0;n<3;++n)v.mUserSelectedSidekick.mValues[n]=eSidekickID(seed+n+2);
    v.mRoundType=int(seed+5);v.mRoundNumber=short(seed+6);v.mGameNumber=short(seed+7);v.mHumanTeams=seed+8;
    for(unsigned r=0;r<Rounds;++r)for(unsigned t=0;t<Teams/2;++t){
        auto* bytes=reinterpret_cast<unsigned char*>(&v.mGameInfo[r][t]);
        for(unsigned n=0;n<sizeof(BasicGameInfo);++n)bytes[n]=(seed+n*17+r*13+t*31)&255;
    }
    for(unsigned t=0;t<=Teams;++t){
        TeamStats* stats=t==Teams?&v.mPreviousTeamStats:&v.mTeamStats[t];
        auto* bytes=reinterpret_cast<unsigned char*>(stats);
        for(unsigned n=0;n<sizeof(TeamStats);++n)bytes[n]=(seed+n*37+t*71)&255;
    }
}
template<unsigned short Teams,unsigned short Rounds>
const unsigned char* CupWire(const unsigned char* p,const Cup<Teams,Rounds>& v) {
    const std::uint32_t header[]={std::uint32_t(v.mUserSelectedTeam),std::uint32_t(v.mUserSelectedSidekick.mValues[0]),
        std::uint32_t(v.mUserSelectedSidekick.mValues[1]),std::uint32_t(v.mUserSelectedSidekick.mValues[2]),std::uint32_t(v.mRoundType)};
    for(unsigned n=0;n<5;++n)Check(ReadBE(p+4*n,4)==header[n],"Original BaseCup header scalar/order changed");
    Check(ReadBE(p+20,2)==std::uint16_t(v.mRoundNumber)&&ReadBE(p+22,2)==std::uint16_t(v.mGameNumber)
        &&ReadBE(p+24,2)==v.mHumanTeams,"Original BaseCup short scalar/order changed");p+=26;
    for(unsigned r=0;r<Rounds;++r)for(unsigned t=0;t<Teams/2;++t,p+=296)Match(p,v.mGameInfo[r][t]);
    for(unsigned t=0;t<Teams;++t,p+=112)Team(p,v.mTeamStats[t]);Team(p,v.mPreviousTeamStats);return p+112;
}
struct Progress {
    int state,final[4],previous[2];bool in_progress,highest;unsigned char reserved[2];CupProgressRecord record;int mode;
};
static_assert(sizeof(Progress)==928&&offsetof(Progress,record)==32&&offsetof(Progress,mode)==924);
void Verify(const unsigned char* p,const UserInfo& user,const GameRules* rules,
        const Cup<4,8>& fire,const Cup<6,12>& crystal,const Cup<10,11>& striker,
        const Progress& progress,const StrikerChallenge& challenge) {
    Scalars(p,&user,0,7,4);Scalars(p,&user,0x20,1,4);Scalars(p,&user,0x24,5,4);
    Scalars(p,&user,0x40,3,4);Scalars(p,&user,0x4C,5,4);Scalars(p,&user,0x68,3,4);Scalars(p,&user,0x74,5,2);
    const auto* native=reinterpret_cast<const unsigned char*>(&user);
    for(unsigned n:{0x1Cu,0x1Du,0x1Eu,0x1Fu,0x38u,0x39u,0x3Au,0x3Bu,0x3Cu,0x3Du,0x3Eu,0x3Fu,
                    0x60u,0x61u,0x62u,0x63u,0x64u,0x65u,0x66u,0x67u,0x7Eu,0x7Fu})
        Check(p[n]==native[n],"Original user boolean/padding bytes changed");
    p+=128;Scalars(p,rules,0,36,4);p+=144;
    p=CupWire(p,fire);p=CupWire(p,crystal);p=CupWire(p,striker);
    Scalars(p,&progress,0,7,4);Scalars(p,&progress,32,6,2);Scalars(p,&progress,924,1,4);
    Check(std::memcmp(p+28,reinterpret_cast<const unsigned char*>(&progress)+28,4)==0,"Original Cup flags/padding changed");
    Check(ReadBE(p+44,2)==((unsigned(progress.record.mUnlockFlags)<<7)|progress.record.mUnidentified86AD),
        "Original MWCC MSB-first Cup unlock bits changed");
    for(unsigned r=0;r<9;++r)for(unsigned n=0;n<12;++n){
        const auto& v=progress.record.mHistory.mRecords[r][n];const auto* h=p+48+(r*12+n)*8;
        const auto first=ReadBE(h,4),second=ReadBE(h+4,4);
        Check((first>>28)==v.mCaptain&&((first>>25)&7)==v.mSidekick1&&((first>>22)&7)==v.mSidekick2
            &&((first>>19)&7)==v.mSidekick3&&((first>>14)&31)==v.mDay&&((first>>10)&15)==v.mMonth
            &&(first&1023)==v.mYearOffset,"Original Wii first history bitfield word differs");
        Check((second>>21)==v.mGoals&&((second>>14)&127)==v.mWins
            &&((second>>7)&127)==v.mLosses&&(second&127)==v.mOvertimeLosses,
            "Original Wii second history bitfield word differs");
    }
    Check(std::memcmp(p+912,progress.record.mHistory.mWriteIndex,9)==0,"Original Cup history write indices changed");
    p+=928;
    for(unsigned n=0;n<12;++n){const auto& v=challenge.mUnlocks.mCompletionDates[n];const auto word=ReadBE(p+4*n,4);
        Check((word>>27)==v.mDay&&((word>>23)&15)==v.mMonth&&((word>>13)&1023)==v.mYearOffset&&(word&8191)==v.mUnidentified,
            "Original MWCC date fields changed");}
    Check(ReadBE(p+48,4)==challenge.mUnlocks.mUnlockedChallenges,"Original Striker unlocked challenge word changed");
}
}
extern "C" __attribute__((visibility("default"))) void SaveGateCold(SaveGateObservation* result) {
    observation=result;Check(!nandIsInitialized(),"Source NAND is not cold before absent-device test");
    nlFlashInitialize();Check(!nandIsInitialized(),"Absent FS endpoint manufactured NAND readiness");
}
extern "C" __attribute__((visibility("default"))) void SaveGateRun(SaveGateObservation* result) {
    observation=result;nlFlashInitialize();Check(nandIsInitialized(),"Original nlFlashInitialize did not own actual NANDInit");
    auto* task=new FlashMemoryTask;const auto arena=StandardAllocator.TotalFreeMemory();
    auto* save=static_cast<unsigned char*>(nlMalloc(35616,32,true));auto* native_copy=static_cast<unsigned char*>(nlMalloc(35578,8,false));
    Check(save&&native_copy&&std::uintptr_t(save)>UINT32_MAX,"Original save-byte allocator backing was not real native width");
    UserInfo user;user.mSaveID=0x12345678;user.mVisualOptions.mCameraZoomLevel=1.375f;
    user.mNumGamesPlayed=0x1234;user.mNumGoalsScored=0xABCD;user.mNumSTSAttempts=0x4567;user.mNumPerfectPasses=0xF010;user.mNumHits=0x2468;
    GameRules rules[12];for(unsigned n=0;n<12;++n)for(unsigned k=0;k<3;++k)rules[n].mValues[k]=eSidekickID(n*19+k*7);
    Cup<4,8> fire;Cup<6,12> crystal;Cup<10,11> striker;Fill(fire,11);Fill(crystal,79);Fill(striker,151);
    Check(fire.GetSaveDataSize()==5322&&crystal.GetSaveDataSize()==11466&&striker.GetSaveDataSize()==17538,
        "Original whole Cup template save sizes no longer match Wii storage");
    Progress progress;std::memset(&progress,0xCD,sizeof(progress));
    progress.state=0x1020304;for(unsigned n=0;n<4;++n)progress.final[n]=int(0x10203040+n*71);
    progress.previous[0]=-1;progress.previous[1]=0x1357;progress.in_progress=true;progress.highest=false;progress.mode=2;
    progress.record.mUnlockFlags=0x155;progress.record.mUnidentified86AD=0x53;
    for(unsigned n=0;n<108;++n){auto& h=progress.record.mHistory.mRecords[n/12][n%12];
        h.mCaptain=n%16;h.mSidekick1=(n+1)%8;h.mSidekick2=(n+2)%8;h.mSidekick3=(n+3)%8;
        h.mDay=(n+5)%32;h.mMonth=(n+7)%16;h.mYearOffset=(n*19)%1024;h.mGoals=(n*31)%2048;
        h.mWins=(n*41)%128;h.mLosses=(n*53)%128;h.mOvertimeLosses=(n*67)%128;}
    for(unsigned n=0;n<9;++n)progress.record.mHistory.mWriteIndex[n]=n+1;
    StrikerChallenge challenge;
    for(unsigned n=0;n<12;++n){auto& d=challenge.mUnlocks.mCompletionDates[n];d.mDay=(n+19)%32;d.mMonth=(n+11)%16;
        d.mYearOffset=(n*103+257)%1024;d.mUnidentified=(n*971+1234)%8192;}
    challenge.mUnlocks.mUnlockedChallenges=0xABCDEFFF;
    auto* p=save+8;std::memcpy(p,&user,128);p+=128;std::memcpy(p,rules,144);p+=144;
    p=static_cast<unsigned char*>(fire.SerializeData(p));p=static_cast<unsigned char*>(crystal.SerializeData(p));
    p=static_cast<unsigned char*>(striker.SerializeData(p));std::memcpy(p,&progress,sizeof(progress));p+=sizeof(progress);
    p=static_cast<unsigned char*>(challenge.SerializeData(p));Check(p==save+35586,"Actual original Cup/Striker cursors widened the stored payload");
    std::memcpy(native_copy,save+8,35578);
    const auto before_raw_crc=nlChecksum32(save+8,35608);
    EncodeOriginalSavePayload(save+8,35578,false);
    Verify(save+8,user,rules,fire,crystal,striker,progress,challenge);
    for(unsigned n=35586;n<35616;++n)Check(save[n]==0xCD,"Adapter changed original padded checksum/write bytes");
    const auto checksum=nlChecksum32(save+8,35608);Check(checksum==CRC(save+8,35608)&&checksum!=before_raw_crc,
        "Original CRC did not inspect encoded Wii payload and original padding");
    auto* header=reinterpret_cast<SaveFileHeader*>(save);header->Size=OriginalSaveWord(35586);header->CRC=OriginalSaveWord(checksum);
    Check(ReadBE(save,4)==35586&&ReadBE(save+4,4)==checksum,"Source file header is not Wii32 big endian");
    bool unknown=false;try{EncodeOriginalSavePayload(save+8,35577,false);}catch(const std::invalid_argument&){unknown=true;}
    Check(unknown&&nlChecksum32(save+8,35608)==checksum,"Unknown ordinary extent changed bytes before rejection");
    bool online=false;try{DecodeOriginalSavePayload(save+8,35608,true);}catch(const std::logic_error&){online=true;}
    Check(online&&nlChecksum32(save+8,35608)==checksum,"Unqualified online format did not reject without mutation");
    Check(nlFlashCreate("save-abi",0x30,Callback)==0,"Original save-byte create did not submit");Deliver(*task,0);
    Check(nlFlashOpen("save-abi",NAND_ACCESS_RW,Callback)==0,"Original save-byte open did not submit");Deliver(*task,0);
    Check(nlFlashWrite(save,35616,Callback)==0,"Original padded save-byte write did not submit");Deliver(*task,35616);
    Check(nlFlashClose(Callback)==0,"Original save-byte write close failed");Deliver(*task,0);
    Check(nlFlashOpen("save-abi",NAND_ACCESS_READ,Callback)==0,"Original save-byte read open failed");Deliver(*task,0);
    void* read{};u32 bytes{};Check(nlFlashRead(&read,&bytes,Callback,false)==0,"Original owned save-byte read did not submit");Deliver(*task,35616);
    observation->allocated_read_bytes=bytes;observation->raw_read_result=callback_result;
    GameAllocationSpan span{};Check(bytes==35616&&FindGameAllocationSpan(read,bytes,span)&&span.owner==&StandardAllocator,
        "Actual read lost original padded size/allocator ownership");
    Check(std::memcmp(read,save,35616)==0,"Actual filesystem changed original encoded save bytes");
    auto* loaded=static_cast<unsigned char*>(read);const auto stored_crc=ReadBE(loaded+4,4);
    Check(nlChecksum32(loaded+8,35608)==stored_crc,"Original loaded CRC did not validate raw persisted bytes before conversion");
    loaded[23]^=1;Check(nlChecksum32(loaded+8,35608)!=stored_crc,"Original checksum accepted corrupt persisted payload");loaded[23]^=1;
    DecodeOriginalSavePayload(loaded+8,35608,false);
    Check(std::memcmp(loaded+8,native_copy,35578)==0,"Decoded original native source records/padding differ after actual flash round trip");
    Cup<4,8> loaded_fire;Cup<6,12> loaded_crystal;Cup<10,11> loaded_striker;StrikerChallenge loaded_challenge;
    p=loaded+8+272;p=static_cast<unsigned char*>(loaded_fire.DeserializeData(p));
    p=static_cast<unsigned char*>(loaded_crystal.DeserializeData(p));p=static_cast<unsigned char*>(loaded_striker.DeserializeData(p));
    p+=928;p=static_cast<unsigned char*>(loaded_challenge.DeserializeData(p));
    Check(p==loaded+35586&&std::memcmp(&loaded_challenge.mUnlocks,&challenge.mUnlocks,52)==0,
        "Actual whole original Striker deserialize changed records/cursor");
    auto* scratch=static_cast<unsigned char*>(nlMalloc(34326,8,false));p=static_cast<unsigned char*>(loaded_fire.SerializeData(scratch));
    p=static_cast<unsigned char*>(loaded_crystal.SerializeData(p));p=static_cast<unsigned char*>(loaded_striker.SerializeData(p));
    Check(p==scratch+34326&&std::memcmp(scratch,native_copy+272,34326)==0,
        "Actual original Cup deserialize/serialize round trip changed source fields or padding");
    nlFree(scratch);nlFree(read);nlFree(save);nlFree(native_copy);
    Check(StandardAllocator.TotalFreeMemory()==arena,"Real source save-byte owners did not restore original arena backing");
    Check(nlFlashClose(Callback)==0,"Original save-byte read close failed");Deliver(*task,0);
    Check(nlFlashDelete("save-abi",Callback)==0,"Original save-byte delete failed");Deliver(*task,0);
    delete task;Check(!SaveGateIOSPending()&&!nlFlashCallbackPending(),"Original source terminal boundary retained work");

}

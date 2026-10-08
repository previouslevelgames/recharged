#include "platform/save_data.h"

#include "Game/GameInfo.h"
#include "Game/DB/GameProgress.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace mscharged::platform {
namespace {
static_assert(std::endian::native == std::endian::little,
              "Original save transport currently qualifies little-endian native hosts only");
static_assert(sizeof(int) == 4 && sizeof(float) == 4 && sizeof(bool) == 1);
static_assert(sizeof(UserInfo) == 0x80 && sizeof(GameRules) == 12);
static_assert(sizeof(TeamStats) == 0x70 && sizeof(PlayerStats) == 0x54);
static_assert(sizeof(BasicGameInfo) == 0x128);
static_assert(sizeof(CupHistoryRecord) == 8 && sizeof(CupHistory) == 0x36C);
static_assert(sizeof(CupProgressRecord) == 0x37C);
static_assert(sizeof(ChallengeCompletionDate) == 4 && sizeof(ChallengeUnlockRecord) == 52);
static_assert(offsetof(TeamStats, mPlayerTotalStats) == 0x1C);
static_assert(offsetof(PlayerStats, mBallPossessionTime) == 0x3C);
static_assert(offsetof(CupProgressRecord, mHistory) == 0x10);
static_assert(offsetof(CupHistory, mWriteIndex) == 0x360);
static_assert(__builtin_offsetof(CupManager,mCupRecord)-__builtin_offsetof(CupManager,mState)==0x20);
static_assert(__builtin_offsetof(CupManager,mCurrentMode)-__builtin_offsetof(CupManager,mState)+sizeof(int)==0x3A0);
static_assert(std::is_trivially_copyable_v<CupHistoryRecord>);
static_assert(std::is_trivially_copyable_v<ChallengeCompletionDate>);

// Exact two source u16 bitfield declarations, viewed independently of live
// CupManager backing. All16 bits are represented; no padding/flags are cleared.
struct CupUnlockBits {
    u16 mUnlockFlags : 9;
    u16 mUnidentified86AD : 7;
};
static_assert(sizeof(CupUnlockBits)==2);
using Bytes = unsigned char;
std::uint16_t Load16(const Bytes* p) {
    std::uint16_t value; std::memcpy(&value, p, sizeof(value)); return value;
}
std::uint32_t Load32(const Bytes* p) {
    std::uint32_t value; std::memcpy(&value, p, sizeof(value)); return value;
}
void Store16(Bytes* p, std::uint16_t value) {std::memcpy(p, &value, sizeof(value));}
void Store32(Bytes* p, std::uint32_t value) {std::memcpy(p, &value, sizeof(value));}
void Swap16(Bytes* p) {Store16(p, std::uint16_t((Load16(p) >> 8) | (Load16(p) << 8)));}
void Swap32(Bytes* p) {Store32(p, OriginalSaveWord(Load32(p)));}
void Words32(Bytes* p, std::size_t count) {for(std::size_t i=0; i<count; ++i)Swap32(p+4*i);}
void Words16(Bytes* p, std::size_t count) {for(std::size_t i=0; i<count; ++i)Swap16(p+2*i);}

// These source records have real byte padding. It remains untouched, including
// the original uninitialized/padded bytes covered by the original checksum.
void User(Bytes* p) {
    Words32(p,7); Swap32(p+0x20); Words32(p+0x24,5); Words32(p+0x40,3);
    Words32(p+0x4C,5); Words32(p+0x68,3); Words16(p+0x74,5);
}
void Player(Bytes* p) {
    Words16(p,0x3A/2); Words32(p+0x3C,2); Words16(p+0x44,4); Words32(p+0x4C,2);
}
void Team(Bytes* p) {
    Words32(p,4); Words16(p+0x10,4); Swap32(p+0x18); Player(p+0x1C);
}
void Match(Bytes* p) {
    Words32(p,9); Team(p+0x24); Team(p+0x94); Words16(p+0x104,18);
}
Bytes* Cup(Bytes* p, unsigned teams, unsigned rounds) {
    Words32(p,5); Words16(p+0x14,3); p+=0x1A;
    for(unsigned i=0; i<rounds*(teams/2); ++i,p+=0x128)Match(p);
    for(unsigned i=0; i<=teams; ++i,p+=0x70)Team(p);
    return p;
}
void History(Bytes* p, bool encode) {
    CupHistoryRecord record;
    if(encode) {
        std::memcpy(&record,p,sizeof(record));
        const std::uint32_t first=(std::uint32_t(record.mCaptain)<<28)|(record.mSidekick1<<25)
            |(record.mSidekick2<<22)|(record.mSidekick3<<19)|(record.mDay<<14)
            |(record.mMonth<<10)|record.mYearOffset;
        const std::uint32_t second=(std::uint32_t(record.mGoals)<<21)|(record.mWins<<14)
            |(record.mLosses<<7)|record.mOvertimeLosses;
        Store32(p,OriginalSaveWord(first)); Store32(p+4,OriginalSaveWord(second));
    } else {
        const auto first=OriginalSaveWord(Load32(p));
        const auto second=OriginalSaveWord(Load32(p+4));
        record.mCaptain=first>>28; record.mSidekick1=(first>>25)&7;
        record.mSidekick2=(first>>22)&7; record.mSidekick3=(first>>19)&7;
        record.mDay=(first>>14)&31; record.mMonth=(first>>10)&15;
        record.mYearOffset=first&1023; record.mGoals=second>>21;
        record.mWins=(second>>14)&127;
        record.mLosses=(second>>7)&127; record.mOvertimeLosses=second&127;
        std::memcpy(p,&record,sizeof(record));
    }
}
void Date(Bytes* p, bool encode) {
    ChallengeCompletionDate record;
    if(encode) {
        std::memcpy(&record,p,sizeof(record));
        Store32(p,OriginalSaveWord((std::uint32_t(record.mDay)<<27)|(record.mMonth<<23)
            |(record.mYearOffset<<13)|record.mUnidentified));
    } else {
        const auto word=OriginalSaveWord(Load32(p));
        record.mDay=word>>27; record.mMonth=(word>>23)&15;
        record.mYearOffset=(word>>13)&1023; record.mUnidentified=word&8191;
        std::memcpy(p,&record,sizeof(record));
    }
}
void Transform(void* payload, std::size_t bytes, bool online, bool encode) {
    if(online)throw std::logic_error("Original online save-slot byte transport remains unqualified");
    if(!payload || (bytes!=OriginalNormalSavePayloadBytes
            && (encode || bytes!=35608)))
        throw std::invalid_argument("Original ordinary save payload extent is not the qualified Wii record layout");
    auto* p=static_cast<Bytes*>(payload);
    User(p); p+=0x80; Words32(p,36); p+=0x90;
    p=Cup(p,4,8); p=Cup(p,6,12); p=Cup(p,10,11);
    Words32(p,7); // source state + four final and two previous opponent scalars
    Words16(p+0x20,6); // two actual three-counter records
    CupUnlockBits unlock;
    if(encode) {
        std::memcpy(&unlock,p+0x2C,sizeof(unlock));
        const auto word=std::uint16_t((unlock.mUnlockFlags<<7)|unlock.mUnidentified86AD);
        Store16(p+0x2C,std::uint16_t((word>>8)|(word<<8)));
    } else {
        auto word=Load16(p+0x2C);word=std::uint16_t((word>>8)|(word<<8));
        unlock.mUnlockFlags=word>>7;unlock.mUnidentified86AD=word&127;
        std::memcpy(p+0x2C,&unlock,sizeof(unlock));
    }
    for(unsigned i=0; i<108; ++i)History(p+0x30+8*i,encode);
    Swap32(p+0x39C); // original current-mode scalar; padding and history indices untouched
    p+=0x3A0;
    for(unsigned i=0; i<12; ++i)Date(p+4*i,encode);
    Swap32(p+48);
}
} // namespace

std::uint32_t OriginalSaveWord(std::uint32_t value) {
    return ((value&0xFFu)<<24)|((value&0xFF00u)<<8)
        |((value&0xFF0000u)>>8)|(value>>24);
}
void EncodeOriginalSavePayload(void* payload, std::size_t bytes, bool online) {
    Transform(payload,bytes,online,true);
}
void DecodeOriginalSavePayload(void* payload, std::size_t bytes, bool online) {
    Transform(payload,bytes,online,false);
}
} // namespace mscharged::platform

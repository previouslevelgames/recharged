#include "NL/MemAlloc.h"
#include "platform/game_allocation_ownership.h"
#include "platform/native_hbm_sound_archive.h"
#include "revolution/hbm/nw4hbm/snd/MemorySoundArchive.h"
#include "revolution/hbm/nw4hbm/snd/SoundArchiveFile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
unsigned checks;
void Check(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
template<class Action> void Reject(Action action) {
    ++checks;
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Unsupported archive owner was accepted");
}
void W(unsigned char* p, std::uint32_t value) {
    p[0]=value>>24; p[1]=value>>16; p[2]=value>>8; p[3]=value;
}
void H(unsigned char* p, std::uint16_t value) { p[0]=value>>8; p[1]=value; }
std::array<unsigned char,256> Archive() {
    std::array<unsigned char,256> data{};
    auto* p=data.data();
    W(p,0x52534152); H(p+4,0xfeff); H(p+6,0x101); W(p+8,data.size());
    H(p+12,40); H(p+14,3);
    W(p+16,64); W(p+20,80); W(p+24,144); W(p+28,96); W(p+32,240); W(p+36,16);
    p=data.data()+64;
    W(p,0x53594d42); W(p+4,80); W(p+8,20);
    // One label and four trees borrowing the same deliberately unaligned leaf.
    for(unsigned i=1;i<5;++i) W(p+8+i*4,33);
    W(p+28,1); W(p+32,28); p[36]='A';
    W(p+41,0); W(p+45,1); H(p+49,1); H(p+51,0);
    W(p+53,0); W(p+57,0); W(p+61,0); W(p+65,7);
    p=data.data()+144;
    W(p,0x494e464f); W(p+4,96);
    p[48]=1; W(p+52,48); // Original base is INFO+8, config is INFO+56.
    for(unsigned i=0;i<7;++i) H(p+56+i*2,0x101+i);
    W(data.data()+240,0x46494c45); W(data.data()+244,16);
    return data;
}
}

int main() {
    using namespace mscharged::platform;
    using nw4hbm::snd::MemorySoundArchive;
    using nw4hbm::snd::SoundArchive;
    try {
        alignas(64) std::array<std::byte,32768> arena{};
        MemoryAllocator allocator{};
        allocator.Initialize(arena.data(),arena.size());
        const auto initial=allocator.TotalFreeMemory();
        const auto authored=Archive();
        std::uint64_t previous=0;
        for(unsigned iteration=0;iteration<2;++iteration) {
            auto* raw=static_cast<unsigned char*>(allocator.Allocate(authored.size(),32,false));
            Check(raw != nullptr,"Original allocator failed");
            Reject([&]{NativeHBMSoundArchiveHeader(raw);});
            {
                GameByteWriteReservation write(raw,authored.size());
                std::memcpy(raw,authored.data(),authored.size());
                write.Complete(GameByteDomain::WiiSerialized);
            }
            GameCompletedSpan completed{};
            Check(FindGameCompletedSpan(raw,authored.size(),completed)
                  && completed.allocation.incarnation!=previous,"New source lifetime is absent");
            previous=completed.allocation.incarnation;
            MemorySoundArchive source;
            Check(source.Setup(raw) && source.IsAvailable(),"Original source reader setup failed");
            Check(source.ConvertLabelStringToSoundId("A")==7,"Original sound tree lookup changed");
            Check(source.ConvertLabelStringToPlayerId("A")==7,"Original player tree lookup changed");
            Check(source.ConvertLabelStringToGroupId("A")==7,"Original group tree lookup changed");
            Check(source.ConvertLabelStringToSoundId("B")==SoundArchive::INVALID_ID,
                  "Missing label acquired a fabricated source ID");
            Check(source.GetPlayerCount()==0 && source.GetGroupCount()==0,"Null source tables changed");
            SoundArchive::SoundArchivePlayerInfo info{};
            Check(source.ReadSoundArchivePlayerInfo(&info),"Original source config lookup failed");
            const std::array<int,7> counts{info.seqSoundCount,info.seqTrackCount,info.strmSoundCount,
                info.strmTrackCount,info.strmChannelCount,info.waveSoundCount,info.waveTrackCount};
            for(unsigned i=0;i<counts.size();++i)
                Check(counts[i]==int(0x101+i),"Native config cell lost original byte order");
            const auto* symbols=NativeHBMSoundArchiveSymbols(raw+64,80);
            namespace File=nw4hbm::snd::detail::SoundArchiveFile;
            const auto* block=static_cast<const File::SymbolBlock*>(symbols);
            const auto* tree=reinterpret_cast<const unsigned char*>(&block->stringBlock)
                +block->stringBlock.stringChunk.soundTreeOffset;
            Check(reinterpret_cast<std::uintptr_t>(tree)%alignof(File::StringTree)==0,
                  "Unaligned serialized tree was not adapted for native access");
            Check(symbols==NativeHBMSoundArchiveSymbols(raw+64,80),"Repeated view lost source ownership");
            Check(std::memcmp(raw,authored.data(),authored.size())==0,"Original archive bytes changed");
            Check(FindGameByteDomain(raw,authored.size())==GameByteDomain::WiiSerialized,
                  "Raw FILE/source domain was converted");
            Reject([&]{NativeHBMSoundArchiveSymbols(raw+64,20);});
            Reject([&]{NativeHBMSoundArchiveInfo(raw+144,96,0x100);});
            source.Shutdown();
            Check(!source.IsAvailable(),"Original shutdown retained reader availability");
            GameNativeBackingSpan backing{};
            Check(FindGameNativeBacking(raw,40,backing)
                  && FindGameNativeBacking(raw+64,80,backing)
                  && FindGameNativeBacking(raw+144,96,backing),"Source metadata owner is absent");
            allocator.Free(raw);
            Check(!FindGameNativeBacking(raw,40,backing)
                  && !FindGameNativeBacking(raw+64,80,backing)
                  && !FindGameNativeBacking(raw+144,96,backing),"Original free retained native metadata");
            Reject([&]{NativeHBMSoundArchiveHeader(raw);});
        }
        Check(allocator.TotalFreeMemory()==initial,"Original allocator storage was not restored");
        std::printf("Original HBM archive: %u checks passed\n",checks);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Original HBM archive at check %u: %s\n",checks,error.what());
        return 1;
    }
}

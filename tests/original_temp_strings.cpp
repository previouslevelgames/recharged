#include "NL/nlBasicString.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#if defined(MSCHARGED_DIAGNOSTIC_STRINGS) || defined(MSCHARGED_DIAGNOSTIC_VECTORS)
#error Original temporary-string source qualifier cannot use diagnostic policies
#endif

namespace {
using Allocator = Detail::TempStringAllocator;
using String = BasicString<char, Allocator>;
using WideString = BasicString<unsigned short, Allocator>;
unsigned checks;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

Detail::StringBlock* ManagerField(unsigned offset) {
    Detail::StringBlock* result;
    std::memcpy(&result, reinterpret_cast<const unsigned char*>(&Detail::sStringBlockAllocator)+offset, sizeof(result));
    return result;
}

unsigned FreeBlocks() {
    auto* base = ManagerField(sizeof(void*));
    std::uint64_t seen = 0;
    unsigned count = 0;
    for (auto* block=ManagerField(0);block;block=block->next) {
        auto address = reinterpret_cast<std::uintptr_t>(block), start = reinterpret_cast<std::uintptr_t>(base);
        Check(address >= start && address < start+64*sizeof(Detail::StringBlock), "Manager free block escaped original backing");
        Check((address-start)%sizeof(Detail::StringBlock)==0, "Manager block stride changed");
        auto index=(address-start)/sizeof(Detail::StringBlock);
        Check(!(seen&(std::uint64_t(1)<<index)), "Manager free-list cycle/alias");
        seen|=std::uint64_t(1)<<index;
        ++count;
    }
    return count;
}

void Requests() {
    auto* base=ManagerField(sizeof(void*));
    void* blocks[64];
    const auto memory=StandardAllocator.TotalFreeMemory();
    Check(FreeBlocks()==64, "Original static manager did not create64 slots");
    for (unsigned i=0;i<64;++i) {
        // The literal source manager ignores its requested byte count.
        blocks[i]=Allocator::Alloc(i%3==0?0:i%3==1?-17:0x7fffffff);
        Check(blocks[i]==base+i, "Native wrapper replaced fixed source block selection");
        Check(reinterpret_cast<std::uintptr_t>(blocks[i])%alignof(String::Data)==0, "Manager Data native alignment changed");
    }
    Check(!Allocator::Alloc(24), "Original65th manager request did not return null");
    Check(StandardAllocator.TotalFreeMemory()==memory, "Exhausted source manager acquired fallback arena memory");
    for (unsigned i=0;i<64;++i) Allocator::Free(blocks[i*17%64]);
    for (unsigned i=0;i<64;++i)
        Check(Allocator::Alloc(1)==blocks[(63-i)*17%64], "Original manager Free LIFO order changed");
    for (int i=63;i>=0;--i) Allocator::Free(blocks[i]);
}

void TypedLifetimes() {
    {
        String original("ab"), copy(original);
        Check(original.mData==copy.mData && original.mData->mRefCount==2, "Source copy refcount changed");
        copy[0]='A';
        Check(std::strcmp(original.c_str(),"ab")==0 && std::strcmp(copy.c_str(),"Ab")==0, "Source COW character result differs");
        Check(original.mData!=copy.mData && original.mData->mRefCount==1 && copy.mData->mRefCount==1, "Source COW owner transition changed");
        copy.AppendInPlace("CD");
        Check(std::strcmp(copy.c_str(),"AbCD")==0 && copy.mData->mData.mCapacity==5, "Original concatenation/growth changed");
        copy.mData->reserve(11);
        copy.erase(copy.begin()+1,copy.begin()+3);
        Check(std::strcmp(copy.c_str(),"AD")==0 && copy.mData->mData.mCapacity==11, "Source reserve/erase result differs");
        const unsigned short text[]={0x41,0x03a9,0};
        WideString wide(text), shared(wide);
        shared[1]=0x4b;
        Check(wide.c_str()[0]==0x41 && wide.c_str()[1]==0x03a9 && shared.c_str()[1]==0x4b && shared.c_str()[2]==0,
              "Original Wii16 COW words differ");
        Check(reinterpret_cast<std::uintptr_t>(wide.mData)%8==0 && reinterpret_cast<std::uintptr_t>(wide.c_str())%8==0,
              "Native typed strings lost fixed-block alignment");
    }
    Check(FreeBlocks()==64, "Actual typed source lifetimes retained blocks");
    {
        String live[33];
        for (unsigned i=0;i<32;++i) {
            live[i]=String("n");
            Check(live[i].mData && std::strcmp(live[i].c_str(),"n")==0, "Original two-block string exhausted early");
            Check(FreeBlocks()==64-2*(i+1), "Native string changed the original two-block request count");
        }
        live[32]=String("n");
        Check(!live[32].mData && !live[32].size() && !live[32].c_str()[0] && FreeBlocks()==0,
              "Compiler did not skip the original null Data constructor under -fcheck-new");
    }
    Check(FreeBlocks()==64, "Original exhausted string lifetimes did not return64 blocks");
}
}

int main() {
    try {
        static_assert(sizeof(Detail::StringBlock)==1032 && alignof(Detail::StringBlock)==8);
        static_assert(sizeof(String::Data)==24 && alignof(String::Data)==8);
        static_assert(sizeof(WideString::Data)==24 && alignof(WideString::Data)==8);
        const auto standard=StandardAllocator.TotalFreeMemory(), virtualMemory=VirtualAllocator.TotalFreeMemory();
        Requests(); TypedLifetimes();
        Check(StandardAllocator.TotalFreeMemory()==standard && VirtualAllocator.TotalFreeMemory()==virtualMemory,
              "Actual source requests/lifetimes lost fixture arena memory");
        std::printf("actual original temporary string checks=%u;64 blocks, typed source lifetime/null-new; terminal CPU fixture, no module/SDK/main/CRT teardown readiness\n",checks);
        std::fflush(nullptr);
        // Retain the original global manager's separate null CRT-destructor
        // branch. This terminal CPU fixture does not establish safe teardown.
        std::_Exit(0);
    } catch (const std::exception& error) {
        std::fprintf(stderr,"%s\n",error.what()); std::fflush(nullptr); std::_Exit(1);
    }
}

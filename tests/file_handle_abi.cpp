#include "platform/file_handle_abi.h"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <cstdint>
namespace { unsigned live, allocs, failAt, cleaned, checks; }
extern "C" void* ChargedNativeMetadataAllocate(std::size_t size) {
    if (++allocs==failAt) throw std::bad_alloc();
    auto* p=std::malloc(size);if(!p)throw std::bad_alloc();++live;return p;
}
extern "C" void ChargedNativeMetadataRelease(void* p) noexcept { if(p){--live;std::free(p);} }
void Check(bool value,const char* reason) { ++checks;if(!value)throw std::runtime_error(reason); }
void Cleanup(void* p) { Check(p!=nullptr,"Cleanup lost actual context");++cleaned; }
int main() {
    using namespace mscharged::platform;
    try {
        int a,b,file;
        auto* actual=reinterpret_cast<void*>(std::uintptr_t{0x100000090});
        for(unsigned position=1;position<=2;++position) {
            failAt=allocs+position;bool failed=false;
            try { ReserveFileLoad(&a,&file,Cleanup); } catch(const std::bad_alloc&){failed=true;}
            Check(failed && live==0,"Pre-submission metadata failure leaked reservation");failAt=0;
        }
        auto* lease=ReserveFileLoad(&a,&file,Cleanup);
        const auto token=BindFileLoad(lease,actual);
        Check(ResolveFileReadHandle(token)==actual,"Actual native pointer identity was truncated");
        FinishFileLoad(&a);
        Check(live==1 && cleaned==0,"Completion removed identity or fabricated cleanup");
        lease=ReserveFileLoad(&b,&file,Cleanup);
        Check(BindFileLoad(lease,actual)==token,"Original slot reuse changed 32-bit identity");
        AbortFileLoadsForFile(&file);
        Check(cleaned==1 && live==1,"Actual file abort retained source owner");
        bool rejected=false;
        try { ResolveFileReadHandle(0); } catch(const std::invalid_argument&){rejected=true;}
        Check(rejected,"Zero native token was represented as valid pointer");
        lease=ReserveFileLoad(&a,&file,Cleanup);
        BindFileLoad(lease,actual);
        ShutdownFileLoads();
        Check(live==0 && cleaned==2,"Shutdown retained owner or identity metadata");
        rejected=false;
        try{ResolveFileReadHandle(token);}catch(const std::invalid_argument&){rejected=true;}
        Check(rejected,"Old lifecycle pointer remained represented");
        lease=ReserveFileLoad(&b,&file,Cleanup);
        Check(BindFileLoad(lease,actual)!=token,"Reset aliased previous lifecycle token");
        FinishFileLoad(&b);ShutdownFileLoads();
        Check(live==0,"Final metadata reset leaked");
        std::printf("Pointer identity metadata ABI: %u checks; native >4GiB identity, failure-before-submit, source slot reuse and reset passed.\n",checks);
        return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}

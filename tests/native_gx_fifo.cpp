#include <dolphin/gx/GXFifo.h>
#include <dolphin/os.h>
#include "platform/interrupts.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>

struct NativeLayout {void*base;void*top;u32 size,hi,lo;void*read;void*write;s32 count;u8 wrap,bindCpu,bindGp;};
NativeLayout Inspect(const GXFifoObj* fifo){NativeLayout result;std::memcpy(&result,fifo,sizeof(result));return result;}
unsigned checks;
void Check(bool value,const char*message){++checks;if(!value)throw std::runtime_error(message);}
int main(){
 try{
  // Public object is byte aligned, including an unaligned caller allocation.
  std::array<unsigned char,sizeof(GXFifoObj)+2> storage;storage.fill(0xa5);
  auto* fifo=reinterpret_cast<GXFifoObj*>(storage.data()+1);
  const auto base=reinterpret_cast<void*>(std::uintptr_t{0x100001000});
  constexpr std::uint32_t size=0x100000;
  GXInitFifoBase(fifo,base,size);
  Check(GXGetFifoBase(fifo)==base,"Native FIFO truncated a source allocation above4GiB");
  Check(Inspect(fifo).top==reinterpret_cast<void*>(std::uintptr_t{0x100100ffc}),"Native FIFO top address truncated");
  Check(Inspect(fifo).count==0,"Initial source FIFO count changed");
  Check(GXGetFifoSize(fifo)==size,"Source FIFO size changed");
  void*r,*w;GXGetFifoPtrs(fifo,&r,&w);
  Check(r==base&&w==base,"Source initial FIFO pointers changed");
  std::uint32_t hi,lo;GXGetFifoLimits(fifo,&hi,&lo);
  Check(hi==size-16384&&lo==size/2,"Retail initial watermark requests changed");
  // Literal whole glPlat startup requests override SDK defaults.
  GXInitFifoLimits(fifo,size-0x10000,size-0x40000);
  GXGetFifoLimits(fifo,&hi,&lo);
  Check(hi==0xf0000&&lo==0xc0000,"Original glPlat watermark requests lost");
  Check(storage.front()==0xa5&&storage.back()==0xa5,"Opaque descriptor corrupted caller sentries");
  auto* read=reinterpret_cast<void*>(std::uintptr_t{0x100001200});
  auto* write=reinterpret_cast<void*>(std::uintptr_t{0x100001800});
  GXInitFifoPtrs(fifo,read,write);GXGetFifoPtrs(fifo,&r,&w);
  Check(Inspect(fifo).count==0x600,"Forward source FIFO count changed");
  Check(r==read&&w==write,"Borrowed read/write address transport changed");
  Check(mscharged::platform::NativeInterruptsEnabled(),"Pointer update lost enabled exclusion state");
  const auto enabled=OSDisableInterrupts();
  GXInitFifoPtrs(fifo,write,read);GXGetFifoPtrs(fifo,&r,&w);
  Check(Inspect(fifo).count==size-0x600,"Wrapped source FIFO count changed");
  Check(r==write&&w==read,"Wrapped source FIFO pointer requests changed");
  Check(!mscharged::platform::NativeInterruptsEnabled(),"Nested update prematurely enabled source interrupts");
  OSRestoreInterrupts(enabled);
  Check(mscharged::platform::NativeInterruptsEnabled(),"Source outer exclusion not restored");
  GXSetCPUFifo(fifo);GXSetGPFifo(fifo);
  Check(GXGetCPUFifo()==fifo&&GXGetGPFifo()==fifo,"Original FIFO selection identity changed");
  std::printf("Native GX FIFO: %u address/watermark/exclusion checks; no GPU queue/status acceptance\n",checks);
 }catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}

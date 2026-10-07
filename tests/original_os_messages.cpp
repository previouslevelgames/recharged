#include "platform/interrupts.h"
#include "platform/thread_queues.h"
#include <dolphin/os.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace mscharged::platform;
using namespace std::chrono_literals;
std::atomic<unsigned> checks{}, blocked_returns{}, irq_sends{}, wait_services{};
std::thread::id owner;
OSContext* owner_context{};
OSMessageQueue* irq_queue{};
OSMessage irq_message{};
std::atomic<bool> pending{};

void Check(bool value,const char* message) {
    ++checks;
    if(!value)throw std::runtime_error(message);
}
template<class F> void Until(F predicate) {
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(!predicate()) {
        if(std::chrono::steady_clock::now()>=deadline)
            throw std::runtime_error("Original message queue operation timed out");
        std::this_thread::yield();
    }
}
unsigned Waiters(OSThreadQueue& queue) {
    const auto mask=OSDisableInterrupts();
    unsigned count{};
    for(auto* p=queue.head;p;p=p->link.next)++count;
    OSRestoreInterrupts(mask);
    return count;
}
void IRQSend() {
    Check(std::this_thread::get_id()==owner,"Original message IRQ escaped actual owner");
    Check(NativeInterruptDispatchActive()&&!NativeInterruptsEnabled(),"Original message IRQ mask/identity lost");
    Check(OSGetCurrentContext()!=owner_context,"Original message IRQ lacked temporary context");
    Check(OSSendMessage(irq_queue,irq_message,OS_MESSAGE_NOBLOCK),"Original send could not wake actual blocked owner");
    Check(!NativeInterruptsEnabled(),"Original send changed interrupt caller mask");
    ++irq_sends;
}
void WaitService() {
    ++wait_services;
    Check(std::this_thread::get_id()==owner&&NativeInterruptsEnabled(),"Message wait service changed actual owner/exclusion");
    if(pending.exchange(false))Check(DispatchNativeInterrupt(IRQSend),"Real latched message IRQ failed to dispatch");
}

void Blocking(OSMessage first,OSMessage second) {
    OSMessage cell{};
    OSMessageQueue queue;
    OSInitMessageQueue(&queue,&cell,1);
    Check(OSSendMessage(&queue,first,0),"Original queue could not prime actual full ring");
    std::atomic<bool> finished{};
    std::thread sender([&] {
        auto* context=OSGetCurrentContext();
        Check(OSSendMessage(&queue,second,3),"Original blocked sender failed after genuine receive");
        Check(NativeInterruptsEnabled()&&OSGetCurrentContext()==context,"Original blocked sender lost native mask/context");
        finished=true;
        ++blocked_returns;
    });
    Until([&] { return Waiters(queue.queueSend)==1; });
    Check(!finished&&queue.usedCount==1,"Original full send bypassed its source wait");
    OSMessage message{};
    Check(OSReceiveMessage(&queue,&message,0)&&message==first,"Original receive changed FIFO before blocked send");
    sender.join();
    Check(finished&&queue.usedCount==1&&Waiters(queue.queueSend)==0,"Original source sender completion/queue state lost");
    Check(OSReceiveMessage(&queue,&message,0)&&message==second,"Original blocked send truncated a native pointer");

    finished=false;
    std::thread receiver([&] {
        auto* context=OSGetCurrentContext();
        OSMessage received{};
        Check(OSReceiveMessage(&queue,&received,OS_MESSAGE_BLOCK)&&received==second,
            "Original blocked receive changed native message identity");
        Check(NativeInterruptsEnabled()&&OSGetCurrentContext()==context,"Original receiver lost native caller state");
        finished=true;
        ++blocked_returns;
    });
    Until([&] { return Waiters(queue.queueReceive)==1; });
    Check(!finished&&!queue.usedCount,"Original empty receive bypassed its source wait");
    Check(OSSendMessage(&queue,second,0),"Original send failed to release actual receiver");
    receiver.join();
    Check(finished&&!queue.usedCount&&!Waiters(queue.queueReceive),"Original receiver left ring/waiter live");
}

void MultipleReceivers(const std::array<OSMessage,3>& values) {
    OSMessage cell{};
    OSMessageQueue queue;
    OSInitMessageQueue(&queue,&cell,1);
    std::array<OSMessage,3> received{};
    std::atomic<unsigned> completed{};
    std::vector<std::thread> workers;
    for(unsigned n=0;n<3;++n) {
        workers.emplace_back([&,n] {
            Check(OSReceiveMessage(&queue,&received[n],OS_MESSAGE_BLOCK),"Broadcast wake fabricated or lost original receive");
            ++completed;
            ++blocked_returns;
        });
        Until([&] {return Waiters(queue.queueReceive)==n+1;});
    }
    for(unsigned n=0;n<3;++n) {
        Check(OSSendMessage(&queue,values[n],OS_MESSAGE_NOBLOCK),"Original broadcast send failed");
        Until([&] {return completed==n+1&&Waiters(queue.queueReceive)==2-n;});
        Check(!queue.usedCount,"Broadcast wake bypassed source empty-ring loop");
    }
    for(auto& worker:workers)worker.join();
    for(auto value:values) {
        unsigned count{};
        for(auto message:received)if(message==value)++count;
        Check(count==1,"Multiple source waiters consumed or fabricated the same native message");
    }
}
} // namespace

int main() {
    try {
        static_assert(sizeof(s32)==4);
        static_assert(sizeof(OSMessage)==sizeof(void*));
        static_assert(sizeof(OSMessageQueue::msgCount)==4);
        static_assert(sizeof(OSMessageQueue::firstIndex)==4);
        static_assert(sizeof(OSMessageQueue::usedCount)==4);
        owner=std::this_thread::get_id();owner_context=OSGetCurrentContext();
        auto one=std::make_unique<unsigned>(0x13579bdfu);
        auto two=std::make_unique<unsigned>(0x2468ace0u);
        auto three=std::make_unique<unsigned>(0xabcdef01u);
        const std::array<OSMessage,3> objects{one.get(),two.get(),three.get()};
        Check(std::uintptr_t(objects[0])>0xffffffffULL&&std::uintptr_t(objects[1])>0xffffffffULL,
            "Actual message object owners were not above 4 GiB");

        struct Ring { OSMessage left; OSMessage cells[3]; OSMessage right; } ring;
        constexpr auto marker=std::uintptr_t{0xa55a7f0088112233ULL};
        ring.left=ring.right=reinterpret_cast<OSMessage>(marker);
        for(auto& cell:ring.cells)cell=reinterpret_cast<OSMessage>(marker);
        OSMessageQueue queue;
        std::memset(&queue,0xa5,sizeof(queue));
        std::array<unsigned char,sizeof(queue)> expected;
        std::memcpy(expected.data(),&queue,sizeof(queue));
        auto write=[&](std::size_t offset,const auto& value) { std::memcpy(expected.data()+offset,&value,sizeof(value)); };
        OSThreadQueue empty{};
        OSMessage* cells=ring.cells;
        s32 count=3,zero=0;
        write(offsetof(OSMessageQueue,queueSend),empty);write(offsetof(OSMessageQueue,queueReceive),empty);
        write(offsetof(OSMessageQueue,msgArray),cells);write(offsetof(OSMessageQueue,msgCount),count);
        write(offsetof(OSMessageQueue,firstIndex),zero);write(offsetof(OSMessageQueue,usedCount),zero);
        OSInitMessageQueue(&queue,ring.cells,3);
        Check(std::memcmp(expected.data(),&queue,sizeof(queue))==0,"Original message initializer changed extra native fields/padding");
        OSMessage out=reinterpret_cast<OSMessage>(marker);
        Check(!OSReceiveMessage(&queue,&out,0)&&out==reinterpret_cast<OSMessage>(marker),"Empty nonblocking receive changed source output");
        Check(!OSReceiveMessage(&queue,&out,2)&&!queue.firstIndex&&!queue.usedCount,"Original unknown nonblocking flag gained behavior");
        for(unsigned n=0;n<objects.size();++n) {
            Check(OSSendMessage(&queue,objects[n],0),"Original ring send rejected native object");
            Check(ring.cells[n]==objects[n],"Original live message cell truncated pointer or used Wii32 stride");
            Check(queue.usedCount==s32(n+1)&&!queue.firstIndex,"Original send ring counters changed");
        }
        Check(ring.left==reinterpret_cast<OSMessage>(marker)&&ring.right==reinterpret_cast<OSMessage>(marker),"Native message writes overran original caller array");
        Check(!OSSendMessage(&queue,objects[0],0)&&!OSSendMessage(&queue,objects[0],2),"Full nonblocking send bypassed source flags");
        Check(queue.usedCount==3&&!queue.firstIndex,"Rejected full send changed source ring");
        Check(OSReceiveMessage(&queue,&out,0)&&out==objects[0]&&queue.firstIndex==1&&queue.usedCount==2,"Original receive ordering/counters changed");
        Check(OSSendMessage(&queue,objects[0],0)&&ring.cells[0]==objects[0]&&queue.firstIndex==1&&queue.usedCount==3,"Original native ring wrap changed tail computation");
        for(auto value:{objects[1],objects[2],objects[0]})Check(OSReceiveMessage(&queue,&out,0)&&out==value,"Original wrapped FIFO sequence changed");
        Check(queue.firstIndex==1&&!queue.usedCount,"Original wrapped receive ring indexes changed");

        const std::array<std::uintptr_t,7> scalar{0,1,0x7fffffffu,0x80000000u,0xffffffffu,0x100000001ULL,~std::uintptr_t{0}};
        for(auto word:scalar) {
            const auto value=reinterpret_cast<OSMessage>(word);
            Check(OSSendMessage(&queue,value,0)&&OSReceiveMessage(&queue,&out,0)&&std::uintptr_t(out)==word,
                "Source scalar/native pointer message bit pattern changed");
        }
        Check(OSSendMessage(&queue,objects[2],0),"Original null-output setup send failed");
        Check(OSReceiveMessage(&queue,nullptr,0)&&!queue.usedCount,"Original null-output consume decision changed");
        Check(*one==0x13579bdfu&&*two==0x2468ace0u&&*three==0xabcdef01u,"Source message queue incorrectly took payload ownership");
        const auto mask=OSDisableInterrupts();
        Check(OSSendMessage(&queue,objects[1],0)&&!NativeInterruptsEnabled(),"Original send lost nested caller mask");
        Check(OSReceiveMessage(&queue,&out,0)&&out==objects[1]&&!NativeInterruptsEnabled(),"Original receive lost nested caller mask");
        OSRestoreInterrupts(mask);
        Check(OSGetCurrentContext()==owner_context&&NativeInterruptsEnabled(),"Ordinary source message API changed caller context");

        Blocking(objects[0],objects[1]);
        MultipleReceivers(objects);

        OSMessage cell{};
        OSMessageQueue owner_ring;
        OSInitMessageQueue(&owner_ring,&cell,1);
        irq_queue=&owner_ring;irq_message=objects[2];
        Check(!SetNativeThreadWaitService(WaitService),"Actual source message owner wait hook was occupied");
        std::thread producer([] {std::this_thread::sleep_for(8ms);pending=true;});
        const auto outer=OSDisableInterrupts();
        Check(OSReceiveMessage(&owner_ring,&out,OS_MESSAGE_BLOCK)&&out==objects[2],"Original owner receive lost real IRQ message");
        Check(!NativeInterruptsEnabled(),"Original owner blocked receive lost caller mask");
        OSRestoreInterrupts(outer);producer.join();++blocked_returns;
        Check(irq_sends==1&&wait_services>0&&!pending,"Original message owner IRQ repeated or fabricated data");
        Check(SetNativeThreadWaitService(nullptr)==WaitService,"Actual source message wait hook was not retired");
        Check(OSGetCurrentContext()==owner_context&&NativeInterruptsEnabled()&&!NativeInterruptDispatchActive(),"Original message IRQ caller context leaked");

        for(s32 capacity:{0,-1}) {
            OSMessageQueue invalid;
            OSInitMessageQueue(&invalid,nullptr,capacity);
            Check(invalid.msgCount==capacity&&!invalid.firstIndex&&!invalid.usedCount,
                "Source initializer repaired a retail capacity argument");
            Check(!OSSendMessage(&invalid,objects[0],0)&&!OSReceiveMessage(&invalid,&out,0),
                "Source nonblocking invalid-capacity decisions changed");
        }
        std::printf("Original OSMessage: %u checks, %u actual blocked returns, %u owner IRQ send; whole Matching TU, native messages=%zu queue=%zu, all three ring scalars=32 bits. OSJam/thread creation/AX readiness not claimed.\n",
            checks.load(),blocked_returns.load(),irq_sends.load(),sizeof(OSMessage),sizeof(OSMessageQueue));
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Original OSMessage failure: %s (%u checks/%u blocked returns)\n",error.what(),checks.load(),blocked_returns.load());
        return 1;
    }
}

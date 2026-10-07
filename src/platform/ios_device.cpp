#include "platform/ios_device.h"
#include "platform/interrupts.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
struct Handle { std::uint64_t device; s32 local; };
struct Request {
    IPCRequest command;
    IPCAsyncCallback callback;
    void* context;
    bool completed{};
    s32 result{};
};
struct Bus {
    std::recursive_mutex mutex;
    std::thread::id owner;
    std::uint64_t generation{};
    s32 next_fd{1};
    std::map<std::uint64_t, NativeIOSDevice> devices;
    std::map<s32, Handle> handles;
    std::deque<Request> pending;
    bool active{};
};
Bus& State() { static Bus bus; return bus; }

s32 Execute(Bus& bus, const IPCRequest& request) {
    IPCRequest local=request;
    NativeIOSDevice* driver=nullptr;
    std::uint64_t device=0;
    if (request.type==IPC_REQ_OPEN) {
        if (!request.open.path) return IPC_RESULT_INVALID;
        for (auto& [identity, candidate] : bus.devices) {
            if (!candidate.owns_path(candidate.context, request.open.path)) continue;
            if (driver) throw std::logic_error("Native IOS devices claim the same path");
            driver=&candidate; device=identity;
        }
        if (!driver) return IPC_RESULT_NOEXISTS;
        if (bus.next_fd==std::numeric_limits<s32>::max()) return IPC_RESULT_MAXFD;
        // Allocate the native routing cell before an actual device open, so an
        // allocation failure cannot leak a physical handle created afterward.
        const auto fd=bus.next_fd;
        auto [cell, inserted]=bus.handles.emplace(fd, Handle{device,-1});
        if (!inserted) throw std::logic_error("Native IOS descriptor identity reused");
        s32 result;
        try { result=driver->execute(driver->context,local); }
        catch (...) { bus.handles.erase(cell); throw; }
        if (result<0) { bus.handles.erase(cell); return result; }
        cell->second.local=result;
        ++bus.next_fd;
        return fd;
    }
    auto found=bus.handles.find(request.fd);
    if (found==bus.handles.end()) return IPC_RESULT_INVALID;
    driver=&bus.devices.at(found->second.device);
    local.fd=found->second.local;
    const auto result=driver->execute(driver->context,local);
    if (request.type==IPC_REQ_CLOSE && result==IPC_RESULT_OK) bus.handles.erase(found);
    return result;
}
void CompletePriorHardware(Bus& bus) {
    // A later synchronous operation observes earlier submitted device work.
    // Completion does not invoke source callbacks on a submitting worker.
    for (auto& request : bus.pending) {
        if (request.completed) continue;
        request.result=Execute(bus,request.command);
        request.completed=true;
    }
}
s32 Synchronous(const IPCRequest& request) {
    auto& bus=State();
    std::lock_guard lock(bus.mutex);
    CompletePriorHardware(bus);
    return Execute(bus,request);
}
s32 Queue(const IPCRequest& request, IPCAsyncCallback callback, void* context) {
    auto& bus=State();
    std::lock_guard lock(bus.mutex);
    // Original ipcclt treats a null callback as a synchronous request.
    if (!callback) { CompletePriorHardware(bus);return Execute(bus,request); }
    if (bus.devices.empty()) return IPC_RESULT_NOEXISTS;
    if (request.type==IPC_REQ_IOCTL) {
        auto handle=bus.handles.find(request.fd);
        if (handle!=bus.handles.end()) {
            auto& driver=bus.devices.at(handle->second.device);
            if (driver.ioctl_async) {
                CompletePriorHardware(bus);
                IPCRequest local=request;local.fd=handle->second.local;
                return driver.ioctl_async(driver.context,local,callback,context);
            }
        }
    }
    // Real bounded transport exhaustion; no callback or partial work is issued.
    if (bus.pending.size()>=64) return IPC_RESULT_BUSY_INTERNAL;
    bus.pending.push_back({request,callback,context});
    return IPC_RESULT_OK;
}
IPCRequest Simple(IPCRequestType type,s32 fd) {
    IPCRequest request{};request.type=type;request.fd=fd;return request;
}
}

namespace mscharged::platform {
NativeIOSDeviceLease RegisterNativeIOSDevice(NativeIOSDevice device) {
    if (!device.context || !device.owns_path || !device.execute)
        throw std::invalid_argument("Native IOS requires a complete retained device");
    NativeInterruptGuard exclusion;
    auto& bus=State();std::lock_guard lock(bus.mutex);
    if (!bus.devices.empty() && bus.owner!=std::this_thread::get_id())
        throw std::logic_error("Native IOS registration requires its hardware owner");
    for (const auto& [identity, candidate] : bus.devices)
        if (candidate.context==device.context)
            throw std::logic_error("Native IOS device already registered");
    if (bus.generation==std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Native IOS device identity exhausted");
    const auto identity=bus.generation+1;
    bus.devices.emplace(identity,device);
    bus.generation=identity;bus.owner=std::this_thread::get_id();
    return {identity};
}
void UnregisterNativeIOSDevice(NativeIOSDeviceLease lease) {
    NativeInterruptGuard exclusion;
    auto& bus=State();std::lock_guard lock(bus.mutex);
    if (bus.owner!=std::this_thread::get_id() || !bus.devices.count(lease.generation))
        throw std::logic_error("Native IOS retirement requires its exact hardware owner");
    if (bus.active || !bus.pending.empty())
        throw std::logic_error("Cannot retire native IOS with pending source callbacks");
    for (auto handle=bus.handles.begin();handle!=bus.handles.end();)
        if (handle->second.device==lease.generation) handle=bus.handles.erase(handle);
        else ++handle;
    bus.devices.erase(lease.generation);
    if (bus.devices.empty()) bus.owner={};
}
NativeIOSStatus GetNativeIOSStatus() {
    auto& bus=State();std::lock_guard lock(bus.mutex);
    return {bus.devices.size(),bus.handles.size(),bus.pending.size(),bus.active};
}
bool ServiceNativeIOSRequests() {
    NativeInterruptRead exclusion;
    if (!exclusion || !NativeInterruptsEnabled()) return false;
    auto& bus=State();Request request; s32 result;
    {
        std::lock_guard lock(bus.mutex);
        if (bus.devices.empty()) return false;
        if (bus.owner!=std::this_thread::get_id())
            throw std::logic_error("Native IOS completions require their hardware owner");
        if (bus.active || bus.pending.empty()) return false;
        request=bus.pending.front();
        // Preserve the request if host execution throws before completion.
        result=request.completed ? request.result : Execute(bus,request.command);
        bus.pending.pop_front();bus.active=true;
    }
    struct Finish { Bus& bus; ~Finish(){std::lock_guard lock(bus.mutex);bus.active=false;} } finish{bus};
    struct Delivery {
        Request request;s32 result;
        static void Run(void* data) {
            auto& delivery=*static_cast<Delivery*>(data);
            delivery.request.callback(delivery.result,delivery.request.context);
        }
    } delivery{request,result};
    if (!DispatchNativeInterrupt(Delivery::Run,&delivery))
        throw std::logic_error("Native IOS completion lost the enabled owner interrupt boundary");
    return true;
}
}

extern "C" s32 IOS_Open(const char* path,IPCOpenMode mode) {
    auto r=Simple(IPC_REQ_OPEN,-1);r.open={path,mode};return Synchronous(r);
}
extern "C" s32 IOS_OpenAsync(const char* path,IPCOpenMode mode,IPCAsyncCallback cb,void* arg) {
    auto r=Simple(IPC_REQ_OPEN,-1);r.open={path,mode};return Queue(r,cb,arg);
}
extern "C" s32 IOS_Close(s32 fd) { return Synchronous(Simple(IPC_REQ_CLOSE,fd)); }
extern "C" s32 IOS_CloseAsync(s32 fd,IPCAsyncCallback cb,void* arg) {
    return Queue(Simple(IPC_REQ_CLOSE,fd),cb,arg);
}
extern "C" s32 IOS_Read(s32 fd,void* data,s32 length) {
    if(length<0) return IPC_RESULT_INVALID;
    auto r=Simple(IPC_REQ_READ,fd);r.rw={data,static_cast<u32>(length)};return Synchronous(r);
}
extern "C" s32 IOS_ReadAsync(s32 fd,void* data,s32 length,IPCAsyncCallback cb,void* arg) {
    if(length<0) return IPC_RESULT_INVALID;
    auto r=Simple(IPC_REQ_READ,fd);r.rw={data,static_cast<u32>(length)};return Queue(r,cb,arg);
}
extern "C" s32 IOS_Write(s32 fd,const void* data,s32 length) {
    if(length<0) return IPC_RESULT_INVALID;
    auto r=Simple(IPC_REQ_WRITE,fd);r.rw={const_cast<void*>(data),static_cast<u32>(length)};return Synchronous(r);
}
extern "C" s32 IOS_WriteAsync(s32 fd,const void* data,s32 length,IPCAsyncCallback cb,void* arg) {
    if(length<0) return IPC_RESULT_INVALID;
    auto r=Simple(IPC_REQ_WRITE,fd);r.rw={const_cast<void*>(data),static_cast<u32>(length)};return Queue(r,cb,arg);
}
extern "C" s32 IOS_Seek(s32 fd,s32 offset,IPCSeekMode mode) {
    auto r=Simple(IPC_REQ_SEEK,fd);r.seek={offset,mode};return Synchronous(r);
}
extern "C" s32 IOS_SeekAsync(s32 fd,s32 offset,IPCSeekMode mode,IPCAsyncCallback cb,void* arg) {
    auto r=Simple(IPC_REQ_SEEK,fd);r.seek={offset,mode};return Queue(r,cb,arg);
}
extern "C" s32 IOS_Ioctl(s32 fd,s32 type,void* in,s32 inSize,void* out,s32 outSize) {
    if(inSize<0 || outSize<0) return IPC_RESULT_INVALID;
    auto r=Simple(IPC_REQ_IOCTL,fd);r.ioctl={type,in,inSize,out,outSize};return Synchronous(r);
}
extern "C" s32 IOS_IoctlAsync(s32 fd,s32 type,void* in,s32 inSize,void* out,s32 outSize,IPCAsyncCallback cb,void* arg) {
    if(inSize<0 || outSize<0) return IPC_RESULT_INVALID;
    auto r=Simple(IPC_REQ_IOCTL,fd);r.ioctl={type,in,inSize,out,outSize};return Queue(r,cb,arg);
}
extern "C" s32 IOS_Ioctlv(s32 fd,s32 type,s32 inCount,s32 outCount,IPCIOVector* vectors) {
    if(inCount<0 || outCount<0) return IPC_RESULT_INVALID;
    auto r=Simple(IPC_REQ_IOCTLV,fd);r.ioctlv={type,static_cast<u32>(inCount),static_cast<u32>(outCount),vectors};return Synchronous(r);
}
extern "C" s32 IOS_IoctlvAsync(s32 fd,s32 type,s32 inCount,s32 outCount,IPCIOVector* vectors,IPCAsyncCallback cb,void* arg) {
    if(inCount<0 || outCount<0) return IPC_RESULT_INVALID;
    auto r=Simple(IPC_REQ_IOCTLV,fd);r.ioctlv={type,static_cast<u32>(inCount),static_cast<u32>(outCount),vectors};return Queue(r,cb,arg);
}

#include "platform/filesystem_device.h"
#include "platform/ios_device.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace mscharged::platform;
namespace fs=std::filesystem;
constexpr s32 CreateDir=3,ReadDir=4,GetAttr=6,Delete=7,Rename=8,CreateFile=9,
    GetFileStats=11,GetUsage=12,Shutdown=13;
constexpr s32 GetTitleId=0x20,GetDataDir=0x1d;
constexpr std::size_t PathBytes=64,NameBytes=13,ClusterBytes=0x4000;
struct FileIoctl {
    u32 uid; u16 gid; char path[64];
    u8 owner,group,other,attr; u8 unused[2];
};
static_assert(sizeof(FileIoctl)==76&&offsetof(FileIoctl,path)==6&&offsetof(FileIoctl,owner)==70);
struct Metadata { u32 uid{};u16 gid{};u8 owner{},group{},other{},attr{};bool directory{};u64 order{}; };
enum class Kind { Fs, Es, File };
struct Handle { Kind kind;int physical{-1};IPCOpenMode mode{};std::string path; };
struct Device {
    std::mutex mutex;
    bool ready{};
    std::thread::id owner;
    NativeFilesystemSettings settings;
    NativeIOSDeviceLease lease{};
    fs::path data,metadata;
    std::string home;
    std::map<std::string,Metadata> entries;
    std::map<s32,Handle> handles;
    s32 next_fd{1};
    u64 next_order{1};
};
Device& State(){static Device state;return state;}
s32 Error(int error) {
    switch(error) {
    case ENOENT:return IPC_RESULT_NOEXISTS;
    case EEXIST:return IPC_RESULT_EXISTS;
    case EACCES:case EPERM:return IPC_RESULT_ACCESS;
    case EINVAL:case ENOTDIR:case EISDIR:case ELOOP:case ENAMETOOLONG:return IPC_RESULT_INVALID;
    case EMFILE:case ENFILE:return IPC_RESULT_MAXFD;
    case ENOSPC:return IPC_RESULT_MAXBLOCKS;
    case ENOTEMPTY:return IPC_RESULT_NOTEMPTY;
    default:return IPC_RESULT_FATAL_ERROR;
    }
}
std::string Path(const char* bytes) {
    if(!bytes)return {};
    const auto length=::strnlen(bytes,PathBytes);
    if(!length||length>=PathBytes||bytes[0]!='/')return {};
    std::string path(bytes,length);
    if(path=="/")return path;
    if(path.back()=='/')return {};
    std::size_t begin=1,depth{};
    while(begin<path.size()) {
        const auto end=path.find('/',begin);
        const auto count=(end==std::string::npos?path.size():end)-begin;
        if(!count||count>=NameBytes||++depth>8)return {};
        const auto component=path.substr(begin,count);
        if(component=="."||component==".."||component.find('\\')!=std::string::npos)return {};
        for(unsigned char c:component)if(c<32||c==127)return {};
        if(end==std::string::npos)break;
        begin=end+1;
    }
    return path;
}
std::string Parent(const std::string& path){const auto end=path.rfind('/');return end==0?"/":path.substr(0,end);}
fs::path Physical(const Device& d,const std::string& path){return path=="/"?d.data:d.data/path.substr(1);}
void CheckPhysical(const Device& d,const std::string& path) {
    // The IOS namespace never imports host symlinks as console files.
    // Existing owned backing must remain consistent with its persistent catalog.
    fs::path value=d.data;
    std::size_t begin=1;
    while(begin<path.size()) {
        const auto end=path.find('/',begin);value/=path.substr(begin,(end==std::string::npos?path.size():end)-begin);
        std::error_code ec;const auto status=fs::symlink_status(value,ec);
        if(ec&&ec!=std::errc::no_such_file_or_directory)throw std::system_error(ec);
        if(fs::is_symlink(status))throw std::system_error(ELOOP,std::generic_category());
        if(end==std::string::npos)break;begin=end+1;
    }
}
bool Allows(const Device& d,const Metadata& m,unsigned mode) {
    const unsigned allowed=d.settings.uid==m.uid?m.owner:d.settings.gid==m.gid?m.group:m.other;
    return (allowed&mode)==mode;
}
s32 Existing(Device& d,const std::string& path,unsigned mode,Metadata*& result) {
    if(path.empty())return IPC_RESULT_INVALID;
    auto entry=d.entries.find(path);
    if(entry==d.entries.end())return IPC_RESULT_NOEXISTS;
    CheckPhysical(d,path);
    struct stat info{};
    if(::lstat(Physical(d,path).c_str(),&info))return Error(errno);
    if((entry->second.directory&&!S_ISDIR(info.st_mode))||(!entry->second.directory&&!S_ISREG(info.st_mode)))return IPC_RESULT_CORRUPT;
    if(!Allows(d,entry->second,mode))return IPC_RESULT_ACCESS;
    result=&entry->second;return 0;
}
void Persist(Device& d,const std::map<std::string,Metadata>& entries) {
    std::ostringstream text;
    text<<"mscharged-native-ios-fs 1\n"<<d.settings.title_id<<' '<<d.settings.uid<<' '<<d.settings.gid<<' '<<d.next_order<<'\n';
    for(const auto& [path,m]:entries)
        text<<std::quoted(path)<<' '<<m.uid<<' '<<m.gid<<' '<<unsigned(m.owner)<<' '<<unsigned(m.group)<<' '<<unsigned(m.other)<<' '<<unsigned(m.attr)<<' '<<m.directory<<' '<<m.order<<'\n';
    const auto bytes=text.str();const auto temporary=d.metadata.string()+".tmp";
    const int fd=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)throw std::system_error(errno,std::generic_category());
    std::size_t written{};int failure{};
    while(written<bytes.size()) {
        const auto result=::write(fd,bytes.data()+written,bytes.size()-written);
        if(result<0&&errno==EINTR)continue;
        if(result<=0){failure=result<0?errno:EIO;break;}
        written+=static_cast<std::size_t>(result);
    }
    if(!failure&&::fsync(fd))failure=errno;
    if(::close(fd)&&!failure)failure=errno;
    if(!failure&&::rename(temporary.c_str(),d.metadata.c_str()))failure=errno;
    if(failure){::unlink(temporary.c_str());throw std::system_error(failure,std::generic_category());}
}
std::vector<std::string> Children(const Device& d,const std::string& path) {
    std::vector<std::string> children;
    for(const auto& [entry,m]:d.entries)if(entry!="/"&&Parent(entry)==path)children.push_back(entry);
    std::sort(children.begin(),children.end(),[&](const auto& a,const auto& b){return d.entries.at(a).order<d.entries.at(b).order;});
    return children;
}
s32 Flush(Device& d) {
    for(const auto& [fd,handle]:d.handles)
        if(handle.kind==Kind::File&&(handle.mode&IPC_OPEN_WRITE)&&::fsync(handle.physical))return Error(errno);
    Persist(d,d.entries);return 0;
}
s32 Create(Device& d,const FileIoctl& fields,bool directory) {
    const auto path=Path(fields.path);
    if(path.empty()||path=="/"||fields.owner>3||fields.group>3||fields.other>3)return IPC_RESULT_INVALID;
    if(d.entries.count(path))return IPC_RESULT_EXISTS;
    Metadata* parent{};const auto status=Existing(d,Parent(path),IPC_OPEN_WRITE,parent);
    if(status<0)return status;if(!parent->directory)return IPC_RESULT_INVALID;
    CheckPhysical(d,path);
    if(directory) {if(::mkdir(Physical(d,path).c_str(),0700))return Error(errno);}
    else {
        const int fd=::open(Physical(d,path).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)return Error(errno);if(::close(fd))return Error(errno);
    }
    auto next=d.entries;
    next.emplace(path,Metadata{d.settings.uid,d.settings.gid,fields.owner,fields.group,fields.other,fields.attr,directory,d.next_order++});
    try {Persist(d,next);}catch(...){directory?::rmdir(Physical(d,path).c_str()) : ::unlink(Physical(d,path).c_str());throw;}
    d.entries.swap(next);return 0;
}
s32 Remove(Device& d,const std::string& path) {
    if(path.empty()||path=="/"||path==d.home)return IPC_RESULT_INVALID;
    Metadata *m{},*parent{};auto status=Existing(d,path,0,m);if(status<0)return status;
    status=Existing(d,Parent(path),IPC_OPEN_WRITE,parent);if(status<0)return status;
    for(const auto& [fd,handle]:d.handles)if(handle.kind==Kind::File&&handle.path==path)return IPC_RESULT_OPENFD;
    if(m->directory&&!Children(d,path).empty())return IPC_RESULT_NOTEMPTY;
    if((m->directory?::rmdir(Physical(d,path).c_str()) : ::unlink(Physical(d,path).c_str())))return Error(errno);
    auto next=d.entries;next.erase(path);Persist(d,next);d.entries.swap(next);return 0;
}
s32 Move(Device& d,const char* old_bytes,const char* new_bytes) {
    const auto old_path=Path(old_bytes),new_path=Path(new_bytes);
    if(old_path.empty()||new_path.empty()||old_path=="/"||new_path=="/"||old_path==d.home)return IPC_RESULT_INVALID;
    Metadata *source{},*parent{};auto status=Existing(d,old_path,0,source);if(status<0)return status;
    status=Existing(d,Parent(old_path),IPC_OPEN_WRITE,parent);if(status<0)return status;
    status=Existing(d,Parent(new_path),IPC_OPEN_WRITE,parent);if(status<0)return status;
    if(!parent->directory)return IPC_RESULT_INVALID;
    for(const auto& [fd,handle]:d.handles)
        if(handle.kind==Kind::File&&(handle.path==old_path||handle.path==new_path))return IPC_RESULT_OPENFD;
    if(old_path==new_path)return 0;
    if(source->directory)throw std::logic_error("Native IOS directory-rename subtree transport remains unqualified");
    CheckPhysical(d,new_path);
    auto next=d.entries;auto metadata=*source;next.erase(old_path);next[new_path]=metadata;
    if(::rename(Physical(d,old_path).c_str(),Physical(d,new_path).c_str()))return Error(errno);
    Persist(d,next);d.entries.swap(next);return 0;
}
s32 Ioctl(Device& d,Handle& handle,const IPCIoctlArgs& args) {
    if(handle.kind==Kind::File) {
        if(args.type!=GetFileStats||args.inSize!=0||!args.out||args.outSize<8)return IPC_RESULT_INVALID;
        struct stat info{};if(::fstat(handle.physical,&info))return Error(errno);
        const auto position=::lseek(handle.physical,0,SEEK_CUR);if(position<0)return Error(errno);
        if(info.st_size>UINT32_MAX||static_cast<u64>(position)>UINT32_MAX)return IPC_RESULT_MAXBLOCKS;
        const u32 fields[2]={static_cast<u32>(info.st_size),static_cast<u32>(position)};
        std::memcpy(args.out,fields,sizeof(fields));return 0;
    }
    if(handle.kind!=Kind::Fs)return IPC_RESULT_INVALID;
    if(args.type==Shutdown) {
        if(args.inSize||args.outSize)return IPC_RESULT_INVALID;return Flush(d);
    }
    if(args.type==CreateDir||args.type==CreateFile) {
        if(!args.in||args.inSize!=sizeof(FileIoctl)||args.outSize)return IPC_RESULT_INVALID;
        FileIoctl fields;std::memcpy(&fields,args.in,sizeof(fields));return Create(d,fields,args.type==CreateDir);
    }
    if(args.type==Rename) {
        if(!args.in||args.inSize!=PathBytes*2||args.outSize)return IPC_RESULT_INVALID;
        return Move(d,static_cast<const char*>(args.in),static_cast<const char*>(args.in)+PathBytes);
    }
    if(args.type==Delete) {
        if(!args.in||args.inSize!=PathBytes||args.outSize)return IPC_RESULT_INVALID;
        return Remove(d,Path(static_cast<const char*>(args.in)));
    }
    if(args.type==GetAttr) {
        if(!args.in||args.inSize!=PathBytes||!args.out||args.outSize!=sizeof(FileIoctl))return IPC_RESULT_INVALID;
        const auto path=Path(static_cast<const char*>(args.in));Metadata* metadata{};
        const auto result=Existing(d,path,IPC_OPEN_READ,metadata);if(result<0)return result;
        FileIoctl fields{};fields.uid=metadata->uid;fields.gid=metadata->gid;
        fields.owner=metadata->owner;fields.group=metadata->group;fields.other=metadata->other;fields.attr=metadata->attr;
        std::memcpy(fields.path,path.c_str(),path.size()+1);std::memcpy(args.out,&fields,sizeof(fields));return 0;
    }
    return IPC_RESULT_INVALID;
}
s32 Ioctlv(Device& d,Handle& handle,const IPCIoctlvArgs& args) {
    if(!args.vectors)return IPC_RESULT_INVALID;
    auto* v=args.vectors;
    if(handle.kind==Kind::Es) {
        if(args.type==GetTitleId&&args.inCount==0&&args.outCount==1&&v[0].base&&v[0].length==8) {
            std::memcpy(v[0].base,&d.settings.title_id,8);return 0;
        }
        if(args.type==GetDataDir&&args.inCount==1&&args.outCount==1&&v[0].base&&v[0].length==8&&v[1].base&&v[1].length==30) {
            u64 title{};std::memcpy(&title,v[0].base,8);if(title!=d.settings.title_id)return IPC_RESULT_NOEXISTS;
            std::memcpy(v[1].base,d.home.c_str(),d.home.size()+1);return 0;
        }
        return IPC_RESULT_INVALID;
    }
    if(handle.kind!=Kind::Fs||!v[0].base||v[0].length!=PathBytes)return IPC_RESULT_INVALID;
    const auto path=Path(static_cast<const char*>(v[0].base));Metadata* metadata{};
    const auto status=Existing(d,path,IPC_OPEN_READ,metadata);if(status<0)return status;
    if(!metadata->directory)return IPC_RESULT_INVALID;
    if(args.type==ReadDir) {
        const bool count_only=args.inCount==1&&args.outCount==1;
        const bool names=args.inCount==2&&args.outCount==2;
        if(!count_only&&!names)return IPC_RESULT_INVALID;
        auto* count=count_only?&v[1]:&v[3];
        if(!count->base||count->length!=4)return IPC_RESULT_INVALID;
        const auto children=Children(d,path);u32 available=static_cast<u32>(children.size());
        if(names) {
            if(!v[1].base||v[1].length!=4||!v[2].base)return IPC_RESULT_INVALID;
            u32 capacity{};std::memcpy(&capacity,v[1].base,4);
            if(static_cast<u64>(capacity)*NameBytes>v[2].length)return IPC_RESULT_INVALID;
            if(capacity<available)available=capacity;
            auto* output=static_cast<char*>(v[2].base);std::size_t offset{};
            for(std::size_t n=0;n<available;++n) {
                const auto name=children[n].substr(children[n].rfind('/')+1);
                std::memcpy(output+offset,name.c_str(),name.size()+1);offset+=name.size()+1;
            }
        }
        std::memcpy(count->base,&available,4);return 0;
    }
    if(args.type==GetUsage) {
        if(args.inCount!=1||args.outCount!=2||!v[1].base||v[1].length!=4||!v[2].base||v[2].length!=4)return IPC_RESULT_INVALID;
        u64 clusters{},inodes{};const auto prefix=path=="/"?"/":path+"/";
        for(const auto& [entry,m]:d.entries) {
            if(entry!=path&&entry.rfind(prefix,0)!=0)continue;
            ++inodes;
            if(!m.directory) {
                struct stat info{};if(::lstat(Physical(d,entry).c_str(),&info))return Error(errno);
                if(info.st_size<0)return IPC_RESULT_CORRUPT;
                clusters+=(static_cast<u64>(info.st_size)+ClusterBytes-1)/ClusterBytes;
            }
        }
        if(clusters>UINT32_MAX||inodes>UINT32_MAX)return IPC_RESULT_MAXBLOCKS;
        const u32 blocks=static_cast<u32>(clusters),files=static_cast<u32>(inodes);
        std::memcpy(v[1].base,&blocks,4);std::memcpy(v[2].base,&files,4);return 0;
    }
    return IPC_RESULT_INVALID;
}
bool Owns(void*,const char* path) {
    if(!path||path[0]!='/')return false;
    if(std::strcmp(path,"/dev/fs")==0||std::strcmp(path,"/dev/es")==0)return true;
    return std::strncmp(path,"/dev/",5)!=0;
}
s32 Execute(void* context,const IPCRequest& r) {
    auto& d=*static_cast<Device*>(context);std::lock_guard lock(d.mutex);
    if(!d.ready)throw std::logic_error("Native filesystem endpoint retired before its request");
    try {
        if(r.type==IPC_REQ_OPEN) {
            if(d.next_fd==INT32_MAX)return IPC_RESULT_MAXFD;
            if(r.open.mode<IPC_OPEN_NONE||r.open.mode>IPC_OPEN_RW)return IPC_RESULT_INVALID;
            Handle handle{};
            if(std::strcmp(r.open.path,"/dev/fs")==0)handle.kind=Kind::Fs;
            else if(std::strcmp(r.open.path,"/dev/es")==0)handle.kind=Kind::Es;
            else {
                if(std::count_if(d.handles.begin(),d.handles.end(),[](const auto& h){return h.second.kind!=Kind::Es;})>=16)return IPC_RESULT_MAXFD;
                handle.path=Path(r.open.path);Metadata* metadata{};
                auto result=Existing(d,handle.path,r.open.mode,metadata);if(result<0)return result;
                if(metadata->directory||r.open.mode==IPC_OPEN_NONE)return IPC_RESULT_INVALID;
                handle.kind=Kind::File;handle.mode=r.open.mode;
                const int access=r.open.mode==IPC_OPEN_READ?O_RDONLY:r.open.mode==IPC_OPEN_WRITE?O_WRONLY:O_RDWR;
                handle.physical=::open(Physical(d,handle.path).c_str(),access|O_CLOEXEC|O_NOFOLLOW);
                if(handle.physical<0)return Error(errno);
            }
            const auto fd=d.next_fd;
            try {d.handles.emplace(fd,handle);}catch(...){if(handle.physical>=0)::close(handle.physical);throw;}
            ++d.next_fd;return fd;
        }
        auto found=d.handles.find(r.fd);if(found==d.handles.end())return IPC_RESULT_INVALID;
        auto& h=found->second;
        if(r.type==IPC_REQ_CLOSE) {
            if(h.kind==Kind::File) {
                if((h.mode&IPC_OPEN_WRITE)&&::fsync(h.physical))return Error(errno);
                if(::close(h.physical))return Error(errno);
            }
            d.handles.erase(found);return 0;
        }
        if(r.type==IPC_REQ_IOCTL)return Ioctl(d,h,r.ioctl);
        if(r.type==IPC_REQ_IOCTLV)return Ioctlv(d,h,r.ioctlv);
        if(h.kind!=Kind::File)return IPC_RESULT_INVALID;
        if(r.type==IPC_REQ_READ||r.type==IPC_REQ_WRITE) {
            if((r.rw.length&&!r.rw.data)||r.rw.length>INT32_MAX)return IPC_RESULT_INVALID;
            if(!(h.mode&(r.type==IPC_REQ_READ?IPC_OPEN_READ:IPC_OPEN_WRITE)))return IPC_RESULT_ACCESS;
            ssize_t count;
            do {count=r.type==IPC_REQ_READ?::read(h.physical,r.rw.data,r.rw.length) : ::write(h.physical,r.rw.data,r.rw.length);}while(count<0&&errno==EINTR);
            return count<0?Error(errno):static_cast<s32>(count);
        }
        if(r.type==IPC_REQ_SEEK) {
            if(r.seek.mode<IPC_SEEK_BEG||r.seek.mode>IPC_SEEK_END)return IPC_RESULT_INVALID;
            struct stat info{};if(::fstat(h.physical,&info))return Error(errno);
            const auto current=::lseek(h.physical,0,SEEK_CUR);if(current<0)return Error(errno);
            const s64 base=r.seek.mode==IPC_SEEK_BEG?0:r.seek.mode==IPC_SEEK_CUR?current:info.st_size;
            const auto position=base+r.seek.offset;
            if(position<0||position>info.st_size||position>INT32_MAX)return IPC_RESULT_INVALID;
            const auto result=::lseek(h.physical,position,SEEK_SET);return result<0?Error(errno):static_cast<s32>(result);
        }
        return IPC_RESULT_INVALID;
    }catch(const std::system_error& error){return Error(error.code().value());}
}
void Load(Device& d) {
    std::ifstream input(d.metadata);
    std::string signature;unsigned version{};input>>signature>>version;
    u64 title{},next{};u32 uid{};unsigned gid{};
    input>>title>>uid>>gid>>next;
    if(!input||signature!="mscharged-native-ios-fs"||version!=1||title!=d.settings.title_id||uid!=d.settings.uid||gid!=d.settings.gid||!next)
        throw std::runtime_error("Native IOS persistent metadata/identity is invalid");
    d.next_order=next;std::string path;
    while(input>>std::quoted(path)) {
        Metadata m;unsigned group{},owner_mode{},group_mode{},other_mode{},attr{},directory{};
        input>>m.uid>>group>>owner_mode>>group_mode>>other_mode>>attr>>directory>>m.order;
        if(!input||Path(path.c_str())!=path||group>UINT16_MAX||owner_mode>3||group_mode>3||other_mode>3||attr>255||directory>1||!m.order||m.order>=next)
            throw std::runtime_error("Native IOS persistent metadata record is invalid");
        m.gid=group;m.owner=owner_mode;m.group=group_mode;m.other=other_mode;m.attr=attr;m.directory=directory;
        if(!d.entries.emplace(path,m).second)throw std::runtime_error("Duplicate native IOS persistent metadata path");
        CheckPhysical(d,path);struct stat info{};
        if(::lstat(Physical(d,path).c_str(),&info)||(m.directory?!S_ISDIR(info.st_mode):!S_ISREG(info.st_mode)))
            throw std::runtime_error("Native IOS catalog does not match actual physical backing");
    }
    if(!input.eof()||!d.entries.count("/")||!d.entries.count(d.home))throw std::runtime_error("Native IOS persistent metadata is incomplete");
}
}
namespace mscharged::platform {
void InitializeNativeFilesystem(NativeFilesystemSettings settings) {
    if(settings.root.empty()||!settings.title_id)throw std::invalid_argument("Native filesystem requires explicit backing and actual title identity");
    auto& d=State();
    {
        std::lock_guard lock(d.mutex);
        if(d.ready)throw std::logic_error("Native filesystem is already installed");
        d.owner=std::this_thread::get_id();d.settings=std::move(settings);d.entries.clear();d.handles.clear();
        fs::create_directories(d.settings.root);d.settings.root=fs::canonical(d.settings.root);
        d.data=d.settings.root/"data";d.metadata=d.settings.root/"metadata.txt";
        std::ostringstream home;home<<"/title/"<<std::hex<<std::setw(8)<<std::setfill('0')<<u32(d.settings.title_id>>32)<<'/'<<std::setw(8)<<u32(d.settings.title_id)<<"/data";d.home=home.str();
        if(fs::exists(d.metadata))Load(d);
        else {
            if(fs::exists(d.data)&&!fs::is_empty(d.data))throw std::runtime_error("Native IOS refuses backing without its persistent ownership metadata");
            fs::create_directories(d.data);d.next_order=1;
            const std::array<std::string,6> paths={"/","/tmp","/title",Parent(Parent(d.home)),Parent(d.home),d.home};
            for(const auto& path:paths) {
                fs::create_directories(Physical(d,path));
                const bool owned=path==d.home;
                d.entries.emplace(path,Metadata{owned?d.settings.uid:0,owned?d.settings.gid:u16(0),3,u8(owned?0:3),u8(owned?0:path=="/tmp"?3:1),0,true,d.next_order++});
            }
            Persist(d,d.entries);
        }
        d.ready=true;
    }
    try {d.lease=RegisterNativeIOSDevice({&d,Owns,Execute});}
    catch(...){std::lock_guard lock(d.mutex);d.ready=false;d.owner={};throw;}
}
void ShutdownNativeFilesystem() {
    auto& d=State();
    {
        std::lock_guard lock(d.mutex);
        if(!d.ready)return;
        if(d.owner!=std::this_thread::get_id())throw std::logic_error("Filesystem retirement requires its actual hardware owner");
    }
    // Bus retirement checks real queued/active callbacks before physical close.
    // No source/device callbacks execute after the routing lease is retired.
    UnregisterNativeIOSDevice(d.lease);
    std::lock_guard lock(d.mutex);
    int failure{};
    for(const auto& [fd,h]:d.handles)if(h.physical>=0) {
        if((h.mode&IPC_OPEN_WRITE)&&::fsync(h.physical)&&!failure)failure=errno;
        if(::close(h.physical)&&!failure)failure=errno;
    }
    d.handles.clear();d.entries.clear();d.ready=false;d.lease={};d.owner={};
    if(failure)throw std::system_error(failure,std::generic_category());
}
}

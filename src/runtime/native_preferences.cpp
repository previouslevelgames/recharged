#include "runtime/native_preferences.h"
#include "Game/DB/SaveStateSteps.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>
#include <system_error>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mscharged
{
namespace
{
using Values=resources::NativePreferencesValues;
using Bytes=resources::NativePreferencesBytes;
void Check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
struct FileCloser { void operator()(FILE* f)const noexcept { if(f)std::fclose(f); } };
using File=std::unique_ptr<FILE,FileCloser>;
File OpenRead(const std::filesystem::path& path)
{
#ifdef _WIN32
    return File(_wfopen(path.c_str(),L"rb"));
#else
    return File(std::fopen(path.c_str(),"rb"));
#endif
}
std::optional<Bytes> ReadFile(const std::filesystem::path& path)
{
    std::error_code error;const auto status=std::filesystem::symlink_status(path,error);
    if(error==std::errc::no_such_file_or_directory)return {};
    if(error)throw std::filesystem::filesystem_error("Read native preferences status",path,error);
    if(!std::filesystem::exists(status))return {};
    Check(std::filesystem::is_regular_file(status),"Native preferences path is not a regular file");
    auto file=OpenRead(path);Check(bool(file),"Cannot open native preferences file");
    Bytes bytes{};const auto count=std::fread(bytes.data(),1,bytes.size(),file.get());
    Check(count==bytes.size()&&!std::ferror(file.get()),"Native preferences read is incomplete");
    Check(std::fgetc(file.get())==EOF&&!std::ferror(file.get()),"Native preferences file has trailing data");
    return bytes;
}
struct TemporaryFile
{
    std::filesystem::path path;
    bool owns=false;
    ~TemporaryFile(){ if(owns){std::error_code error;std::filesystem::remove(path,error);} }
    void Remove()
    {
        if(!owns)return;
        std::filesystem::remove(path);owns=false;
    }
};
void StageFile(const std::filesystem::path& path,const Bytes& bytes,TemporaryFile& temp)
{
    static std::atomic<std::uint64_t> counter=0;
    File file;
    for(unsigned attempt=0;attempt<32&&!file;++attempt)
    {
        temp.path=path;temp.path+=".pending-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(counter.fetch_add(1));
#ifdef _WIN32
        file.reset(_wfopen(temp.path.c_str(),L"wbx"));
#else
        const int fd=open(temp.path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd>=0){file.reset(fdopen(fd,"wb"));if(!file){close(fd);std::error_code e;std::filesystem::remove(temp.path,e);}}
#endif
        if(!file&&errno!=EEXIST)break;
    }
    Check(bool(file),"Cannot create native preferences temporary file");temp.owns=true;
    Check(std::fwrite(bytes.data(),1,bytes.size(),file.get())==bytes.size(),"Native preferences write is incomplete");
    Check(std::fflush(file.get())==0,"Cannot flush native preferences file");
#ifdef _WIN32
    Check(_commit(_fileno(file.get()))==0,"Cannot synchronize native preferences file");
#else
    Check(fsync(fileno(file.get()))==0,"Cannot synchronize native preferences file");
#endif
    Check(std::fclose(file.release())==0,"Cannot close native preferences file");
}
struct Work
{
    std::optional<Bytes> bytes;
    Values::Handle values;
    std::unique_ptr<TemporaryFile> temporary;
    std::exception_ptr error;
};
}
struct NativePreferences::Implementation
{
    const std::thread::id thread=std::this_thread::get_id();
    const std::filesystem::path path;
    Values::Handle current=std::make_shared<const Values>(resources::DefaultNativePreferences());
    Values::Handle candidate;
    NativePreferencesStatus status;
    bool saving=false,observed=false,reset_paused=false;
    unsigned banner_mode=0;
    std::optional<Bytes> baseline;
    std::future<Work> work;
    std::unique_ptr<TemporaryFile> temporary;
    std::exception_ptr failure;
    explicit Implementation(std::filesystem::path p):path(std::move(p))
    {
        Check(path.is_absolute()&&path.has_filename()&&path.filename()!="."&&path.filename()!="..","Native preferences path must be an absolute file path");
        status.save_enabled=SaveStateInitiallyEnabled;
    }
    void Thread()const { Check(thread==std::this_thread::get_id(),"Native preferences used from a foreign thread"); }
    bool Pending()const { return status.state==NativePreferencesState::DirectoryPending||status.state==NativePreferencesState::FilePending; }
    void CanStart()const
    {
        Thread();Check(!Pending(),"Native preferences already has pending work");
        Check(status.state!=NativePreferencesState::Failed,"Resolve failed native preferences with Cancel before creating a new owner");
        Check(SaveStateCanStart(status.save_enabled,[]{return false;}),"Native preferences saving was cancelled");
    }
    void Begin(bool save,Values::Handle values)
    {
        CanStart();auto future=std::async(std::launch::async,[p=path]{
            Check(std::filesystem::is_directory(p.parent_path()),"Native preferences parent directory is unavailable");return Work{};
        });
        saving=save;candidate=std::move(values);work=std::move(future);
        status.state=NativePreferencesState::DirectoryPending;status.host_pending=true;failure={};
    }
    void Drain()
    {
        if(work.valid())
        {
            Work result;
            try{result=work.get();}catch(...){/* cancellation still drains failed worker */}
            if(result.temporary)temporary=std::move(result.temporary);
        }
        if(temporary){temporary->Remove();temporary.reset();}
    }
    ~Implementation()
    {
        // No worker borrows this object. future::get joins completion before
        // current/candidate/path are released, including during exception unwind.
        try{Drain();}catch(...){/* TemporaryFile destructor retries cleanup */}
    }
};
NativePreferences::NativePreferences(std::filesystem::path path):impl_(std::make_unique<Implementation>(std::move(path))){}
NativePreferences::~NativePreferences()=default;
void NativePreferences::StartLoad()
{
    impl_->CanStart();
    // The native file may be reloaded, but original NormalSaveLoaded remains
    // false. This is a fresh scoped operation, not an original StartLoad bypass.
    Check(SaveStateCanLoad(impl_->status.save_enabled,false,[]{return false;}),"Native preferences load is disabled");
    impl_->Begin(false,{});
}
void NativePreferences::StartSave(const Values& values)
{
    impl_->CanStart();Check(impl_->observed,"Load native preferences before saving");
    resources::ValidateNativePreferences(values);
    auto retained=std::make_shared<const Values>(values);impl_->Begin(true,std::move(retained));
}
void NativePreferences::Poll()
{
    auto& p=*impl_;p.Thread();if(!p.Pending())return;
    if(p.work.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
    try
    {
        if(p.status.state==NativePreferencesState::DirectoryPending)++p.status.directory_callbacks;
        else ++p.status.file_callbacks;
        auto result=p.work.get();
        if(result.temporary)p.temporary=std::move(result.temporary);
        if(result.error)std::rethrow_exception(result.error);
        if(p.status.state==NativePreferencesState::DirectoryPending)
        {
            SaveStateDirectory(p.saving?1:0,p.banner_mode,p.status.in_operation);
            // This callback admits only typed preference I/O. It does not open
            // a Wii banner or report that those original operations completed.
            if(p.saving)
            {
                const auto bytes=resources::EncodeNativePreferences(*p.candidate);
                p.work=std::async(std::launch::async,[path=p.path,bytes]{
                    Work w;w.bytes=bytes;w.temporary=std::make_unique<TemporaryFile>();
                    try{StageFile(path,bytes,*w.temporary);}catch(...){w.error=std::current_exception();}
                    return w;
                });
            }
            else p.work=std::async(std::launch::async,[path=p.path]{
                Work w;w.bytes=ReadFile(path);
                if(w.bytes)w.values=std::make_shared<const Values>(resources::DecodeNativePreferences(*w.bytes));
                return w;
            });
            p.status.state=NativePreferencesState::FilePending;return;
        }
        if(p.saving)
        {
            // All allocations/validation precede rename and no-throw publication.
            // This exact-byte optimistic guard detects edits made since Load.
            Check(ReadFile(p.path)==p.baseline,"Native preferences changed externally; reload using a new owner");
            std::filesystem::rename(p.temporary->path,p.path);
            p.temporary->owns=false;p.temporary.reset();
            p.current=std::move(p.candidate);p.baseline=result.bytes;p.observed=true;
            p.status.preferences_loaded=true;p.status.state=NativePreferencesState::Ready;
        }
        else
        {
            p.baseline=result.bytes;p.observed=true;
            if(result.values)p.current=std::move(result.values);
            p.status.preferences_loaded=result.bytes.has_value();
            p.status.state=result.bytes?NativePreferencesState::Ready:NativePreferencesState::Missing;
        }
        SaveStateFinish(p.status.in_operation);p.status.host_pending=false;
    }
    catch(...)
    {
        p.failure=std::current_exception();p.status.state=NativePreferencesState::Failed;
        p.status.save_error=true;p.status.host_pending=false;p.candidate.reset();
        // Keep a failed cleanup owner so a later explicit Cancel can retry after
        // external directory permissions are restored.
        try{if(p.temporary){p.temporary->Remove();p.temporary.reset();}}catch(...){}
        // Do not execute FinishOperation on failure. A begun operation remains
        // busy until explicit Cancel; directory failures never begin one.
        throw;
    }
}
void NativePreferences::Cancel()
{
    auto& p=*impl_;p.Thread();
    try{p.Drain();}catch(...){p.failure=std::current_exception();p.status.state=NativePreferencesState::Failed;p.status.host_pending=false;p.status.save_error=true;throw;}
    SaveStateCancel(p.reset_paused,p.status.save_enabled,p.status.in_operation);
    p.candidate.reset();p.failure={};p.status.host_pending=false;p.status.state=NativePreferencesState::Cancelled;
}
NativePreferences::Handle NativePreferences::Current()const { impl_->Thread();return impl_->current; }
NativePreferencesStatus NativePreferences::Status()const { impl_->Thread();return impl_->status; }
bool NativePreferences::DepartureBlocked(NativePreferencesScope scope)const
{
    impl_->Thread();Check(scope==NativePreferencesScope::NativePreferences,"Original game-save authority is unavailable");
    Check(impl_->status.state!=NativePreferencesState::Failed,"Failed native preferences require an explicit decision");
    return SaveStateDepartureBlocked(impl_->status.save_enabled,impl_->status.in_operation);
}
void NativePreferences::RethrowFailure()const { impl_->Thread();if(impl_->failure)std::rethrow_exception(impl_->failure); }
}

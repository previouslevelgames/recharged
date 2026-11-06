#pragma once

// Physical storage only. IOS names, metadata, permissions and callback delivery
// remain in filesystem_device.cpp. A Windows catalog never imports POSIX names.
#include "platform/path.h"
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <filesystem>
#include <limits>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mscharged::platform::filesystem_detail {
namespace fs = std::filesystem;
struct Info { std::int64_t size{}; bool directory{},regular{}; };

// Security software may hold a file while it scans it, mostly one just written,
// so creating or renaming that file fails for a moment with "access denied" or
// a sharing violation (#123). Windows retries those for about a second:
// `attempt` returns whether it worked and sets errno when not; `wait` sleeps
// the given milliseconds. errno keeps the last failure.
inline constexpr std::array<unsigned,8> RefusedRetryDelays{10,20,40,80,160,250,250,250};
template<class Attempt,class Wait> bool RetryRefused(Attempt&& attempt,Wait&& wait) {
    for(const auto delay:RefusedRetryDelays) {
        if(attempt())return true;
        if(errno!=EACCES)return false;
        wait(delay);
    }
    return attempt();
}
#ifdef _WIN32
using File = HANDLE;
inline File InvalidFile() { return INVALID_HANDLE_VALUE; }
inline bool Valid(File file) { return file != INVALID_HANDLE_VALUE; }
inline constexpr const char* CatalogSignature = "mscharged-native-ios-fs-winhex";
inline std::wstring WindowsPath(const fs::path& path) {
    auto value=fs::absolute(path).lexically_normal();value.make_preferred();
    const auto text=value.native();
    if(text.rfind(L"\\\\?\\",0)==0)return text;
    if(text.rfind(L"\\\\",0)==0)return L"\\\\?\\UNC\\"+text.substr(2);
    return L"\\\\?\\"+text;
}
inline int WindowsErrno(DWORD error) {
    switch(error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return ENOENT;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT: case ERROR_PRIVILEGE_NOT_HELD: return EACCES;
    case ERROR_INVALID_NAME: case ERROR_INVALID_PARAMETER: case ERROR_INVALID_HANDLE:
    case ERROR_DIRECTORY: case ERROR_BAD_PATHNAME: return EINVAL;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_CANT_RESOLVE_FILENAME: case ERROR_REPARSE_TAG_INVALID:
    case ERROR_REPARSE_TAG_MISMATCH: return ELOOP;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    default: return EIO;
    }
}
inline int Fail() { errno=WindowsErrno(GetLastError());return -1; }
// The system's own code of the last failure, for messages: errno alone folds
// several Windows errors into one (sharing violation and access denied both
// read "permission denied").
inline std::string ErrorDetail() {
    const auto code=GetLastError();
    return code?" (Win32 error "+std::to_string(code)+")":std::string();
}
inline void ThrowLast(const fs::path& path={}) {
    const auto code=GetLastError();const auto error=WindowsErrno(code);
    throw std::system_error(error,std::generic_category(),"Win32 error "+std::to_string(code)+" at "+::mscharged::PathUtf8(path));
}
inline void RejectReparse() { throw std::system_error(ELOOP,std::generic_category()); }
inline fs::path Component(const std::string& bytes) {
    // Only canonical lowercase hex is emitted: case-insensitive host comparison
    // cannot collapse different console bytes, including non-UTF8 names.
    static constexpr char digits[]="0123456789abcdef";
    std::string name="n";name.reserve(1+bytes.size()*2);
    for(const unsigned char byte:bytes){name+=digits[byte>>4];name+=digits[byte&15];}
    return fs::path(name);
}

// Hold every existing ancestor against host rename/delete while the actual
// operation is performed. OPEN_REPARSE_POINT checks the object, never its target.
class PathGuard {
    std::vector<HANDLE> held_;
    void Hold(const fs::path& path,bool missing) {
        const auto file=CreateFileW(WindowsPath(path).c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file==INVALID_HANDLE_VALUE) {
            const auto error=GetLastError();
            if(missing&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND))return;
            SetLastError(error);ThrowLast(path);
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(file,&info)){const auto error=GetLastError();CloseHandle(file);SetLastError(error);ThrowLast(path);}
        SetLastError(ERROR_SUCCESS);const auto type=GetFileType(file);const auto type_error=GetLastError();
        if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||type!=FILE_TYPE_DISK){
            CloseHandle(file);throw std::system_error(ELOOP,std::generic_category(),
                "Rejected reparse/non-disk path: "+::mscharged::PathUtf8(path)+
                " (attributes="+std::to_string(info.dwFileAttributes)+", type="+std::to_string(type)+", error="+std::to_string(type_error)+")");
        }
        try{held_.push_back(file);}catch(...){CloseHandle(file);throw;}
    }
public:
    PathGuard(const fs::path& path,bool include_leaf=false) {
        const auto absolute=fs::absolute(path).lexically_normal();
        auto value=absolute.root_path();
        try {
            Hold(value,false);
            const auto relative=absolute.relative_path();
            for(auto it=relative.begin();it!=relative.end();++it) {
                auto next=it;++next;
                if(!include_leaf&&next==relative.end())break;
                value/=*it;Hold(value,true);
            }
        }catch(...){for(auto file:held_)CloseHandle(file);throw;}
    }
    PathGuard(const PathGuard&)=delete;
    ~PathGuard(){for(auto file:held_)CloseHandle(file);}
};
inline void CheckPath(const fs::path& path) { PathGuard guard(path,true); }
inline File Open(const fs::path& path,unsigned mode,bool create=false,bool truncate=false) {
    PathGuard guard(path);
    const DWORD access=(mode&1?GENERIC_READ:0)|(mode&2?GENERIC_WRITE:0);
    const auto file=CreateFileW(WindowsPath(path).c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,create?CREATE_NEW:OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE){Fail();return InvalidFile();}
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info)) {const auto error=GetLastError();CloseHandle(file);SetLastError(error);Fail();return InvalidFile();}
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||GetFileType(file)!=FILE_TYPE_DISK) {
        CloseHandle(file);errno=ELOOP;return InvalidFile();
    }
    // OPEN_EXISTING avoids truncating an unchecked reparse point. The catalog
    // staging file may be absent; its caller uses create first in that case.
    if(truncate) {
        LARGE_INTEGER zero{};
        if(!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)||!SetEndOfFile(file)) {
            const auto error=GetLastError();CloseHandle(file);SetLastError(error);Fail();return InvalidFile();
        }
    }
    return file;
}
inline int Close(File file) {return CloseHandle(file)?0:Fail();}
inline int Sync(File file) {return FlushFileBuffers(file)?0:Fail();}
inline std::int64_t Read(File file,void* bytes,std::size_t count) {
    if(count>std::numeric_limits<DWORD>::max()){errno=EINVAL;return -1;}DWORD done{};
    return ReadFile(file,bytes,static_cast<DWORD>(count),&done,nullptr)?static_cast<std::int64_t>(done):Fail();
}
inline std::int64_t Write(File file,const void* bytes,std::size_t count) {
    if(count>std::numeric_limits<DWORD>::max()){errno=EINVAL;return -1;}DWORD done{};
    return WriteFile(file,bytes,static_cast<DWORD>(count),&done,nullptr)?static_cast<std::int64_t>(done):Fail();
}
inline std::int64_t Seek(File file,std::int64_t offset,int mode) {
    LARGE_INTEGER distance{},result{};distance.QuadPart=offset;
    return SetFilePointerEx(file,distance,&result,mode==SEEK_SET?FILE_BEGIN:mode==SEEK_CUR?FILE_CURRENT:FILE_END)?result.QuadPart:Fail();
}
// Separate name: on Windows File is HANDLE (void*), which libc++ would try to
// turn into an fs::path while resolving an overload.
inline int StatFile(File file,Info& out) {
    BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file,&info))return Fail();
    if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){errno=ELOOP;return -1;}
    const auto size=(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    if(size>INT64_MAX){errno=EOVERFLOW;return -1;}
    out={static_cast<std::int64_t>(size),bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY),
        !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&GetFileType(file)==FILE_TYPE_DISK};return 0;
}
inline int Stat(const fs::path& path,Info& out) {
    PathGuard guard(path);
    const auto file=CreateFileW(WindowsPath(path).c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return Fail();
    const auto result=StatFile(file,out);const auto error=errno;
    if(!CloseHandle(file)&&!result)return Fail();errno=error;return result;
}
inline int MakeDir(const fs::path& path) {PathGuard guard(path);return CreateDirectoryW(WindowsPath(path).c_str(),nullptr)?0:Fail();}
inline int Remove(const fs::path& path,bool directory) {
    PathGuard guard(path);CheckPath(path);
    return (directory?RemoveDirectoryW(WindowsPath(path).c_str()):DeleteFileW(WindowsPath(path).c_str()))?0:Fail();
}
inline void WaitRefused(unsigned milliseconds) { Sleep(milliseconds); }
inline int Replace(const fs::path& old_path,const fs::path& new_path) {
    PathGuard old_guard(old_path),new_guard(new_path);CheckPath(old_path);CheckPath(new_path);
    const auto from=WindowsPath(old_path),to=WindowsPath(new_path);
    return RetryRefused([&]{
        if(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
        Fail();return false;
    },WaitRefused)?0:-1;
}
inline fs::path PrepareRoot(const fs::path& root) {
    const auto result=fs::absolute(root).lexically_normal();
    auto value=result.root_path();CheckPath(value);
    for(const auto& component:result.relative_path()) {
        value/=component;
        Info info{};
        if(Stat(value,info)) {
            if(errno!=ENOENT)throw std::system_error(errno,std::generic_category(),"PrepareRoot Stat: "+::mscharged::PathUtf8(value));
            if(MakeDir(value)&&errno!=EEXIST)throw std::system_error(errno,std::generic_category(),"PrepareRoot MakeDir: "+::mscharged::PathUtf8(value));
            if(Stat(value,info))throw std::system_error(errno,std::generic_category());
        }
        if(!info.directory)throw std::system_error(ENOTDIR,std::generic_category());
    }
    return result;
}
inline void MakeDirs(const fs::path& path){(void)PrepareRoot(path);}
inline File OpenStaging(const fs::path& path) {
    auto file=InvalidFile();
    RetryRefused([&]{
        file=Open(path,2,false,true);
        if(!Valid(file)&&errno==ENOENT)file=Open(path,2,true);
        return Valid(file);
    },WaitRefused);
    return file;
}
inline bool Exists(const fs::path& path) {
    Info info{};if(!Stat(path,info))return true;
    if(errno==ENOENT)return false;
    throw std::system_error(errno,std::generic_category());
}
// A folder's entries, without "." and "..".
inline std::vector<fs::path> Children(const fs::path& path) {
    PathGuard guard(path,true);WIN32_FIND_DATAW data{};std::vector<fs::path> children;
    const auto find=FindFirstFileW(WindowsPath(path/L"*").c_str(),&data);
    if(find==INVALID_HANDLE_VALUE) {
        if(GetLastError()==ERROR_FILE_NOT_FOUND)return children;ThrowLast(path);
    }
    try {
        do {
            if(std::wcscmp(data.cFileName,L".")&&std::wcscmp(data.cFileName,L".."))children.push_back(path/data.cFileName);
        }while(FindNextFileW(find,&data));
    }catch(...){FindClose(find);throw;}
    const auto error=GetLastError();FindClose(find);
    if(error!=ERROR_NO_MORE_FILES){SetLastError(error);ThrowLast(path);}
    return children;
}
inline std::string ReadCatalog(const fs::path& path) {
    const auto file=Open(path,1);if(!Valid(file))throw std::system_error(errno,std::generic_category());
    std::string text;
    try {
        char buffer[4096];
        for(;;){const auto count=Read(file,buffer,sizeof(buffer));if(count<0)throw std::system_error(errno,std::generic_category());if(!count)break;text.append(buffer,static_cast<std::size_t>(count));}
    }catch(...){Close(file);throw;}
    if(Close(file))throw std::system_error(errno,std::generic_category());return text;
}
inline int ErrorNumber(const std::error_code& error) {
    return error.category()==std::system_category()?WindowsErrno(static_cast<DWORD>(error.value())):error.value();
}
#else
using File=int;
inline File InvalidFile(){return -1;}
inline bool Valid(File file){return file>=0;}
inline constexpr const char* CatalogSignature="mscharged-native-ios-fs";
inline fs::path Component(const std::string& bytes){return fs::path(bytes);}
inline File Open(const fs::path& path,unsigned mode,bool create=false,bool truncate=false) {
    const int access=mode==1?O_RDONLY:mode==2?O_WRONLY:O_RDWR;
    return ::open(path.c_str(),access|O_CLOEXEC|O_NOFOLLOW|(create?O_CREAT|O_EXCL:0)|(truncate?O_TRUNC:0),0600);
}
inline int Close(File file){return ::close(file);}
inline int Sync(File file){return ::fsync(file);}
inline std::int64_t Read(File file,void* bytes,std::size_t size){return ::read(file,bytes,size);}
inline std::int64_t Write(File file,const void* bytes,std::size_t size){return ::write(file,bytes,size);}
inline std::int64_t Seek(File file,std::int64_t offset,int mode){return ::lseek(file,offset,mode);}
inline int StatFile(File file,Info& out){struct stat info{};if(::fstat(file,&info))return -1;out={info.st_size,bool(S_ISDIR(info.st_mode)),bool(S_ISREG(info.st_mode))};return 0;}
inline int Stat(const fs::path& path,Info& out){struct stat info{};if(::lstat(path.c_str(),&info))return -1;out={info.st_size,bool(S_ISDIR(info.st_mode)),bool(S_ISREG(info.st_mode))};return 0;}
inline int MakeDir(const fs::path& path){return ::mkdir(path.c_str(),0700);}
inline int Remove(const fs::path& path,bool directory){return directory?::rmdir(path.c_str()): ::unlink(path.c_str());}
inline int Replace(const fs::path& old_path,const fs::path& new_path){return ::rename(old_path.c_str(),new_path.c_str());}
inline fs::path PrepareRoot(const fs::path& root){fs::create_directories(root);return fs::canonical(root);}
inline void MakeDirs(const fs::path& path){fs::create_directories(path);}
inline File OpenStaging(const fs::path& path){return ::open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);}
inline bool Exists(const fs::path& path){return fs::exists(path);}
inline std::vector<fs::path> Children(const fs::path& path) {
    std::vector<fs::path> children;
    for(const auto& entry:fs::directory_iterator(path))children.push_back(entry.path());
    return children;
}
inline int ErrorNumber(const std::error_code& error){return error.value();}
inline std::string ErrorDetail(){return {};}
#endif

// A file that could not be written: its path, the system's own code (detail,
// from ErrorDetail) and, when Windows still refuses after the retries, what
// usually causes it (#123). code() stays the errno value.
class WriteError : public std::system_error {
public:
    WriteError(int error,const fs::path& path,const std::string& detail)
        :std::system_error(error,std::generic_category()),
         message_("Cannot write "+::mscharged::PathUtf8(path)+detail+": "+code().message()+Hint(error)) {}
    const char* what() const noexcept override { return message_.c_str(); }

private:
    static std::string Hint([[maybe_unused]] int error) {
#ifdef _WIN32
        if(error==EACCES)
            return ". Security software, such as Controlled folder access in Windows Security or an "
                   "antivirus, may block recharged.exe in this folder: allow recharged.exe there, or move "
                   "the Recharged folder elsewhere, for example to C:\\Games.";
#endif
        return {};
    }
    std::string message_;
};

// A save folder (backing) without its catalog (metadata.txt) is what a first
// start or an import leaves when writing the catalog failed: its folders are
// there, the catalog is not (#123). With only empty folders nothing is lost and
// it is set up again; files without a catalog are never taken over.

// Whether `path` holds anything but empty folders: a file, a link or reparse
// point (never followed), or a file in its place. A missing path holds
// nothing; a tree deeper than any Wii path counts as holding files.
inline bool HoldsFiles(const fs::path& path,int depth=0) {
    Info info{};
    if(Stat(path,info)) {
        if(errno==ENOENT)return false;
        if(errno==ELOOP)return true;
        throw std::system_error(errno,std::generic_category(),::mscharged::PathUtf8(path));
    }
    if(!info.directory||depth>16)return true;
    for(const auto& child:Children(path))if(HoldsFiles(child,depth+1))return true;
    return false;
}
// Removes `path` and the folders in it, deepest first. Only empty folders
// are removed: a file that appears meanwhile makes this fail instead.
inline void RemoveEmptyFolders(const fs::path& path,int depth=0) {
    Info info{};
    if(Stat(path,info)) {
        if(errno==ENOENT)return;
        throw std::system_error(errno,std::generic_category(),::mscharged::PathUtf8(path));
    }
    if(!info.directory||depth>16)throw std::system_error(ENOTEMPTY,std::generic_category(),::mscharged::PathUtf8(path));
    for(const auto& child:Children(path))RemoveEmptyFolders(child,depth+1);
    if(Remove(path,true))throw std::system_error(errno,std::generic_category(),::mscharged::PathUtf8(path)+ErrorDetail());
}
// The error for a save folder that holds files but no catalog.
inline std::string NoCatalogMessage(const fs::path& root) {
    return "The save folder "+::mscharged::PathUtf8(root)+" has files but no catalog (metadata.txt). "
           "Move that folder elsewhere to start with new saves.";
}
} // namespace mscharged::platform::filesystem_detail

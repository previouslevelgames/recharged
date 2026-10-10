# Cross-compile for 64-bit Windows with LLVM-MinGW (UCRT, libc++) from Linux or
# macOS. Point LLVM_MINGW_ROOT (cache or environment) at the extracted
# toolchain, e.g. llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT LLVM_MINGW_ROOT AND DEFINED ENV{LLVM_MINGW_ROOT})
    set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
endif()
if(NOT LLVM_MINGW_ROOT)
    message(FATAL_ERROR "Set LLVM_MINGW_ROOT to the LLVM-MinGW toolchain directory")
endif()
set(LLVM_MINGW_ROOT "${LLVM_MINGW_ROOT}" CACHE PATH "LLVM-MinGW toolchain directory")
# Compiler checks run this file again in try_compile projects; pass the root on.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT)
set(_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER "${LLVM_MINGW_ROOT}/bin/${_triple}-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/${_triple}-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW_ROOT}/bin/${_triple}-windres")
set(CMAKE_DLLTOOL "${LLVM_MINGW_ROOT}/bin/${_triple}-dlltool")
set(CMAKE_AR "${LLVM_MINGW_ROOT}/bin/llvm-ar")
set(CMAKE_RANLIB "${LLVM_MINGW_ROOT}/bin/llvm-ranlib")
set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/${_triple}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Rust (nod) for the same ABI.
set(Rust_CARGO_TARGET x86_64-pc-windows-gnullvm)
set(CMAKE_CROSSCOMPILING_EMULATOR wine)

# On case-sensitive hosts, Windows sources spell headers with capitals that the
# MinGW headers (all lower case) do not have. A folder of symlinks fills the gap.
set(_shims "${LLVM_MINGW_ROOT}-case-shims")
foreach(_header IN ITEMS Windows.h WinSock2.h WS2tcpip.h ShlObj.h Shlwapi.h ShellScalingApi.h
        VersionHelpers.h Psapi.h DbgHelp.h TlHelp32.h WinUser.h Unknwn.h Objbase.h WinError.h
        KnownFolders.h ShObjIdl.h ShlGuid.h Mmsystem.h MMSystem.h WinBase.h
        Wbemidl.h WbemCli.h OleAuto.h ComDef.h Comdef.h SetupAPI.h Dbt.h Hidsdi.h Hidpi.h
        CfgMgr32.h Cfgmgr32.h WinIoCtl.h Xinput.h XInput.h Dxgi.h D3D11.h DbgEng.h)
    string(TOLOWER "${_header}" _lower)
    if(EXISTS "${CMAKE_FIND_ROOT_PATH}/include/${_lower}" AND NOT EXISTS "${_shims}/${_header}")
        file(MAKE_DIRECTORY "${_shims}")
        file(CREATE_LINK "${CMAKE_FIND_ROOT_PATH}/include/${_lower}" "${_shims}/${_header}" SYMBOLIC)
    endif()
endforeach()
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${_shims}")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${_shims}")

# The Windows SDK's PIX capture interface, which MinGW does not ship. Dawn only
# includes it; the D3D12 capture code that uses it is not built here.
if(NOT EXISTS "${_shims}/DXProgrammableCapture.h")
    file(MAKE_DIRECTORY "${_shims}")
    file(WRITE "${_shims}/DXProgrammableCapture.h" [=[
#pragma once
#include <unknwn.h>
MIDL_INTERFACE("9f251514-9d4d-4902-9d60-18988ab7d4b5")
IDXGraphicsAnalysis : public IUnknown {
    virtual void STDMETHODCALLTYPE BeginCapture() = 0;
    virtual void STDMETHODCALLTYPE EndCapture() = 0;
};
__CRT_UUID_DECL(IDXGraphicsAnalysis, 0x9f251514, 0x9d4d, 0x4902, 0x9d, 0x60, 0x18, 0x98, 0x8a, 0xb7, 0xd4, 0xb5)
]=])
endif()

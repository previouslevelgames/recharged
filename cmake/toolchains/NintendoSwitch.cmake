# Nintendo Switch homebrew: devkitPro's devkitA64, libnx and portlibs, from
# the pinned toolchain image (tools/switch/devkitpro.sh runs a build inside).
# devkitPro's Switch.cmake sets CMAKE_SYSTEM_NAME NintendoSwitch, the compilers,
# the architecture flags, PIE and the libnx/portlib search paths unchanged.
if(DEFINED ENV{DEVKITPRO})
    file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" _mscharged_devkitpro)
else()
    set(_mscharged_devkitpro /opt/devkitpro)
endif()
if(NOT EXISTS "${_mscharged_devkitpro}/cmake/Switch.cmake")
    message(FATAL_ERROR "No devkitPro at ${_mscharged_devkitpro}: build inside tools/switch/devkitpro.sh")
endif()
set(ENV{DEVKITPRO} "${_mscharged_devkitpro}")
include("${_mscharged_devkitpro}/cmake/Switch.cmake")

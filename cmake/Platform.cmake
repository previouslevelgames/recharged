# The systems Recharged builds for, decided once for every build file: test
# these variables, not CMAKE_SYSTEM_NAME, so that a new system is added here.
#   MSCHARGED_SUPPORTED_SYSTEM  Linux, macOS, iOS/iPadOS or Windows
#   MSCHARGED_APPLE             macOS or iOS/iPadOS (Apple's SDKs and compilers)
#   MSCHARGED_IOS               iOS/iPadOS (preset ios-release, tools/package_ios.py):
#                               an Apple system like macOS, without AppKit,
#                               IOKit, other processes or in-place updates
# Sources test MSCHARGED_PLATFORM_IOS; macOS-only code keeps testing __APPLE__
# beside it.
include_guard(GLOBAL)

if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin|iOS|Windows)$")
    set(MSCHARGED_SUPPORTED_SYSTEM ON)
else()
    set(MSCHARGED_SUPPORTED_SYSTEM OFF)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "^(Darwin|iOS)$")
    set(MSCHARGED_APPLE ON)
else()
    set(MSCHARGED_APPLE OFF)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(MSCHARGED_IOS ON)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        message(FATAL_ERROR "iOS builds need CMAKE_OSX_DEPLOYMENT_TARGET (use the ios-release preset)")
    endif()
    add_compile_definitions(MSCHARGED_PLATFORM_IOS=1)
else()
    set(MSCHARGED_IOS OFF)
endif()

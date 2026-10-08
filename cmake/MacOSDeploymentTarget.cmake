include_guard(GLOBAL)

function(mscharged_set_macos_deployment_target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_OSX_DEPLOYMENT_TARGET)
        return()
    endif()

    # An empty CMake target lets Clang and Cargo's C dependencies choose
    # different defaults. Retain this compiler's effective target and make it
    # explicit so Corrosion forwards the same value to Cargo/cc-rs.
    set(_probe "${CMAKE_BINARY_DIR}/CMakeFiles/mscharged-macos-target")
    file(WRITE "${_probe}.c" [=[
#define STRINGIFY_IMPL(value) #value
#define STRINGIFY(value) STRINGIFY_IMPL(value)
#ifndef __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__
#error The compiler must select a macOS deployment target
#endif
const char mscharged_macos_target[] = "MSCHARGED_MACOS_MIN["
    STRINGIFY(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) "]";
]=])
    # Compile only: this also works for cross builds and needs no host process
    # execution or SDK headers. try_compile inherits the real compiler flags.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(_compiled "${_probe}-build" "${_probe}.c"
        COPY_FILE "${_probe}.a" OUTPUT_VARIABLE _output)
    if(NOT _compiled)
        message(FATAL_ERROR "Cannot detect the macOS deployment target:\n${_output}")
    endif()
    file(STRINGS "${_probe}.a" _versions REGEX "MSCHARGED_MACOS_MIN\\[[0-9]+\\]")
    list(REMOVE_DUPLICATES _versions)
    list(LENGTH _versions _count)
    if(NOT _count EQUAL 1 OR NOT _versions MATCHES "MSCHARGED_MACOS_MIN\\[([0-9]+)\\]")
        message(FATAL_ERROR "Compiler did not select one macOS deployment target; set CMAKE_OSX_DEPLOYMENT_TARGET explicitly")
    endif()
    set(_encoded "${CMAKE_MATCH_1}")
    math(EXPR _major "${_encoded} / 10000")
    math(EXPR _minor "(${_encoded} / 100) % 100")
    math(EXPR _patch "${_encoded} % 100")
    set(_version "${_major}.${_minor}")
    if(_patch)
        string(APPEND _version ".${_patch}")
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${_version}" CACHE STRING
        "Minimum macOS version for both native and Cargo dependencies" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${_version}" PARENT_SCOPE)
    message(STATUS "macOS deployment target: ${_version} (compiler default, shared with Cargo)")
endfunction()

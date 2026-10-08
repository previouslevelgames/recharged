include_guard(GLOBAL)

# Whole original scalar codec below the original NL loader. This code lives
# inside each isolated source module; host SDK/nod retain their own zlib-ng.
# The source inflater's fixed output-window requests remain unchanged.
include("${CMAKE_CURRENT_LIST_DIR}/OriginalNativeCompilerProfile.cmake")
mscharged_original_native_profile_supported(_original_native_profile CXX)
if(NOT _original_native_profile)
    return()
endif()
add_library(charged_original_scalar_inflate OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/zlib/adler32.c"
    "${MSCHARGED_PREPARED}/src/zlib/crc32.c"
    "${MSCHARGED_PREPARED}/src/zlib/inffast.c"
    "${MSCHARGED_PREPARED}/src/zlib/inflate.c"
    "${MSCHARGED_PREPARED}/src/zlib/inftrees.c"
    "${MSCHARGED_PREPARED}/src/zlib/zutil.c")
add_dependencies(charged_original_scalar_inflate verify_prepared)
set_target_properties(charged_original_scalar_inflate PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(charged_original_scalar_inflate PRIVATE
    "${MSCHARGED_PREPARED}/src/zlib")
target_compile_options(charged_original_scalar_inflate PRIVATE
    -ffunction-sections -fdata-sections -fno-strict-aliasing)

# Invoke once for each original module/inventory that compiles nlFile.cpp.
# The whole original InflateStream and its six C providers must be together;
# do not resolve these game-module functions to the modern host decoder.
function(mscharged_add_original_inflater target)
    target_sources("${target}" PRIVATE
        "${MSCHARGED_PREPARED}/src/NL/InflateStream.cpp"
        $<TARGET_OBJECTS:charged_original_scalar_inflate>)
    target_include_directories("${target}" BEFORE PRIVATE
        "${MSCHARGED_PREPARED}/src/zlib")
endfunction()

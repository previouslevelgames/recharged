include_guard(GLOBAL)

# The pinned Dec 7 2006 retail HBM build enables assertion expressions in every
# HBM TU. Keep their original side effects and sole source-owned failure flow.
# This compiler property selects no provider and admits no HomeButton lifecycle.
file(GLOB_RECURSE _charged_hbm_retail_sources CONFIGURE_DEPENDS
    "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/*.cpp")
foreach(_source IN LISTS _charged_hbm_retail_sources)
    set_property(SOURCE "${_source}" APPEND PROPERTY COMPILE_DEFINITIONS HBM_ASSERT=1)
endforeach()

# The original callback header defines an anonymous abstract class and erases/
# recasts registrations across TUs. GCC O3 replaces the manager's cross-TU
# dispatch with a direct pure-virtual call. Preserve the source-owned virtual
# calls in this one TU for both production consumers and the heap fixture.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_property(SOURCE
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/snd_DisposeCallbackManager.cpp"
        APPEND PROPERTY COMPILE_OPTIONS -fno-devirtualize)
endif()

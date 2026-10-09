include_guard(GLOBAL)

# GCC reports, by default, several console idioms that the reconstructed game
# sources and their headers use on purpose: four-character constants ('RLYT'),
# classes in a header's anonymous namespace (nw4hbm snd) and offsetof on
# non-standard-layout game classes. Every project target that includes the
# original headers sees them. Call this once after all targets are defined: it
# adds the GCC-only suppressions to targets of this directory only, so the
# third-party subdirectories (SDL, Aurora, Dawn, nod) keep their flags and are
# not rebuilt. Clang keeps its own policy.
function(mscharged_apply_gnu_original_warning_policy)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()
    get_property(targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
            target_compile_options(${target} PRIVATE
                "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-multichar>"
                "$<$<COMPILE_LANGUAGE:CXX>:-Wno-subobject-linkage;-Wno-invalid-offsetof>")
        endif()
    endforeach()
    # False positive: the path length is checked against FILE_PATH_MAX before
    # the strncat whose bound GCC questions.
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/snd_SoundArchive.cpp"
        APPEND PROPERTY COMPILE_OPTIONS -Wno-stringop-overflow)
endfunction()

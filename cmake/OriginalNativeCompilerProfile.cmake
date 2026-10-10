include_guard(GLOBAL)

# Compiler admission for the existing isolated original-source graph.
# Darwin still requires its own SDK/compiler, module lifetime and GPU/runtime
# qualification. This predicate changes no source selection or flow option.
function(mscharged_original_native_profile_supported output)
    set("${output}" FALSE PARENT_SCOPE)
    if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
            CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
            (CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)) OR
            NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR MSVC)
        return()
    endif()
    if(NOT ARGN)
        message(FATAL_ERROR "Original-source compiler admission requires explicit languages")
    endif()
    foreach(_language IN LISTS ARGN)
        if(NOT CMAKE_${_language}_COMPILER_ID MATCHES "Clang|GNU")
            return()
        endif()
    endforeach()
    set("${output}" TRUE PARENT_SCOPE)
endfunction()

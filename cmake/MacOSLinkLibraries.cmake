include_guard(GLOBAL)

# Corrosion copies Rust's native-static-libs into nod-static's link interface.
# AppleClang already supplies libSystem; keep the remaining Rust requirements
# and remove only that duplicate when both native compiler drivers provide it.
function(mscharged_remove_macos_implicit_system_library target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
            NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR
            NOT "System" IN_LIST CMAKE_C_IMPLICIT_LINK_LIBRARIES OR
            NOT "System" IN_LIST CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
        return()
    endif()
    get_target_property(libraries "${target}" INTERFACE_LINK_LIBRARIES)
    if("System" IN_LIST libraries)
        list(REMOVE_ITEM libraries "System")
        set_property(TARGET "${target}" PROPERTY INTERFACE_LINK_LIBRARIES "${libraries}")
    endif()
endfunction()

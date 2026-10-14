include_guard(GLOBAL)

# The original game modules build at -O1 (OriginalMainCreditsDiagnostic.cmake),
# and every source in them inherits that, including the port's own platform
# code compiled into the module: ownership registry, data transports, audio
# devices, skin rows. That code runs on every frame's emission path. Call this
# once after all targets are defined: in each game module built at -O1 it
# compiles only the port-owned sources (src/, tests/diagnostics/) at -O2. The
# prepared original sources keep -O1, and the module's aliasing, FP-contraction
# and operator-new flags still apply. The option is scoped to the module, so a
# port source shared with an -O3 host target keeps -O3 there.
# -fwrapv and GCC's -fno-isolate-erroneous-paths-dereference keep game inline
# functions that a port source emits from exploiting retail undefined behaviour
# harder than the -O1 copies would.
function(mscharged_optimize_game_module_port_sources)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        return()
    endif()
    set(options -O2 -fwrapv)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND options -fno-isolate-erroneous-paths-dereference)
    endif()
    list(JOIN options "$<SEMICOLON>" joined)
    set(option "$<$<BOOL:$<TARGET_PROPERTY:MSCHARGED_PORT_SOURCES_O2>>:${joined}>")
    get_property(targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        get_target_property(target_options ${target} COMPILE_OPTIONS)
        if(NOT type STREQUAL "MODULE_LIBRARY" OR NOT "-O1" IN_LIST target_options)
            continue()
        endif()
        set_property(TARGET ${target} PROPERTY MSCHARGED_PORT_SOURCES_O2 ON)
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS sources)
            cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
            if(source MATCHES "^${PROJECT_SOURCE_DIR}/(src|tests/diagnostics)/.*\\.(c|cc|cpp)$")
                set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS "${option}")
            endif()
        endforeach()
    endforeach()
endfunction()

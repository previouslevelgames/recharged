include_guard(GLOBAL)

# The decompilation builds every release of the game from one tree; its
# VERSION_* conditionals pick the release. A target here compiles the USA
# release unless its MSCHARGED_GAME_VERSION property names another, as the
# European game module does. Call this once after all targets are defined:
# like the warning policy it changes targets of this directory only, so the
# third-party subdirectories keep their flags.
set(MSCHARGED_GAME_VERSIONS R4QE01 R4QP01_REV1 R4QP01_REV2 R4QJ01 R4QK01)

function(mscharged_apply_game_version)
    get_property(targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(NOT type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
            continue()
        endif()
        get_target_property(version ${target} MSCHARGED_GAME_VERSION)
        if(NOT version)
            set(version R4QE01)
        endif()
        if(NOT version IN_LIST MSCHARGED_GAME_VERSIONS)
            message(FATAL_ERROR "${target}: unknown game version ${version}")
        endif()
        target_compile_definitions(${target} PRIVATE VERSION_${version})
    endforeach()
endfunction()

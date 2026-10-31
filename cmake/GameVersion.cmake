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

# The decomp builds some releases from their own copy of a source file:
# src/<folder>/<version>/<file> instead of src/<folder>/<file> (the Korean
# release's newer SDK, configure.py's `source=`). A target of that version
# compiles the copy in its place, with the file's own per-source settings; its
# quoted includes still find the headers beside the original file.
function(mscharged_use_version_sources target version)
    get_target_property(_sources ${target} SOURCES)
    set(_prefix "${MSCHARGED_PREPARED}/src/")
    string(LENGTH "${_prefix}" _prefix_length)
    set(_result)
    foreach(_source IN LISTS _sources)
        string(SUBSTRING "${_source}" 0 ${_prefix_length} _head)
        get_filename_component(_folder "${_source}" DIRECTORY)
        get_filename_component(_name "${_source}" NAME)
        set(_copy "${_folder}/${version}/${_name}")
        if(_head STREQUAL _prefix AND EXISTS "${_copy}")
            foreach(_property IN ITEMS COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES LANGUAGE)
                get_source_file_property(_value "${_source}" ${_property})
                if(_value)
                    set_source_files_properties("${_copy}" PROPERTIES ${_property} "${_value}")
                endif()
            endforeach()
            set_property(SOURCE "${_copy}" APPEND PROPERTY INCLUDE_DIRECTORIES "${_folder}")
            list(APPEND _result "${_copy}")
        else()
            list(APPEND _result "${_source}")
        endif()
    endforeach()
    set_property(TARGET ${target} PROPERTY SOURCES "${_result}")
endfunction()

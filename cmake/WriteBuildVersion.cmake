# Refresh at build time so committing or editing source cannot leave a stale
# revision in an existing build directory. Ignored local files have no effect.
foreach(required IN ITEMS GIT_EXECUTABLE SOURCE_DIR OUTPUT_DIR PORT_VERSION)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing build-version input: ${required}")
    endif()
endforeach()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE PORT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env GIT_OPTIONAL_LOCKS=0
            "${GIT_EXECUTABLE}" status --porcelain=v1
            --untracked-files=normal --ignore-submodules=none
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE source_changes OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)

set(PORT_VERSION_STRING "${PORT_VERSION}+g${PORT_REVISION}")
if(NOT source_changes STREQUAL "")
    string(APPEND PORT_VERSION_STRING ".dirty")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
# configure_file preserves the output timestamp when its contents are unchanged.
configure_file("${CMAKE_CURRENT_LIST_DIR}/build_version.h.in"
               "${OUTPUT_DIR}/build_version.h" @ONLY)

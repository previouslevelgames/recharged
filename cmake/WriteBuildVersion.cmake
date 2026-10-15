# Refresh at build time so committing or editing source cannot leave a stale
# revision in an existing build directory. Ignored local files have no effect.
foreach(required IN ITEMS GIT_EXECUTABLE SOURCE_DIR OUTPUT_DIR PORT_VERSION
                         BUILD_CHANNEL BUILD_REPOSITORY BUILD_PULL_REQUEST BUILD_RUN BUILD_TIME)
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
# The whole commit and its time, for the launcher's About page and update check.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" show -s --format=%H%n%ct HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE commit_lines OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
string(REPLACE "\n" ";" commit_lines "${commit_lines}")
list(GET commit_lines 0 PORT_COMMIT)
list(GET commit_lines 1 PORT_COMMIT_TIME)
if(NOT PORT_COMMIT MATCHES "^[0-9a-f]+$" OR NOT PORT_COMMIT_TIME MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Unexpected commit description: ${commit_lines}")
endif()
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

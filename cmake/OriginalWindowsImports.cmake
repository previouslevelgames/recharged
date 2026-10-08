include_guard(GLOBAL)

# Link plumbing only. This does not admit Windows source compilation, AX image
# initialization, graphics or original-game execution. No provider is defined
# by an import archive, and no original source selection is changed here.
function(mscharged_require_original_windows_linker)
    if(NOT WIN32 OR NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
            NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC OR
            NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x86_64")
        message(FATAL_ERROR "Original PE linkage requires the qualified x86_64 LLVM-MinGW profile")
    endif()
    if(NOT CMAKE_DLLTOOL)
        message(FATAL_ERROR "Original PE linkage requires the selected toolchain's DLLTOOL")
    endif()
endfunction()

function(mscharged_validate_original_windows_symbol symbol)
    # Actual x64 C/Itanium COFF names only: no alias, ordinal, wildcard or
    # normalization of a Linux mangled name. The caller supplies CODE vs DATA.
    if(NOT symbol MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Original PE linkage needs an explicit COFF symbol: ${symbol}")
    endif()
endfunction()

# Call after the source selectors have registered all host requirements.
# REQUIRE_TARGETS collects mscharged_require_original_host_symbol requests from
# the actual host OBJECT/archive targets. FUNCTIONS and DATA cover further
# source/COFF-reviewed contracts. HOST is the real executable, not a path alias.
#
# The resulting INTERFACE target can be linked by either original MODULE. It
# depends only on DEF generation, never on the linked HOST executable, avoiding
# the existing executable -> original MODULE -> executable dependency cycle.
# Each different EXE identity needs its own matched module/import graph.
function(mscharged_add_original_windows_host_imports target)
    mscharged_require_original_windows_linker()
    cmake_parse_arguments(_imports "" "HOST" "FUNCTIONS;DATA;REQUIRE_TARGETS" ${ARGN})
    if(_imports_UNPARSED_ARGUMENTS OR _imports_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "Invalid original PE import arguments: ${ARGN}")
    endif()
    if(TARGET "${target}" OR NOT _imports_HOST OR NOT TARGET "${_imports_HOST}")
        message(FATAL_ERROR "Original PE imports need a new target and an existing actual HOST executable")
    endif()
    get_target_property(_kind "${_imports_HOST}" TYPE)
    if(NOT _kind STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "Original PE imports require an executable HOST: ${_imports_HOST}")
    endif()
    get_target_property(_prior "${_imports_HOST}" MSCHARGED_WINDOWS_IMPORT_FACILITY)
    if(_prior)
        message(FATAL_ERROR "Original PE host already has its matched import facility: ${_prior}")
    endif()
    set(_functions ${_imports_FUNCTIONS})
    set(_data ${_imports_DATA})
    set(_request_targets ${_imports_HOST} ${_imports_REQUIRE_TARGETS})
    list(REMOVE_DUPLICATES _request_targets)
    foreach(_provider IN LISTS _request_targets)
        if(NOT TARGET "${_provider}")
            message(FATAL_ERROR "Original PE host requirement target is absent: ${_provider}")
        endif()
        get_target_property(_bound "${_provider}" MSCHARGED_WINDOWS_IMPORT_FACILITY)
        if(_bound)
            message(FATAL_ERROR "Original PE requirements already belong to a matched EXE graph: ${_provider}")
        endif()
        get_target_property(_required "${_provider}" MSCHARGED_WINDOWS_REQUIRED_CODE_EXPORTS)
        if(_required)
            list(APPEND _functions ${_required})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _functions)
    list(REMOVE_DUPLICATES _data)
    list(SORT _functions)
    list(SORT _data)
    if(NOT _functions AND NOT _data)
        message(FATAL_ERROR "Original PE host imports need explicit CODE or DATA contracts")
    endif()
    set(_exports "EXPORTS\n")
    foreach(_symbol IN LISTS _functions)
        mscharged_validate_original_windows_symbol("${_symbol}")
        if(_symbol IN_LIST _data)
            message(FATAL_ERROR "Original PE symbol cannot be both CODE and DATA: ${_symbol}")
        endif()
        string(APPEND _exports "    ${_symbol}\n")
    endforeach()
    foreach(_symbol IN LISTS _data)
        mscharged_validate_original_windows_symbol("${_symbol}")
        string(APPEND _exports "    ${_symbol} DATA\n")
    endforeach()
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/${target}/$<CONFIG>")
    set(_import_def "${_directory}/host.imports.def")
    set(_host_def "${_directory}/host.exports.def")
    set(_archive "${_directory}/host.imports.a")
    # LIBRARY identifies the real load-time EXE dependency. Keep it out of the
    # separate host export DEF: the host remains an executable, not a DLL.
    file(GENERATE OUTPUT "${_import_def}" CONTENT
        "LIBRARY \"$<TARGET_FILE_NAME:${_imports_HOST}>\"\n${_exports}")
    file(GENERATE OUTPUT "${_host_def}" CONTENT "${_exports}")
    add_custom_command(OUTPUT "${_archive}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_directory}"
        COMMAND "${CMAKE_DLLTOOL}" -m i386:x86-64 -d "${_import_def}" -l "${_archive}"
        DEPENDS "${_import_def}"
        VERBATIM)
    add_custom_target("${target}_generate" DEPENDS "${_archive}")
    add_library("${target}" INTERFACE)
    add_dependencies("${target}" "${target}_generate")
    target_link_libraries("${target}" INTERFACE "${_archive}")
    set_target_properties("${target}" PROPERTIES
        MSCHARGED_WINDOWS_IMPORT_HOST "${_imports_HOST}"
        MSCHARGED_WINDOWS_CODE_IMPORTS "${_functions}"
        MSCHARGED_WINDOWS_DATA_IMPORTS "${_data}"
        MSCHARGED_WINDOWS_IMPORT_DEF "${_import_def}"
        MSCHARGED_WINDOWS_IMPORT_ARCHIVE "${_archive}")
    foreach(_provider IN LISTS _request_targets)
        set_property(TARGET "${_provider}" PROPERTY MSCHARGED_WINDOWS_IMPORT_FACILITY "${target}")
    endforeach()
    target_sources("${_imports_HOST}" PRIVATE "${_host_def}")
    set_target_properties("${_imports_HOST}" PROPERTIES
        ENABLE_EXPORTS ON WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    target_link_options("${_imports_HOST}" PRIVATE "LINKER:--exclude-all-symbols")
    # A DEF export is a required real definition. These additional roots retain
    # the archive members even when normal source references are dormant.
    foreach(_symbol IN LISTS _functions _data)
        target_link_options("${_imports_HOST}" PRIVATE "LINKER:--undefined,${_symbol}")
    endforeach()
    set_property(TARGET "${_imports_HOST}" APPEND PROPERTY LINK_DEPENDS "${_host_def}")
endfunction()

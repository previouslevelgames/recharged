include_guard(GLOBAL)

function(mscharged_preserve_original_return_semantics)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        return()
    endif()

    # These two whole original TUs explicitly mark unspecified retail returns.
    # Do not let modern Clang turn their fallthrough into unreachable execution.
    # Return warnings are scoped to the original pragma regions in source.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-fno-strict-return"
        MSCHARGED_CLANG_HAS_NO_STRICT_RETURN)
    if(NOT MSCHARGED_CLANG_HAS_NO_STRICT_RETURN)
        message(FATAL_ERROR
            "Original retail fallthrough requires Clang -fno-strict-return")
    endif()

    foreach(relative IN ITEMS
            src/Game/FE/feHelpFuncs.cpp
            src/Game/Transitions/ModelTransition.cpp)
        set(source "${MSCHARGED_PREPARED}/${relative}")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Missing original return owner: ${source}")
        endif()
        get_source_file_property(options "${source}" COMPILE_OPTIONS)
        if(NOT "-fno-strict-return" IN_LIST options)
            set_property(SOURCE "${source}" APPEND PROPERTY
                COMPILE_OPTIONS -fno-strict-return)
        endif()
    endforeach()
endfunction()

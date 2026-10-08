function(mscharged_add_native_word_checks prepared_dir aurora_dir verification_target)
    # The address fixtures reserve colliding high addresses with Linux/Win32
    # APIs. These checks do not admit a game runtime on either platform.
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR MSVC OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" OR
       NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        return()
    endif()
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    foreach(_case IN ITEMS audio_context_abi emission_owner_word replay_word_abi shared_host_abi)
        set(_target "native_${_case}_tests")
        add_executable("${_target}" "${_root}/tests/native_${_case}.cpp")
        add_dependencies("${_target}" "${verification_target}")
        target_include_directories("${_target}" PRIVATE "${aurora_dir}/include"
            "${_root}/src" "${prepared_dir}/include" "${prepared_dir}/libs/RVL_SDK/include")
        target_compile_features("${_target}" PRIVATE cxx_std_20)
        target_compile_definitions("${_target}" PRIVATE MSCHARGED_NATIVE=1 TARGET_PC=1
            MSCHARGED_ORIGINAL_FUNCTION_POOLS=1 AURORA_WII_CLOCK=1 dSINGLE=1)
        target_compile_options("${_target}" PRIVATE -fno-strict-aliasing -ffp-contract=off
            -fsigned-char -fno-rtti -ffunction-sections -fdata-sections
            -Wno-unknown-pragmas -Wno-invalid-offsetof)
        target_link_options("${_target}" PRIVATE LINKER:--gc-sections)
        if(MINGW)
            target_link_options("${_target}" PRIVATE -static)
        endif()
        add_test(NAME "native_${_case}" COMMAND "${_target}")
        set_tests_properties("native_${_case}" PROPERTIES TIMEOUT 30)
    endforeach()
    target_sources(native_audio_context_abi_tests PRIVATE "${prepared_dir}/src/NL/nlDebugString.cpp")
    target_sources(native_shared_host_abi_tests PRIVATE "${_root}/src/platform/string_format.cpp")
    target_compile_definitions(native_shared_host_abi_tests PRIVATE MSCHARGED_GAME_MODULE=1)
endfunction()

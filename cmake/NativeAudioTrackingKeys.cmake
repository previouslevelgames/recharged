function(mscharged_add_audio_tracking_key_checks prepared_dir aurora_dir verification_target)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR MSVC OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        return()
    endif()
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    find_package(Threads REQUIRED)
    add_executable(native_audio_tracking_key_tests
        "${_root}/tests/native_audio_tracking_keys.cpp"
        "${prepared_dir}/src/NL/nlAVLTree.cpp"
        "${prepared_dir}/src/NL/MemAlloc.cpp"
        "${_root}/src/platform/game_allocation_ownership.cpp"
        "${_root}/src/platform/host_metadata.cpp")
    add_dependencies(native_audio_tracking_key_tests "${verification_target}")
    target_include_directories(native_audio_tracking_key_tests PRIVATE
        "${_root}/src" "${prepared_dir}/include"
        "${prepared_dir}/libs/RVL_SDK/include" "${aurora_dir}/include")
    target_compile_definitions(native_audio_tracking_key_tests PRIVATE
        MSCHARGED_NATIVE=1 MSCHARGED_GAME_MODULE=1 MSCHARGED_ORIGINAL_FUNCTION_POOLS=1)
    target_compile_features(native_audio_tracking_key_tests PRIVATE cxx_std_20)
    target_compile_options(native_audio_tracking_key_tests PRIVATE
        -fno-strict-aliasing -ffp-contract=off -fsigned-char
        -ffunction-sections -fdata-sections -Wno-unknown-pragmas)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(native_audio_tracking_key_tests PRIVATE
            -fno-assume-sane-operator-new -Wno-register)
    endif()
    target_link_libraries(native_audio_tracking_key_tests PRIVATE Threads::Threads)
    if(APPLE)
        target_link_options(native_audio_tracking_key_tests PRIVATE LINKER:-dead_strip)
    else()
        target_link_options(native_audio_tracking_key_tests PRIVATE LINKER:--gc-sections)
    endif()
    if(MINGW)
        target_link_options(native_audio_tracking_key_tests PRIVATE -static)
    endif()
    add_test(NAME native_audio_tracking_keys COMMAND native_audio_tracking_key_tests)
    set_tests_properties(native_audio_tracking_keys PROPERTIES TIMEOUT 30)
endfunction()

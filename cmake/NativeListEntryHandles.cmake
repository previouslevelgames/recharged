function(mscharged_add_list_entry_checks prepared_dir verification_target)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR MSVC OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        return()
    endif()
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    find_package(Threads REQUIRED)
    add_executable(native_list_entry_tests
        "${_root}/tests/native_list_entry_handles.cpp"
        "${prepared_dir}/src/NL/MemAlloc.cpp"
        "${_root}/src/platform/game_allocation_ownership.cpp"
        "${_root}/src/platform/host_metadata.cpp")
    add_dependencies(native_list_entry_tests "${verification_target}")
    target_include_directories(native_list_entry_tests PRIVATE
        "${_root}/src" "${prepared_dir}/include" "${prepared_dir}/libs/RVL_SDK/include")
    target_compile_definitions(native_list_entry_tests PRIVATE MSCHARGED_NATIVE=1)
    target_compile_features(native_list_entry_tests PRIVATE cxx_std_20)
    target_compile_options(native_list_entry_tests PRIVATE -fno-strict-aliasing
        -ffp-contract=off -fsigned-char -ffunction-sections -fdata-sections -Wno-unknown-pragmas)
    target_link_libraries(native_list_entry_tests PRIVATE Threads::Threads)
    if(APPLE)
        target_link_options(native_list_entry_tests PRIVATE LINKER:-dead_strip)
    else()
        target_link_options(native_list_entry_tests PRIVATE LINKER:--gc-sections)
    endif()
    if(MINGW)
        target_link_options(native_list_entry_tests PRIVATE -static)
    endif()
    add_test(NAME native_list_entry_handles COMMAND native_list_entry_tests)
    set_tests_properties(native_list_entry_handles PROPERTIES TIMEOUT 30)
endfunction()

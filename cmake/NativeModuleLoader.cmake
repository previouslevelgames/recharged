include_guard(GLOBAL)

# Host-only loader/file incarnation service. Adding this library does not admit
# a Windows original-game profile or relax its unresolved-import/LLP64 gates.
get_filename_component(_module_loader_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_library(charged_native_module_loader STATIC "${_module_loader_root}/src/platform/native_module_loader.cpp")
target_include_directories(charged_native_module_loader PUBLIC "${_module_loader_root}/src")
target_compile_features(charged_native_module_loader PUBLIC cxx_std_17)
target_link_libraries(charged_native_module_loader PRIVATE ${CMAKE_DL_LIBS})

if(BUILD_TESTING)
    add_executable(native_module_loader_tests "${_module_loader_root}/tests/native_module_loader.cpp")
    target_link_libraries(native_module_loader_tests PRIVATE charged_native_module_loader)
    set_target_properties(native_module_loader_tests PROPERTIES ENABLE_EXPORTS ON)
    add_library(native_module_loader_fixture SHARED "${_module_loader_root}/tests/fixtures/native_module_loader.cpp")
    add_library(native_module_loader_foreign SHARED "${_module_loader_root}/tests/fixtures/native_module_loader.cpp")
    target_compile_definitions(native_module_loader_foreign PRIVATE FIXTURE_FOREIGN=1)
    foreach(_fixture native_module_loader_fixture native_module_loader_foreign)
        target_link_libraries(${_fixture} PRIVATE native_module_loader_tests)
        target_compile_features(${_fixture} PRIVATE cxx_std_17)
    endforeach()
    add_test(NAME native_module_loader COMMAND native_module_loader_tests
        "$<TARGET_FILE:native_module_loader_fixture>" "$<TARGET_FILE:native_module_loader_foreign>"
        "${CMAKE_CURRENT_BINARY_DIR}/module-loader-test-data")
    if(WIN32 AND CMAKE_DLLTOOL)
        # A real PE imports a deliberately unavailable executable export. The
        # import library declares the contract; it supplies no implementation.
        set(_missing_def "${CMAKE_CURRENT_BINARY_DIR}/native_module_loader_missing.def")
        file(GENERATE OUTPUT "${_missing_def}" CONTENT
            "LIBRARY $<TARGET_FILE_NAME:native_module_loader_tests>\nEXPORTS\nfixture_host_event\nfixture_absent_host_service\n")
        set(_missing_lib "${CMAKE_CURRENT_BINARY_DIR}/native_module_loader_missing.a")
        add_custom_command(OUTPUT "${_missing_lib}"
            COMMAND "${CMAKE_DLLTOOL}" -m i386:x86-64 -d "${_missing_def}" -l "${_missing_lib}"
            DEPENDS "${_missing_def}" VERBATIM)
        add_library(native_module_loader_missing SHARED "${_module_loader_root}/tests/fixtures/native_module_loader.cpp" "${_missing_lib}")
        target_compile_definitions(native_module_loader_missing PRIVATE FIXTURE_MISSING_IMPORT=1)
        target_link_libraries(native_module_loader_missing PRIVATE "${_missing_lib}")
        add_test(NAME native_module_loader_missing_imports COMMAND native_module_loader_tests
            "$<TARGET_FILE:native_module_loader_fixture>" "$<TARGET_FILE:native_module_loader_foreign>"
            "${CMAKE_CURRENT_BINARY_DIR}/module-loader-missing-test-data" "$<TARGET_FILE:native_module_loader_missing>")
        set_tests_properties(native_module_loader_missing_imports PROPERTIES TIMEOUT 20)
    endif()
    if(MINGW)
        foreach(_fixture native_module_loader_tests native_module_loader_fixture
                native_module_loader_foreign native_module_loader_missing)
            if(TARGET "${_fixture}")
                target_link_options("${_fixture}" PRIVATE -static)
            endif()
        endforeach()
    endif()
    set_tests_properties(native_module_loader PROPERTIES TIMEOUT 20)

    if(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        # Separate shared-CRT boundary. Existing static fixtures retain their
        # profile; this check must use one actual LLVM-MinGW libc++/unwind pair.
        find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
        get_filename_component(_mingw_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(_mingw_readobj NAMES llvm-readobj HINTS "${_mingw_compiler_dir}"
            NO_DEFAULT_PATH REQUIRED)
        find_path(_mingw_shared_runtime_dir NAMES libc++.dll HINTS
            "${_mingw_compiler_dir}/../x86_64-w64-mingw32/bin" "${_mingw_compiler_dir}"
            NO_DEFAULT_PATH REQUIRED)
        add_executable(native_module_shared_runtime_tests
            "${_module_loader_root}/tests/native_module_shared_runtime.cpp")
        target_link_libraries(native_module_shared_runtime_tests PRIVATE charged_native_module_loader)
        set_target_properties(native_module_shared_runtime_tests PROPERTIES ENABLE_EXPORTS ON
            WINDOWS_EXPORT_ALL_SYMBOLS OFF)
        add_library(native_module_shared_runtime_fixture SHARED
            "${_module_loader_root}/tests/fixtures/native_module_shared_runtime.cpp")
        target_compile_definitions(native_module_shared_runtime_fixture PRIVATE FIXTURE_MODULE=1)
        target_link_libraries(native_module_shared_runtime_fixture PRIVATE native_module_shared_runtime_tests)
        set_target_properties(native_module_shared_runtime_fixture PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
        foreach(_target native_module_shared_runtime_tests native_module_shared_runtime_fixture)
            target_compile_features("${_target}" PRIVATE cxx_std_20)
            target_compile_options("${_target}" PRIVATE -fno-assume-sane-operator-new -fno-strict-aliasing)
        endforeach()
        add_custom_target(native_module_shared_runtime_dependencies ALL
            COMMAND "${Python3_EXECUTABLE}" -B "${_module_loader_root}/tools/stage_mingw_shared_runtime.py"
                --readobj "${_mingw_readobj}" --runtime-dir "${_mingw_shared_runtime_dir}"
                --output-dir "$<TARGET_FILE_DIR:native_module_shared_runtime_tests>"
                --image "$<TARGET_FILE:native_module_shared_runtime_tests>"
                --image "$<TARGET_FILE:native_module_shared_runtime_fixture>"
                --manifest "${CMAKE_CURRENT_BINARY_DIR}/native-module-shared-runtime-imports.json"
            DEPENDS native_module_shared_runtime_tests native_module_shared_runtime_fixture
            VERBATIM)
        add_test(NAME native_module_shared_runtime COMMAND native_module_shared_runtime_tests
            "$<TARGET_FILE:native_module_shared_runtime_fixture>")
        set_tests_properties(native_module_shared_runtime PROPERTIES TIMEOUT 30)
    endif()
endif()

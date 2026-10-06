include_guard(GLOBAL)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/OriginalFunctionPools.cmake")

# Complete source/compiler inventory only. Real original registry/function-pool
# static execution awaits the native game module. The reviewed typed pool profile
# preserves source categories and adapts native physical slot storage only.
# Do not link the old charged_events/runtime queue replicas as its providers.
add_library(charged_original_events OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/nlEvent.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlBind.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlFunctionMemory.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlAVLTree.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/DispatchEventsTask.cpp")
add_dependencies(charged_original_events verify_prepared)
target_include_directories(charged_original_events PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_events PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1)
target_compile_features(charged_original_events PRIVATE cxx_std_17)
target_link_libraries(charged_original_events PRIVATE
    aurora::os charged_original_function_pool_abi)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_events PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
# Original ordinary new may observe CurrentAllocator. Keep that source scope
# visible to optimizers; this compiler inventory is not the isolated game module.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_events PRIVATE
        -fno-assume-sane-operators-new-delete)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    target_compile_options(charged_original_events PRIVATE
        -fno-assume-sane-operator-new)
endif()
add_custom_target(charged_event_flow_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_events>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-event-flow-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_events
    COMMENT "Record original event/function-pool providers (not an execution gate)"
    VERBATIM)

# Header/record ABI only. This target does not link the source object inventory
# or execute original registry/pool/task constructors. GCC emits unused original
# virtual members while measuring the callable types; ELF collection excludes
# those methods, with no fake allocator/event providers supplied.
if(BUILD_TESTING AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND NOT MSVC)
    add_executable(original_event_abi_tests tests/original_event_abi.cpp)
    target_include_directories(original_event_abi_tests PRIVATE src
        "${MSCHARGED_PREPARED}/include"
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
        "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(original_event_abi_tests PRIVATE
        MSCHARGED_NATIVE=1 TARGET_PC=1)
    target_compile_features(original_event_abi_tests PRIVATE cxx_std_17)
    target_compile_options(original_event_abi_tests PRIVATE
        -ffunction-sections -fdata-sections -fno-strict-aliasing
        -Wno-unknown-pragmas -Wno-invalid-offsetof)
    target_link_options(original_event_abi_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME original_event_abi COMMAND original_event_abi_tests)
    set_tests_properties(original_event_abi PROPERTIES TIMEOUT 30)
endif()

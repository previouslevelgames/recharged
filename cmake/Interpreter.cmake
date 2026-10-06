include_guard(GLOBAL)
include(cmake/Bytecode.cmake)
add_library(charged_interpreter STATIC
    "${MSCHARGED_PREPARED}/src/Game/InterpreterNative.cpp"
    "${MSCHARGED_PREPARED}/src/Game/InterpreterOperations.cpp"
    src/runtime/interpreter.cpp)
add_dependencies(charged_interpreter verify_prepared)
target_include_directories(charged_interpreter PRIVATE "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs")
target_compile_definitions(charged_interpreter PRIVATE MSCHARGED_NATIVE=1
    PUBLIC MSCHARGED_DIAGNOSTIC_INTERPRETER=1)
target_link_libraries(charged_interpreter PUBLIC charged_bytecode_reader)
target_compile_features(charged_interpreter PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_interpreter PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char)
endif()
if(BUILD_TESTING)
    add_executable(interpreter_tests tests/interpreter.cpp)
    target_link_libraries(interpreter_tests PRIVATE charged_interpreter)
    add_test(NAME interpreter COMMAND interpreter_tests)
    set_tests_properties(interpreter PROPERTIES TIMEOUT 30)
endif()

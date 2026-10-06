include_guard(GLOBAL)
add_library(charged_tasks STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlTask.cpp"
    "${MSCHARGED_PREPARED}/src/NL/TaskBase.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Sys/movie.cpp"
    src/runtime/tasks.cpp)
add_dependencies(charged_tasks verify_prepared)
# Borrowed-task lifetime/exception checks and absent movie-service stops are
# explicit legacy diagnostics, with a consistent task ABI in their consumers.
target_compile_definitions(charged_tasks PUBLIC
    MSCHARGED_DIAGNOSTIC_TASKS=1 MSCHARGED_DIAGNOSTIC_MOVIES=1)
target_include_directories(charged_tasks PUBLIC src
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_tasks PUBLIC charged_graphics_memory)
target_compile_features(charged_tasks PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_tasks PRIVATE -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()

include_guard(GLOBAL)
# Whole reconstructed task, handler and movie units. No borrowed-task wrappers,
# native scene replacements or missing-service implementations are linked here.
# This compiler inventory is not an executable link or original scene startup.
add_library(charged_original_task_flow OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/nlTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Game.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Team.cpp"
    "${MSCHARGED_PREPARED}/src/Game/AI/StatsGatherer.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/FrontEndTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/GameRenderTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SH/SHMoviePlayer.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SH/SHCredits.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Sys/movie.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/thp/THPSimple.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLMovieMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXMovieMaterialProgram.cpp"
    "${MSCHARGED_PREPARED}/src/NL/glx/GXMovieMaterialProgramRender.cpp")
add_dependencies(charged_original_task_flow verify_prepared)
target_include_directories(charged_original_task_flow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_task_flow PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 dSINGLE=1)
target_compile_features(charged_original_task_flow PRIVATE cxx_std_17)
target_link_libraries(charged_original_task_flow PRIVATE aurora::os)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_task_flow PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas)
endif()
add_custom_target(charged_task_flow_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_task_flow>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-task-flow-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_task_flow
    COMMENT "Record original task/handler/movie providers (not a scene link check)"
    VERBATIM)

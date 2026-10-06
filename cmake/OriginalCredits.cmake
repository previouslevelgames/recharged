include_guard(GLOBAL)
# Whole original Credits/factory/font/task source inventory. These objects do
# not establish a source scene link, original main execution or GPU readiness.
# The actual Clang Replay pointer-word carrier remains a separate ABI gate.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR MSVC)
    return()
endif()

include(cmake/OriginalFunctionPools.cmake)
add_library(charged_original_credits OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/SH/SHCredits.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SH/SHMoviePlayer.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SH/SHNavigation.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/BaseGameSceneManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Font/FontLoading.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Task/FrontEndTask.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Sys/simpleparser.cpp")
add_dependencies(charged_original_credits verify_prepared)
set_target_properties(charged_original_credits PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_credits PRIVATE cxx_std_20)
target_include_directories(charged_original_credits PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
# Genuine module ABI plus one SDK foundation; no copied frontend, font registry,
# movie, camera, task or ordinary allocation provider is linked into this root.
target_link_libraries(charged_original_credits PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_credits PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_credits PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    -fcheck-new -fno-gnu-unique -fno-assume-sane-operators-new-delete)
add_custom_target(charged_original_credits_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_credits>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-credits-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_credits
    COMMENT "Compile original Credits/providers; complete source scene and startup remain pending"
    VERBATIM)

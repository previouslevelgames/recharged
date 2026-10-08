include_guard(GLOBAL)

# Whole original camera TUs under the isolated game-module ABI. This is a
# compiler/provider inventory, not a game library, camera runtime or scene test.
# Clang's Replay pointer carrier and other host ABIs remain separate gates.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR MSVC)
    return()
endif()

add_library(charged_original_cameras OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/AI/AiUtil.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/CameraMan.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/BaseCam.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/DebugCam.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/animcam.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/noisefilter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/rumblefilter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/GameplayCam.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Camera/GameplayCameraEffects.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Physics/PhysicsEventQueue.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Physics/PhysicsThwomp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/camera_data_transport.cpp")
add_dependencies(charged_original_cameras verify_prepared)
set_target_properties(charged_original_cameras PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_cameras PRIVATE cxx_std_20)
target_include_directories(charged_original_cameras PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
# No legacy CameraCore/AnimatedCameraNative/DebugCameraNative or catalog helper,
# configuration/tweak/task replica, or diagnostic feature definition enters here.
target_link_libraries(charged_original_cameras PRIVATE
    charged_original_function_pool_abi charged_native_metadata aurora::os)
target_compile_definitions(charged_original_cameras PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
target_compile_options(charged_original_cameras PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    -fcheck-new -fno-gnu-unique -fno-assume-sane-operators-new-delete)
add_custom_target(charged_original_camera_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_cameras>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-camera-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_cameras
    COMMENT "Compile whole original camera providers; full runtime and scene pending"
    VERBATIM)

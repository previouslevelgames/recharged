include_guard(GLOBAL)
add_library(charged_animation_retarget STATIC src/resources/animation_retarget.cpp
    src/runtime/animation_retarget.cpp "${MSCHARGED_PREPARED}/src/Game/SAnim/AnimRetargeter.cpp")
add_dependencies(charged_animation_retarget verify_prepared)
target_include_directories(charged_animation_retarget PUBLIC src "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_animation_retarget PUBLIC MSCHARGED_NATIVE=1)
target_compile_features(charged_animation_retarget PUBLIC cxx_std_20)

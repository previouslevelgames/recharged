if(TARGET charged_native_hardware_owner)
    return()
endif()

include(cmake/NativeWpad.cmake)
include(cmake/NativeSTM.cmake)
add_library(charged_native_hardware_owner STATIC src/platform/hardware_owner.cpp)
target_compile_features(charged_native_hardware_owner PUBLIC cxx_std_20)
target_include_directories(charged_native_hardware_owner PUBLIC src
    PRIVATE "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_native_hardware_owner PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
target_link_libraries(charged_native_hardware_owner PUBLIC
    charged_native_wpad charged_native_stm aurora::os aurora::core)
add_dependencies(charged_native_hardware_owner verify_prepared)

include_guard(GLOBAL)
add_library(charged_native_video_device STATIC src/platform/video_device.cpp)
add_dependencies(charged_native_video_device verify_prepared)
target_include_directories(charged_native_video_device PUBLIC src
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_native_video_device PUBLIC TARGET_PC=1)
target_compile_features(charged_native_video_device PUBLIC cxx_std_17)
target_link_libraries(charged_native_video_device PUBLIC
    charged_native_interrupt_controller aurora::vi)

# Bounded native VI request/owner gate. It uses one actual native VI object;
# no game/core replica, GPU initialization or idle-dimming completion fixture.
if(BUILD_TESTING)
    add_executable(native_vi_dimming_tests
        tests/native_vi_dimming.cpp
        "${MSCHARGED_AURORA_PREPARED}/lib/dolphin/vi/vi.cpp")
    add_dependencies(native_vi_dimming_tests verify_prepared)
    target_include_directories(native_vi_dimming_tests PRIVATE
        src "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_definitions(native_vi_dimming_tests PRIVATE AURORA_NATIVE_VIDEO=1 TARGET_PC=1)
    target_compile_features(native_vi_dimming_tests PRIVATE cxx_std_20)
    target_link_libraries(native_vi_dimming_tests PRIVATE charged_native_video_device)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        target_compile_options(native_vi_dimming_tests PRIVATE -ffunction-sections -fdata-sections)
        if(APPLE)
            target_link_options(native_vi_dimming_tests PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(native_vi_dimming_tests PRIVATE -Wl,--gc-sections)
        endif()
    endif()
    add_test(NAME native_vi_dimming COMMAND native_vi_dimming_tests)
endif()

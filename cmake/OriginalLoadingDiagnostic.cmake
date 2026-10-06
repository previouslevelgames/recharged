include_guard(GLOBAL)

# Temporary loading-only source diagnostic derived from the same original
# source graph as the main/Credits diagnostic. Original main owns startup and
# DisplayLoadingMessageFast; only the named checkpoint is selected here.
option(MSCHARGED_BUILD_ORIGINAL_LOADING_DIAGNOSTIC "Build the original loading source diagnostic" OFF)
if(NOT MSCHARGED_BUILD_ORIGINAL_LOADING_DIAGNOSTIC)
    return()
endif()
if(NOT TARGET mscharged_original_main_credits_module)
    message(FATAL_ERROR "The original loading diagnostic requires the original-main source graph")
endif()

get_target_property(_charged_loading_sources mscharged_original_main_credits_module SOURCES)
add_library(mscharged_original_loading_module MODULE
    ${_charged_loading_sources}
    "${MSCHARGED_PREPARED}/src/Game/FE/LidOpenMessage.cpp")
add_dependencies(mscharged_original_loading_module verify_prepared)
set_target_properties(mscharged_original_loading_module PROPERTIES
    PREFIX "" POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(mscharged_original_loading_module PRIVATE cxx_std_20)

# Reuse exact source/native compiler and ownership interfaces. Every original
# TU is compiled whole; this does not extract or copy game-side algorithms.
get_target_property(_charged_loading_includes mscharged_original_main_credits_module INCLUDE_DIRECTORIES)
get_target_property(_charged_loading_options mscharged_original_main_credits_module COMPILE_OPTIONS)
get_target_property(_charged_loading_definitions mscharged_original_main_credits_module COMPILE_DEFINITIONS)
get_target_property(_charged_loading_libraries mscharged_original_main_credits_module LINK_LIBRARIES)
get_target_property(_charged_loading_link_options mscharged_original_main_credits_module LINK_OPTIONS)
list(FILTER _charged_loading_definitions EXCLUDE REGEX
    "^MSCHARGED_DIAGNOSTIC_MAIN_FRONTEND(_SCENE)?=|^MSCHARGED_DIAGNOSTIC_CREDITS_MOVIE=")
target_include_directories(mscharged_original_loading_module PRIVATE ${_charged_loading_includes})
target_compile_options(mscharged_original_loading_module PRIVATE ${_charged_loading_options})
target_compile_definitions(mscharged_original_loading_module PRIVATE
    ${_charged_loading_definitions} MSCHARGED_DIAGNOSTIC_MAIN_LOADING_MESSAGE=1)
target_link_libraries(mscharged_original_loading_module PRIVATE ${_charged_loading_libraries})
target_link_options(mscharged_original_loading_module PRIVATE ${_charged_loading_link_options})
set_property(TARGET mscharged_original_loading_module APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_main_credits_exports.map")

add_executable(mscharged-original-loading-check
    tests/diagnostics/original_loading_window.cpp
    src/platform/os.cpp src/platform/host_metadata.cpp src/platform/string_format.cpp
    src/platform/report.cpp src/platform/thread.cpp)
add_dependencies(mscharged-original-loading-check mscharged_original_loading_module)
target_compile_features(mscharged-original-loading-check PRIVATE cxx_std_20)
target_include_directories(mscharged-original-loading-check BEFORE PRIVATE "${MSCHARGED_AURORA_PREPARED}/include")
target_include_directories(mscharged-original-loading-check PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/lib" src
    "${MSCHARGED_PREPARED}/include" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(mscharged-original-loading-check PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1 AURORA_WII_CLOCK=1
    MSCHARGED_ORIGINAL_LOADING_MODULE_FILENAME="$<TARGET_FILE_NAME:mscharged_original_loading_module>")
target_compile_options(mscharged-original-loading-check PRIVATE
    -O2 -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off)
target_link_options(mscharged-original-loading-check PRIVATE
    -Wl,--gc-sections -Wl,--export-dynamic
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_main_credits_host_exports.map")
set_property(TARGET mscharged-original-loading-check APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/original_main_credits_host_exports.map")
set_property(TARGET mscharged-original-loading-check PROPERTY LINK_LIBRARY_OVERRIDE
    "WHOLE_ARCHIVE,aurora_gx,aurora_mtx,aurora_os")
target_link_libraries(mscharged-original-loading-check PRIVATE
    mscharged_original_main_credits_vi
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,aurora::gx,aurora::mtx,aurora::os>"
    aurora::core aurora::dvd charged_wii_string_format charged_native_stm
    charged_native_system_settings charged_native_video_device
    charged_native_video_output_device Threads::Threads ${CMAKE_DL_LIBS})

if(BUILD_TESTING AND MSCHARGED_TEST_VULKAN AND MSCHARGED_CREDITS_TEST_DISC)
    add_test(NAME original_loading_vulkan COMMAND mscharged-original-loading-check
        --disc "${MSCHARGED_CREDITS_TEST_DISC}")
    set_tests_properties(original_loading_vulkan PROPERTIES TIMEOUT 70
        LABELS "gpu;vulkan;owned-data" RESOURCE_LOCK gx_check
        ENVIRONMENT "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
        FAIL_REGULAR_EXPRESSION "VUID-|Validation Error|Original loading source gate stopped:")
endif()

include_guard(GLOBAL)
# Complete reconstructed source providers; no extracted lighting/lookup managers.
# This object inventory compiles original static requests but executes none.
# Clang retains the existing pnSAnimController packed Replay pointer ABI hold.
add_library(charged_original_lighting OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/GameObjectLighting.cpp"
    "${MSCHARGED_PREPARED}/src/Game/Render/ImpostorLighting.cpp")
add_dependencies(charged_original_lighting verify_prepared)
target_compile_features(charged_original_lighting PRIVATE cxx_std_20)
target_include_directories(charged_original_lighting PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include" "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_lighting PRIVATE charged_original_core)
target_compile_definitions(charged_original_lighting PRIVATE dSINGLE=1
    __alloca=__builtin_alloca)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_lighting PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(charged_original_lighting PRIVATE -Wno-register)
    endif()
elseif(MSVC)
    target_compile_options(charged_original_lighting PRIVATE /Gy /Gw)
endif()
add_custom_target(charged_lighting_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_lighting>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-lighting-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_lighting
    COMMENT "Compile whole original lighting providers; resource/GX lifetime remains pending"
    VERBATIM)

if(BUILD_TESTING)
    # Pure native data transport only. No game ctor/manager/resource/GX call runs.
    add_executable(original_lighting_transport_tests tests/original_lighting_transport.cpp)
    target_compile_features(original_lighting_transport_tests PRIVATE cxx_std_20)
    target_link_libraries(original_lighting_transport_tests PRIVATE charged_original_core)
    add_test(NAME original_lighting_transport COMMAND original_lighting_transport_tests)
    set_tests_properties(original_lighting_transport PROPERTIES TIMEOUT 15)
endif()

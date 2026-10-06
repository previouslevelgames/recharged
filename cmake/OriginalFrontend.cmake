include_guard(GLOBAL)
# The temporary Wii32/native data transport sits beneath original FEScene.
# It owns no game scene, resource manager, animation policy or readiness.
add_library(charged_frontend_package_abi STATIC src/platform/frontend_package.cpp)
add_dependencies(charged_frontend_package_abi verify_prepared)
target_compile_features(charged_frontend_package_abi PUBLIC cxx_std_20)
target_include_directories(charged_frontend_package_abi PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MSCHARGED_PREPARED}/include"
    PRIVATE "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_compile_definitions(charged_frontend_package_abi PUBLIC MSCHARGED_NATIVE=1 TARGET_PC=1)

# These entire reconstructed methods remain the game-side record authority.
add_library(charged_original_frontend_records STATIC
    "${MSCHARGED_PREPARED}/src/Game/FE/fePackage.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/fePresentation.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlSlide.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feAnimation.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlComponent.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlComponentInstance.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlInstance.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feLibObject.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feText.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlTextInstance.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/tlTextInstance_runtime.cpp"
    "${MSCHARGED_PREPARED}/src/NL/utility.cpp")
add_dependencies(charged_original_frontend_records verify_prepared)
target_include_directories(charged_original_frontend_records PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_original_frontend_records PUBLIC
    charged_frontend_package_abi charged_original_core)

# Complete original scene/resource/render providers compile independently.
# This inventory does not establish the complete loading or renderer link.
# BaseGameSceneManager still needs the native Mii/SDK declaration boundary.
add_library(charged_original_frontend OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/Game/FE/BaseSceneHandler.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/FEAudio.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feInput.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/fePointer.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/fePointerManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/fePointerButton.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feBackButton.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/fePageControls.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SH/SHStadiumSelect.cpp"
    "${MSCHARGED_PREPARED}/src/Game/SH/SHOptions.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/GameSceneManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feScene.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feSceneManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feResourceManager.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feRender.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feFontResource.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feTextureResource.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feSceneResource.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feFinder.cpp")
add_dependencies(charged_original_frontend verify_prepared)
target_include_directories(charged_original_frontend PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_frontend PRIVATE
    charged_original_frontend_records charged_original_fonts charged_original_bundles)
foreach(frontend_target IN ITEMS charged_frontend_package_abi charged_original_frontend_records charged_original_frontend)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        target_compile_options(${frontend_target} PRIVATE
            -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
            -ffunction-sections -fdata-sections)
    elseif(MSVC)
        target_compile_options(${frontend_target} PRIVATE /Gy /Gw)
    endif()
endforeach()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    # Original inherited/virtual records use the compiler's native ABI offsets.
    # Independent member/alias fixtures qualify those offsets on each host ABI.
    target_compile_options(charged_frontend_package_abi PRIVATE -Wno-invalid-offsetof)
endif()
if(MSVC)
    target_compile_definitions(charged_original_frontend_records PRIVATE __alloca=_alloca)
    target_compile_options(charged_original_frontend_records PRIVATE /FImalloc.h)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_definitions(charged_original_frontend_records PRIVATE __alloca=__builtin_alloca)
endif()
add_custom_target(charged_frontend_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_frontend>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-frontend-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_frontend
    COMMENT "Record original frontend source providers (not a loading/rendering link check)"
    VERBATIM)
if(BUILD_TESTING)
    set(original_frontend_fixture "${CMAKE_CURRENT_BINARY_DIR}/original-frontend-fixture")
    add_custom_command(OUTPUT "${original_frontend_fixture}/generated.fen"
        "${original_frontend_fixture}/generated_offsets.h"
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_original_frontend_fixture.py"
            "${original_frontend_fixture}"
        DEPENDS tests/generate_original_frontend_fixture.py VERBATIM)
    add_executable(original_frontend_abi_tests tests/original_frontend_abi.cpp
        "${original_frontend_fixture}/generated_offsets.h")
    target_include_directories(original_frontend_abi_tests PRIVATE
        "${original_frontend_fixture}" "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_frontend_abi_tests PRIVATE charged_original_frontend_records)
    # This partial-link data/method qualifier does not invoke Render/StringDraw
    # or source scene/resource callbacks. Their real providers remain required.
    if(MSVC)
        target_compile_options(original_frontend_abi_tests PRIVATE /Gy /Gw)
        target_link_options(original_frontend_abi_tests PRIVATE /OPT:REF)
    elseif(APPLE)
        target_link_options(original_frontend_abi_tests PRIVATE -Wl,-dead_strip)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_link_options(original_frontend_abi_tests PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME original_frontend_abi COMMAND original_frontend_abi_tests
        "${original_frontend_fixture}/generated.fen")
    set_tests_properties(original_frontend_abi PROPERTIES TIMEOUT 60)
endif()

include(cmake/OriginalFrontendRender.cmake)

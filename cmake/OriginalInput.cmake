include_guard(GLOBAL)

# Whole original input and KPAD providers. The core SDL Wii Remote seam is a
# bounded hardware provider; full device/IR/source startup acceptance is pending.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR MSVC)
    return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NativeWpad.cmake")

add_library(charged_original_input OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/NL/plat/PadBackend.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/globalpad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/cGlobalPad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/cPlatPad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/PlatPadManager.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiPad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiRemotePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiFreestylePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiClassicPad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/SwappablePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/GameCubePad.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/DPDData.cpp"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/kpad/KPAD.c"
    "${MSCHARGED_PREPARED}/src/Game/PadActions.cpp"
    "${MSCHARGED_PREPARED}/src/Game/PadMonkey.cpp"
    "${MSCHARGED_PREPARED}/src/NL/plat/WiiPadMonkey.cpp"
    "${MSCHARGED_PREPARED}/src/Game/FE/feInput.cpp")
add_dependencies(charged_original_input verify_prepared)
set_target_properties(charged_original_input PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(charged_original_input PRIVATE cxx_std_20 c_std_99)
target_include_directories(charged_original_input PRIVATE
    "${MSCHARGED_AURORA_PREPARED}/include")
target_link_libraries(charged_original_input PRIVATE
    charged_original_function_pool_abi charged_native_metadata
    charged_native_wpad aurora::os)
target_compile_definitions(charged_original_input PRIVATE
    MSCHARGED_GAME_MODULE=1 dSINGLE=1 __alloca=__builtin_alloca)
# The original compiler profile explicitly uses -RTTI off. Missing base virtual
# declarations remain source/link boundaries; no replacement bodies are added.
target_compile_options(charged_original_input PRIVATE
    -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
    "$<$<COMPILE_LANGUAGE:CXX>:-fcheck-new;-fno-rtti>")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(charged_original_input PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-gnu-unique;-fno-assume-sane-operators-new-delete>")
else()
    target_compile_options(charged_original_input PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-assume-sane-operator-new;-Wno-register>")
endif()
add_custom_target(charged_original_input_scan
    COMMAND "${CMAKE_COMMAND}" "-DNM:FILEPATH=${CMAKE_NM}"
        "-DOBJECT:STRING=$<TARGET_OBJECTS:charged_original_input>"
        "-DOUTPUT:FILEPATH=${CMAKE_CURRENT_BINARY_DIR}/original-input-undefined.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReportNativeSymbols.cmake"
    DEPENDS charged_original_input
    COMMENT "Compile original input providers; device services and startup pending"
    VERBATIM)

include_guard(GLOBAL)
include(cmake/OriginalNativeCompilerProfile.cmake)
include(cmake/NativeHBMDebug.cmake)

# Actual source statics and production pre-arena reservation, with and without
# HBM's optional silence extent. No source sound initializer or worker executes.
# This Linux loader leaf does not establish the Mach-O constructor contract.
function(mscharged_add_native_hbm_storage_tests)
    mscharged_original_native_profile_supported(_profile C CXX)
    if(NOT BUILD_TESTING OR NOT _profile OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    add_library(original_hbm_storage_objects OBJECT
        "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
        "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/snd_AxManager.cpp"
        tests/diagnostics/original_hbm_storage.cpp)
    mscharged_select_original_hbm_debug(original_hbm_storage_objects)
    foreach(_name IN ITEMS AX AXAlloc AXAux AXCL AXComp AXSPB AXVPB AXProf AXOut DSPCode)
        target_sources(original_hbm_storage_objects PRIVATE
            "${MSCHARGED_PREPARED}/src/RVL_SDK/ax/${_name}.c")
    endforeach()
    set(_providers original_hbm_storage_objects)
    foreach(_count IN ITEMS 13 14)
        set(_target "original_hbm_storage_${_count}")
        add_library("${_target}" MODULE src/platform/native_ax_module_storage.cpp
            "$<TARGET_OBJECTS:original_hbm_storage_objects>")
        list(APPEND _providers "${_target}")
        if(_count EQUAL 14)
            target_compile_definitions("${_target}" PRIVATE MSCHARGED_NATIVE_HBM_MODULE_MEMORY=1)
        endif()
        target_link_options("${_target}" PRIVATE
            "LINKER:--gc-sections" "LINKER:-Bsymbolic-functions"
            "LINKER:--version-script=${PROJECT_SOURCE_DIR}/tests/diagnostics/original_hbm_storage_exports.map")
        set_property(TARGET "${_target}" APPEND PROPERTY LINK_DEPENDS
            "${PROJECT_SOURCE_DIR}/tests/diagnostics/original_hbm_storage_exports.map")
    endforeach()
    foreach(_target IN LISTS _providers)
        add_dependencies("${_target}" verify_prepared)
        set_target_properties("${_target}" PROPERTIES POSITION_INDEPENDENT_CODE ON
            C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
        target_compile_features("${_target}" PRIVATE c_std_17 cxx_std_20)
        target_include_directories("${_target}" PRIVATE "${PROJECT_SOURCE_DIR}/src"
            "${MSCHARGED_AURORA_PREPARED}/include" "${MSCHARGED_PREPARED}/include"
            "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
            "${MSCHARGED_PREPARED}/libs/Runtime/include")
        target_compile_definitions("${_target}" PRIVATE MSCHARGED_NATIVE=1
            MSCHARGED_GAME_MODULE=1 MSCHARGED_NATIVE_AX_MODULE_MEMORY=1 TARGET_PC=1 AURORA_WII_CLOCK=1)
        target_compile_options("${_target}" PRIVATE -ffunction-sections -fdata-sections
            -fno-strict-aliasing -ffp-contract=off -fsigned-char -Wno-unknown-pragmas
            "$<$<COMPILE_LANGUAGE:C>:-fexceptions>"
            "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
            "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-fno-gnu-unique>")
    endforeach()
    add_executable(native_hbm_storage_tests tests/native_hbm_storage.cpp)
    add_dependencies(native_hbm_storage_tests original_hbm_storage_13 original_hbm_storage_14)
    target_compile_features(native_hbm_storage_tests PRIVATE cxx_std_20)
    mscharged_link_original_hbm_debug_host(native_hbm_storage_tests CPU_FIXTURE)
    target_compile_definitions(native_hbm_storage_tests PRIVATE HBM_ASSERT=1)
    target_link_libraries(native_hbm_storage_tests PRIVATE charged_native_ax_module_memory
        charged_native_interrupts aurora::os "${CMAKE_DL_LIBS}")
    target_link_options(native_hbm_storage_tests PRIVATE "LINKER:--export-dynamic")
    foreach(_count IN ITEMS 13 14)
        add_test(NAME "native_hbm_storage_${_count}" COMMAND native_hbm_storage_tests
            "$<TARGET_FILE:original_hbm_storage_${_count}>" "${_count}")
        set_tests_properties("native_hbm_storage_${_count}" PROPERTIES TIMEOUT 15 LABELS "Platform")
    endforeach()
endfunction()

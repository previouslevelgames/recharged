include_guard(GLOBAL)

# Compile whole original SDK owners without inventing IOS/ES, flash storage or
# completion. MEMCLR in unused NANDMove remains an unresolved source import.
add_library(charged_original_nand_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nand/NANDCore.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nand/NANDOpenClose.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nand/NANDCheck.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nand/nand.c")
add_dependencies(charged_original_nand_sources verify_prepared)
target_include_directories(charged_original_nand_sources PRIVATE
    src "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_nand_sources PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1)
target_link_libraries(charged_original_nand_sources PRIVATE aurora::os)
target_compile_features(charged_original_nand_sources PRIVATE c_std_17)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(charged_original_nand_sources PRIVATE
        -ffunction-sections -fdata-sections
        -fexceptions -fno-strict-aliasing -Wno-unknown-pragmas
        -Werror=pointer-to-int-cast -Werror=implicit-function-declaration)
endif()

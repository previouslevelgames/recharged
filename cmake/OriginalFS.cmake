include_guard(GLOBAL)

# Whole original filesystem library compiler inventory. The real IOS device,
# IPC arena and asynchronous completion services remain separate providers.
# This target does not initialize NAND or claim working game saves.
add_library(charged_original_fs_sources OBJECT EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/fs/fs.c")
add_dependencies(charged_original_fs_sources verify_prepared)
target_include_directories(charged_original_fs_sources PRIVATE
    src "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_fs_sources PRIVATE
    MSCHARGED_NATIVE=1 TARGET_PC=1)
target_link_libraries(charged_original_fs_sources PRIVATE aurora::os)
target_compile_features(charged_original_fs_sources PRIVATE c_std_17)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(charged_original_fs_sources PRIVATE
        -ffunction-sections -fdata-sections -fexceptions -fno-strict-aliasing
        -Wno-unknown-pragmas)
endif()

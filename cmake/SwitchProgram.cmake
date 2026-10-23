include_guard(GLOBAL)

# The Switch program: the game alone, started from the Homebrew Menu. It reads
# recharged.ini and the disc image from its folder on the SD card, as the
# desktop program does when started with a disc.
add_library(charged_session_log STATIC src/platform/session_log.cpp)
target_include_directories(charged_session_log PUBLIC src)
target_compile_features(charged_session_log PRIVATE cxx_std_20)
target_link_libraries(charged_session_log PUBLIC SDL3::SDL3)
# Crash reports name no modules: Horizon has no dynamic loader.
target_include_directories(charged_session_log SYSTEM BEFORE PRIVATE src/platform/switch/include)

add_executable(mscharged src/platform/switch/main.cpp src/platform/switch/nx_init.cpp
    src/platform/switch/launcher_services.cpp src/platform/switch/sqlite_vfs.c)
set_target_properties(mscharged PROPERTIES OUTPUT_NAME recharged)
target_compile_features(mscharged PRIVATE cxx_std_20)
target_include_directories(mscharged PRIVATE src)
target_link_libraries(mscharged PRIVATE mscharged_build_info charged_host sqlite3 charged_switch_vulkan)
# 8 MiB stacks for threads that ask for none, and renames that replace a file
# as on PC (src/platform/switch/nx_init.cpp).
target_link_options(mscharged PRIVATE -Wl,--wrap=pthread_create -Wl,--wrap=rename)

# Beside the program, as on PC: the renderer configurations prepared at start
# and the overlay's font.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PipelineCatalog.cmake")
mscharged_stage_pipeline_catalog(mscharged)
add_custom_command(TARGET mscharged POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:mscharged>/assets/launcher"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MSCHARGED_IMGUI_PREPARED}/misc/fonts/Roboto-Medium.ttf"
            "$<TARGET_FILE_DIR:mscharged>/assets/launcher"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/Apache-2.0.txt"
            "$<TARGET_FILE_DIR:mscharged>/assets/launcher/LICENSE-APACHE"
    VERBATIM)

# The Homebrew Menu's entry: title, version and icon (assets/switch).
nx_generate_nacp(recharged.nacp NAME "Recharged" AUTHOR "Recharged" VERSION "${MSCHARGED_PORT_VERSION}")
nx_create_nro(mscharged NACP recharged.nacp ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/switch/icon.jpg")

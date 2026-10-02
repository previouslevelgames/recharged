foreach(name IN ITEMS zlib-ng libpng freetype sqlite zstd)
    string(REPLACE "-" "_" variable "${name}")
    mscharged_prepare_dependency("${name}" "MSCHARGED_${variable}_PREPARED")
endforeach()
mscharged_prepare_dependency(imgui MSCHARGED_IMGUI_PREPARED)
find_package(Threads REQUIRED)

find_program(MSCHARGED_SQLITE_MAKE NAMES gmake make REQUIRED)
find_program(MSCHARGED_SQLITE_TCLSH NAMES tclsh tclsh8.6 REQUIRED)
find_program(MSCHARGED_SQLITE_HOST_CC NAMES cc gcc clang REQUIRED)
set(sqlite_args --build-dir "${CMAKE_BINARY_DIR}" --cc "${MSCHARGED_SQLITE_HOST_CC}"
    --make "${MSCHARGED_SQLITE_MAKE}" --tclsh "${MSCHARGED_SQLITE_TCLSH}")
set(sqlite_generator "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_sqlite.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sqlite_generator}")
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${sqlite_generator}" ${sqlite_args}
    OUTPUT_VARIABLE MSCHARGED_SQLITE_GENERATED OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(verify_sqlite_generated
    COMMAND "${Python3_EXECUTABLE}" -B "${sqlite_generator}" ${sqlite_args} --check
    COMMENT "Verify generated SQLite inputs and source/tool provenance" VERBATIM)
add_dependencies(verify_sqlite_generated verify_sqlite)
add_dependencies(verify_prepared verify_sqlite_generated)

function(mscharged_add_graphics_providers)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(ZLIB_COMPAT ON)
    set(ZLIB_INSTALL OFF)
    add_subdirectory("${MSCHARGED_zlib_ng_PREPARED}" "${CMAKE_CURRENT_BINARY_DIR}/extern/zlib" EXCLUDE_FROM_ALL)
    get_target_property(zlib_target zlibstatic ALIASED_TARGET)
    add_library(ZLIB::ZLIB ALIAS ${zlib_target})
    # libpng's FindZLIB must use the already configured pinned target.
    file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/ZLIBConfig.cmake"
        "set(ZLIB_FOUND TRUE)\nset(ZLIB_INCLUDE_DIRS \"${MSCHARGED_zlib_ng_PREPARED};${CMAKE_CURRENT_BINARY_DIR}/extern/zlib\")\n")
    set(PNG_SHARED OFF)
    set(PNG_STATIC ON)
    set(PNG_TESTS OFF)
    set(PNG_TOOLS OFF)
    add_subdirectory("${MSCHARGED_libpng_PREPARED}" "${CMAKE_CURRENT_BINARY_DIR}/extern/png" EXCLUDE_FROM_ALL)
    add_library(PNG::PNG ALIAS png_static)
    set(FT_DISABLE_BROTLI ON)
    set(FT_DISABLE_HARFBUZZ ON)
    set(FT_DISABLE_PNG ON)
    set(FT_DISABLE_ZLIB ON)
    set(FT_DISABLE_BZIP2 ON)
    add_subdirectory("${MSCHARGED_freetype_PREPARED}" "${CMAKE_CURRENT_BINARY_DIR}/extern/freetype" EXCLUDE_FROM_ALL)
    add_library(Freetype::Freetype ALIAS freetype)
    set(ZSTD_BUILD_PROGRAMS OFF)
    set(ZSTD_BUILD_TESTS OFF)
    set(ZSTD_BUILD_SHARED OFF)
    set(ZSTD_BUILD_STATIC ON)
    add_subdirectory("${MSCHARGED_zstd_PREPARED}/build/cmake" "${CMAKE_CURRENT_BINARY_DIR}/extern/zstd" EXCLUDE_FROM_ALL)
    add_library(zstd::libzstd ALIAS libzstd_static)
    add_library(sqlite3 STATIC "${MSCHARGED_SQLITE_GENERATED}/sqlite3.c")
    target_include_directories(sqlite3 PUBLIC "${MSCHARGED_SQLITE_GENERATED}")
    target_link_libraries(sqlite3 PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    add_library(imgui STATIC
        "${MSCHARGED_IMGUI_PREPARED}/imgui.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/imgui_draw.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/imgui_tables.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/imgui_widgets.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/misc/cpp/imgui_stdlib.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/misc/freetype/imgui_freetype.cpp")
    target_include_directories(imgui PUBLIC "${MSCHARGED_IMGUI_PREPARED}")
    target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE)
    target_compile_features(imgui PRIVATE cxx_std_20)
    target_link_libraries(imgui PRIVATE Freetype::Freetype)
    add_library(imgui_backends STATIC
        "${MSCHARGED_IMGUI_PREPARED}/backends/imgui_impl_sdl3.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/backends/imgui_impl_sdlrenderer3.cpp"
        "${MSCHARGED_IMGUI_PREPARED}/backends/imgui_impl_wgpu.cpp")
    target_compile_features(imgui_backends PRIVATE cxx_std_20)
    target_compile_definitions(imgui_backends PRIVATE IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
    target_link_libraries(imgui_backends PRIVATE imgui SDL3::SDL3 dawn::webgpu_dawn)
    target_link_libraries(imgui PUBLIC imgui_backends)
endfunction()

mscharged_add_graphics_providers()

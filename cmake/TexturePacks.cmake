# HD texture packs: finding folder and zip packs (Dolphin's format) and reading
# their images. The launcher lists them; the game host hands them to Aurora.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Zlib.cmake")
add_library(charged_texture_packs STATIC src/platform/texture_packs.cpp src/platform/zip_archive.cpp)
target_include_directories(charged_texture_packs PUBLIC src)
target_compile_features(charged_texture_packs PUBLIC cxx_std_17)
target_link_libraries(charged_texture_packs PRIVATE ZLIB::ZLIB)
if(BUILD_TESTING)
    add_executable(texture_packs_tests tests/texture_packs.cpp)
    target_link_libraries(texture_packs_tests PRIVATE charged_texture_packs)
    add_test(NAME texture_packs
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/texture_packs_fixture.py"
                "$<TARGET_FILE:texture_packs_tests>" "${CMAKE_CURRENT_BINARY_DIR}/texture-packs-tests")
    set_tests_properties(texture_packs PROPERTIES TIMEOUT 120)
endif()

# The launcher's update check and in-place updates (#50). HTTPS goes through
# what the system provides (WinHTTP, or the system's libcurl loaded at run
# time); unpacking uses the pinned zlib-ng. No other third-party code.
include_guard(GLOBAL)
include(cmake/Zlib.cmake)
find_package(Threads REQUIRED)
# The JSON reader, shared with the image library's cache (#65).
add_library(charged_json STATIC src/platform/json.cpp)
target_include_directories(charged_json PUBLIC src)
target_compile_features(charged_json PUBLIC cxx_std_17)
add_library(charged_updates STATIC
    src/platform/http_client.cpp
    src/platform/sha256.cpp
    src/launcher/release_notes.cpp
    src/launcher/update_check.cpp
    src/launcher/update_install.cpp
    src/launcher/update_service.cpp
)
target_include_directories(charged_updates PUBLIC src)
target_compile_features(charged_updates PUBLIC cxx_std_17)
target_link_libraries(charged_updates PRIVATE ZLIB::ZLIB PUBLIC Threads::Threads charged_json)
if(WIN32)
    target_link_libraries(charged_updates PRIVATE winhttp)
else()
    target_link_libraries(charged_updates PRIVATE ${CMAKE_DL_LIBS})
endif()
if(BUILD_TESTING)
    add_executable(launcher_update_tests tests/launcher_updates.cpp)
    target_link_libraries(launcher_update_tests PRIVATE charged_updates)
    # Packages made the way tools/package_build.py makes them (Python's
    # tarfile and zipfile), then the C++ checks against them.
    add_test(NAME launcher_updates
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_update_fixture.py"
                "$<TARGET_FILE:launcher_update_tests>" "${CMAKE_CURRENT_BINARY_DIR}/launcher-update-tests")
endif()

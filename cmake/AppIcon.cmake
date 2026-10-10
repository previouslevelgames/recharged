# Embed the original PNG so launcher and direct game startup share one icon.
# Keep image conversion out of the user's build prerequisites.
set(_app_icon "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/icon.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_app_icon}")
file(READ "${_app_icon}" _app_icon_hex HEX)
string(REGEX REPLACE "(................................................................)" "\\1\n"
    _app_icon_hex "${_app_icon_hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1,"
    MSCHARGED_APP_ICON_BYTES "${_app_icon_hex}")
configure_file(cmake/app_icon_bytes.h.in generated/mscharged/app_icon_bytes.h @ONLY)
unset(_app_icon_hex)
unset(MSCHARGED_APP_ICON_BYTES)

add_library(charged_app_icon STATIC src/platform/app_icon.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    # Nested namespaces and the generated inline PNG array require C++17;
    # AppleClang otherwise compiles this target in its older default dialect.
    target_compile_features(charged_app_icon PRIVATE cxx_std_17)
endif()
target_include_directories(charged_app_icon PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(charged_app_icon PRIVATE SDL3::SDL3)

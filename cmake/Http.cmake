# HTTPS through what the system provides (WinHTTP, or the system's libcurl
# loaded at run time): the launcher's updates and competitive play.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
add_library(charged_http STATIC src/platform/http_client.cpp)
target_include_directories(charged_http PUBLIC src)
target_compile_features(charged_http PUBLIC cxx_std_17)
target_link_libraries(charged_http PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(charged_http PRIVATE winhttp)
else()
    target_link_libraries(charged_http PRIVATE ${CMAKE_DL_LIBS})
endif()
if(NINTENDO_SWITCH)
    # No system libcurl to load: HTTPS fails as without one on Linux (src/platform/switch/include).
    target_include_directories(charged_http SYSTEM BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src/platform/switch/include")
endif()

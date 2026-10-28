# The WFC server's match reports: the game module records each online series
# this machine plays and the error codes its online menus show
# (src/platform/online_match_snapshot.cpp, tweak patches 0037 and 0045), and
# the game host sends them to the server (src/platform/online_reports.h).
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/Http.cmake")
find_package(Threads REQUIRED)
if(NOT TARGET charged_json)
    add_library(charged_json STATIC src/platform/json.cpp)
    target_include_directories(charged_json PUBLIC src)
    target_compile_features(charged_json PUBLIC cxx_std_17)
endif()
add_library(charged_online_reports STATIC src/platform/online_reports.cpp)
target_include_directories(charged_online_reports PUBLIC src)
target_compile_features(charged_online_reports PUBLIC cxx_std_17)
target_link_libraries(charged_online_reports PUBLIC charged_http charged_json Threads::Threads)

if(BUILD_TESTING)
    add_executable(online_reports_tests tests/online_reports.cpp)
    target_link_libraries(online_reports_tests PRIVATE charged_online_reports)
    add_test(NAME online_reports COMMAND online_reports_tests)
    set_tests_properties(online_reports PROPERTIES TIMEOUT 60)
endif()

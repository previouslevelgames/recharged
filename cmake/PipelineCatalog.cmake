include_guard(GLOBAL)

# Portable renderer configurations, generated only from the reviewed source.
set(_pipeline_catalog_source "${CMAKE_CURRENT_SOURCE_DIR}/assets/graphics/pipelines.json")
set(_pipeline_catalog_tool "${CMAKE_CURRENT_SOURCE_DIR}/tools/pipeline_catalog.py")
set(_pipeline_catalog_db "${CMAKE_CURRENT_BINARY_DIR}/generated/graphics/initial_pipeline_cache.db")
set(_pipeline_catalog_readme "${CMAKE_CURRENT_SOURCE_DIR}/assets/graphics/README.md")
add_custom_command(OUTPUT "${_pipeline_catalog_db}"
    COMMAND "${Python3_EXECUTABLE}" -B "${_pipeline_catalog_tool}"
        --catalog "${_pipeline_catalog_source}" --output "${_pipeline_catalog_db}"
    DEPENDS "${_pipeline_catalog_source}" "${_pipeline_catalog_tool}"
    VERBATIM)
# One staging task serves every original host. Resolve and deduplicate directories
# after all targets exist; launcher and standalone commonly share their output.
# TARGET_FILE_DIR honors multi-config and per-target runtime output settings and,
# with CMP0112 NEW, does not add a dependency back onto those executables.
add_custom_target(charged_pipeline_catalog
    COMMAND "${CMAKE_COMMAND}"
        "-DMSCHARGED_PIPELINE_CATALOG_DB=${_pipeline_catalog_db}"
        "-DMSCHARGED_PIPELINE_CATALOG_README=${_pipeline_catalog_readme}"
        "-DMSCHARGED_PIPELINE_CATALOG_DIRECTORIES=$<REMOVE_DUPLICATES:$<TARGET_GENEX_EVAL:charged_pipeline_catalog,$<TARGET_PROPERTY:charged_pipeline_catalog,MSCHARGED_PIPELINE_CATALOG_DIRECTORIES>>>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StagePipelineCatalog.cmake"
    DEPENDS "${_pipeline_catalog_db}" "${_pipeline_catalog_readme}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StagePipelineCatalog.cmake"
    VERBATIM)
set_target_properties(charged_pipeline_catalog PROPERTIES
    MSCHARGED_PIPELINE_CATALOG_DB "${_pipeline_catalog_db}"
    MSCHARGED_PIPELINE_CATALOG_README "${_pipeline_catalog_readme}")

function(mscharged_stage_pipeline_catalog target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot stage pipeline catalogue for absent target ${target}")
    endif()
    get_target_property(_staged "${target}" MSCHARGED_PIPELINE_CATALOG_STAGED)
    if(_staged)
        return()
    endif()
    add_dependencies("${target}" charged_pipeline_catalog)
    set_property(TARGET charged_pipeline_catalog APPEND PROPERTY
        MSCHARGED_PIPELINE_CATALOG_DIRECTORIES "$<TARGET_FILE_DIR:${target}>")
    get_target_property(_db charged_pipeline_catalog MSCHARGED_PIPELINE_CATALOG_DB)
    get_target_property(_readme charged_pipeline_catalog MSCHARGED_PIPELINE_CATALOG_README)
    set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS "${_db}" "${_readme}")
    set_property(TARGET "${target}" PROPERTY MSCHARGED_PIPELINE_CATALOG_STAGED TRUE)
endfunction()

if(BUILD_TESTING)
    add_test(NAME pipeline_catalog
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pipeline_catalog.py")
endif()
if(BUILD_TESTING AND TARGET aurora::gx)
    add_executable(pipeline_catalog_layout_tests tests/pipeline_catalog_layout.cpp)
    target_include_directories(pipeline_catalog_layout_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MSCHARGED_AURORA_PREPARED}/lib")
    target_compile_features(pipeline_catalog_layout_tests PRIVATE cxx_std_20)
    target_link_libraries(pipeline_catalog_layout_tests PRIVATE aurora::gx aurora::core absl::flat_hash_map)
    add_test(NAME pipeline_catalog_layout COMMAND pipeline_catalog_layout_tests)
    add_executable(pipeline_demand_log_tests tests/pipeline_demand_log.cpp src/runtime/pipeline_demand_log.cpp)
    target_include_directories(pipeline_demand_log_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MSCHARGED_AURORA_PREPARED}/include")
    target_compile_features(pipeline_demand_log_tests PRIVATE cxx_std_17)
    target_link_libraries(pipeline_demand_log_tests PRIVATE ZLIB::ZLIB)
    add_test(NAME pipeline_demand_log COMMAND pipeline_demand_log_tests)
endif()

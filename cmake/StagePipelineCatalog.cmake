# Invoked once per build by the shared resource target, including multi-config.
foreach(_directory IN LISTS MSCHARGED_PIPELINE_CATALOG_DIRECTORIES)
    set(_assets "${_directory}/assets/graphics")
    file(MAKE_DIRECTORY "${_assets}")
    file(COPY_FILE "${MSCHARGED_PIPELINE_CATALOG_DB}"
        "${_assets}/initial_pipeline_cache.db" ONLY_IF_DIFFERENT RESULT _result)
    if(NOT _result STREQUAL "0")
        message(FATAL_ERROR "Cannot stage pipeline catalogue: ${_result}")
    endif()
    file(COPY_FILE "${MSCHARGED_PIPELINE_CATALOG_README}"
        "${_assets}/README.md" ONLY_IF_DIFFERENT RESULT _result)
    if(NOT _result STREQUAL "0")
        message(FATAL_ERROR "Cannot stage pipeline catalogue README: ${_result}")
    endif()
endforeach()

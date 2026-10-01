if(NOT NM OR NOT EXISTS "${OBJECT}")
    message(FATAL_ERROR "A symbol tool and compiled game entry object are required")
endif()
execute_process(COMMAND "${NM}" -u -C "${OBJECT}"
    OUTPUT_FILE "${OUTPUT}" COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "Original entry unit compiled; unresolved references are in ${OUTPUT}")

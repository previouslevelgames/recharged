include_guard(GLOBAL)

# The native host installs persistent storage and IPC memory. Original main
# owns flash initialization and its actual priority13/all-states task; save
# loading, banners, Mii and complete menu readiness remain separate gates.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_FLASH
    "Admit original flash initialization/task in the frontend diagnostic" OFF)
function(mscharged_select_original_frontend_flash target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_FLASH)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE OR
            NOT MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE)
        message(FATAL_ERROR "Flash admission requires the original frontend/task diagnostic")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original flash providers belong to the isolated game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_FLASH=1)
    get_target_property(_existing "${target}" SOURCES)
    # Keep original SDK state/functions in this same isolated source module.
    # Its hidden source sections avoid retaining uncalled reset/MEMCLR paths;
    # hardware, advancing IPC boot cursor and thread registry have one host owner.
    foreach(_source IN ITEMS
        src/RVL_SDK/fs/fs.c
        src/RVL_SDK/ipc/memory.c
        src/RVL_SDK/nand/NANDCore.c
        src/RVL_SDK/nand/NANDOpenClose.c
        src/RVL_SDK/nand/NANDCheck.c
        src/RVL_SDK/nand/nand.c
        src/RVL_SDK/os/OSReset.c)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_path}")
        endif()
    endforeach()
endfunction()

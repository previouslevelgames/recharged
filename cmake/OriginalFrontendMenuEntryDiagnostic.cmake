include_guard(GLOBAL)

option(MSCHARGED_DIAGNOSTIC_FRONTEND_MENU_ENTRY
    "Admit the original requested frontend state and movie task" OFF)

function(mscharged_select_original_frontend_menu_entry target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_MENU_ENTRY)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_FINALIZE
            MSCHARGED_DIAGNOSTIC_FRONTEND_WORLD_DRAW)
        if(NOT ${_required})
            message(FATAL_ERROR "Original frontend entry requires ${_required}")
        endif()
    endforeach()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_MENU_ENTRY=1)
    target_sources("${target}" PRIVATE
        "${MSCHARGED_PREPARED}/src/Game/Task/MovieRenderTask.cpp")
endfunction()

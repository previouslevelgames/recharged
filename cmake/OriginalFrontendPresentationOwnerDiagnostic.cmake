include_guard(GLOBAL)

option(MSCHARGED_DIAGNOSTIC_MAIN_PRESENTATION_OWNER
    "Admit original main presentation initialization" OFF)

# Default-off selected-source leaf. Call only after the genuine frontend
# allocator/filesystem/tweak and 0481 source network owner are admitted.
# This admits the original main GetPresentation statement, not script execution.
function(mscharged_add_original_frontend_presentation_owner_diagnostic target)
    if(NOT MSCHARGED_DIAGNOSTIC_MAIN_PRESENTATION_OWNER)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_MAIN_NETWORK_OWNER)
        message(FATAL_ERROR "Original presentation initialization requires the original network registry owner")
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Presentation owner diagnostic requires its existing original frontend target")
    endif()
    set(presentation_owner_sources
        "${MSCHARGED_PREPARED}/src/Game/Render/Presentation.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GameObjectLighting.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Render/StadiumLoading.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Transitions/ScreenTransitionManager.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Render/Jumbotron.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Render/MegastrikeBackgroundOverlay.cpp"
        "${MSCHARGED_PREPARED}/src/Game/SAnim/pnSAnimController.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Render/WorldNPC.cpp"
        "${MSCHARGED_PREPARED}/src/Game/WorldTriggers.cpp"
        "${MSCHARGED_PREPARED}/src/Game/AnimProps/goalieanimproperties.cpp"
        "${MSCHARGED_PREPARED}/src/Game/AnimProps/globalanimproperties.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/ShaderSkinMesh.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLSkinMesh.cpp"
    )
    get_target_property(existing_sources "${target}" SOURCES)
    get_target_property(owner_source_dir "${target}" SOURCE_DIR)
    set(existing_absolute_sources)
    foreach(existing_source IN LISTS existing_sources)
        get_filename_component(existing_absolute "${existing_source}" ABSOLUTE BASE_DIR "${owner_source_dir}")
        list(APPEND existing_absolute_sources "${existing_absolute}")
    endforeach()
    foreach(source IN LISTS presentation_owner_sources)
        if(NOT source IN_LIST existing_absolute_sources)
            target_sources("${target}" PRIVATE "${source}")
        endif()
    endforeach()
    target_compile_definitions("${target}" PRIVATE MSCHARGED_DIAGNOSTIC_MAIN_PRESENTATION_OWNER=1)
endfunction()

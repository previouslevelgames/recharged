include_guard(GLOBAL)
include(cmake/OriginalWorldOwners.cmake)

# Temporary admission of the original FE-to-game loading route (decomp patch
# 0610 onward): SuperLoadingScene, the authored match transition and the
# FELoadingToGame async services as their original providers qualify. The
# original scripts, handlers and task states own every request and poll.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_MATCH_LOADING
    "Admit the original FE-to-game match loading route" OFF)

function(mscharged_select_original_frontend_match_loading target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_MATCH_LOADING)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_DOMINATION
            MSCHARGED_DIAGNOSTIC_FRONTEND_FE_MODELS)
        if(NOT ${_required})
            message(FATAL_ERROR "Original match loading requires ${_required}")
        endif()
    endforeach()
    get_target_property(_match_type "${target}" TYPE)
    if(NOT _match_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original match loading belongs to the isolated original game module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_MATCH_LOADING=1)
    _mscharged_world_owned_sources("${target}" _match_known)
    # Original game/ODE profile: dNODEBUG=1 (cflags_game_common/cflags_ode)
    # and the decomp include/ ahead of src/ as in the original -i order, so
    # Game code's "ode/objects.h" is the public ODE header, not src/ode's.
    target_compile_definitions("${target}" PRIVATE dNODEBUG=1)
    target_include_directories("${target}" BEFORE PRIVATE "${MSCHARGED_PREPARED}/include")
    # Loading scenes plus the genuine gameplay providers the admitted async
    # services, match tasks and handlers reach: stadium/world, characters,
    # AI desires/scripts, physics/ODE, drawables, cameras, overlays, triggers.
    # Online, Cup and Hall of Fame scenes are not part of this offline route.
    foreach(_relative IN ITEMS
            src/Game/SH/SHLoading.cpp
            src/Game/AI/AISandbox.cpp
            src/Game/AI/AvoidController.cpp
            src/Game/AI/AvoidableObject.cpp
            src/Game/AI/Desire.cpp
            src/Game/AI/DesireCutAndBreak.cpp
            src/Game/AI/DesireDeke.cpp
            src/Game/AI/DesireGetInPosition.cpp
            src/Game/AI/DesireGetOpen.cpp
            src/Game/AI/DesireHit.cpp
            src/Game/AI/DesireInterceptBall.cpp
            src/Game/AI/DesireMark.cpp
            src/Game/AI/DesireMegaStrike.cpp
            src/Game/AI/DesirePass.cpp
            src/Game/AI/DesireReceivePass.cpp
            src/Game/AI/DesireRunToNet.cpp
            src/Game/AI/DesireShoot.cpp
            src/Game/AI/DesireSlideAttack.cpp
            src/Game/AI/DesireStatusEffects.cpp
            src/Game/AI/DesireSteering.cpp
            src/Game/AI/DesireSuperPower.cpp
            src/Game/AI/DesireUsePowerup.cpp
            src/Game/AI/DesireUserControlled.cpp
            src/Game/AI/FielderAbility.cpp
            src/Game/AI/FielderActions.cpp
            src/Game/AI/FielderDesireMachine.cpp
            src/Game/AI/FielderDesireTransitions.cpp
            src/Game/AI/Fuzzy.cpp
            src/Game/AI/GoalieActions.cpp
            src/Game/AI/GoalieLooseBall.cpp
            src/Game/AI/GoalieSave.cpp
            src/Game/AI/HeadTrack.cpp
            src/Game/AI/ScriptMachine.cpp
            src/Game/AI/ScriptActionQueue.cpp
            src/Game/AI/Scripts/FuzzyAIRuntime.cpp
            src/Game/AI/Scripts/ScriptCaching.cpp
            src/Game/AI/Scripts/ScriptDefines.cpp
            src/Game/AI/Scripts/ScriptQuestions.cpp
            src/Game/AI/ShotMeter.cpp
            src/Game/AI/SkillTweaks.cpp
            src/Game/AI/SpaceSearch.cpp
            src/Game/AI/TeamDesire.cpp
            src/Game/AI/TeamPlayMachine.cpp
            src/Game/AI/TransitionFunc.cpp
            src/Game/AI/TutorialMegastrikeDesire.cpp
            src/Game/AI/shdStateMachine.cpp
            src/Game/AI/tu_803115F4.cpp
            src/Game/Camera/AnimViewerCam.cpp
            src/Game/Camera/FaceCam.cpp
            src/Game/Camera/FollowCam.cpp
            src/Game/Camera/GameplayCam.cpp
            src/Game/Camera/GoalCam.cpp
            src/Game/Camera/MatrixEffectCam.cpp
            src/Game/Camera/ReplayCamera.cpp
            src/Game/Camera/ShootToScoreCam.cpp
            src/Game/Camera/TopDownCamera.cpp
            src/Game/Camera/kickoffcam.cpp
            src/Game/Character.cpp
            src/Game/CharacterEffects.cpp
            src/Game/CharacterLoader.cpp
            src/Game/CharacterTriggers.cpp
            src/Game/CharacterTweaks.cpp
            src/Game/CrowdRiot.cpp
            src/Game/Debug/Histogram.cpp
            src/Game/Debug/TimeRegions.cpp
            src/Game/Drawable/DrawableBall.cpp
            src/Game/Drawable/DrawableBirdoEgg.cpp
            src/Game/Drawable/DrawableBulletBill.cpp
            src/Game/Drawable/DrawableDaisyFist.cpp
            src/Game/Drawable/DrawableFlyingCamera.cpp
            src/Game/Drawable/DrawableHammer.cpp
            src/Game/Drawable/DrawableKoopaShell.cpp
            src/Game/Drawable/DrawableModel.cpp
            src/Game/Drawable/DrawableNetMesh.cpp
            src/Game/Drawable/DrawablePowerup.cpp
            src/Game/Drawable/DrawableThwomp.cpp
            src/Game/Drawable/DrawableYoshiEgg.cpp
            src/Game/Effects/PhotoFlashEffect.cpp
            src/Game/ExcitementSystem.cpp
            src/Game/FE/MatchSummary.cpp
            src/Game/FE/MenuListComponentInstantiations.cpp
            src/Game/FE/Overlay/OverlayHandlerChallengePreview.cpp
            src/Game/FE/Overlay/OverlayHandlerControllerMap.cpp
            src/Game/FE/Overlay/OverlayHandlerDefensivePlay.cpp
            src/Game/FE/Overlay/OverlayHandlerDemo.cpp
            src/Game/FE/Overlay/OverlayHandlerGoal.cpp
            src/Game/FE/Overlay/OverlayHandlerHUD.cpp
            src/Game/FE/Overlay/OverlayHandlerInGameText.cpp
            src/Game/FE/Overlay/OverlayHandlerMegaStrikeMeter.cpp
            src/Game/FE/Overlay/OverlayHandlerPIP.cpp
            src/Game/FE/Overlay/OverlayHandlerStrikerTimes.cpp
            src/Game/FE/Overlay/OverlayHandlerSuperAbility.cpp
            src/Game/FE/SHCrossFader.cpp
            src/Game/FE/feButtonComponent.cpp
            src/Game/FE/feSlideMenu.cpp
            src/Game/Formation.cpp
            src/Game/FormationDefines.cpp
            src/Game/GL/GLShadowBlendMeshWriter.cpp
            src/Game/Goalie.cpp
            src/Game/GoalieFatigue.cpp
            src/Game/GoalieTweaks.cpp
            src/Game/LANDiscoveryMessages.cpp
            src/Game/LANMessageRegistry.cpp
            src/Game/NetMeshEdge.cpp
            src/Game/NetMeshModelLoader.cpp
            src/Game/NetworkDiagnostics.cpp
            src/Game/NetworkMessageSerializer.cpp
            src/Game/PackedDetInput.cpp
            src/Game/Physics/CharacterPhysicsElement.cpp
            src/Game/Physics/CollisionSpace.cpp
            src/Game/Physics/LoadablePhysicsMesh.cpp
            src/Game/Physics/PhysicsBall.cpp
            src/Game/Physics/PhysicsBanana.cpp
            src/Game/Physics/PhysicsBirdoEgg.cpp
            src/Game/Physics/PhysicsBox.cpp
            src/Game/Physics/PhysicsBulletBill.cpp
            src/Game/Physics/PhysicsCapsule.cpp
            src/Game/Physics/PhysicsCharacter.cpp
            src/Game/Physics/PhysicsCharacterBase.cpp
            src/Game/Physics/PhysicsCharacterBaseData.cpp
            src/Game/Physics/PhysicsColumn.cpp
            src/Game/Physics/PhysicsCompositeObject.cpp
            src/Game/Physics/PhysicsCylinder.cpp
            src/Game/Physics/PhysicsFakeBall.cpp
            src/Game/Physics/PhysicsFinitePlane.cpp
            src/Game/Physics/PhysicsGoalie.cpp
            src/Game/Physics/PhysicsGroundPlane.cpp
            src/Game/Physics/PhysicsHammer.cpp
            src/Game/Physics/PhysicsKoopaShell.cpp
            src/Game/Physics/PhysicsNPC.cpp
            src/Game/Physics/PhysicsNet.cpp
            src/Game/Physics/PhysicsObject.cpp
            src/Game/Physics/PhysicsPlane.cpp
            src/Game/Physics/PhysicsRoundedCorner.cpp
            src/Game/Physics/PhysicsShell.cpp
            src/Game/Physics/PhysicsShockwave.cpp
            src/Game/Physics/PhysicsSphere.cpp
            src/Game/Physics/PhysicsThwomp.cpp
            src/Game/Physics/PhysicsTransform.cpp
            src/Game/Physics/PhysicsTriggerVolume.cpp
            src/Game/Physics/PhysicsWall.cpp
            src/Game/Physics/PhysicsWaluigiWall.cpp
            src/Game/Physics/PhysicsWorld.cpp
            src/Game/Physics/PhysicsYoshiEgg.cpp
            src/Game/PhysicsAIBall.cpp
            src/Game/Player.cpp
            src/Game/Render/AttackSideIndicators.cpp
            src/Game/Render/BirdoEgg.cpp
            src/Game/Render/BulletBill.cpp
            src/Game/Render/ChainChomp.cpp
            src/Game/Render/ChargeShadowDrawable.cpp
            src/Game/Render/DaisyFist.cpp
            src/Game/Render/DiddyBanana.cpp
            src/Game/Render/ElectricFence.cpp
            src/Game/Render/FlareHandler.cpp
            src/Game/Render/FlyingCamera.cpp
            src/Game/Render/HammerObject.cpp
            src/Game/Render/HomeButtonFade.cpp
            src/Game/Render/Indicators.cpp
            src/Game/Render/KoopaShellObject.cpp
            src/Game/Render/MegaBallIndicators.cpp
            src/Game/Render/PlanarShadowDrawable.cpp
            src/Game/Render/ShootToScoreArrow.cpp
            src/Game/Render/SkinAnimatedMovableNPC.cpp
            src/Game/Render/StadiumTweaks.cpp
            src/Game/Render/ThwompObject.cpp
            src/Game/Render/WarbleOwner.cpp
            src/Game/Render/WindDebris.cpp
            src/Game/Render/WindDebrisConfig.cpp
            src/Game/Render/Wiper.cpp
            src/Game/Render/YoshiEggObject.cpp
            src/Game/Replay.cpp
            src/Game/ReplayChoreo.cpp
            src/Game/ReplayManager.cpp
            src/Game/SAnim/pnFeather.cpp
            src/Game/SAnim/pnScaleBlender.cpp
            src/Game/SAnim/pnSingleAxisBlender.cpp
            src/Game/SH/SHGameResults.cpp
            src/Game/SH/SHPause.cpp
            src/Game/SH/SHPausePostGame.cpp
            src/Game/ScriptTuning.cpp
            src/Game/Sys/clock.cpp
            src/Game/Sys/tweak.cpp
            src/Game/Task/DispatchEventsTask.cpp
            src/Game/Task/ParticleUpdateCallbacks.cpp
            src/Game/Task/TextWindowTask.cpp
            src/Game/Task/TransitionTask.cpp
            src/Game/Task/WorldUpdateTask.cpp
            src/Game/Terrain.cpp
            src/Game/TerrainTweaks.cpp
            src/Game/Transitions/ColourBlendScreenTransition.cpp
            src/Game/Transitions/ScriptedTransition.cpp
            src/Game/Transitions/TransitionSequence.cpp
            src/Game/Triggers/AnimTagScript.cpp
            src/Game/Triggers/AnimTrigger.cpp
            src/Game/Triggers/BinaryTriggerFile.cpp
            src/Game/Triggers/SebringAnimScript.cpp
            src/Game/TweaksBase.cpp
            src/Game/Weather.cpp
            src/Game/WeatherData.cpp
            src/Game/tu_8013E2EC.cpp
            src/NL/PointerEntryTable.cpp
            src/NL/gl/glMultiTextureModelWriter.cpp
            src/NL/gl/glShadowedTexturedColourModelWriter.cpp
            src/NL/glx/glxLight.cpp
            src/NL/nlAllocatorStack.cpp
            src/NL/nlDebugViews.cpp
            src/NL/nlEndian.cpp
            src/NL/nlIntersection.cpp
            src/NL/nlPolygonRegion.cpp
            src/NL/nlTimer.cpp
            src/NL/polar.cpp
            src/ode/NLGAdditions.cpp
            src/ode/body_debug.cpp
            src/ode/collision_kernel.cpp
            src/ode/collision_space.cpp
            src/ode/collision_std.cpp
            src/ode/collision_transform.cpp
            src/ode/collision_util.cpp
            src/ode/dCylinder.cpp
            src/ode/error.cpp
            src/ode/ext/dColumn.cpp
            src/ode/ext/dFinitePlane.cpp
            src/ode/ext/dRoundedCorner.cpp
            src/ode/joint.cpp
            src/ode/mass.cpp
            src/ode/matrix.cpp
            src/ode/memory.cpp
            src/ode/obstack.cpp
            src/ode/ode.cpp
            src/ode/odemath.cpp
            src/ode/quickstep.cpp
            src/ode/rotation.cpp
            src/ode/util.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _match_known)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _match_known "${_source}")
        endif()
    endforeach()
    # Native representation of the original binary animation trigger files and
    # character physics records, and the MWCC runtime helpers reconstructed
    # source calls explicitly.
    get_target_property(_match_sources "${target}" SOURCES)
    foreach(_port_source IN ITEMS
            src/platform/trigger_file_transport.cpp
            src/platform/character_physics_wire.cpp
            src/platform/mwcc_runtime_helpers.cpp)
        if(NOT "${_port_source}" IN_LIST _match_sources)
            target_sources("${target}" PRIVATE "${_port_source}")
        endif()
    endforeach()
    # NisPlayer's original NIS load queue queries the console type through the
    # native disc boot identity's retail console.
    target_link_libraries(charged_original_main_credits_host PRIVATE
        charged_native_os_shutdown_requests)
    mscharged_require_original_host_symbol(charged_original_main_credits_host
        INTERFACE OSGetConsoleType)
    # MatchLoadingScene, the HUD clock, the goal overlay's time/score and the
    # in-game text spacing format Wii16 L"..." literals through the original
    # 16-bit formatter/strings; these TUs make no host wide-character calls.
    # Other TUs retain their existing native wchar width.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        foreach(_match_wii16 IN ITEMS
                src/Game/SH/SHLoading.cpp src/Game/FE/Overlay/OverlayHandlerHUD.cpp
                src/Game/FE/Overlay/OverlayHandlerGoal.cpp src/Game/FE/Overlay/OverlayHandlerInGameText.cpp)
            set_property(SOURCE "${MSCHARGED_PREPARED}/${_match_wii16}"
                APPEND PROPERTY COMPILE_OPTIONS -fshort-wchar)
        endforeach()
    endif()
endfunction()

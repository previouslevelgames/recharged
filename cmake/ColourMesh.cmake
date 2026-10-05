include_guard(GLOBAL)
# Original short-UV, vertex-colour mesh writer shared by particles and fonts.
add_library(charged_colour_mesh STATIC
    "${MSCHARGED_PREPARED}/src/Game/GL/GLTexturedColourMeshWriter.cpp"
    "${MSCHARGED_PREPARED}/src/Game/GL/GLFloatTexturedColourMeshWriter.cpp")
add_dependencies(charged_colour_mesh verify_prepared)
# Historical previews keep their bookkeeping/checks in a separate object ABI.
# Original game objects compile the real writer layout and source requests.
target_compile_definitions(charged_colour_mesh PUBLIC MSCHARGED_DIAGNOSTIC_MESH_WRITERS=1)
target_link_libraries(charged_colour_mesh PUBLIC charged_shadows)
target_compile_features(charged_colour_mesh PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    target_compile_options(charged_colour_mesh PRIVATE -ffp-contract=off)
endif()

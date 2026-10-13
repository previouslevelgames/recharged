# Binary shader compilers are explicit, checksum-pinned SDK dependencies.
# This file never downloads; run tools/setup_windows_graphics.py first.
set(MSCHARGED_WINDOWS_GRAPHICS_DIR "${CMAKE_SOURCE_DIR}/build/windows-graphics"
    CACHE PATH "Verified x64 Windows shader compiler runtime directory")
set(_graphics_check "${CMAKE_SOURCE_DIR}/tools/setup_windows_graphics.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${_graphics_check}"
    --output-dir "${MSCHARGED_WINDOWS_GRAPHICS_DIR}" --check
    RESULT_VARIABLE _graphics_status OUTPUT_VARIABLE _graphics_output ERROR_VARIABLE _graphics_error)
if(NOT _graphics_status EQUAL 0)
    message(FATAL_ERROR "${_graphics_output}${_graphics_error}")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    message(FATAL_ERROR "The pinned Windows graphics runtimes are x86_64 only")
endif()

# Dawn loads DXC dynamically. Supply the runtime without compiling DXC's LLVM
# source tree or introducing a link-time dependency on dxcompiler.dll.
set(DAWN_DXC_RUNTIME_DIR "${MSCHARGED_WINDOWS_GRAPHICS_DIR}")
set(DAWN_USE_BUILT_DXC OFF)
# Use the D3D12 runtime supplied by Windows; no Agility SDK DLLs are packaged.
set(DAWN_USE_AGILITY_SDK OFF)
set(DAWN_FORCE_SYSTEM_COMPONENT_LOAD OFF)
set(_graphics_output_dir "${CMAKE_BINARY_DIR}")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND _graphics_output_dir "/$<CONFIG>")
endif()
add_custom_target(copy_d3dcompiler_dll
    COMMAND "${Python3_EXECUTABLE}" -B "${_graphics_check}"
        --output-dir "${MSCHARGED_WINDOWS_GRAPHICS_DIR}" --check
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_graphics_output_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${MSCHARGED_WINDOWS_GRAPHICS_DIR}/dxcompiler.dll"
        "${MSCHARGED_WINDOWS_GRAPHICS_DIR}/dxil.dll"
        "${MSCHARGED_WINDOWS_GRAPHICS_DIR}/d3dcompiler_47.dll"
        "${_graphics_output_dir}"
    COMMENT "Verify and copy Windows shader compiler DLLs"
    VERBATIM)

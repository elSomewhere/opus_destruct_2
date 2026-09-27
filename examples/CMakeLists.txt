# Minimal hosts of the physics core (they link svx_core only).
add_executable(svx_core_minimal core_minimal/main.cpp)
target_link_libraries(svx_core_minimal PRIVATE svx_core)
svx_wasm_exe_options(svx_core_minimal)

# The same from plain C, through the C API (svx/svx_core.h).
enable_language(C)
add_executable(svx_core_c c_api/main.c)
set_target_properties(svx_core_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
target_link_libraries(svx_core_c PRIVATE svx_core)
svx_wasm_exe_options(svx_core_c)

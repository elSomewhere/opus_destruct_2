# A minimal host of the physics core (links svx_core only).
add_executable(svx_core_minimal core_minimal/main.cpp)
target_link_libraries(svx_core_minimal PRIVATE svx_core)
svx_wasm_exe_options(svx_core_minimal)

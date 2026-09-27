add_executable(svx_content_study content_study/main.cpp)
target_link_libraries(svx_content_study PRIVATE svx_core)
svx_wasm_exe_options(svx_content_study)

# Matrix-free lattice kernel throughput benchmark (research/wasmbench, plan §A5).
add_executable(svx_bench_kernel bench/lattice_kernel_bench.cpp)
target_link_libraries(svx_bench_kernel PRIVATE svx_flags)
if(SVX_WASM)
  target_link_options(svx_bench_kernel PRIVATE -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sINITIAL_MEMORY=768MB)
endif()
if(NOT SVX_WASM OR SVX_WASM_THREADS)
  find_package(Threads REQUIRED)
  target_link_libraries(svx_bench_kernel PRIVATE Threads::Threads)
endif()

# Event bubble step cost on the rooms world, in thread CPU time (plan Phase 2 step gate).
add_executable(svx_step_bench bench/step_bench.cpp)
target_link_libraries(svx_step_bench PRIVATE svx_core)
svx_wasm_exe_options(svx_step_bench)

# Streaming cost while flying through the procedural city (plan Phase 6 hitch gate).
add_executable(svx_stream_bench bench/stream_bench.cpp)
target_link_libraries(svx_stream_bench PRIVATE svx_core)
svx_wasm_exe_options(svx_stream_bench)

add_executable(svx_wall_slenderness wall_slenderness/main.cpp)
target_link_libraries(svx_wall_slenderness PRIVATE svx_core)
svx_wasm_exe_options(svx_wall_slenderness)

# Golden parity against the prototype oracle fixtures (native only; uses nlohmann/json).
if(NOT SVX_WASM)
  add_executable(svx_fixture_compare fixture_compare/main.cpp)
  target_include_directories(svx_fixture_compare PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
  target_link_libraries(svx_fixture_compare PRIVATE svx_core)
endif()

# Doom WAD graphics: palette, TEXTURE1/2 composites and flats -> counts + PNG dump (plan §B10).
add_executable(svx_wad_textures wad_textures/main.cpp)
target_link_libraries(svx_wad_textures PRIVATE svx_core)
svx_wasm_exe_options(svx_wad_textures)

# Scenario library: static closure (+DIF) vs implicit dynamics, S invariance (plan Phase 1 gates).
add_executable(svx_sim_library sim_library/main.cpp)
target_link_libraries(svx_sim_library PRIVATE svx_core)
svx_wasm_exe_options(svx_sim_library)

# Composite (telescoping bubble) accuracy vs full fine solves (plan §A5 / Phase 2).
add_executable(svx_composite_study composite_study/main.cpp)
target_link_libraries(svx_composite_study PRIVATE svx_core)
svx_wasm_exe_options(svx_composite_study)

# Event bubble (composite dynamics) vs full-fine dynamics truth (plan Phase 2 gates).
add_executable(svx_bubble_study bubble_study/main.cpp)
target_link_libraries(svx_bubble_study PRIVATE svx_core)
svx_wasm_exe_options(svx_bubble_study)

# Headless end-to-end engine run (plan Phase 3).
add_executable(svx_engine_demo engine_demo/main.cpp)
target_link_libraries(svx_engine_demo PRIVATE svx_core)
svx_wasm_exe_options(svx_engine_demo)

# Record / replay sessions from command logs (plan Phase 3 determinism gate, Phase 7 lockstep).
add_executable(svx_replay replay/main.cpp)
target_link_libraries(svx_replay PRIVATE svx_core)
svx_wasm_exe_options(svx_replay)

# Freedoom import / bake / stability check of every map (plan Phase 5 gate).
add_executable(svx_map_check map_check/main.cpp)
target_link_libraries(svx_map_check PRIVATE svx_core)
svx_wasm_exe_options(svx_map_check)

# Browser engine module for the web worker (docs/API.md): ES6 module, written into
# web/src/wasm/ so the Vite worker bundle imports it directly (generated, gitignored).
if(SVX_WASM)
  add_executable(svx_web wasm_module/module.cpp)
  target_link_libraries(svx_web PRIVATE svx_core)
  set(_svx_exports
    _svx_create _svx_destroy _svx_set_threads _svx_set_params _svx_load_procedural _svx_load_wad
    _svx_last_error _svx_save_delta _svx_load_delta _svx_modified _svx_texture_count _svx_texture_size _svx_texture_name _svx_texture_rgba _svx_bake
    _svx_world_info _svx_tick _svx_viewer _svx_carve _svx_blast _svx_use _svx_raycast _svx_collide _svx_poll_meshes
    _svx_mesh_info _svx_mesh_vertices _svx_mesh_indices _svx_poll_removed _svx_removed_chunk _svx_chunk_occupancy
    _svx_poll_events _svx_event_info _svx_event_vertices _svx_event_indices _svx_stats
    _svx_debris _svx_debris_data _svx_set_debris _svx_set_gpu_displacement _svx_poll_fields
    _svx_field_info _svx_field_data _svx_poll_far _svx_far_info _svx_far_vertices _svx_far_indices
    _svx_poll_far_removed _svx_far_removed _svx_state_hash _malloc _free)
  string(REPLACE ";" "," _svx_exports_csv "${_svx_exports}")
  target_link_options(svx_web PRIVATE
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createSvxModule -sENVIRONMENT=worker
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB
    -sFILESYSTEM=0 --no-entry -sDYNAMIC_EXECUTION=0
    "-sEXPORTED_FUNCTIONS=[${_svx_exports_csv}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPU8,HEAPU32,HEAP32,HEAPF64,UTF8ToString,stringToUTF8,lengthBytesUTF8]")
  set_target_properties(svx_web PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/web/src/wasm
    SUFFIX ".js")
endif()

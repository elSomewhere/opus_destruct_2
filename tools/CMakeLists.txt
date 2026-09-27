# Streaming cost while flying through the procedural city.
add_executable(svx_stream_bench bench/stream_bench.cpp)
target_link_libraries(svx_stream_bench PRIVATE svx_game)
svx_wasm_exe_options(svx_stream_bench)

# Doom WAD graphics: palette, TEXTURE1/2 composites and flats -> counts + PNG dump.
add_executable(svx_wad_textures wad_textures/main.cpp)
target_link_libraries(svx_wad_textures PRIVATE svx_game)
svx_wasm_exe_options(svx_wad_textures)

# Headless end-to-end engine run: scenario scripts (tower, rooms, city, WAD), stats, frames.
add_executable(svx_engine_demo engine_demo/main.cpp)
target_link_libraries(svx_engine_demo PRIVATE svx_game)
svx_wasm_exe_options(svx_engine_demo)

# Record / replay sessions from command logs (determinism).
add_executable(svx_replay replay/main.cpp)
target_link_libraries(svx_replay PRIVATE svx_game)
svx_wasm_exe_options(svx_replay)

# Freedoom import / design / stability check of every map.
add_executable(svx_map_check map_check/main.cpp)
target_link_libraries(svx_map_check PRIVATE svx_game)
svx_wasm_exe_options(svx_map_check)

# Browser engine module for the web worker (docs/API.md): ES6 module, written into
# web/src/wasm/ so the Vite worker bundle imports it directly (generated, gitignored).
if(SVX_WASM)
  add_executable(svx_web wasm_module/module.cpp)
  target_link_libraries(svx_web PRIVATE svx_game)
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

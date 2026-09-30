if(SVX_BUILD_GAME)
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
    _svx_poll_removed_grid _svx_removed_grid_chunk _svx_grid_chunk_occupancy _svx_poll_grids _svx_grid_info _svx_poll_grids_removed
    _svx_grid_removed _svx_poll_joints _svx_joint_info
    _svx_poll_events _svx_event_info _svx_event_vertices _svx_event_indices _svx_event_occupancy _svx_stats
    _svx_debris _svx_debris_data _svx_set_debris _svx_set_gpu_displacement _svx_poll_fields
    _svx_field_info _svx_field_data _svx_poll_far _svx_far_info _svx_far_vertices _svx_far_indices
    _svx_poll_far_removed _svx_far_removed _svx_state_hash _svx_ignite _svx_extinguish _svx_poll_env _svx_env_flames _svx_env_smoke_count _svx_env_smoke
    _svx_pour _svx_drain _svx_heat _svx_set_env _svx_get_env _svx_env_param_count _svx_env_param_name _svx_env_param_range
    _svx_set_tunable _svx_get_tunable _svx_tunable_count _svx_tunable_name _svx_tunable_setup _svx_stats_count _svx_poll_water _svx_water_info _svx_water_vertices _svx_water_indices _svx_poll_water_removed _svx_water_removed
    _svx_spawn_vehicle _svx_remove_vehicle _svx_enter_vehicle _svx_exit_vehicle _svx_player_vehicle _svx_drive _svx_vehicle_near
    _svx_vehicles _svx_vehicles_data _svx_wheels _svx_wheels_data _svx_shoot _svx_set_traffic
    _svx_set_pedestrians _svx_poll_character_meshes _svx_character_mesh_info _svx_character_mesh_vertices
    _svx_character_mesh_indices _svx_poll_character_meshes_removed _svx_character_mesh_removed
    _svx_poll_character_palettes _svx_character_palette _svx_characters _svx_characters_data
    _svx_characters_skin _svx_characters_prop _svx_raycast_shot _svx_wound_character
    _malloc _free)
  string(REPLACE ";" "," _svx_exports_csv "${_svx_exports}")
  target_link_options(svx_web PRIVATE
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createSvxModule -sENVIRONMENT=worker
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB
    -sFILESYSTEM=0 --no-entry -sDYNAMIC_EXECUTION=0
    "-sEXPORTED_FUNCTIONS=[${_svx_exports_csv}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPU8,HEAPU32,HEAP32,HEAPF32,HEAPF64,UTF8ToString,stringToUTF8,lengthBytesUTF8]")
  set_target_properties(svx_web PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/web/src/wasm
    SUFFIX ".js")
endif()

# Long sessions and their memory: streamed city crossings, bounded levels (World::memory).
add_executable(svx_soak soak/main.cpp)
target_link_libraries(svx_soak PRIVATE svx_game)
svx_wasm_exe_options(svx_soak)

add_executable(svx_env_bench env_bench/main.cpp)
target_link_libraries(svx_env_bench PRIVATE svx_game)
svx_wasm_exe_options(svx_env_bench)
endif()  # SVX_BUILD_GAME

# The physics core alone for JavaScript hosts (docs/CORE.md): the C API of svx/svx_core.h.
if(SVX_WASM)
  add_executable(svx_core_web wasm_core/module.cpp)
  target_link_libraries(svx_core_web PRIVATE svx_core)
  set(_svxc_exports
    _svxc_apply_impulse _svxc_bake _svxc_blast _svxc_carve _svxc_chunk_voxels _svxc_collide _svxc_create _svxc_destroy _svxc_enable_streaming _svxc_event_at _svxc_get _svxc_get_stats _svxc_load_box _svxc_load_delta _svxc_material_find _svxc_material_get _svxc_material_set _svxc_materials_reset _svxc_modified _svxc_piece_at _svxc_piece_voxels _svxc_poll_changed_chunks _svxc_poll_events _svxc_poll_evicted_chunks _svxc_poll_pieces _svxc_raycast _svxc_remove_piece _svxc_save_delta _svxc_session_hash _svxc_set _svxc_set_focus _svxc_set_threads _svxc_set_voxels _svxc_state_hash _svxc_tick _svxc_add_layer _svxc_layer_index _svxc_set_layer _svxc_layer _svxc_set_piece_layer _svxc_piece_layer _svxc_remove_piece_voxels _svxc_set_loads _svxc_apply_force _svxc_wake_piece _svxc_add_system _svxc_add_system_ex _svxc_chunk_layer _svxc_poll_layer_changes
    _svxc_add_grid _svxc_add_grid_desc _svxc_remove_grid _svxc_grids _svxc_grid_frame _svxc_set_grid_frame _svxc_grid_voxel_size _svxc_grid_priority _svxc_grid_body _svxc_grid_velocity _svxc_set_grid_voxels _svxc_grid_chunk_voxels _svxc_poll_changed_grid_chunks _svxc_piece_shape_count _svxc_piece_shape _svxc_sweep _svxc_collide_ex _svxc_sweep_ex
    _svxc_add_kinematic _svxc_remove_kinematic _svxc_drive_kinematic _svxc_set_kinematic_velocity _svxc_kinematic _svxc_kinematics
    _svxc_joint_defaults _svxc_add_joint _svxc_remove_joint _svxc_set_joint_motor _svxc_set_joint_limits _svxc_joint _svxc_joints
    _malloc _free)
  string(REPLACE ";" "," _svxc_exports_csv "${_svxc_exports}")
  target_link_options(svx_core_web PRIVATE
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createSvxCore -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB
    -sFILESYSTEM=0 --no-entry -sDYNAMIC_EXECUTION=0 -sALLOW_TABLE_GROWTH=1
    "-sEXPORTED_FUNCTIONS=[${_svxc_exports_csv}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPU8,HEAP32,HEAPF64,addFunction,UTF8ToString,stringToUTF8,lengthBytesUTF8]")
  set_target_properties(svx_core_web PROPERTIES SUFFIX ".js")
endif()

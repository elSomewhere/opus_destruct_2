# Core tests: link the physics core alone (no mesh, no game): the core must stand on its own.
add_executable(svx_core_tests
  test_main.cpp
  core/test_stress.cpp
  core/test_fragments.cpp
  core/test_rigid.cpp
  core/test_world.cpp
  core/test_capi.cpp
  core/test_ext.cpp
  core/test_grids.cpp
  core/test_machines.cpp
  core/test_joints.cpp
  core/test_wheels.cpp
  core/test_crumple.cpp
  core/test_dmath.cpp
  core/test_articulations.cpp
  core/test_streaming.cpp
  core/test_columns.cpp
  core/test_seams.cpp
  core/test_config.cpp)
target_include_directories(svx_core_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(svx_core_tests PRIVATE svx_core)
svx_wasm_exe_options(svx_core_tests)
add_test(NAME svx_core_tests COMMAND svx_core_tests)

# Environment tests: fire, smoke, water on the core (no mesh, no game).
if(SVX_BUILD_ENV)
  add_executable(svx_env_tests
    test_main.cpp
    env/test_fire.cpp
    env/test_smoke.cpp
    env/test_water.cpp)
  target_include_directories(svx_env_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
  target_link_libraries(svx_env_tests PRIVATE svx_env)
  svx_wasm_exe_options(svx_env_tests)
  add_test(NAME svx_env_tests COMMAND svx_env_tests)
endif()

# Character tests: motion, bodies (the core's articulations and the body of its own), behaviours,
# voxel models (no mesh, no game).
if(SVX_BUILD_ANIM)
  file(GLOB SVX_ANIM_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/anim/*.cpp)
  add_executable(svx_anim_tests test_main.cpp ${SVX_ANIM_TEST_SOURCES})
  target_include_directories(svx_anim_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
  target_link_libraries(svx_anim_tests PRIVATE svx_anim)
  svx_wasm_exe_options(svx_anim_tests)
  add_test(NAME svx_anim_tests COMMAND svx_anim_tests)
endif()

# City generator tests: the port of voxel_city against its reference, stage by stage (records
# whose digests tests/city/conformance.txt holds), and the reference's own golden samples.
if(SVX_BUILD_CITY)
  file(GLOB SVX_CITY_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/city/*.cpp)
  add_executable(svx_city_tests test_main.cpp ${SVX_CITY_TEST_SOURCES})
  target_include_directories(svx_city_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest
                             ${PROJECT_SOURCE_DIR}/level/city/src ${CMAKE_CURRENT_SOURCE_DIR}/city)
  target_link_libraries(svx_city_tests PRIVATE svx_city)
  target_compile_definitions(svx_city_tests PRIVATE SVX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  svx_wasm_exe_options(svx_city_tests)
  add_test(NAME svx_city_tests COMMAND svx_city_tests)
endif()

# Game harness tests: levels (procedural, city, Doom), movers, replays, meshing, the C ABI.
if(NOT SVX_BUILD_GAME)
  return()
endif()
add_executable(svx_game_tests
  test_main.cpp
  game/test_collapse.cpp
  game/test_columns.cpp
  game/test_doom.cpp
  game/test_doom_textures.cpp
  game/test_game.cpp
  game/test_movers.cpp
  game/test_replay.cpp
  game/test_vehicles.cpp
  game/test_drive_city.cpp
  game/test_pedestrians.cpp
  game/test_api_people.cpp)
target_include_directories(svx_game_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(svx_game_tests PRIVATE svx_game svx_procgen svx_api)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # (GCC 13 takes a pair's copy of a one-element vector in a nested initializer list - the
  # synthetic WAD builders' patches - for an out-of-bounds write: a known false positive)
  set_source_files_properties(game/test_doom_textures.cpp PROPERTIES COMPILE_OPTIONS "-Wno-array-bounds;-Wno-stringop-overflow")
endif()
target_compile_definitions(svx_game_tests PRIVATE SVX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
svx_wasm_exe_options(svx_game_tests)
add_test(NAME svx_game_tests COMMAND svx_game_tests)

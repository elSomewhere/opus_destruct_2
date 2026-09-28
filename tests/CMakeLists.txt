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
  core/test_kinematic.cpp
  core/test_joints.cpp
  core/test_dmath.cpp)
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

# Game harness tests: levels (procedural, city, Doom), movers, replays, meshing.
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
  game/test_replay.cpp)
target_include_directories(svx_game_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(svx_game_tests PRIVATE svx_game)
target_compile_definitions(svx_game_tests PRIVATE SVX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
svx_wasm_exe_options(svx_game_tests)
add_test(NAME svx_game_tests COMMAND svx_game_tests)

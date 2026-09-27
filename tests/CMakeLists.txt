add_executable(svx_tests
  test_main.cpp
  test_columns.cpp
  test_doom.cpp
  test_doom_textures.cpp
  test_dmath.cpp
  test_stress.cpp
  test_fragments.cpp
  test_rigid.cpp
  test_engine.cpp
  test_movers.cpp
  test_replay.cpp)
target_include_directories(svx_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(svx_tests PRIVATE svx_core)
target_compile_definitions(svx_tests PRIVATE SVX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
svx_wasm_exe_options(svx_tests)
add_test(NAME svx_tests COMMAND svx_tests)

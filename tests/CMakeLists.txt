add_executable(svx_tests
  test_main.cpp
  test_lattice.cpp
  test_multigrid.cpp
  test_columns.cpp
  test_doom.cpp
  test_doom_textures.cpp
  test_sim.cpp
  test_engine.cpp
  test_debris.cpp
  test_dmath.cpp
  test_replay.cpp
  test_contact.cpp
  test_movers.cpp
  test_coarse.cpp
  test_composite.cpp
  test_bubble.cpp
  test_collapse.cpp)
target_include_directories(svx_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(svx_tests PRIVATE svx_core)
target_compile_definitions(svx_tests PRIVATE SVX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
svx_wasm_exe_options(svx_tests)
add_test(NAME svx_tests COMMAND svx_tests)

# Golden parity vs the prototype oracle (tests/fixtures/prototype, svx-fixture-v1).
if(NOT SVX_WASM)
  set(_fx ${PROJECT_SOURCE_DIR}/tests/fixtures/prototype)
  file(GLOB _fx_all ${_fx}/*.json)
  list(REMOVE_ITEM _fx_all ${_fx}/index.json)
  # Static equilibrium (corotational) vs the oracle's static block, fixture k0 injected.
  add_test(NAME golden_parity_static COMMAND svx_fixture_compare --variant prototype --tol 1e-4 ${_fx_all})
  # structvox's own section / profile model (no injection), every variant.
  foreach(_v prototype fixY fixJ fixYJ)
    add_test(NAME golden_parity_model_${_v} COMMAND svx_fixture_compare --variant ${_v} --k0 model --tol 1e-4 ${_fx_all})
  endforeach()
  # Outcomes (blast prefracture + static closure): gated scenes must match exactly.
  add_test(NAME golden_outcome_fixture_law COMMAND svx_fixture_compare --outcome --variant prototype ${_fx_all})
  foreach(_v prototype fixYJ)
    add_test(NAME golden_outcome_model_${_v}
      COMMAND svx_fixture_compare --outcome --k0 model --law model --variant ${_v} ${_fx_all})
  endforeach()
endif()

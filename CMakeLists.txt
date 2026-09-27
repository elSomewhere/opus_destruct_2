cmake_minimum_required(VERSION 3.24)
project(structvox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Determinism + portability flags (plan §B9): no fast-math, no FP contraction.
# ---------------------------------------------------------------------------
add_library(svx_flags INTERFACE)
target_compile_options(svx_flags INTERFACE
  -Wall -Wextra -Wno-unused-parameter
  -ffp-contract=off -fno-fast-math)

if(EMSCRIPTEN)
  set(SVX_WASM ON)
  target_compile_options(svx_flags INTERFACE -msimd128)
  target_link_options(svx_flags INTERFACE -msimd128)
  option(SVX_WASM_THREADS "Build WASM with pthreads" OFF)
  if(SVX_WASM_THREADS)
    target_compile_options(svx_flags INTERFACE -pthread)
    target_link_options(svx_flags INTERFACE -pthread -sPTHREAD_POOL_SIZE=16)
  endif()
else()
  set(SVX_WASM OFF)
endif()

# ---------------------------------------------------------------------------------------------
# Libraries (docs/CORE.md):
#   svx_core  the destruction physics engine: voxel grid, materials, fragments, structures,
#             stress solver, rigid pieces, streaming, persistence. Generic: no rendering, no
#             levels, no players. Depends on nothing but the C++20 standard library.
#   svx_mesh  voxel meshing (greedy chunk meshes, piece meshes) for renderers. Depends on core.
#   svx_game  the prototype game harness: Game (movers, viewer, far tier, command log, piece
#             meshes), procedural and city levels, Doom WAD import, the web worker C ABI.
# No exceptions / RTTI are needed by the libraries.
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE SVX_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.cpp)
add_library(svx_core STATIC ${SVX_CORE_SOURCES})
target_include_directories(svx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_link_libraries(svx_core PUBLIC svx_flags)
target_compile_options(svx_core PRIVATE -fno-exceptions -fno-rtti)

file(GLOB_RECURSE SVX_MESH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mesh/src/*.cpp)
add_library(svx_mesh STATIC ${SVX_MESH_SOURCES})
target_include_directories(svx_mesh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/mesh/include)
target_link_libraries(svx_mesh PUBLIC svx_core)
target_compile_options(svx_mesh PRIVATE -fno-exceptions -fno-rtti)

file(GLOB_RECURSE SVX_GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/game/src/*.cpp)
add_library(svx_game STATIC ${SVX_GAME_SOURCES})
target_include_directories(svx_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/game/include)
target_link_libraries(svx_game PUBLIC svx_core svx_mesh)
target_compile_options(svx_game PRIVATE -fno-exceptions -fno-rtti)

# Helper for executables that must run under node when cross-compiled to WASM.
function(svx_wasm_exe_options target)
  if(SVX_WASM)
    target_link_options(${target} PRIVATE
      -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
      -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB -sNODERAWFS=1)
  endif()
endfunction()

add_subdirectory(examples)
add_subdirectory(tools)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

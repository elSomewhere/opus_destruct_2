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
target_compile_options(svx_flags INTERFACE -ffp-contract=off -fno-fast-math)
# (warnings: this project's own targets only, never a consumer's)
set(SVX_WARNINGS -Wall -Wextra -Wno-unused-parameter)

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
#   svx_env   environment systems on the core's extension points: fire, smoke, water
#             (docs/ENV.md). Depends on the core's public API only.
#   svx_anim  characters (docs/ANIM.md): voxel people with a motion plan, a physical body of the
#             core's articulations (or a body of its own), and behaviours between them.
#             Depends on the core's public API only.
#   svx_game  the prototype game harness: Game (movers, viewer, far tier, command log, piece
#             meshes), procedural and city levels, Doom WAD import, the web worker C ABI.
# No exceptions / RTTI are needed by the libraries.
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE SVX_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.cpp)
add_library(svx_core STATIC ${SVX_CORE_SOURCES})
target_include_directories(svx_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(svx_core PUBLIC svx_flags)
target_compile_options(svx_core PRIVATE -fno-exceptions -fno-rtti ${SVX_WARNINGS})
option(SVX_DIAGNOSTICS "SVX_* environment diagnostics (printing only) in the core" ON)
if(NOT SVX_DIAGNOSTICS)
  target_compile_definitions(svx_core PUBLIC SVX_NO_DIAGNOSTICS)
endif()

# A project that adds this one as a subdirectory gets the core (and the mesher) only, unless it
# asks for more.
option(SVX_BUILD_MESH "svx_mesh (voxel meshing for renderers)" ON)
option(SVX_BUILD_ENV "svx_env (fire, smoke, water)" ON)
option(SVX_BUILD_ANIM "svx_anim (characters: motion, bodies, behaviours)" ON)
option(SVX_BUILD_GAME "the game harness, its tools and the web module" ${PROJECT_IS_TOP_LEVEL})
option(SVX_BUILD_EXAMPLES "the core's example hosts" ${PROJECT_IS_TOP_LEVEL})
if(SVX_BUILD_GAME)
  set(SVX_BUILD_MESH ON)
  set(SVX_BUILD_ENV ON)
  set(SVX_BUILD_ANIM ON)
endif()

if(SVX_BUILD_MESH)
  file(GLOB_RECURSE SVX_MESH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mesh/src/*.cpp)
  add_library(svx_mesh STATIC ${SVX_MESH_SOURCES})
  target_include_directories(svx_mesh PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mesh/include> $<INSTALL_INTERFACE:include>)
  target_link_libraries(svx_mesh PUBLIC svx_core)
  target_compile_options(svx_mesh PRIVATE -fno-exceptions -fno-rtti ${SVX_WARNINGS})
endif()

if(SVX_BUILD_ENV)
  file(GLOB_RECURSE SVX_ENV_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/env/src/*.cpp)
  add_library(svx_env STATIC ${SVX_ENV_SOURCES})
  target_include_directories(svx_env PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/env/include> $<INSTALL_INTERFACE:include>)
  target_link_libraries(svx_env PUBLIC svx_core)
  target_compile_options(svx_env PRIVATE -fno-exceptions -fno-rtti ${SVX_WARNINGS})
endif()

if(SVX_BUILD_ANIM)
  file(GLOB_RECURSE SVX_ANIM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/anim/src/*.cpp)
  add_library(svx_anim STATIC ${SVX_ANIM_SOURCES})
  target_include_directories(svx_anim PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/anim/include> $<INSTALL_INTERFACE:include>)
  target_link_libraries(svx_anim PUBLIC svx_core)
  target_compile_options(svx_anim PRIVATE -fno-exceptions -fno-rtti ${SVX_WARNINGS})
endif()

if(SVX_BUILD_GAME)
  file(GLOB_RECURSE SVX_GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/game/src/*.cpp)
  add_library(svx_game STATIC ${SVX_GAME_SOURCES})
  target_include_directories(svx_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/game/include)
  target_link_libraries(svx_game PUBLIC svx_core svx_mesh svx_env svx_anim)
  target_compile_options(svx_game PRIVATE -fno-exceptions -fno-rtti ${SVX_WARNINGS})
endif()

# Helper for executables that must run under node when cross-compiled to WASM.
function(svx_wasm_exe_options target)
  target_compile_options(${target} PRIVATE ${SVX_WARNINGS})
  if(SVX_WASM)
    target_link_options(${target} PRIVATE
      -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
      -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB -sNODERAWFS=1)
  endif()
endfunction()

if(SVX_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
add_subdirectory(tools)  # (the game's tools, and the core's WASM module)

if(PROJECT_IS_TOP_LEVEL)
  include(CTest)
  if(BUILD_TESTING)
    add_subdirectory(tests)
  endif()
endif()

# Install: the core (and the mesher) as a CMake package: find_package(structvox) then link
# structvox::svx_core.
include(GNUInstallDirs)
set(SVX_INSTALL_TARGETS svx_core svx_flags)
if(SVX_BUILD_MESH)
  list(APPEND SVX_INSTALL_TARGETS svx_mesh)
  install(DIRECTORY mesh/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
if(SVX_BUILD_ENV)
  list(APPEND SVX_INSTALL_TARGETS svx_env)
  install(DIRECTORY env/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
if(SVX_BUILD_ANIM)
  list(APPEND SVX_INSTALL_TARGETS svx_anim)
  install(DIRECTORY anim/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
install(TARGETS ${SVX_INSTALL_TARGETS} EXPORT structvoxTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY core/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT structvoxTargets NAMESPACE structvox:: FILE structvoxConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/structvox)

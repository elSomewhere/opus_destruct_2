# The functions a C header declares, as Emscripten exports (_name): comments and static inline
# helpers left out. The WASM modules export every function of their C API - read from its header,
# so a module never lags the API it wraps.
function(svx_c_exports out header prefix)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${header})
  file(READ ${header} _h)
  string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _h "${_h}")
  string(REGEX REPLACE "//[^\n]*" "" _h "${_h}")
  string(REGEX MATCHALL "static inline [^(;]*${prefix}[a-z0-9_]+\\(" _inline "${_h}")
  set(_skip)
  foreach(m ${_inline})
    string(REGEX MATCH "${prefix}[a-z0-9_]+" n "${m}")
    list(APPEND _skip ${n})
  endforeach()
  string(REGEX MATCHALL "[ *]${prefix}[a-z0-9_]+\\(" _decl "${_h}")
  set(_names)
  foreach(m ${_decl})
    string(REGEX MATCH "${prefix}[a-z0-9_]+" n "${m}")
    list(FIND _skip ${n} i)
    if(i EQUAL -1)
      list(APPEND _names _${n})
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _names)
  set(${out} ${_names} PARENT_SCOPE)
endfunction()

if(SVX_BUILD_GAME)
# Streaming cost while flying through the procedural city.
add_executable(svx_stream_bench bench/stream_bench.cpp)
target_link_libraries(svx_stream_bench PRIVATE svx_game svx_procgen)
svx_wasm_exe_options(svx_stream_bench)

# The drive city's people: their cost by how the bodies are simulated and the crowd's size.
# A streamed preset driven through at speed: tick distribution, first touches, streaming ahead.
add_executable(svx_city_bench bench/city_bench.cpp)
target_link_libraries(svx_city_bench PRIVATE svx_game svx_procgen)
svx_wasm_exe_options(svx_city_bench)

add_executable(svx_people_bench bench/people_bench.cpp)
target_link_libraries(svx_people_bench PRIVATE svx_game svx_procgen)
svx_wasm_exe_options(svx_people_bench)

# Doom WAD graphics: palette, TEXTURE1/2 composites and flats -> counts + PNG dump.
add_executable(svx_wad_textures wad_textures/main.cpp)
target_link_libraries(svx_wad_textures PRIVATE svx_game)
svx_wasm_exe_options(svx_wad_textures)

# Headless end-to-end engine run: scenario scripts (tower, rooms, city, WAD), stats, frames.
add_executable(svx_engine_demo engine_demo/main.cpp)
target_link_libraries(svx_engine_demo PRIVATE svx_game svx_procgen)
svx_wasm_exe_options(svx_engine_demo)

# Record / replay sessions from command logs (determinism).
add_executable(svx_replay replay/main.cpp)
target_link_libraries(svx_replay PRIVATE svx_game svx_procgen)
svx_wasm_exe_options(svx_replay)

# Freedoom import / design / stability check of every map.
add_executable(svx_map_check map_check/main.cpp)
target_link_libraries(svx_map_check PRIVATE svx_game)
svx_wasm_exe_options(svx_map_check)

# Browser engine module for the web worker (docs/API.md): ES6 module, written into
# web/src/wasm/ so the Vite worker bundle imports it directly (generated, gitignored).
if(SVX_WASM)
  add_executable(svx_web wasm_module/module.cpp)
  target_link_libraries(svx_web PRIVATE svx_api)
  svx_c_exports(_svx_exports ${PROJECT_SOURCE_DIR}/game/include/svx/game/api/svx_api.h svx_)
  list(APPEND _svx_exports _malloc _free)
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

# A district of the reference city in the engine (docs/CITY.md): the reference harness's checks
# and first-touch costs on a dump (tools/procgen_ref/district.mjs).
add_executable(svx_district_check district_check/main.cpp)
target_link_libraries(svx_district_check PRIVATE svx_game svx_procgen)
svx_wasm_exe_options(svx_district_check)

# Long sessions and their memory: streamed city crossings, bounded levels (World::memory).
add_executable(svx_soak soak/main.cpp)
target_link_libraries(svx_soak PRIVATE svx_game svx_procgen)
svx_wasm_exe_options(svx_soak)

add_executable(svx_env_bench env_bench/main.cpp)
target_link_libraries(svx_env_bench PRIVATE svx_game svx_procgen)
svx_wasm_exe_options(svx_env_bench)
endif()  # SVX_BUILD_GAME

# The physics core alone for JavaScript hosts (docs/CORE.md): the C API of svx/svx_core.h.
if(SVX_WASM)
  add_executable(svx_core_web wasm_core/module.cpp)
  target_link_libraries(svx_core_web PRIVATE svx_core)
  svx_c_exports(_svxc_exports ${PROJECT_SOURCE_DIR}/core/include/svx/svx_core.h svxc_)
  list(APPEND _svxc_exports _malloc _free)
  string(REPLACE ";" "," _svxc_exports_csv "${_svxc_exports}")
  target_link_options(svx_core_web PRIVATE
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createSvxCore -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB
    -sFILESYSTEM=0 --no-entry -sDYNAMIC_EXECUTION=0 -sALLOW_TABLE_GROWTH=1
    "-sEXPORTED_FUNCTIONS=[${_svxc_exports_csv}]"
    "-sEXPORTED_RUNTIME_METHODS=[HEAPU8,HEAP32,HEAPF64,addFunction,UTF8ToString,stringToUTF8,lengthBytesUTF8]")
  set_target_properties(svx_core_web PROPERTIES SUFFIX ".js")
endif()

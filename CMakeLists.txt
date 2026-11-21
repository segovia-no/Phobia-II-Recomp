cmake_minimum_required(VERSION 3.16)
project(PhobiaIIRecomp LANGUAGES C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The player's own Game.exe (Phobia II v2.0, WAH-Software 1998). Recompiled C is generated from it at
# build time into the build tree and never committed.
set(GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "Phobia II game folder (Game.exe, pics/, sounds/, dat/)")
set(GAME_EXE "${GAME_DIR}/Game.exe" CACHE FILEPATH "Phobia II Game.exe")
set(RECOMP_PYTHON "${CMAKE_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "Python with pefile + capstone")
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
set(GEN_FILE_COUNT 16)

option(PHOBIA_RECOMP_CHECK "Check every guest return address against its call site (debug)" OFF)
option(PHOBIA_RECOMP_TRACE "Per-instruction trace hook (PHOBIA_TR=lo-hi env var)" OFF)
# Game code as a module (docs/notes/07-builder.md): the program contains no translated code and loads
# gamecode.so / gamecode.dylib at start. Without Game.exe only the program is built (CI, release kits).
option(PHOBIA_GAME_MODULE "Build the game code as a module the program loads at start" OFF)

set(PHOBIA_HAVE_GAME ON)
if(NOT EXISTS "${GAME_EXE}")
  if(PHOBIA_GAME_MODULE)
    set(PHOBIA_HAVE_GAME OFF)
    message(STATUS "No Game.exe: building the program only (game code: recompiler/build_gamecode.py)")
  else()
    message(FATAL_ERROR "Game.exe not found at ${GAME_EXE} (set GAME_DIR, or -DPHOBIA_GAME_MODULE=ON for the program only)")
  endif()
endif()

set(WARN_OFF -Wno-unused-label -Wno-unused-variable -Wno-parentheses-equality -Wno-unused-value
             -Wno-misleading-indentation -Wno-unknown-warning-option)

find_package(SDL2 REQUIRED)
file(GLOB RUNTIME_SOURCES "${CMAKE_SOURCE_DIR}/runtime/*.c")

# ------------------------------------------------------------------ translated game code
if(PHOBIA_HAVE_GAME)
  set(GEN_SOURCES
    "${GEN_DIR}/functable.c" "${GEN_DIR}/importtable.c" "${GEN_DIR}/imports_glue.c"
    "${GEN_DIR}/image.c" "${GEN_DIR}/smctable.c" "${GEN_DIR}/module.c")
  math(EXPR _last "${GEN_FILE_COUNT} - 1")
  foreach(i RANGE ${_last})
    if(i LESS 10)
      set(_n "00${i}")
    elseif(i LESS 100)
      set(_n "0${i}")
    else()
      set(_n "${i}")
    endif()
    list(APPEND GEN_SOURCES "${GEN_DIR}/code_${_n}.c")
  endforeach()
  set(LIFT_FLAGS --files ${GEN_FILE_COUNT})
  if(PHOBIA_GAME_MODULE)
    list(APPEND LIFT_FLAGS --module)
  endif()
  add_custom_command(
    OUTPUT ${GEN_SOURCES} "${GEN_DIR}/gen.h" "${GEN_DIR}/imports.h" "${GEN_DIR}/meta.h"
    COMMAND "${RECOMP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/recompiler/lift.py"
            "${GAME_EXE}" "${GEN_DIR}" ${LIFT_FLAGS}
            --extra "${CMAKE_SOURCE_DIR}/recompiler/extra_entries.txt"
    DEPENDS "${GAME_EXE}" "${CMAKE_SOURCE_DIR}/recompiler/lift.py" "${CMAKE_SOURCE_DIR}/recompiler/patches.py"
            "${CMAKE_SOURCE_DIR}/recompiler/extra_entries.txt"
    COMMENT "Recompiling Game.exe to C"
    VERBATIM)
endif()

function(phobia_common target)
  target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/runtime" "${GEN_DIR}")
  # Generated code relies on wrapping arithmetic and type-punned guest memory accesses.
  target_compile_options(${target} PRIVATE -fwrapv -fno-strict-aliasing ${WARN_OFF})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
  endif()
  if(PHOBIA_RECOMP_CHECK)
    target_compile_definitions(${target} PRIVATE RECOMP_CHECK)
  endif()
  if(PHOBIA_RECOMP_TRACE)
    target_compile_definitions(${target} PRIVATE RECOMP_TRACE)
  endif()
endfunction()

# ------------------------------------------------------------------ the program
if(PHOBIA_GAME_MODULE)
  add_executable(phobia2 ${RUNTIME_SOURCES})
  target_compile_definitions(phobia2 PRIVATE PHOBIA_GAME_MODULE)
  # the module resolves M, dispatch, rt_* and the import shims against the program
  set_target_properties(phobia2 PROPERTIES ENABLE_EXPORTS ON)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # release kits bundle SDL2 in lib/ next to the program
    set_target_properties(phobia2 PROPERTIES BUILD_RPATH "\$ORIGIN/lib;\$ORIGIN")
  endif()
  target_link_libraries(phobia2 PRIVATE ${CMAKE_DL_LIBS})
else()
  add_executable(phobia2 ${RUNTIME_SOURCES} ${GEN_SOURCES})
endif()
phobia_common(phobia2)
target_link_libraries(phobia2 PRIVATE SDL2::SDL2 m)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Threads REQUIRED)
  target_link_libraries(phobia2 PRIVATE Threads::Threads)
endif()
if(APPLE)
  target_link_libraries(phobia2 PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()

# ------------------------------------------------------------------ the module (source builds)
if(PHOBIA_GAME_MODULE AND PHOBIA_HAVE_GAME)
  add_library(gamecode MODULE ${GEN_SOURCES})
  phobia_common(gamecode)
  set_target_properties(gamecode PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
  if(APPLE)
    set_target_properties(gamecode PROPERTIES SUFFIX ".dylib")
    target_link_options(gamecode PRIVATE "-Wl,-undefined,dynamic_lookup")
  endif()
  add_dependencies(phobia2 gamecode)
endif()

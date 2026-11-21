cmake_minimum_required(VERSION 3.20)
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

set(GEN_SOURCES
  "${GEN_DIR}/functable.c" "${GEN_DIR}/importtable.c" "${GEN_DIR}/imports_weak.c"
  "${GEN_DIR}/image.c" "${GEN_DIR}/smctable.c")
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

add_custom_command(
  OUTPUT ${GEN_SOURCES} "${GEN_DIR}/gen.h" "${GEN_DIR}/imports.h" "${GEN_DIR}/meta.h"
  COMMAND "${RECOMP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/recompiler/lift.py"
          "${GAME_EXE}" "${GEN_DIR}" --files ${GEN_FILE_COUNT}
          --extra "${CMAKE_SOURCE_DIR}/recompiler/extra_entries.txt"
  DEPENDS "${GAME_EXE}" "${CMAKE_SOURCE_DIR}/recompiler/lift.py" "${CMAKE_SOURCE_DIR}/recompiler/extra_entries.txt"
  COMMENT "Recompiling Game.exe to C"
  VERBATIM)

find_package(SDL2 REQUIRED)
file(GLOB RUNTIME_SOURCES "${CMAKE_SOURCE_DIR}/runtime/*.c")

add_executable(phobia2 ${RUNTIME_SOURCES} ${GEN_SOURCES})
target_include_directories(phobia2 PRIVATE "${CMAKE_SOURCE_DIR}/runtime" "${GEN_DIR}")
target_link_libraries(phobia2 PRIVATE SDL2::SDL2 m)
# Generated code relies on wrapping arithmetic and type-punned guest memory accesses.
target_compile_options(phobia2 PRIVATE -fwrapv -fno-strict-aliasing
  -Wno-unused-label -Wno-unused-variable -Wno-parentheses-equality -Wno-unused-value -Wno-misleading-indentation)
if(PHOBIA_RECOMP_CHECK)
  target_compile_definitions(phobia2 PRIVATE RECOMP_CHECK)
endif()
if(PHOBIA_RECOMP_TRACE)
  target_compile_definitions(phobia2 PRIVATE RECOMP_TRACE)
endif()
if(APPLE)
  target_link_libraries(phobia2 PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()

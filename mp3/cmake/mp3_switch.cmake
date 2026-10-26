# mp3_switch.cmake - what the codegen-emitted rexglue.cmake gives a title, for the Switch build.
#
#   MP3_CODEGEN_DIR   tools/codegen.sh's output (holds generated/default/, made from your default.xex)
#   REXSDK_DIR          this repository's sdk/ (rexglue-sdk-nx)
#
# The generated tree is used as it is, with one exception: its PCH. Its files include "mp3_pch.h" with
# quotes, which finds the copy next to them first, so the build compiles a mirror of symlinks (tools/prepare_tree.py)
# whose PCH is the Switch one (tools/make_nx_pch.py).

set(MP3_CODEGEN_DIR "" CACHE PATH "tools/codegen.sh output folder (holds generated/default/)")
set(MP3_GENERATED "${MP3_CODEGEN_DIR}/generated/default")
if(NOT EXISTS "${MP3_GENERATED}/sources.cmake")
    message(FATAL_ERROR
        "MP3_CODEGEN_DIR must point at tools/codegen.sh's output "
        "(no generated/default/sources.cmake under '${MP3_CODEGEN_DIR}')")
endif()

set(REXSDK_DIR "" CACHE PATH "Path to the rexglue-sdk(-nx) source tree")
if(NOT REXSDK_DIR)
    get_filename_component(REXSDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../sdk" ABSOLUTE)
endif()
# A build with the guest's own flush-to-zero switches (SDK patch 0014), to compare with the fixed flush mode.
option(MP3_VARIABLE_FLUSH "Keep the guest's FPCR flush-to-zero switches (no fixed flush mode)" OFF)
if(MP3_VARIABLE_FLUSH)
    add_compile_definitions(REX_SWITCH_VARIABLE_FLUSH=1)
    message(STATUS "mp3: variable flush mode (the guest's own switches)")
endif()
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
message(STATUS "mp3: ReXGlue SDK from ${REXSDK_DIR}")

# The mirror of the generated tree with the Switch PCH.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(MP3_TREE "${CMAKE_BINARY_DIR}/generated_nx")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_tree.py"
            "${MP3_GENERATED}" "${MP3_TREE}"
    RESULT_VARIABLE _mp3_tree_rc)
if(NOT _mp3_tree_rc EQUAL 0)
    message(FATAL_ERROR "mp3: tools/prepare_tree.py failed (${_mp3_tree_rc})")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MP3_GENERATED}/mp3_pch.h"
    "${MP3_GENERATED}/sources.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/make_nx_pch.py"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_tree.py")

set(REXGLUE_RECOMP_DEBUG_INFO "none" CACHE STRING
    "Debug info level for generated code: line-tables-only, full, or none")
set(REXGLUE_RECOMP_OPTIONS "")
if(REXGLUE_RECOMP_DEBUG_INFO STREQUAL "none")
    list(APPEND REXGLUE_RECOMP_OPTIONS $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-g0>)
elseif(REXGLUE_RECOMP_DEBUG_INFO STREQUAL "line-tables-only")
    list(APPEND REXGLUE_RECOMP_OPTIONS $<$<CXX_COMPILER_ID:Clang,AppleClang>:-gline-tables-only>)
endif()

# generated/default/sources.cmake: the support units and the recompiled ones. CMAKE_CURRENT_LIST_DIR there is the mirror.
include("${MP3_TREE}/sources.cmake")
set(MP3_GUEST_SOURCES ${GENERATED_SOURCES})
set_source_files_properties(${MP3_GUEST_SOURCES} PROPERTIES COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS}")
# The registration unit is one function of ~37,000 SetFunction calls, run once at startup: at -O3 GCC spends ~6 minutes
# on it alone, on one core, longer than all the other units together on ten. -O1 takes seconds. It needs nothing from
# the PCH (its own includes), and a PCH built at -O3 cannot be used at -O1.
set_source_files_properties("${MP3_TREE}/mp3_register.cpp" PROPERTIES
    COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS};-O1"
    SKIP_PRECOMPILE_HEADERS ON)
list(LENGTH MP3_GUEST_SOURCES _mp3_n)
message(STATUS "mp3: ${_mp3_n} recompiled sources from ${MP3_GENERATED}")

# Usage: mp3_setup_target(<target>)
macro(mp3_setup_target target_name)
    add_library(${target_name}_recomp OBJECT ${MP3_GUEST_SOURCES})
    target_include_directories(${target_name}_recomp PRIVATE "${MP3_TREE}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target_name}_recomp PRIVATE rex::runtime)
    rexglue_apply_target_settings(${target_name}_recomp)
    target_precompile_headers(${target_name}_recomp PRIVATE "${MP3_TREE}/mp3_pch.h")
    target_compile_options(${target_name}_recomp PRIVATE ${REXGLUE_RECOMP_OPTIONS})
    target_link_libraries(${target_name} PRIVATE ${target_name}_recomp)
    target_include_directories(${target_name} PRIVATE "${MP3_TREE}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target_name} PRIVATE rex::runtime)
    rexglue_configure_target(${target_name} ${ARGN})
endmacro()

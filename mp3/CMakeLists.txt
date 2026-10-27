# mp3 - Max Payne 3 (Xbox 360, Games on Demand, Title Update 6) on the Nintendo Switch, on rexglue-sdk-nx with deko3d.
# The tree is GTA IV's port (gta4-nx, the same RAGE engine), renamed; see PORTING.md for what carried over.
#
# The recompiled game code is generated from your own default.xex and default.xexp by tools/codegen.sh (ReXGlue
# v0.10.0's codegen with codegen/mp3_config.toml, then our passes), never committed. Point MP3_CODEGEN_DIR at its output. At configure time tools/prepare_tree.py mirrors
# generated/default/ as symlinks with a Switch PCH (tools/make_nx_pch.py). tools/build_mac.sh does all of it.
#
# Configure (Switch):
#   cmake -S mp3 -B mp3/out/sw -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
#     -DREXSDK_DIR=$PWD/sdk -DMP3_CODEGEN_DIR=$PWD/mp3/out/codegen -DREXGLUE_SWITCH_DEKO3D=ON
#   cmake --build mp3/out/sw

cmake_minimum_required(VERSION 3.25)
project(mp3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/mp3_switch.cmake)

if(NOT REXGLUE_SWITCH_DEKO3D)
    message(FATAL_ERROR "mp3: only the deko3d build exists (-DREXGLUE_SWITCH_DEKO3D=ON)")
endif()

add_executable(mp3
    src/main.cpp
    src/mp3_gpu.cpp
    src/mp3_shaders.cpp
    src/mp3_guards.cpp
    src/mp3_cpu.cpp
    src/mp3_god.cpp
    src/mp3_perf_hooks.cpp
    src/mp3_tiling.cpp
    src/mp3_menu.cpp
    src/mp3_resolution_trace.cpp
    # The shader library reader, shared with the PC tools (shaders/).
    ${CMAKE_CURRENT_SOURCE_DIR}/../shaders/ds_shader_library.cpp
)

mp3_setup_target(mp3)
# A variant with the mtmsrd regions as the codegen's process-wide mutex by default (src/mp3_cpu.cpp), to compare.
option(MP3_MSR_GLOBAL "The mtmsrd regions as one global mutex by default" OFF)
if(MP3_MSR_GLOBAL)
    set_source_files_properties(src/mp3_cpu.cpp PROPERTIES COMPILE_DEFINITIONS MP3_MSR_GLOBAL_DEFAULT=1)
    message(STATUS "mp3: mtmsrd regions as one global mutex by default")
endif()
# The native DSP hooks compute the guest's floating point bit for bit: no FMA fusion of separate multiplies and adds
# (GCC's default for GNU C++ is -ffp-contract=fast; the generated code gets -ffp-contract=off from the SDK).
set_source_files_properties(src/mp3_audio_dsp_neon.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
rexglue_switch_add_deko3d(mp3)
target_compile_definitions(mp3 PRIVATE MP3_DEKO3D=1)
target_include_directories(mp3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shaders ${REXSDK_DIR}/thirdparty/xxHash)

# GTA IV's hooks are compiled only for the names codegen/names.toml gives (MP3_TIENE_<name>): each one waits for its
# Max Payne 3 function to be found and checked. Not built until then: src/mp3_audio_neon.cpp and
# src/mp3_audio_dsp_neon.cpp (GTA IV's mixer DSP, its constants at GTA IV's addresses). src/mp3_menu.cpp is Max Payne 3's
# own now (the 720p / 540p choice).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/codegen/names.toml" MP3_NOMBRES REGEX "^name = \"mp3_[A-Za-z0-9_]+\"")
foreach(linea IN LISTS MP3_NOMBRES)
    string(REGEX REPLACE "^name = \"(mp3_[A-Za-z0-9_]+)\".*" "\\1" nombre "${linea}")
    target_compile_definitions(mp3 PRIVATE MP3_TIENE_${nombre}=1)
endforeach()

# The shader create hooks need the two names codegen/names.toml gives the XDK's Create*Shader (not known before the
# first codegen of the game's executable).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/codegen/names.toml" MP3_NAMES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/codegen/names.toml")
if(MP3_NAMES MATCHES "\nname = \"mp3_D3DDevice_CreateVertexShader\"" AND
   MP3_NAMES MATCHES "\nname = \"mp3_D3DDevice_CreatePixelShader\"")
    target_sources(mp3 PRIVATE src/mp3_shader_dump.cpp)
    message(STATUS "mp3: shader create hooks on")
else()
    message(STATUS "mp3: shader create hooks off (codegen/names.toml has no Create*Shader names yet)")
endif()

# The version (Homebrew Menu and the first lines of the log) and the commit it was built from, so a log a player sends
# says which build made it. The commit is read when CMake configures (build_mac.sh RECONFIGURE=1 refreshes it).
set(MP3_VERSION "0.1.0")
execute_process(COMMAND git rev-parse --short=10 HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE MP3_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain --untracked-files=no --ignore-submodules=all WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE MP3_SUCIO OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT MP3_COMMIT)
    set(MP3_COMMIT "unknown")
elseif(MP3_SUCIO)
    string(APPEND MP3_COMMIT "+changes")
endif()
target_compile_definitions(mp3 PRIVATE MP3_VERSION="${MP3_VERSION}" MP3_COMMIT="${MP3_COMMIT}")

# Nintendo Switch: mp3.nro. On the SD card: sdmc:/switch/mp3-nx/mp3-nx.nro, with game/ beside it
# (src/mp3_app.h finds it there). No icon from the game: the Homebrew Menu shows the default one.
if(REXGLUE_PLATFORM_SWITCH)
    rexglue_switch_add_nro(mp3
        NAME "Max Payne 3"
        AUTHOR "VasilkovND"
        VERSION "${MP3_VERSION}")
endif()

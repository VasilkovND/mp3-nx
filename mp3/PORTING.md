# GTA IV on rexglue-sdk-nx (deko3d): porting notes

This folder builds GTA IV (Xbox 360, USA disc, Title Update 8) for the Switch on the stack of ds2-nx, dantes-inferno-nx
and gears2-nx instead of the one in `source/` (a patch on LibertyRecompAndroid, its ReXGlue 0.9 fork, a Vulkan
renderer on NVK/Mesa, built in a Docker image). Both use the same idea: the game's PowerPC code statically recompiled
to C++ and built for ARM. What changes:

| | `source/` (Vulkan build) | `mp3/` (this build) |
| --- | --- | --- |
| Recompiler | LibertyRecompAndroid's ReXGlue 0.9 fork (generated code checked into that repository) | ReXGlue v0.10.0, code generated from your own default.xex + default.xexp at build time, never committed |
| Runtime | the fork's SDK + `mp3nx-switch.patch` | rexglue-sdk-nx (`sdk/` submodule) + `mp3/sdk-patches/` |
| Renderer | D3D-level native renderer on Vulkan (NVK, Mesa built from source) | the game's command ring read on the console, drawn with deko3d (`src/mp3_gpu.cpp`, the SDK's deko3d backend) |
| Shaders | compiled by NVK on the console (first-start stutter) | translated and compiled on the PC (`shaders/`, XenosRecomp -> SPIR-V -> DKSH), read from the card |
| Build | Docker image `ghcr.io/autorunhq/switch-dev` | `mp3/tools/build_mac.sh` on a Mac with devkitPro (no Docker) |
| CPU passes | direct calls, loop barriers, dcbt prefetch (`source/otimizar_codigo_gerado.py`) | barriers/prefetch, stack, inlining, guest registers in host locals (`CPU_OPTIMIZATION.md`) |

## What is taken from where

- From Liberty Recompiled / LibertyRecompAndroid (GPL-3.0): `codegen/mp3_config.toml` (analysis settings, the CRT
  functions run natively, the function entries automatic discovery misses, names of Direct3D and CRT functions,
  prefixed `mp3_`). The game-specific fixes in its `mp3-recomp/src` are to be carried over one by one as the game
  needs them (see "Open" below).
- From this repository's Vulkan build: its measurements (README, `source/mp3nx-switch.patch` comments), the idea of the
  dcbt prefetch, and the thread priorities and core 3 for the XAudio2 thread (see "Levers").
- From gears2-nx (Gears of War 2): the skeleton (`src/`, `tools/`, `shaders/`, `sdk-patches/` 0001-0049, host patch),
  renamed. Game-specific parts of Gears were removed (its hooks, UE3 object walks, the stack guard, the PC port's code
  fixes); `src/mp3_gpu.cpp` still carries Gears' renderer test modes behind cvars, to be reviewed on the console.
- From dantes-inferno-nx: the newest `patch_local_regs.py` and `patch_inline_calls.py`, the STFS reader for the title
  update (`tools/stage_sd.py`).

## Where the Vulkan build's time goes (its own notes)

- City: all four cores busy (355-377 % of 400 % with core 3 opened through an exheader override).
- XAudio2 processing thread ~85 % of a core (moved alone to core 3); XMA decode ~20 % of the audio thread.
- Its renderer is CPU work too: the producer (the title's render thread) and the recorder ran ~45 ms each per frame
  before they were overlapped; the title's render thread was ready-but-not-running ~19-24 % of its time behind them, the
  main thread waited on it; priorities were raised to fix that.
- GPU: one hospital interior ~10 fps (GPU-bound), resolves 19 ms of a 76 ms GPU frame in one capture; 640x360 + FSR 1.
- So the city is CPU-bound in the sum of threads, not one function: the game's own update and render-list threads,
  the audio, and the renderer's command capture compete for three to four cores.

## Status

- Tools ready and tested on another title's executable (Dante's Inferno, standing in for the disc): codegen, our
  passes (36,088 functions with registers in locals, none skipped), the PCH, a full NRO build.
- Waiting for the user's disc and TU8: `tools/stage_sd.py`, the real codegen, the function names (Create*Shader for
  the shader dump, the game's spin-wait functions), the RPF shader extractor, the first console run.

## The EDRAM as one store (SDK patch 0057)

The deko3d renderer came from ports whose games read a render target only the way they drew it, so it kept every
target as an image of its own, keyed by base, format and pitch. GTA IV looks at the same EDRAM tiles in several ways
in one frame (from the console's frame traces):

- the HDR scene (`2D0`) is drawn at 1280x720 and resolved as 640x360 with 4x MSAA: the hardware averages each 2x2, so
  that resolve is the downsample the bloom and the luminance chain start from (and again 320 -> 160, 80 -> 40);
- the depth is restored at 640x360 4x after the G-buffer and the lights test against it at 1280x720 (their stencil
  passes saw an empty depth: no light reached the scene);
- `2D0` is 8-bit color, 7e3 and 32-bit float in turn; the G-buffer is cleared by draws at 640x360 4x.

`sdk/src/x360/deko/edram.cpp` (cvar `x360_edram_compartida`, on) makes the views share their tiles: each of the
2048 tiles remembers the view that wrote it last; a view drawn or resolved first gets the tiles other views wrote
since it last held them, through a draw that maps each of its samples to its place in EDRAM and reads it from the
writer's image (reinterpreting the bits between 8-bit, 10-bit, 7e3, 16-bit and 32-bit float formats; depth with its
stencil, `x360_edram_estencil`). A view claims the rows its draws and clears wrote when its pass ends. Views bound
together are limited to the tiles up to the next one's base (the G-buffer drawn whole by `mp3_sin_tiling`). Color
and depth tiles are kept apart. `mp3_resolve_msaa_alias`, the earlier workaround for the 640x360 resolve, is off.
`x360_edram_registro` logs every transfer; every 300 frames `[edram]` gives their count and size per frame.

## The spot light volumes' shaders

The shadowed spot light volumes (VS `vs_0872615bd9e7930d`, `vs_22f0ab2b742f2053`, `vs_09f5443e94b30486`) loop over
their shadow map offsets with a Xenos loop (blocks 23-43 of 61). Any jump back used to send XenosRecomp's whole shader
to `while (true) switch (pc)`: 550-700 KB of DKSH, 255 registers, 65-79 KB of spill, 0.8-1.3 s per draw (frozen frames,
black flashes), so the renderer skipped vertex shaders over `x360_vs_codigo_max_kb` (256, kept as a guard). Now
(`shaders/XenosRecomp/shader_recompiler.cpp`, `lineal`) only the blocks between a jump back's target and source go in a
`while (true) { ... break; }` and the rest stays `if (pc <= N)`: 10-12 KB each, no other shader changed.

The rest of the library now compiles too (1309 of 1309 shaders, every DKSH variant; there were 4 + 5 missing):

- GTA IV's GPU particle pixel shaders (collision, gusts, dispersal) read pixel constants up to c230; the block was 224.
  Shaders that read above it declare 256 (`DS_PS_REGISTROS`), the others keep their SPIR-V and DKSH cache keys; the
  renderer binds all 256 (SDK patch 0058).
- Four vertex shaders read `NORMAL1` and `TANGENT1`, which have no attribute location under 16 (uam's GLSL has 16): they
  read them from the vertex buffer themselves at the vertex's index (`dsVfetch`, the path of the fetches at a computed
  index).

## The copies' GPU time (~26 ms of a 55-75 ms frame in run29)

Measured only as a category until now. The profiled frame (every `nfsmw_dk_perfil_gpu_s`) now also times each copy
from its start to the next copy or category mark (its barrier, the copy, the drain before the next work) and logs
`perfil GPU: N copias del fotograma` with a sum per type (3D color/depth resolve, copy engine, EDRAM color, EDRAM depth
with its stencil passes, cube faces ...) and the 24 most expensive with their sizes and MSAA (SDK patch 0059). First
changes from reading the code: an EDRAM depth transfer runs a stencil pass only for the bits a draw, clear or transfer
may have set (RB_DEPTHCONTROL's ops and the write mask; it was always 8), and color transfers no longer invalidate
ZCULL.

In handheld mode (GPU 614 MHz, run41) the copies are ~30 ms of a ~60 ms GPU frame and the GPU is busy 86-90 % of
the frame. The depth transfers between MSAA views with their 8 stencil passes are ~12.5 ms of it: two of them (4.7 ms)
only feed a one-draw pass into the 640x360 4x view that writes neither color nor depth.

Run42 (handheld) profiles copies at 28 ms per frame. SDK patch 0073: a pass prepares only the rows down to its scissor
bottom (`x360_edram_filas_de_tijera`; it was max(720, rows ever used), so the 320x180 and 256x256 passes into 4x views
received 720 rows of color, depth and stencil), and begins again when a later draw reaches lower; GTA IV's depth
restore for the lights is one clear and one draw testing "greater than 0" (`x360_edram_z_de_z_un_pase`; it was a depth
pass and a stencil pass); the stencil passes of a depth transfer done while a pass prepares wait
(`x360_edram_estencil_perezoso`) until a draw of the pass uses the stencil or the pass writes the view, else the
view's tiles are marked stale. The 10-second report counts deferred, run and dropped ones.

## The options menu (mp3_menu.cpp, SDK patch 0088)

ZL + ZR + Minus opens the port's options over the game, as in Dead Space 3 (its SDK patch 0028 as 0088 here: a few
lines of text the deko3d presenter paints, and a controller filter so the game sees the pad idle meanwhile; no ImGui,
which would move presentation to the UI thread). Up / Down choose, Left / Right change the value at once (the ring
thread applies it at the next Swap), A saves to `mp3_opciones.txt` beside the NRO, B closes: DRAW DISTANCE
(`mp3_distancia_lod`, 0.4-1.0), TRAFFIC (`mp3_densidad_coches`) and PEDESTRIANS (`mp3_densidad_peatones`, 0.2-1.0).
The saved values replace mp3.toml's at start.

## The phone's black upper screen (SDK patch 0089)

The dumps of runs 56 and 58 were taken with the phone up and logged their resolves (SDK patch 0086). After the frame
is composed in 2D0 (pitch 1280, 7e3) the game draws the phone's screen (256x256 at 000, resolved to 039F4000), then
clears depth and color at base 000 pitch 1280 with a 1280x8192 rectangle (a whole-EDRAM clear, window scissor
3FFF3FFF), and only then resolves 2D0 to the front buffer. Without the game's tiling (src/mp3_tiling.cpp) the
1280-wide view at 000 reaches 2D0's tiles from its row 720, so the resolve got rows 0-560 as cleared: black. With
`x360_edram_frontal_sin_borrados` a resolve to one of the Swaps' front buffers leaves out the clears another base's
view claimed over its tiles; a clear of the view itself (a black loading screen) still applies.
It did nothing in run61: GTA IV's Swaps do not carry the front buffer in fetch constant 0, so the renderer presents
the last resolved frame and the Swap's address never matched a resolve (the lazy front buffer never ran either). SDK
patch 0090 takes the front buffers from what the renderer actually presents.
That did nothing in run62 either: the cleared rows reached 2D0 when the game next drew on it after the clear, not at
the front-buffer resolve. SDK patch 0092 protects a frame being composed instead: a view resolved with a copy this
frame and drawn on since (the scene at 2D0, resolved for the post-processing, then the HUD) does not take another
base's clear. At the frame's start the 1280-wide views at 200 and 400 still get base 000's whole-EDRAM clear (they
were not resolved this frame yet), and 2D0 gets its own base's.

## Streaming from the Games on Demand package (SDK patch 0091)

Run61 (all options at their minimum: 26 fps against 25 at 0.6) had textures and models arriving when the player was
already on them. The log's slow reads were the game's 32 KB streaming reads from the .img archives (brook_s.img,
componentpeds.img, vehicles.img ...) taking 20-30 ms on average and up to 236 ms: every read of every package went
through one lock and was a seek and a read on the SD card. `StfsContainerFile::ReadSync` now reads each data file
under the lock of its own read-ahead window (8 of them, 1 MB each): a read that follows the last one is a copy, the
card sees one read per megabyte, and reads of different files no longer wait for each other. A lower detail distance
also asks for models and textures later (the streaming radius scales with it), so 0.4 pops in closer than 0.6.
In run62 the 1 MB window made each small random read (a 20-byte header) a 1 MB read, ~13 ms, while loading; SDK patch
0093 fills the 1 MB window only for a read that continues the previous one on that file, 64 KB otherwise.


## Detail distance 0.6 and the population density (mp3_gpu.cpp)

Run60 (15 minutes, `mp3_distancia_lod` 0.6) averaged 25.0 fps against ~22.3 in runs 53-59 at 0.75, with 1517 draws
a frame against ~1850; 0.6 is now the default. The frame time fit runs 58-59 at about 31 ms plus 7.2 ms per 1000
draws, and in run60 the main thread was the limit (76 % of a core, ~40 % of that the game's visibility and physics jobs
it now runs itself). The population density multipliers (random cars 0x82AA1888, parked cars 0x82AA188C, peds
0x82AA2940, scenario peds 0x82AA2944/2948, all 1.0 in the image; found by the detail-distance research) are written by
the game and its scripts, so the ring thread watches them at each Swap and stores the game's value times
`mp3_densidad_coches` / `mp3_densidad_peatones` (0.6 by default): fewer cars and peds to simulate, animate and draw.

## The waiting thread runs queued jobs (mp3_perf_hooks.cpp)

Runs 53-56 all averaged ~22 fps whatever the GPU clock, the reflections, the priorities or the speaker setting; the
main thread spent ~45 % of its time in `sub_8285D170`, the wait for a set of jobs, while the three job workers were
~80 % idle. The queue (from the workers' loop `sub_8285D368` and the producer `sub_8285D2C8`): [worker + 188] + 8 holds
a ring of 256 job pointers, a critical section at +1024, write and read indices at +1056/+1060, the count at +1064
and a semaphore at +1068 released once per job. A worker copies the job's 156 bytes, puts its scratch buffer at
copy + 64, calls [copy + 4] with r3 = copy + 8 and signals [job + 156]. `mp3_JobWait` (`mp3_trabajos_en_espera`)
takes the queued jobs on the waiting thread first, the same way (the semaphore with a zero timeout, so no worker sees
a job twice; a null job, a worker's stop, is left for it), with its own copy and scratch buffer, then waits as before.
The log counts the waits and the jobs the waiting thread ran.

Run58 (A/B of the job stealing, 17 pairs of neighbouring intervals): +0.2 fps, noise. The waiting thread ran ~13 % of
the jobs it waited for; the rest were already on a worker. Worse, a zero-timeout wait that fails yields the core
(XObject::Wait), ~11 % of the main thread's samples: the queue's count is now read first and an empty queue is left
alone. The same run showed the main thread sleeping ~10 % of its time in `sub_826799E0`, the drain of an 8-entry ring
of pending asynchronous requests (owner + 12564, count at owner + 12604): it polls the next request's handle
([request + 12]) and sleeps 10 ms when it is not done. `mp3_AsyncDrain` (`mp3_espera_peticiones`) waits on that
handle for up to the same 10 ms instead, so a request that finishes 1 ms later costs 1 ms; the wait consumes the
signal as the poll did. Silent voices skipped ~10 % of the effect chains and took the mixer thread from ~56 to ~54 %.

Silent voices: 60 % of the mixer's `mp3_AudioMixAddRamp` calls had gain 0. A voice whose every send gain is 0 now
and next still ran its effect chain (`sub_8219AE20`: the filters `mp3_AudioFilterSections` and
`mp3_AudioBiquadChannels`, ~16 % of a core). `mp3_AudioVoiceEffects` skips that chain for such a voice
(`mp3_voces_mudas_sin_efectos`; the sends at [voice + 72], [voice + 69] of them, each send's matrix at [send + 20],
[send + 16] entries of 12 bytes with the current gain at +4 and the target at +8). Only from `sub_8219BA90` (the
return address 0x8219BAA8): the mastering voice's chain (`sub_8219A258`, with its limiter) has one output pointer at +68
and read +69 as a send count, so run57 crashed on the first audio frame. The log counts the skipped chains.

The reflections every other frame (SDK patch 0082) are removed again by SDK patch 0087: they saved nothing measurable
(run56, every frame, ran at the same 22.5 fps) and left the water's reflection red and green on the frames they
skipped.

## Where the frame waits (run54) and three settings to measure it

With the GPU at 921 MHz instead of 614 run54 ran at the same 18-29 fps, and the report's GPU time stayed at 33-47 ms:
the renderer submits every 256 draws (`nfsmw_dk_envio_parcial`), so a job's GPU time includes the GPU waiting for the
ring to record the next 256. The limit is the CPU (365-380 % of 400 %). The main thread spent ~45 % of its time
waiting for the job workers (`sub_8285D368`, three threads at ~15 % of a core and 80 % idle) and ~6 % for the GPU; the
audio took more than a core (mixer thread 55 %, audio engine 33 %, XMA decoder 32 %).

- Every guest thread runs at Horizon priority 0x3B, which Horizon time-slices only about every 10 ms, under the ring
  (0x2C) and the audio threads (0x2B). `mp3_prioridad_trabajos` and `mp3_prioridad_render` (src/mp3_perf_hooks.cpp,
  hooks on the job workers' loop and the render thread's loop `sub_821B6F18`) put those threads above the other guest
  threads; `mp3_prioridad_anillo` moves the ring. All three are applied at the next Swap, so an A/B run can switch
  them; 0 keeps the old priorities.
- `XAudioGetSpeakerConfig` returned Xenia's 0x00010001 (Dolby Digital and analog stereo). GTA IV polls it
  (`sub_8291F508`) and turns the Dolby Digital bit into its mixing mode 3, stereo into 1; its reverb setup
  (`sub_82935A70`) reads it too. `audio_configuracion_altavoces` (SDK patch 0085) sets the value; 1 is stereo only.
- SDK patch 0086: the frame a dump takes (ZL + ZR + Left) also logs its resolves and clears with the window scissor,
  for the phone's black upper screen (an EDRAM clear of view 000 pitch 1280 reaches the finished frame's tiles at 2D0).

## Frame pacing in the report (SDK patch 0084)

Run51 (reflections every other frame and detail distance 0.75) averaged about what run50 did, but played with many
stutters, and the 10-second report could not show them: an even 20 fps and frames alternating 35 and 65 ms have the
same average. The report now has a `[ritmo]` line: the interval's frame-to-frame times (median, p90, p99, maximum),
the frames over 1.5 and 2 times the median, and the mean change between consecutive frames (high when they
alternate), so an A/B run (`x360_ab_estados`) can tell which setting makes them.

## Detail distance (mp3_gpu.cpp)

The float at 0x82A931BC is 1.0 in the image and nothing writes it; `sub_821DFFE8`, which sets up each render phase's
scan (scene, shadows, reflections), multiplies it into the phase's LOD multiplier: an entity is drawn while its LOD
distance (+80) times that exceeds its distance to the camera (`sub_821D8848`, `sub_821D6260`, `sub_821D8CD8`,
`sub_82377170`), and the product stored at 0x82A931B4 also picks model detail levels (`sub_824F3418`), switches car and
ped detail and releases drawables beyond lodDist x scale + 50 (`sub_824E20C8`). It looks like the console's leftover of
the PC detail distance. `mp3_distancia_lod` (0.75 by default, 0.3-2.0) is written there at each Swap when it changes;
low detail parents take over sooner, one draw for many children. Other levers found then (not used): the viewport
manager's factor at 0x82BEFA84 (set to 1.0 by a static constructor, scales two more readers), the timecycle far clip
(cuts little: the horizon is low detail), and the car and ped densities at 0x82AA1888/188C/2940 (reset by the game and
scripts: they need the setters `sub_823F2AB0`/`sub_823F2AC0` hooked).

## The ring thread's register writes and vertex copy (mp3_gpu.cpp, SDK patch 0083)

In run50 the ring thread was 58 % of a core; `RingGraphicsSystem::Packet` 8 % of it, half its samples on loading the
ring's words (just written by the game on another core) and on the old register value. Runs of shader and fetch
constants (type 0 packets, SET_CONSTANT, SET_SHADER_CONSTANTS) are now written in one loop with no per-register checks,
their generation bumped once (`mp3_registros_en_bloque`; the renderer only compares generations for equality), and
the ring is prefetched 256 bytes ahead of each packet. `CopiarVertices`' NEON loop held its block in a uint8x16x4_t and
GCC moved it through the stack (two stores and two loads per 32 bytes): a quarter of `RenderDk::Dibujar`'s samples. It
now uses four registers (ldp, rev32, stp; checked on the Mac against the word loop over 20000 random copies).

## Reflections every other frame (SDK patch 0082)

GTA IV draws its reflection (320x180 into the 7e3 view 0B8 at 4x, 54 draws and 1 ms in run50) and its environment
maps (256x256 down to 32x32 into 0E0, 280 wide at 4x) every frame; with their resolves and the transfers into those
views they were about 3 ms of GPU and some 100 ring draws per frame. With `x360_reflejos_alternos` on odd frames the
draws into 4x 7e3 views up to 320 wide that test or write depth are not recorded, and the resolves from the views they
skipped do not copy (their clears are still done). The resolved textures are kept by address and size, so the
reflection keeps the previous frame's image while the 640x360 scene downsample at the same base gets its own. The
luminance chain's small 4x views are k_32_FLOAT and draw without depth: untouched. The 10-second report counts the
skipped draws and resolves.

## Foliage, bridges and dead transfers (SDK patch 0076)

The host render targets are never multisampled (a "2x" view is EDRAM tile geometry only), so the game's alpha to
coverage (RB_COLORCONTROL bit 4) is drawn as Xenia draws it with one sample: an alpha test, alpha >= 0.5
(`x360_alfa_a_cobertura`). Without it the trees' leaf cards were drawn whole (black squares) and far bridges' cable
planes became dark triangles, depth included. A pass whose first draw paints all its color targets opaque (a
full-target quad without blending, tests or discard) does not receive the other views' content
(`x360_edram_saltar_muertas`): GTA IV's shadow map conversion to R32F got 3 ms of reinterpreting transfers. The rows a
pass prepares are also bounded by the viewport when clipping is on. The mixer skips mixes with gain and ramp 0
(`mp3_mezclador_saltar_ceros`; not bit-exact in the sign of zero, denormals and NaN).

## Core 3 for the rest of the audio (SDK patch 0081, mp3_perf_hooks.cpp)

Run49 (vertex fetches fixed: GPU 37-45 ms) was CPU-bound: the frame's threads (main 54 %, the game's render thread 42 %,
the ring 59 %) shared cores 0-2 with the XMA decoder (30 %), the game's audio engine (26 %) and the job threads, while
core 3 ran the mixer alone at 50 %. The XMA Decoder and Audio Worker host threads now prefer core 3 when the process has
it (`audio_hilos_nucleo3`), and so does the thread that runs the audio engine's update (sub_8291A580, named
mp3_AudioEngineUpdate; `mp3_audio_motor_nucleo3`; never the main thread). Their masks stay the whole process, so they
can still move.

## Vertex fetches into r0 (shaders/XenosRecomp)

Run48's A/B intervals (`x360_ab_estados`) showed the ZCULL guard and alpha to coverage cost nothing; the GPU time grew
~10 us per draw. The translator's check "is r0.x rewritten before this vfetch" scanned the body printed so far, and the
fetch's own destination ("r0.xyz = ") was already in it: every fetch into r0 became a manual one (dsVfetch: a storage
buffer load and a format decode per vertex instead of a vertex attribute), in 188 of 639 vertex shaders, the G-buffer's
VS n1230 among them (8.7 KB of code). The scan now stops at the instruction; 5 vertex shaders keep manual fetches.

## Black buildings after a few minutes (SDK patch 0079)

The texture cache was released by megabytes only (512 MB) and never reached that: after four minutes GTA IV had 9230
textures in 216 MB, more than the 8192 image descriptors, and new textures got slot 0, the empty one: whole buildings
came out black. The cache is now also released by count (`x360_texturas_max`, 6000: textures unused for 120 frames,
down to 75 %), the descriptor pool has 16384 slots, and running out is logged.

## The frame's size (mp3_gpu.cpp)

GTA IV takes a width and height from the video mode XGetVideoMode reports (capped at 1280x720, kept at
0x82B0B45C/60), but its render targets stay 1280x720 whatever they are: run46 with `video_mode_width/height` =
1024x576 still resolved 1280x720 frames, while the fixes that had taken the size from those settings stopped widening
the G-buffer's first band and presented a 1024x576 corner. The size is fixed at 1280x720 again; a lower internal
resolution needs the game's render target creation found and changed.

## Core 3 for the audio mixer (console setup, not in the repository)

Launched by title takeover (holding R), the process gets its core range from hbloader's own NPDM: Atmosphere's loader
copies only the pool partition and the program info flags from the game taken over. The console's
`/atmosphere/hbl.nsp` was patched (2026-10-24, the original kept as `/atmosphere/hbl.nsp.bak`): in `main.npdm` the
ThreadInfo kernel capability of the ACID and of the ACI, 0x020073B7 (priorities 28-59, cores 0-2), became 0x030073B7
(cores 0-3); two bytes, the ACID's signature is empty as before. With core 3 in the process mask
`mp3_AudioThreadEntry` (src/mp3_perf_hooks.cpp) makes the XAudio2 thread prefer it (`mp3_audio_nucleo3`).

## The ring thread's hottest loop (SDK patch 0078)

Run44's ring thread (58-66 % of a core at 3000 draws a frame) spent 60 % of `RenderDk::Dibujar`'s samples on one
instruction: the vertex and constant upload's byte swap, compiled to one word per iteration (ldr, rev, str).
`CopiarVertices` now swaps 64 bytes per iteration with NEON for every Xenos byte order (checked on the Mac against the
word loop over 20000 random copies).

## 4x resolves (SDK patch 0077)

GTA IV halves its scene and its luminance by resolving the target read as a 4x view of half the pitch. The host images
are 1x, so a transfer put one of each 2x2 pixels into the 4x view (1.1 ms for the scene) and the resolve copied it.
When one 1x view of the same base, format and twice the pitch holds all the tiles, the resolve now averages its 2x2
texels directly (`x360_edram_reduccion_directa`), as the Xenos averages the samples; the copy engine paths and the lazy
front buffer still prepare the 4x view.

## Guest semaphores and events without a mutex (SDK patch 0074)

Run42's main thread was blocked 36 % of its time on the internal mutexes of the sync objects, 25 % in
`NtReleaseSemaphore` handing out jobs: the job threads poll their semaphores in WaitMultiple holding that mutex, and on
three busy cores a holder could be preempted for a time slice. On Switch the semaphore count and the event state are
atomics and the waits are the kernel's address arbitration (`svcWaitForAddress` / `svcSignalToAddress`): a release is a
compare-and-swap and a signal. WaitMultiple takes objects through `TryAcquire` (and gives them back with `Undo` when a
WaitAll cannot take all of them), and sleeps on a generation that every event or semaphore signal bumps, so a poll wakes
when something is signaled instead of after its 50 us - 1 ms step. Patch 0075 makes the diagnostics rarer (one
diagnostic frame and snapshot, the per-draw GPU profile every 120 s, a capture every 15 s).

## ZCULL and the lights' squares (SDK patch 0072)

Light and shadow passes showed scattered 4x8-pixel squares on edges and window frames: whole tiles lit or shadowed
wrongly (in 03F1E000 the green channel of a tile was 0). The G-buffer and the stencil volumes test depth
greater-or-equal, the light volumes test less, and the stencil function and reference change between them: the GPU's
ZCULL data, built for one test, culled or kept whole tiles for the other. `x360_zcull_guardia` invalidates it when a
draw tests the bound depth in the other direction or the stencil otherwise; the 10-second report counts them.

## The render targets' exponent bias (SDK patch 0072)

`RB_COLOR_INFO` bits 20-25 scale what the pixel shader writes by 2^bias. GTA IV accumulates its lights into the 7e3
scene target at -1 and resolves it at +1 (`resolve con exp bias 1`); without the -1 the lights clipped at 31.875 and
the resolve doubled them. The translator scales oC0-3 after the alpha test by the biases the renderer packs into byte
492 of the shared constants (`g_SesgosExpColor`, `x360_sesgo_exp_color`).

## Open (in the order they will matter)

1. The real codegen: unresolved branches, functions discovery misses (Liberty's 139 entries help), named functions.
2. Shaders: `.fxc` effects ("rgxa") inside RPF archives; an extractor (Liberty Recompiled has `tools/rage_fxc_extractor`
   for the effect files) plus an RPF2 reader; meanwhile the console's dumps.
3. The game's fixes from Liberty Recompiled's sources that a run turns out to need (stale child object guard in
   `sub_8297CE78`, streaming budget, frame limiter ...).
4. Review `src/mp3_gpu.cpp`'s Gears test modes.

## Levers for later (not started)

- The XAudio2 thread on core 3 and the priorities the Vulkan build tuned (cvars `switch_core3_thread_entry` etc. in
  `source/mp3nx-switch.patch`): rexglue-sdk-nx uses three cores today.
- ds2-nx's SDK patch 0007 (guest clock across sleep) and its section 5 notes on mtmsrd/XMA (0005-0007 here already
  carry the yield backoff, XMA locks and the XMA offset fix).
- A higher internal resolution than 640x360: the deko3d build has no NVK/shader-compile cost on the console.

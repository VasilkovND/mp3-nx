# Max Payne 3 on rexglue-sdk-nx (deko3d): porting notes

This folder builds Max Payne 3 (Xbox 360, Games on Demand, Title Update 6, disc 1) for the Switch on the stack of
gta4-nx: the game's PowerPC code statically recompiled to C++ (ReXGlue v0.10.0) and built for ARM, the game's command
ring read on the console and drawn with deko3d, shaders translated ahead of time. Max Payne 3 (2012) runs on RAGE like
GTA IV (2008), so the starting point is gta4-nx's tree as it was when GTA IV was finished (25-26 fps handheld at CPU
1785 / GPU 614). gta4-nx's `gta4/PORTING.md` and `gta4/CPU_OPTIMIZATION.md` hold the lessons this port starts from.

## The user's copy

| | |
| --- | --- |
| Title | 5454086B, two discs |
| Disc 1 | Games on Demand, Media ID 1DFE6863, header `3054723F8A3477D01A12`, 48 pieces (Data0000-0047, ~7.6 GiB) |
| Disc 2 | Games on Demand, Media ID 1FE2BB66, header `9C105251594712248CCA`, 47 pieces (~7.5 GiB) |
| Title Update 6 | `5454086B/000B0000/tu00000006_00000000` (18 MB, its display name says a Russian fix is packed in it), one folder per disc: `disc001/`, `disc002/`, each with its own `default.xexp` (they differ) and `update.rpf`, `commonup.rpf`, `platformup.rpf`, `mp_memory.rpf`, `sp_memory.rpf`, `common/data/pt/paynethresholds_*.rpf` and 22 levels' `script_final(_patch).rpf` |
| Executable | disc 1's `default.xex` (19.5 MB, version 0.0.0.6, entry 0x821E1228, stack 256 KB); TU6's `disc001/default.xexp` makes it 0.0.9.6 |
| Also there | `5454086B/00000002/`: 9 marketplace packages (DLC, ~640 MB), not used yet |

Both discs carry a `default.xex` of the same size; each disc has the RPF archives at its root (`common.rpf`,
`xbox360.rpf`, `audio*.rpf`), the movies (`common/movies/*.bik`, Bink) and `xbox360/` levels. Disc 1's `default.xex` is in
its piece 7: only Data0000 (the directory) and Data0007 were copied to the Mac to build the code. On the console the
port reads the package where it already is (`mp3_god` in `mp3.toml`).

## What was taken from gta4-nx

- Everything, renamed `gta4` -> `mp3`: `src/`, `tools/`, `cmake/`, `sdk-patches/0001-0093` (unchanged: their cvars are
  `x360_*` / `nfsmw_*`, generic renderer and kernel fixes, some of them tuned on GTA IV's frames), the host patch,
  `shaders/` (XenosRecomp with its loop fix and 256 pixel constants), `tools/switch/`.
- GTA IV's hooks stay in `src/`, each compiled only when `codegen/names.toml` names its function (`MP3_TIENE_<name>`,
  CMakeLists.txt): spin lock, Direct3D waits, job queue stealing, async drain, audio threads on core 3, render thread
  priorities, predicated tiling off, the shader dump. None is named yet. Max Payne 3's counterparts have to be found and
  their bodies checked against GTA IV's notes before a name goes in.
- Not built: `src/mp3_audio_neon.cpp`, `src/mp3_audio_dsp_neon.cpp` (GTA IV's mixer DSP with constants at GTA IV's
  addresses) and `src/mp3_menu.cpp` (GTA IV's detail distance and population options). `src/mp3_gpu.cpp` no longer writes
  GTA IV's detail distance and density addresses (`kDireccionesDeGta4`).
- The codegen config keeps only the analysis settings; GTA IV's [rexcrt] and [functions] (Liberty Recompiled's GTA IV
  addresses) were dropped.

## Codegen

ReXGlue v0.10.0 (the host build of ds3-nx, the same tag and host patch) applies TU6's `default.xexp` (0.0.0.6 ->
0.0.9.6) and generates the code in ~55 s. The first run found 10 branches to 7 addresses outside any function, each a
tail branch to a small function after a blr/b/bctr: added to `codegen/mp3_config.toml` [functions].

## Unresolved-branch traps (tools/patch_branch_traps.py, from ds3-nx)

ReXGlue v0.10.0 writes `REX_FATAL("Unresolved branch/call from A to B")` for a branch it could not tie to a call
target. GTA IV never met one (Liberty Recompiled's config named its targets); Max Payne 3's first codegen has 329. `patch_branch_traps.py` (ds3-nx's) turns a trap whose target is a label of the same function into a goto
(300 of them: the codegen classifies the branch as a call although the label is right there), and one to a function's
entry into a tail call. The other 29 traps went to 18 addresses that are neither (mostly 0x82FFxxxx-0x8303xxxx, XDK and
CRT code with several entries): they are function entries in `codegen/mp3_config.toml` now, so the pass makes them tail
calls. A trap the pass cannot place stops the build.

## Run 1 (2026-10-26, handheld)

Everything up to the game's code works: the package in `god/` is found and mapped (48 pieces; "SVOD uses an XSF layout
... converted using a third-party tool", read fine), `default.xex` is copied to `god_xex/`, TU6's `default.xexp` is
applied (0.0.0.6 -> 0.0.9.6), `update/` is mounted (57 entries), the deko3d presenter and the ring reader start. No
shader library yet (expected). The main thread (start 0x821F1220) died at once on a trap: "Unresolved call from
0x821F42E4 to 0x821F43E0", a branch to the `__finally`-like tail of its own function (`sub_821F3B78`). That is what
the pass above fixes.

## Run 2 (2026-10-26, handheld)

Past the start: the main thread starts the game's threads (two at 0x8311C128, six at 0x82CB3200, two at 0x83092E10),
opens `common.rpf` from the package, registers the GPU interrupt callback (0x83080AD8) and its ring (8192 words), and
begins loading its shaders (`update:\common\shaders\fxl_final\im.fxc` first, not in TU6, so from the disc). Files it
asks for and does not find, as on the console without them: `game:\mega\global.rpf`, `game:\mega\disc1.rpf`,
`update:\audioup.rpf`, `update:\common\disc1.txt`. Then two stops:

- "Call to invalid or unregistered function at guest address 0x82CE4E90 (lr 0x82CF04C0)": a function reached only
  through a pointer, after a b; now a function entry in `codegen/mp3_config.toml`.
- "Jump target 0x83011130 unresolved at bctr 0x83010E20" (fatal): a 56-case jump table (r8 - 'A', the CRT's format
  parser in `sub_830109E8`, emitted twice) whose cases the codegen left as traps although their labels are in the
  function. `patch_branch_traps.py` now makes such a case a goto (112 of them).

## Run 3 (2026-10-26, handheld): the game runs, nothing is drawn

The game runs on its own (sound plays, 30 swaps/s, ~80 ring draws a second), loads its textures, UI and loading
screens (from the disc: TU6 overrides few of the `update:\platform\textures\*.xtd` it asks for first). Every draw is
skipped: no shader is known (`shaders: 0 loads ... objects 0`), as there is no library and no hook told the renderer
where the game's shader objects are. One thread faulted after two calls to functions discovery missed.

- Shaders: XDK Direct3D's `CreateVertexShader` (0x83080390) and `CreatePixelShader` (0x830801A8) found with
  `tools/fuzzy_find.py` against ds3-nx's named tree (ratio 1.0 each); `codegen/names.toml` names them, which turns on
  `src/mp3_shader_dump.cpp`: every container the game creates goes to `shaders_dump/` beside the NRO (none is in a
  library yet), the input of `tools/shaders.sh`. ds3's `D3D_GpuWaitStep` matches `sub_83076C70` exactly too
  (GTA IV's `D3D_WaitStep` hook, not named yet).
- Functions reached only through pointers: 0x824C3528 and 0x8260EFF0 (the run), and 38 more from a scan of the image
  for code addresses built with lis + addi/ori in another function, after a blr/bctr/b, where no function started.
  The 361 such addresses built inside their own function were left alone (labels: exception handlers, jump targets).

## The shader library (runs 4-5)

Runs 4 and 5 dumped 1953 vertex and 1844 pixel shader containers (6 MB; the game stays on its loading screen while
it writes them; one container cut short when the game was closed was dropped). `tools/shaders.sh` translates all of
them (XenosRecomp -> HLSL -> SPIR-V -> DKSH): 3795 shaders, a 67 MB library and a 55 MB DKSH cache (11,882 variants),
built in ~2 minutes. Two translator gaps GTA IV never hit, fixed in `shaders/XenosRecomp`:

- `getWeights2D` (the fetch-weights instruction, 430 pixel shaders: the shadow filters) was called with the slot's
  1/size like tfetch2D but declared without it.
- A 1D fetch from a sampler the constant table declares 2D (`materialIdSampler`, 19 pixel shaders) had no texture to
  read: it now reads the 2D texture's first row (`tfetch1D` in `shader_common.h`), as the Xenos does.

`shaders/build_library.sh` names the library `mp3_shaders.dslib` (it still wrote GTA IV's name).

## Run 6 (2026-10-27, handheld): the first frames drawn

With the library the renderer draws: 378 draws in 10 s at the end, 33 frames/s, every library shader the game loads
found by its object's address. The rest of the skipped draws: 1128 of one 24-word vertex shader loaded inline (no
address), a single point with no color or depth target (Direct3D's own, nothing to draw), and 237 with pixel shaders
the game created after runs 4-5 (23 new containers, dumped, now in the library: 3816 shaders).

Then the process ended when the game created its first save (`MP3_PROGRESSION`, content type 1): the VFS checks that a
cached entry of a writable device still exists with the throwing `std::filesystem::exists`, and on the SD card the stat
of the file the game had just created and still held open failed with an I/O error. SDK patch 0094 uses the
error-code overload: only a clear "not there" drops the entry.

## Run 7 (2026-10-27, handheld)

No crash in 42 s; saves open and close. In its last 10 s the game sent 8218 draws: 6216 were Direct3D's inline point
(nothing to draw), 1126 drawn, 155 rejected: rectangle lists (primitive 8) whose vertex shader (n89) reads packed
elements, which the expansion to quads refused ("not a float format"). `ExpandRectList` (src/mp3_gpu.cpp) now
extrapolates the fourth vertex for packed formats too (a + b - corner per byte, 16-bit or 10-bit lane in modular
arithmetic, half floats through float) and takes a 16_16 position. `telemetria.flag` beside the NRO turns on the
640x360 capture every 15 s (`capturas/`).

## Run 8 (2026-10-27, handheld): the first loading screen, then a freeze

The 15 s captures show the first loading screen drawn: the MAX PAYNE 3 logo (its broken-film effect) and "LOADING ...".
1506 draws in the last 10 s, none rejected (the rectangle lists are expanded). `[mem] process memory: 3281 MB used of
3285 MB` is the same from the start in every run (the reservation at startup, not growth).

Runs 7 and 8 both stopped ~43 s in at the same step, the whole process at once (not even the ring thread's 10 s report
after it): the game lists its saves, opens `MP3_PROGRESSION` (content type 1) and closes it, opens it again, and
nothing more is logged. That save was 0 bytes: run 6 created it and died before writing it (patch 0094's crash). It
was removed from the card (kept in the logs folder on the Mac), so the next run creates it anew; if the freeze
stays, it is in the read path of a save and needs a closer look.

## Run 9 (2026-10-27, handheld): gameplay reached, the scene white

With the empty save gone the game went through its loading screens, the intro (Bink movies, shown fine) and into
the first level: ~1200 draws a frame at 17-21 fps, GPU ~46-56 ms a frame, 47-49 copies and ~28 EDRAM transfers a frame.
The captures of gameplay are white but for the HUD prompt. The renderer's once-only warnings point at:

- cause 423: a `k_24_8_FLOAT` texture (depth as 20e4 float) sampled with no resolve found at its address and size:
  the draw gets an empty texture (depth 0 everywhere: fog, lights or depth of field reading it whiten the frame);
- cause 202: copies into `k_8` (one channel) refused.

SDK patch 0095 resolves into `k_8` and `k_8_8` (R8, RG8) and logs, for the first depth textures without a resolve,
their base and size and the resolves within 8 MB of it. `mp3.toml` takes one automatic dump of a gameplay frame's
resolved textures and three frame traces for the next run.

## Run 10 (2026-10-27): predicated tiling, as in GTA IV

The automatic dump's frame (2361 draws) was traced to the log before the game was closed (its BMPs were not written
yet). Its first 1122 draws: shadow maps (8888, pitch 800), small 8888 passes, the G-buffer at EDRAM 180 (pitch 1280, 4
targets, mask 7777) drawn twice, each time resolving its four targets and the depth (as 8888) to bases 384 rows apart,
with SET_BIN_MASK / SET_BIN_SELECT packets around: predicated tiling, two tiles. Then a linear depth at 640x360
(`k_32_FLOAT`) and one-channel `k_8` passes at 320x180 and 640x360 (refused until patch 0095).

XDK Direct3D's `BeginTiling` is `sub_83079948` (tools/fuzzy_find.py against GTA IV's TU8 tree, generated from the
user's own executable for the comparison: 0.734, next 0.267; the same use of its arguments), named in
`codegen/names.toml`: `src/mp3_tiling.cpp` asks for one tile, the union of the game's, as for GTA IV. Direct3D's wait
step for the GPU (`sub_83076C70`, ds3-nx's exactly) is named too: the waiting thread yields and sleeps.

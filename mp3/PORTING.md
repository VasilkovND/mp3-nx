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

## Runs 11-12: overexposed, not missing

With one tile the scene is still white, but the menu captures show it drawn whole (geometry, the hero's hands, the
bottle; shadows black, everything lit burned white): an exposure or HDR problem, as GTA IV had. All of GTA IV's
exposure fixes are SDK patches 0057-0072 and the translator, already here. Run 10's trace up to draw 1122 (the
G-buffer, the depth resolve 19048000 read as 8888 and as k_24_8_FLOAT, the 640x360 linear depth in k_32_FLOAT, the
k_8 occlusion passes) shows nothing wrong yet; the lighting, luminance and tonemap passes come after it. Run 12's
dumps never fired: with one tile a frame has ~690 draws, under `mp3_auto_volcado_dibujos` (800); 300 now.

## Run 13: the luminance chain is zero (SDK patch 0096)

The first dump with the frame's structure (`volcado_00`, 108 files) shows the scene resolved, and the exposure chain:
the HDR scene (EDRAM 2D0, 7e3) resolved to `k_16_16_16_16` at 080D0000 (1280x720), 05791000 (640x360) and 06E95000
(320x180), then `k_32_FLOAT` 64x64 (06F1D000) -> 16x16 -> 8x4 -> 8x1: all 0 from the 64x64 on, the adapted luminance
(05072000) 0, the exposure infinite. The log: "resolve con exp bias 11 (x2048) desde EDRAM formato 12". 7e3's maximum
31.875 x 2048 = 65280, the value the dump found in the chain: the Xenos stores the scaled 7e3 as a 16-bit integer, which
a fetch of `k_16_16_16_16` returns normalized (/ 65535); the renderer kept v x 2048 as a half float (65280 and beyond,
up to infinity) and read it as it is.

SDK patch 0096: such resolves (7e3 into k_16_16_16_16 / k_16_16, `x360_resolve_16_fijo`) store v x 2^bias / 65535, and a
resolved texture fetched with an exp_adjust (fetch constant dword 3 bits 13-18, ignored until now) is read through a
copy scaled by 2^exp_adjust. `mp3_traza_vport_max` limits the draw trace to small viewports (the luminance chain).

## Run 14: black instead of white; Max falls through the ground

The log of run 14: "resuelta ... leida con exp_adjust -11" and "-13" for the 1280x720, 640x360 and 320x180 scene
resolves. The game reads them as 16-bit integers (num_format integer) and scales them back itself (2^-11, 2^-13): the
resolve's v x 2^bias was right; dividing it by 65535 made the frame black. Patch 0096 now keeps v x 2^bias at the
resolve, scales a resolved texture's fetch by 2^exp_adjust, and divides by 65535 only for a 16-bit fixed-point fetch
as a fraction (num_format 0).

After a jump Max fell through the floor (GTA IV's car did at its start, fixed there by `patch_vmx_exact.py`, which runs
here too). The log had 36 calls to two functions the codegen did not know (0x82FBAFB8, 0x82FBAFC8: vtable thunks, run
only through pointers; their callers went on with nothing done). A scan for that shape (after a blr or bctr, lwz
rX,N(r3), then mtctr and bctr) found 22, all function entries now.

## Run 15: still burned; the log luminance (SDK patch 0097); performance

The fetches now read the scene resolves right (exp_adjust -11/-13 as integers), yet the 64x64 `k_32_FLOAT` of the
exposure chain was still 0. The trace of the small passes: draw 48 reads the 320x180 scene (06E95000) and writes the
64x64 `k_32_FLOAT`, which the next ones average down to 1x1. MP3 keeps the log of the luminance there, negative below
1; the k_32_FLOAT resolve kept, from GTA IV's patch 0070, only finite and non-negative values, so every log came out 0.
SDK patch 0097: k_32_FLOAT resolves drop only NaN and infinities. Patch 0096 marks a resolved image that holds 16-bit
integers (`entero16`: a 7e3 target into k_16_16(_16_16)); only those are divided by 65535 when fetched as a fraction
(two other 16-bit textures, 1536x512 and 512x512, were scaled by it in run 15).

Performance (run 15, CPU 1785 MHz, GPU 614 MHz): gameplay at 15-22 fps with 350-400 % of the 400 % CPU in use. From the
profile's stack samples:
- a host thread on core 0 at ~59 % compiling shaders (nv50_ir, glsl_to_tgsi: DKSH variants missing from the cache):
  the 181 variants the console compiled in runs 1-15 (1108 log lines) are now in `dksh_variants.txt`, and with the 10
  new shaders the cache has 12,230 DKSH;
- lock waits in every game thread (svcArbitrateLock, svcWaitForAddress); the game's spin lock (`sub_8220D850`, GTA IV's
  exactly) and Direct3D's frames-in-flight query (`sub_830853E8`, GTA IV's exactly) are named, so their hooks yield and
  sleep instead of spinning;
- the XMA decoder at ~36 % of a core.

## Run 16: playable picture, still too bright; black spikes around Max after bullet time

The parking garage draws whole, too bright; after bullet time black spiky triangles stand out around Max (vertices
going wrong somewhere: not looked into yet). The exposure chain is still 0 from its 64x64: the pixel shader that
writes it (ps_7e6d0c02a3470149) is a plain 16-tap average of `CurLumSampler` (the 320x180 k_16_16_16_16 resolve
06E95000, read as integers with exp_adjust -13) dotted with the luminance weights: no log, so that texture itself reads
0. The 16-bit resolves never reached a dump (the EDRAM views of each pass filled its 96 MB first): `mp3.toml` now
dumps resolved textures only (`nfsmw_dk_volcado_pase_desde`).

Performance (run 16, profile from ZL+ZR+R): gameplay 16-25 fps at 300-370 % CPU. The busiest thread (~69 %) is the
XAudio2 processing thread started at 0x83037B30 (1 MB stack): named `mp3_AudioThreadEntry`, it prefers core 3 now, as
in GTA IV. Then the main thread (~51 %), the ring (~42 %), the XMA decoder (~33 %). Most samples of the game threads
are waits (svcWaitForAddress, svcArbitrateLock), not work.

## Run 17: black spikes in the G-buffer; the 64x64 luminance still 0

The dumps now hold the 16-bit resolves: the 320x180 scene the exposure reads (06E95000) is not empty (0-6796, v x
2048), and another pass reads it right (its output 06DA5000 has the same range), yet the 64x64 written from it by
ps_7e6d0c02a3470149 (CurLumSampler in slot 0, as bound) is 0. `mp3.toml` dumps the EDRAM view of each pass at base
0x5A0 next, to tell whether the draw or its resolve loses it. One dumped frame was presented all black (the flashes).

The black spikes around Max are in the G-buffer itself (06A0D000): a mesh drawn with broken vertices. The library
match "by similarity" ranked candidates by the differing code words first and the differing fetch words (the vertex
layout, which D3D leaves alone) second: n1772 was taken at "3+5" over a "4+0" (84 times in runs 15-17), n2430 the same.
They are ranked by the total now, and `mp3_vs_por_parecido = false` skips such draws instead, to test.

## Run 18: the exposure chain works; black final frames; the spikes are patched vertex shaders

The dump of the EDRAM views at 0x5A0 showed the 64x64 luminance drawn right (0-3.15). The 64x64 resolve 06F1D000 is
written twice a frame: first from the scene (06E95000) by ps_7e6d0c02a3470149, then from a 2x2 texture at 1FCA2000 by
ps_3c1702bfdb485e92 (a sun-ray occlusion sum, 05072000): the dumps keep the last. The exposure itself is
`AdapLumSampler` = 05071000 (1.5 in the dump), read by the bright pass ps_08b725c9259e23cd as 1/adapted. The HDR scene
resolve (080D0000) looks right (a dark garage with its lamps); the dumped final frame (05F45000) was black but for the
HUD: the flashes. Next: the full-screen passes after the scene (`mp3_traza_solo_quads`: the draw trace keeps draws of
6 vertices or fewer).

The spikes: some vertex shaders still match no container exactly (n1778 "3+5", next "9+0"): Direct3D patches them
beyond the fetch bits when it binds a vertex declaration. With `mp3_volcar_desconocidos` the console writes each such
load as `vs_<microcode>_de_<container>.bin` in `shaders_unknown/`; `shaders/mp3_parecidos.cpp` (run by tools/shaders.sh
from `mp3/out/console_unknown/`) copies that container with the loaded microcode, so the library gets the shader as the
game really runs it.

## Run 19: patched skinned shaders into the library; the tonemap's exposure is a constant

`shaders_unknown/` brought 15 vertex shaders the console had taken by similarity. Against their containers: every
vertex fetch of the container is a placeholder (format 0, stride 0: `00000E50 00000000`) that Direct3D fills from the
bound vertex declaration (`00393E50 0000000C` ...), and in some it also turns ALU instructions into nops and a fetch
off (all components masked). Translated from the container, those fetches read nothing right: the black spikes.
`mp3_parecidos` built 16 containers with the loaded microcode; XenosRecomp now skips a fetch with no component read
(it wrote `r4. = ...` and 9 of them did not compile). Library: 3842 shaders.

The tonemap is draw 389, ps_cd9469857a0c532a (into the 8-bit 2D0): Reinhard on the HDR scene (080D0000, fetched with
exp_adjust -13), blur, bloom, depth of field; its exposure is not a texture but constants (`gAdaptedLum` c88,
`Exposures` c137, `ToneMapParams` c139, `BurnoutLimit` c142), set by the CPU from the small luminance resolves it reads
back. Those read-backs (`[exposicion]`) swing: 05071000 (adapted) 0.8, 1.5 and 128, 19000000 (current) 0 or 65280 =
7e3's 31.875 x 2048: some frames feed the chain the scene as raw 16-bit integers. The draw trace of full-screen passes
now prints every non-zero pixel shader constant, to see gAdaptedLum and Exposures. Run 19 had the render targets' color
exp bias off (`x360_sesgo_exp_color = false`): too dark, so it stays on.

## Run 20: no more similarity matches, the spikes stay; the tonemap's constants

With the 16 exact patched containers no vertex shader is taken by similarity any more (0 in two runs), yet Max himself
is still a black mass of spikes in gameplay (an enemy beside him, and the cutscenes, draw right): not the shader
choice. No vertex shader exports to memory (memexport would not translate; all 3842 do). Next A/B, settings only:
the geometry store fully checked every frame (`nfsmw_dk_geometria_confianza = 1`, `nfsmw_dk_geometria_muestreo =
false`) and the shader constants sent whole (`nfsmw_dk_constantes_por_diferencias = false`).

Tonemap constants (draw trace): gAdaptedLum c88 = 1.5, Exposures c137 0.85-2.1, ToneMapParams c139 (0.257 0.634 0.5
0.5), BurnoutLimit c142 8 (1 and 0.78 at times). Current luminance read back 0.2-0.9. With those the Reinhard curve
does not burn the frame: the brightness comes from what the tonemap reads or from what runs after it. The cutscene
captures look close to right; the garage in gameplay is washed out, with green-cyan glow at the foot of the pillars.

## Language, the final picture, automatic dumps off

- SDK patch 0098: `XGetLanguage` returned English always (a region guess) and `XamGetLanguage` was a stub; both return
  the `user_language` setting now (12 = Russian), as `XCONFIG_USER_LANGUAGE` already did. `mp3.toml` sets 12.
- `src/mp3_imagen.cpp`: the renderer's hooks give the output pass a brightness, contrast and gamma after the game's
  gamma ramp (`mp3_brillo`, `mp3_contraste`, `mp3_gamma`; 0.9 by default: the frame was a little too bright).
- The console's `mp3.toml`: automatic dumps, frame traces and the unknown-shader dump off (each automatic dump stalled
  the game 7-18 s); the 15 s captures stay; a dump on ZL + ZR + Left still works.

## Run 22: the spikes are not the geometry store

With the geometry store off (`nfsmw_dk_geometria_mb = 0`: no store statistics in the log) and the shader constants sent
whole, Max still turned into spikes after the jump: neither the store nor the constant diffs. Both are back on. Next: a
manual dump with the whole frame's draw trace (vertex fetch formats of Max's draws). `mp3_gamma` 0.8 meanwhile (the
picture still too bright at 0.9).

## Runs 29-31: the overexposure is not in the post-processing

- The tonemap is not one shader: cutscenes use ps_cd9469857a0c532a, gameplay ps_6671591252a5bce2 and
  ps_fe226603f40e2533. The gameplay ones blend `Exposures.x` towards `.y` (2.01 and 3.0 in the club),
  `ToneMapParams.z` towards `.w` and the gamma towards `deSatContrastGammaIFX` (c75) by an effects mask: a 128x128
  target (07367000) drawn from two textures (ps_7448e38c98eeb177). In the dumps that mask is white in R and B, so the
  effect values always apply (x1.5 exposure in the club). It is not known yet whether the real console's mask is
  white too.
- Checked against a dump with the HDR scene and the final frame from the same frame (club): the translated tonemap
  plus the mask reproduce the screen pixel for pixel (x1.5 fits best). The post-processing does what the game asks.
- The FXAA pass has no color exp bias (logged: RB_COLOR_INFO 00030198, bias 0); c2d9b50's guess was wrong and
  changes nothing.
- What is wrong is the scene itself: the HDR target is dark (garage walls ~0.4 under a lamp, most of the scene
  near 0), the luminance the game measures is 0.1-0.9, always below the level's AdaptedLumMin (1.5 garage, 0.71
  club), so the adaptation is pinned at its minimum and the exposure at its maximum. A dark scene pushed up to the
  maximum exposure: flat, grainy (noise amplified) and burnt where the few lights are. Next: the lighting passes
  (ambient, deferred lights), which give 5-10 times less light than the levels expect.
- `mp3_exposicion` and the 10 s exposure log now cover the gameplay tonemaps too (they only covered the cutscenes').

## Run 31: every color clear was red

Direct3D's clear shader writes its pixel constant c0, which holds the D3DCOLOR as A, R, G, B: the game clears with
c0 (1 0 0 0) and RB_COLOR_CLEAR FF000000, opaque black. The clear emulation took c0 as R, G, B, A, so every
single-sampled clear was red (000000FF). Visible where a target is drawn over only in part: the per-ped blood targets
(64x128, 128x256: red everywhere, so dead and wounded enemies were painted red all over), and the gameplay tonemap's
effects mask (07367000, R and B white: the effect exposure and gamma always on, x1.5 brighter). c0 is now read as
A, R, G, B.

## Run 32: the screen shows the raw HDR scene

With honest dumps (resolves copied, not swapped) the final front buffer matches the HDR scene target's EDRAM content
x4 (the lights' color exp bias), clamped to 1, pixel for pixel (error 0.0006; run 31's two dumps 0.0005 and 0.0017):
the game's tonemap (ps_fe226603f40e2533 / ps_6671591252a5bce2 / ps_cd9469857a0c532a into c198 f0, resolved to
0654D000) and its FXAA (ps_8d42760b0383c0d8 into c198 f3, the scene's own target) never reach the screen. That is the
overexposure since run 11: flat, burnt, no game gamma, and why mp3_exposicion barely changed anything. The HUD drawn
after the FXAA is there. The EDRAM log of the dumped frame shows no transfer between the tonemap's resolve and the
final one, so the FXAA's writes to c198 f3 are lost or never happen. Both passes are a single full-screen opaque
draw, the only ones on the HDR target's base taking x360_edram_saltar_muertas (no transfer before the pass): first
try, that off in mp3.toml. The 0654D000 dumps are black in every run (the tonemap's output, or its copy, empty).

## Run 35: black cutscenes, stripes over effects

Diagnostics (SDK patches 0300, 0301) showed the tonemap and FXAA draw into the right targets and the Swap presents
the right front buffer; run 32's "raw HDR on screen" was a dump artifact. After the FXAA, cutscenes (and effects)
draw their post-processing as 8-bit color on base 000, the depth's EDRAM base (ps n2516, n3681, n3629), resolve it
to 0634F000 and blend that over the frame (pass 155). The SDK took such a pass for GTA IV's depth restore
(x360_edram_z_por_color): its draws were not recorded and the depth's bits were written into the texture as color.
That is the black cutscene and the green-yellow stripes seen in captures. Off in mp3.toml.
Run 36: x360_edram_z_por_color off broke the main menu and darkened gameplay (Max Payne 3 does restore depth that way
too). Back on; SDK patch 0302 instead takes such a pass for a depth restore only when its pixel shader samples a
depth (k_24_8 or a resolved depth), so the cutscene post-processing, drawn from color textures, is drawn.

## Textures rechecked when the game writes them

Max Payne 3 streams textures into memory other textures used (Max in an enemy's black jacket, flicker, missing
textures). The renderer only learned of it on its periodic recheck (up to 39 frames; patch 0303 capped it, which cost
9-12 MB of hashing a frame). Now the XDK functions the game places data with are hooked (codegen/names.toml:
XMemCpyStreaming 0x8309B808, XMemCpy 0x8309B8B8, XMemDecompress 0x8309DAC0 / 0x8309DA98, found with
tools/fuzzy_find.py against Dead Space 2's named tree at ratio 1.0), src/mp3_texture_writes.cpp marks the physical
range each one wrote, and SDK patch 0306 rechecks the textures there at their next use. The periodic recheck stays as a
safety net at its default. The report line "texturas por escritura" gives the changes found with and without a write
before them: the second number says whether the hooks cover everything.

## Run 41: the clear color is RB_COLOR_CLEAR

The texture write tracking found no texture whose content changed in a whole run (0 changes with or without a
write): the flicker is not stale textures. The clear shader's c0 is (1 0 0 0) for every clear, so it is not the color;
RB_COLOR_CLEAR is (FF000000 for the G-buffer, 0 for the rest). From c0 the per-ped blood targets started opaque: enemies
black (blood everywhere, colour 0), and before ec6a1f4 red. Now from RB_COLOR_CLEAR.

## Run 41, read again: green blood and the green pools on the floor

Max's blood target (512x1024, resolved as k_4_4_4_4 to 0820A000) is cleared twice: as color, then its depth through
a 4x view of the same EDRAM (depth-only mode). The second clear's bits are the color the target keeps: z = 007F00 (the
log's "c0=000=007EFF00 (by the depth over it)"). Truncated, the color bits were 7EFF00: green 255 and blue 126 instead of
green 0 and blue 127. The ped shaders' blood color is (red, green, min of both), so every half-painted blood texel came
out green. EmulateClear now rounds the depth (D24) in both places: the color gets the same bits as the depth.

The large green patches on the floor and tables are the blood pool decals (ps_f82a9ef07114419e, library n3733). They
read the enemies' blood targets (64x128 k_8_8_8_8) and are clipped where the target's alpha is 0. From c0 those
targets were cleared opaque black (alpha 1), so each pool decal was drawn whole: a big ellipse showing the room's
reflection. 61aecfd (RB_COLOR_CLEAR, 0 for them) already fixes that.

Ruled out from the same data: auto-exposure (the adapted luminance stays at its minimum 0.71); textures changing
(0 changes); occlusion queries (none counted during play). The lamps' shadow maps are read correctly: 3x3 manual PCF
with point taps. The cached ones (16_16_FLOAT, restored each frame through oDepth by ps_71a67230a40a5d84) are read as
the game reads them.

Shader library numbers (the log's n) map to containers in the library's order: the scratch script that rebuilt the
table compared each entry's original bytes with out/shaders/contenedores.

## Run 42 (d27874e): the clear color is the clear rectangle's vertex color

Enemies still black, pools still on the floor: the enemies' blood targets still came out opaque black (dump: background
0 0 0 alpha 255). RB_COLOR_CLEAR is not the clear color either: Direct3D leaves it at whatever its last fast clear set,
so the same targets were cleared to 0 in one frame and FF000000 in another (run 42: bases 000, 030 and 040 both ways).
Direct3D's clear pixel shader (shaders_unknown/ps_d40dc7ffe00e2108, 36 bytes) is a single MAX of its interpolator
with itself into oC0. Its vertex shader (vs_6d38cfa50aa12e68) fetches the vertex's float4 color
(k_32_32_32_32_FLOAT, swizzle xyzw) and exports it. EmulateClear now packs that color as R, G, B, A bytes for 8-bit
targets, and logs it in the "clear draw" lines.

## Run 43 (aa0f4fe): colors right, the screen still flickers

The blood targets are now cleared transparent (enemies' background 0 0 127 alpha 0, their blood red), no green
pools in the three dumps, enemies not black. The clear log confirms the vertex color: the normal-map clear's vertex
color (0.498 0.498 1 1) matches the bits the game writes through the depth path to the same target (FFFF7F7F). The
whole-screen flicker cannot be seen in one-frame dumps, and it lasts one frame: too short to press anything during it.
mp3_grabacion (90) keeps the last frames presented: a thread of its own copies each new guest output at half size into
a ring. ZL + ZR + Down writes them as capturas/grabacion_NNN_KKK.bmp, and the log line gives the Swap count of each, so
the flicker can be looked for after it was seen.

## Run 44 (7b655dc): the green on floors is the decals' oC2 in the wrong target

G-buffer target 3 (0850A000, the lights' gDeferredLightSampler0P) is green in every dump since run 41: red 0, blue a
fixed 0.39 of green within a frame. That is not a color the dynamic environment map (05976000: dark, neutral) can give.
The decals that write c0, c2 and c3 with c1's mask 0 (ps_232fdc717e5e401f, library n528, 126 draws a frame on the
rooftop) made the renderer bind only the targets it had, packed: c2 at slot 1, c3 at slot 2. The pixel shader writes
oCi to slot i, so c3 got oC2 (the decal's specular terms) and c2 got oC1 (normals). SDK patch 0307 binds each target at
its own slot and fills a gap with another bound target's view under a write mask of 0. libdeko3d's
dkCmdBufBindRenderTargets reads every entry (objdump: no null check), so an empty slot cannot be passed.

## Run 45 (fdcf473): green gone; the flicker is 2-3 dark frames every 10-20

Recording 2 (a fight, Max on the ground) has, every 10 to 20 frames, 2 or 3 frames where the scene is dark navy: no
lighting, only particles, blood, the HUD and Max's black silhouette, then the normal frame again. The other jumps in
the recordings are other camera angles (the game's own flash cuts: 1, 4, 5). The recorder now checks each frame's mean
(FlashCheck). For the first three dark frames it logs every frame's mean and traces the structure of the next 30
frames ("[frame] end ... (Swap N)" against "[flash] Swap N: mean M"), and it writes the recording 40 frames later
without a button. The dark ones can then be compared with the frames around them.

## Run 46 (00752c9): the dark frames are frames that redraw a lamp's shadow map

The dark-frame trap traced 30 frames. The dark ones (Swap 2869-2870, 2850-2853) are exactly those where the game
redraws a lamp's shadow map after the G-buffer: depth passes at pitch 320 and 160 on the scene depth's EDRAM base,
resolved to 0808A000 and 081BA000. The game then restores the scene depth with a color pass on its base
(EdramZPorColor), which looked for the last depth resolve of base 000. That was the 128x128 shadow map, so the restore
was skipped (log: "profundidad 000 (960) por color: no se restaura, la resuelta tiene otro ancho"). The lights test
the stencil that restore writes (0xFF on geometry), so they drew nothing and only particles and the HUD were left.
SDK patch 0308 keys the resolve by base and pitch. The first two trap windows were loading screens (false alarms).

## Overexposure: the display's gamma ramp was never applied

The exposure chain is consistent. The scene target's color exp bias (x4) and the resolve's (x2048) are undone by the
tonemap's fetch (exp_adjust -13). The adapted luminance sits at AdaptedLumMin (0.71): the luminance measured is a
bright-pass average and stays under it, and with AdaptedLumMax 0.80 the exposure can move only about 12% anyway. The
effects mask (07367000, the DeathSampler) is 1 everywhere, but in the normal state Exposures.x == .y and the two
gammas match, so it changes nothing. What was missing is the last step of the console's output: the Xenos scans the
front buffer out through the gamma ramp the game programs in DC_LUT_* (256 entries of 10-bit RGB, or 128
piecewise-linear entries). The renderer always had RampaGamma, but nothing in the port fed it, so the frame went out
through an identity ramp. GammaRampWrite now collects the ramp from MMIO and ring writes (Xenia's command processor
logic; examples in register_table.inc). ApplyGammaRamp gives it to the renderer at the next Swap and logs it ("[gamma]
the game's ... ramp"). mp3_rampa_juego = false goes back to the identity.

## GPU: the HDR scene's resolve and read were two full copies

The GPU profile (run 46/47) shows 46-56 3D copies a frame. Among the costliest (0.5 ms each) are 960x540 RGBA16F
copies of the HDR scene: the resolve multiplies by 2^11 (its copy exp bias), then every fetch with exp_adjust -13 makes
another full copy multiplied by 2^-13 (ResueltaEscalada). SDK patch 0309 (x360_resolve_exp_previsto) writes the resolve
already scaled by the exp_adjust its texture's last fetch used, so that fetch samples it directly. A fetch with another
exp_adjust still gets a copy, scaled by the difference only. Not for resolves read back to guest memory, shrunk with a
blit or presented lazily.

## GPU: EDRAM reinterpreting transfers into the G-buffer

The real (not "as a clear") EDRAM transfers in the logs are mostly into the G-buffer's color views. Examples:
198 f3 960 -> 1B0 f0 960, 198 f3 960 -> 288 f0 960, 330 f3 240 -> 288/360 f0 960. These are the HDR scene's and the
bloom's tiles reinterpreted from 16F to 8888, about 1.5 ms of GPU a frame. Direct3D's clear of the G-buffer clears
c1-c3 through the depth path (EmulateClear's clear_depth_at), which on the console writes the same EDRAM bytes as
their color. Only the depth view was cleared, so at the next G-buffer pass the color views took the tiles' older
owners, and the sky kept them. mp3_borrar_color_de_alias (default true) now clears those color views to the same bits.

mp3_ver_texturas (ZL + ZR + Right) lists only screen-sized resolves now: G-buffer, HDR, tonemap, FXAA, the cutscenes'
film passes and the front buffer. Each press shows the next one on screen, captures it and logs which it is.

## Overexposure: scaled resolves overflowed half float

Run 49 (139bfe1): in gameplay 15-20% of the tonemap's output pixels (05F53000, before FXAA) are pure white. The final
pass does not add it. The bloom and HDR resolves (7e3 targets, written x4 by their color exp bias 2) are resolved with
copy exp bias 11 into k_16_16_16_16 and stored in RGBA16F images, up to 65280. Besides the -13 and -11 reads, the game
also reads the 240x135 bloom textures with exp_adjust +2. ResueltaEscalada made that x4 copy in RGBA16F, so every
value above 16376 became infinity: the glow around lights and windows went white. Patch 0309's exp_previo baked the
same +2 into the resolve itself. SDK patch 0310: copies scaled up go to RGBA32F, and a resolve bakes in only as much of
the expected exp_adjust as stays finite in half float. Nothing goes into an 8 or 10-bit texture, which would clamp it.

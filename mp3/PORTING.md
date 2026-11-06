# Max Payne 3 on rexglue-sdk-nx (deko3d): porting notes

## 0327: every f0 resolve installs - the latch fed the wrong one

0326's run: the ghosting GONE (the light adds were the accumulation), the white MUCH WORSE - and that names the
latch as the culprit: the f0 image resolves twice a frame, the once-a-frame latch fed only the FIRST, and the
tonemap's own resolve (the one the FXAA reads through 0654D000) is the SECOND - it carried the composite's white
into the FXAA unanswered (0325, latchless, had both fed, so its white was only the lights'). The latch is
removed: both resolves install the same scene (idempotent), the lights are already skipped, the claim stays.
Expected: the 0322-quality picture WITH the HUD and the FXAA's smoothing.

## 0326: skip the additive light adds on the installed scene

0325's run (mp3_070) confirmed the user's direction: the f0-resolve install (the only point where the HUD
survives) brings the white and the ghosting WITH it - the install fired cleanly (1.0 with the latch, the claim
in place), and c130 stayed (0.99, 0.01) through the windows (the slider untested). The white and the trails are
the DEFERRED LIGHT ADDS (the additive f12 draws, blend 0706, ps 3788): their amounts are tuned for the
TONEMAPPED base the game no longer has under them - on the installed scene they blow out, and their additive
output accumulates (whatever the bookkeeping does with it, frame over frame - the ghosting). 0326: the FXAA's
own draw (the last kPostEscena shader, into the f3/f12 image) arms a per-frame state, and the ADDITIVE draws
into that image are skipped for the rest of the frame (a 10 s log line counts them). The HUD that follows is
not additive and draws. The spikes of mp3_070 had ZERO address takes and no new dumps - not the address step;
the next suspect is the folds' elementos tables (D3D can reorder fetch instructions; the fold keeps the base's
indices) - deferred to its own check.

## 0325: the f0 install again, with the claim - and the game's brightness

mp3_065's trace settled the last structural fact: the HUD draws BETWEEN the FXAA and the lights (after draw
1950 the very next line is the final resolve and the frame end) - so the ONLY place the picture can be replaced
while the HUD survives is the f0 resolve (0323's spot), and 0323's white+ghosting was NOT the lights meeting the
raw scene (the scene is already LDR - mean 0.19, and the tonemap is nearly identity there) but the missing tile
claim: Copiar3D wrote f0 without claiming its rows, the next frame's EdramPreparar transferred an older owner
over them, the additive lights accumulated frame over frame - white WITH trails. 0325 restores the f0-resolve
install with the two fixes: EdramReclamar claims the written rows as f0's own (as EdramLlevar8Bits marked its
carried rows) and a once-a-frame latch (the f0 image resolves twice). The HUD rides on top by construction; the
FXAA smooths the scene (the grain).

THE GAME'S BRIGHTNESS (the Settings slider): the tonemap's own BrightnessSetting c130 is colour * x + y (the
logged default (0.99, 0.01), a lerp towards white) - dead with the tonemap. The tonemap draw now records c130
and RecalcularRampa applies it right after the game's ramp, before the menu's post-processing: the slider works
again (its value is visible in every [exposicion] line, so a run confirms whether it tracks).

## 0324: back to the frontal install, the omit back

mp3_068 (957adcccb9) read: the f0-resolve install FIRES (the counter 2.0 - the f0 image resolves twice a frame,
both fire) and the frame came back WHITE WITH GHOSTING while the HUD appeared. The white: the additive lights
(ps 3788, blend 0706) land on the RAW scene - they expect the tonemapped, compressed base and overbrighten it
(the wash mechanism, alive through the new path). The ghosting: the double install plus Copiar3D not updating
the f0 tiles' bookkeeping - the next frame's EdramPreparar transfers over it. 0322's frontal install (after the
lights, before nothing - the last operation of the frame) had neither problem: THE PICTURE WAS RIGHT. 0324
reverts to it; the HUD and the grain both hinge on the tonemap's own output and wait for that investigation.
The spikes of mp3_067: it ran BEFORE the folded library landed (the two address-takes it logged are exactly the
variants the rebuild folded; mp3_068 took them exact) - but the omit removal also unleashed unfunded jacket
variants into the scene passes; mp3_omitir_vs is back in the toml (the shirt stays invisible until its variants
are folded - the address dump keeps catching them every run).

## The scene into the tonemap's own resolve (0323) and the shirt

The first scene-install run: THE WASH IS GONE (the user's report). Four remarks followed, all addressed:

- THE HUD BURIED AND THE GRAIN: 0322 installed the scene at the FRONTAL copy, over the FXAA, the additive
  lights AND the HUD (mp3_065's dump frame settled the real order: tonemap(f0) -> composite stencil rounds ->
  the f0 RESOLVE -> FXAA(f3, ps 2276) -> the additive lights (f12, ps 3788, blend 0706) -> the HUD -> the copy).
  0323 moves the install to BEFORE THAT F0 RESOLVE - into the 8-bit target the tonemap drew on, recorded at its
  draw - so the game's own chain proceeds ON the scene: the FXAA smooths it (the grain), the lights and the HUD
  land on top, the frontal copy presents the lot. The base is re-looked-up at the install, so gaps with no
  tonemap draw (bullet time's entry second, streaming) keep installing from the last valid record - the white
  frames there came from the same-frame check 0322 had.
- THE SHIRT (level 3): mp3_omitir_vs listed "the two most re-patched jacket containers" - their loads are
  SKIPPED BY DESIGN, which is an invisible costume piece wherever that variant family dresses the model. The
  line is REMOVED from the toml: the 23 folds already carry the captured re-patches exactly, and anything
  unfunded now runs captured (the address-step dump) and folds next rebuild. One load still matches nothing
  (vs_ac27946eb1ea52b3, 78 words - no skeleton within tolerance in the whole library): its draws stay skipped;
  if it is the shirt after all, its container has never been dumped and needs the shader_dump path looked at.
- THE GHOST: a duplicated model beside the player is the wrong-layout draw class - the two new _de_ captures of
  this run (vs_979154c708f4b826, vs_a5d2c23549854cf8) fold now; whatever ghost remains next run gets captured
  the same way.

## The scene install (0322, 2026-11-05 night): present the healthy scene while the tonemap stays closed

mp3_066 (afbf26c) settled two things. (1) The 0321 gates made the machinery follow the FOURTH tonemap - [exposicion]
009B lines, probes on its draw, snapshots in the menu too - and its output is STILL 00000000 with sane constants
(Exposures 1.18), the folded quad, and the alpha test cleared: whatever kills the tonemap's output, it is none of
the cleared gates, and it has been there since run 19 (0654D000 black in every era). (2) The regression: the
after-draw SNAPSHOT's copy-engine dance (AntesDeTransferir ends the pass mid-frame) breaks the passes whose draws
FOLLOW the tonemap - the menu's and bullet time's own post chains died after it (the black menu, bullet time
gone). The installs in the menu windows read 0.0: the black menu was the snapshot's pass surgery, not the install.

THE STOPGAP THAT GIVES A PICTURE (0322, x360_edram_instalar_escena): the tonemap's own input - the scene resolve
- is healthy and already 0..1 (0309's baked scale; the 2026-11-05 dumps: 0674B000 max 1.01 mean 0.19, 08408000
max 2.2 mean 0.28). The tonemap draw now only READS REGISTERS (its fmt-26 sampler of screen width -> the resolve's
image), and the frontal copy installs THAT into the f3 view, between passes, like the resolve itself - no mid-pass
footprint. The frame presents the healthy lit scene: un-tonemapped, flat highlights, but real - not the wash, not
black. The snapshot path stays cvar-gated OFF in the toml (x360_edram_8bits_a_flotante removed): menus and
bullet time return to their own chains. The tonemap's zero-output remains the open question, now free of
presentation pressure.

## The fourth tonemap (mp3_065, 2026-11-05 late): the fold worked, the gates were looking elsewhere

The folded library did its job - "VS 8E66F3E2621BB0FA taken exact as n1088", the quad under its own fetch words
([verts-tonemap] PS n3266 VS n1088), zero new address-step dumps - and the picture did not change. The dump
frame's trace found why: the draw into 198/f0 pitch 960 (the tonemap's own sampler set: scene 08408000, half-res
0674B000, bloom 07317000, depth 0820A000 fmt 23, 2x2 05761000, mask 07367000) ran **ps n13 =
ps_009b4466c815681b** - a FOURTH tonemap, 267 words, none of the known codes (6671/FE22/CD94/8D42/FXAA). Every
gate list (the CD94 substitution, the pase_post flag, the snapshot's kPostDibujo, IsTonemap for the
sin_alpha/sin_sesgo epilogues and the [exposicion] line, kTonemapas, kTm) held the CLUB's tonemaps - this level's
own ran ungated: no pass flag, no snapshot, no install, and its frames presented the game's raw 8-bit copy (the
WHITE WASH). The CD94-huella windows (cutscene-ish states) still snapshotted and installed an empty snapshot -
the BLACK. Both user-visible symptoms, one missing list entry. Bullet time works through the game's own
composite-free path, as ever. The probes' 00000000 were reading the CD94 windows' draws, never this one - the
rooftop tonemap's own output has never been probed (patch 0321 adds it to every list; the substitution is left
alone: its own translation runs, which is the point). The v7 machinery now has everything it needs: a quad under
its own layout, a healthy input (mean 0.28), a faithful snapshot, a proven install - the missing link was the
gate knowing this shader is a tonemap.

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

## Runs 23-35 (2026-10-30/31): the overexposure taken apart

Patches 0311 (x360_traza_fetch_escalado: every scaled fetch of a resolved texture logged with its base, exp_adjust,
the resolve's baked exp_previo and whether it went through a scaled copy) and 0312 (the same trace names the pixel
shader, its slot, the fetch's TextureSign and format). Both default off; the console's mp3.toml runs them on.

Established, in order:

- The fetch scales are all correct. The tonemap (ps_3224, gameplay 6671) reads the scene resolve (base 198 as f12,
  x2048) with exp_adjust -13 net /4; the bloom tiles with -11 net x1 - the game's own constants, both faithful. The
  previous "the tonemap input is 16x too bright" was an artifact of a rough model: the port's scene matches the
  Xbox-derived expectation at the median (0.095 vs 0.086) and is only ~3x hotter in the top lit decile.
- The frame ends lights(f12) -> copy of the f12 view into an 8-bit texture (05F53000, the Swap's fetch) -> Swap.
  There is no tonemap pass after the lights in the traced frames: the game's own copy IS the final step. The 8-bit
  copy holds the raw HDR scene clipped white (median 0.94): what the user sees as the white wash.
- The tonemap that does run (draw 1205, ps_3237 = cd94, into base 198's f0 view) leaves its resolve (0654D000)
  black. Not the occlusion queries (mp3_oclusion_real = false changed nothing), not the lazy stencil
  (x360_edram_estencil_perezoso = false broke the whole scene flat 1.47 - that path is lazy for a reason).
- Scaling the 7e3->8bit resolve (patch 0313, x360_resolve_7e3_8bits_div) is wrong: those 8-bit copies are read back
  by the game's feedback passes; divided by 16 the picture collapses to black in a few frames, by 2 everything
  (UI included) darkens. Reverted to the default 1.0.
- The textures themselves are washed white in gameplay (the user's own reading, and the right one): the game's
  world diffuses are fetched with gamma TextureSigns, and nfsmw_dk_gamma_lineal = true (which reads them linear on
  purpose) changes nothing - they are ALREADY read without the gamma decode. FormatoVistaSigno cancelled the sRGB
  view whenever the alpha's own sign was not unsigned or gamma; patch 0314 lets any alpha sign keep the sRGB view
  (it decodes only RGB) and logs unhandled host formats as "causa gamma".

### Still open, in order

1. Patch 0314 (commit d19646c) verified on the console: NO visual change and no "causa gamma" line ever logged -
   the gamma-sign path is excluded. The washed look of the gameplay textures is not a fetch-sign problem; the
   strongest remaining explanation is item 2 (the whole presented frame is the raw clipped copy: its dump median
   is 0.94 with half the pixels at 1.0 - "white textures" and "a washed frame" are the same artifact).
2. The black tonemap output (0654D000) with a live input: its draw runs (-> drawn, stencil 00FFFF00, depthctl
   00700764 - the stencil test is on) and writes nothing. mp3_post_sin_stencil (d75ee85) took the test off to see
   the tonemap paint: the result was WORSE - the frame full of speckles and more white (the toml sets it false
   again). So the tonemap's draw is not simply stencil-gated: with the test off it paints noise, which points at
   its INPUT binding (a texture it samples resolved to garbage) rather than the coverage. The next step is to dump
   or trace what its samplers hold at the draw - the fetch map of 0312 covers the resolved ones, but its bound
   guest textures flow through the second PrepararTextura call site, which the trace does not cover yet.
3. CLOSED BY SUBSTITUTION (dba42a): the gameplay tonemaps ran as the cutscenes' CD94 (the log proves it fired) and
   the picture did not change - the working shader in the gameplay draw context gives the same broken presentation.
   The shader translation hypothesis is dead with proof; the tonemap's write-bias epilogue too (bias 0 at the draw,
   2e282cd's [exposicion] line), and the constants (register == UBO, 0319).
   THE REMAINING ROOT, consistent with everything including the cutscenes rendering right: the port's EDRAM view
   fragmentation. Base 198 lives as three host images (f0 8-bit, f3, f12 RGBA16F); the tonemap writes the f0 image,
   the game's final copy reads the f12 image (05F53000, the Swap's fetch), and run 32 already noted no transfer
   between the tonemap's resolve and the final one. Cutscenes are untouched: they present through base 000's own
   post chain (run 35). The fix is in the EDRAM one-store machinery: EdramPreparar/EdramCerrarPase must carry the
   f0 view's writes into the f12 view (or the copy must read through the store) for base 198's shape - a renderer
   architecture change, best started fresh (sdk/src/x360/deko/edram.cpp:289-411, destinos.cpp Copiar's EdramPreparar
   call ~1462-1467, and the per-(base,format,pitch,msaa) image keying in FormatoDestinoColorDk/DestinoColor).
4. The carry, ON CONSOLE (patch 0320, x360_edram_8bits_a_flotante, still DEFAULT FALSE): the first gated-off run
   (e725970, b.log) settles the three questions it shipped with. (a) The carry WORKS: the gameplay windows of the
   300-frame [edram] line counted 4.6-12.0 "de 8 bits a flotante" per frame, and the scenes whose last 8-bit
   writer before the final copy was the tonemap rendered with right colors - the f0->f12 handoff as values is the
   right idea. (b) The count was never stuck at 0: every window that read "0.0 de 8 bits a flotante" also read
   0.0 transferencias and 0.00 Mpixeles - Bink movies and loading screens where nothing claims EDRAM rows at all;
   the counter's reset (EdramFotograma) and increment (per carried destination) are sound, and the per-carry line
   never printed only because EdramRegistrando() was off. (c) The ungated carry IS the black screen: EVERY 8-bit
   pass on the base carried (composites, clears feeding back - about 12 a frame), and their stale or cleared rows
   buried the lit scene under black in most frames. The amendment folded into 0320: the carry fires only for a
   pass that drew with one of the frame's post shaders (edram_pase_post_, the tonemaps 6671/FE22/CD94 and the
   FXAA 8D42, set in Dibujar AFTER EmpezarPase returns - EmpezarPase closes the previous pass first, and that
   close would consume and reset a flag set before the call, so the tonemap's own carry would never fire and the
   pass before each post pass would carry in its stead); the destination lookup stays at format 3 and that is
   complete (a 12 bind never reaches destinos_ as 12: DestinoColor folds it into the f3 image of the same key and
   FormatoBase(12) is 3 - f3 and f12 are ONE image, and the b.log counts prove the lookup found it); the per-carry
   line now prints once per 10 s outside a dump frame, and a carry that fails says why once per 10 s (no 7e3 view
   of that base/pitch/msaa, or Copiar3D refused). One run with x360_edram_8bits_a_flotante = true: about ONE
   carry a frame in gameplay (the tonemap, "c 198 f0 960 -> 198 f3 960: filas 0-540" every 10 s in the log), the
   gameplay picture cutscene-like where it went black; anything wrong, toml-off reverts to today.

The spikes around Max, closed along the way: D3D re-patches the costume vertex shaders at the same address for
other vertex declarations (the jacket for the cloth, the jacket again when wounded, for the blood overlay's
streams). The validadas cache compared three sentinel words and served the stale layout (the word-for-word compare
of 49b9d61 re-applied); the masked-fetch match then accepted the base container for a re-patched layout - now
optional (mp3_vs_por_mascara, default on, off in the console's toml together with mp3_vs_por_parecido = false and
mp3_omitir_vs listing the two most re-patched jacket containers). New variants keep landing in shaders_unknown/
and fold into the library with tools/shaders.sh.

## Runs 36-37 (2026-11-01): the tonemap's own vertex shader is a masked-only match

final.log (14:53, the build of 7a2e0f3; the log itself starts mid-session, so the run is identified by its toml:
mp3_vs_por_mascara = false, mp3_vs_por_parecido = false, the two jacket containers omitted, the unknown dump on -
which is why the new masked-match logger printed nothing: the masked path itself was off) settles it:

- The tonemap did not draw at all: no "[exposicion] tonemap" line (l3.log, the build of ca77855 with the masked
  match on, has one every 10 s) and no ps 3237/3224 fetch line (l3 has n3237's five: the scene 08408000 at -13,
  the half-res 0674B000 at -13, the bloom tile 07317000 at -11, the depth 0820A000 fmt 23, the 2x2 05761000),
  while the rest of the post chain did ("[exposicion] fxaa", n660/n1028 fetches). With both matchers off, a draw
  whose VS matches nothing exactly is skipped: the tonemap's VS load matches no container exactly - every run
  before drew it through the masked path.
- Its 11,369 guest fetch lines are all sound: signos 00, exp_adjust 0, formats 6/18/19/20/59, nothing anomalous
  but 05318000 fmt 4 3D (the known k3D constant artefact).
- The masked match is the wrong layout, not a missing one: the tonemap's own resolve 0654D000 (straight off the f0
  view it drew into, no EDRAM handoff involved) is black with its inputs verified, and with the stencil off it
  painted speckles - garbage vertices. The quad VS family entry n1924 (the sondas of runs 25-26 show it driving
  the bloom, blur and FXAA quads, which draw right) has no skeleton twin in the library (checked against
  mp3_shaders.dslib), so the tonemap's load masked-matched a different entry whose vertex elements label other
  streams. The base 198 f0->f12 handoff cannot blacken 0654D000 and stays excluded.
- mp3_shaders.cpp: among masked-equal candidates the entry registered at the load's address is preferred (the
  address check refuses a re-patch's export words, not another layout of the same skeleton), and every distinct
  masked VS match is dumped as vs_<huella>_de_<container>.bin (a masked match was never dumped - that is why no
  exact container for the quad is in console_unknown): tools/shaders.sh folds it, and the next session runs the
  quad under its own layout. SDK patch 0316 caps both fetch traces at 512 lines (the streaming-texture flood cost
  2 fps).

Next console run: the console's toml back at mp3_vs_por_mascara = true (default) - with the dump on, one run
writes the quad's real microcode and names it in "[shaders] VS <hash> taken masked as n<N>", and the folded
container fixes the tonemap the run after.

## Runs 38+ (2026-11-01): the exact containers were never taken - identification step 0

The library of 15:00 (4020 shaders) already held the folded containers of every masked match: mp3_parecidos had
folded all 354 `_de_` dumps long before ("354 ya estaban, 0 nuevos"), among them the tonemap quad's
(vs_471aee299203383c, folded Oct 30 from vs_0be94fd3c65556fd_de_119028f2350aa4c0) and the costume re-patches. The
run after it (m.log, 15:16, 9acadda) STILL printed all 64 "[shaders] VS ... taken masked" lines - the quad's at
15:18:01.688, 0.2 s before its "[exposicion] tonemap" line - so the loads kept running on the BASE containers and
neither bug could change: the experiment said nothing about the exact containers, because no step ever compared a
load against one. `Leer` masks each entry's fetch words down to the bits D3D preserves
(mp3_shaders.cpp, kFetchConserva), so no stored entry can win a word-for-word compare; step 1 (address) and step 3's
address preference then hand the same skeleton's base container, whose translated fetches are another declaration's
(verified offline: the quad's patch touches only fetch words, 0 non-fetch words differ, so the base always passes
the skeleton checks). That IS the vertex-stream mismatch: `CalcularEntrada`
(sdk/src/x360/deko/dibujos.cpp:561) reads the layout (slots, formats, strides, offsets) from the ring's patched
words, but the pipeline's shader is the base's SPIR-V, translated from the base's fetch words - the declaration and
the code disagree inside one draw.

The fix (identification step 0, mp3_shaders.cpp): `Leer` now also keeps the fingerprint of the microcode as the
container holds it (fetch words included, hashed before masking), `Cargar` indexes every entry by it, and
`IdentificarEntrada` looks a load up in that table before the address step - among two entries with the same code
(the same load folded onto two base containers in different sessions) the one registered at the load's address
wins, as the masked step prefers. The report line and a "[shaders] VS <hash> taken exact as n<N>" line (first 64)
say what it takes. mp3_omitir_vs and mp3_vs_por_mascara stay as they are.

What one run shows: the 64 masked lines turn into "taken exact" (or vanish - the loads return at step 0), the
shader report counts a new "exact" bucket in the tens of thousands (every reload of the re-patched VSes), and the
tonemap quad and the wounded-costume draws run under their own layouts: 0654D000 should stop being black, and if
any artifact survives it is no longer explainable by the container's fetch translation.

## The [ubo-tonemap] line of 0318 read two cells that are neither the constants nor each other

The contradiction it left: the shadow half swung wildly (c88 -309.3, 26.96, 25.3, 0; c137 7.17) while [exposicion]
read the same constants sane (1.5, ~1.0). Both were honest readings of the wrong cells. `kRegConstantesPs` is a
word index (0x4400; every use is `r + kRegConstantesPs`), so the register half's `r[kRegConstantesPs / 4 + n]`
read ordinary registers at 0x1100+n, below the constants; and the shadow half's `sombra_ps_[n]` read WORD n =
register n/4, while the c-numbers (gAdaptedLum c88, Exposures c137) and [exposicion] number REGISTERS - its
`0x4000 + (256+n)*4` is word `0x4400 + n*4`, the very cells `EmpujarDiferencias` pushes. The swinging values were
register 22.x and register 34.x of the PS block: one UBO serves every pixel shader of the work (a single
`bloque_ps_`, re-allocated per work), so words outside the last push's range hold whichever draw covered them last
- another shader's constants, swinging draw to draw. The line never compared the UBO against the register file.

The rest follows. The two layers' logs alias the same storage with no copy lag: `p.registros` is
`registers_.data()` (mp3_gpu.cpp:2143), a live pointer the synchronous `Dibujar` (2292) reads before the hooks
restore their patches (2297). And the upload itself cannot go stale within a shader's declared range: any changed
word bumps `generacion_constantes_ps` (702-704, the batch run 818-828, the hooks' patch and restore 2221-2236 /
2297-2298), which resets `bloque_ps_cubierto_` (dibujos.cpp:2607-2610) and re-pushes [0, bytes_ps); a correctly
addressed shadow must equal the registers at the print. That is also why the whole-constant A/B
(`nfsmw_dk_constantes_por_diferencias = false`, dibujos.cpp:2680's memcpy of the same `r + kRegConstantesPs` over
the same declared range) changed nothing: both paths deliver the same bytes, and 54d1f18's "the constants reach
the shader" was concluded from a comparison that did not happen.

Patch 0319 makes both halves read the same cell, register n's first word (`r[kRegConstantesPs + n*4]` against
`sombra_ps_[n*4]`, skipped when `n*16+16 > bytes_ps`). One run: equal halves (c88 3FC00000/3FC00000...) close the
differential upload for good; a real difference is the first true evidence against it.

## The constants' delivery closed; the write-bias epilogue, the last suspect (this commit)

0319 ran: equal halves, register == UBO, every 10 s tick - the constants reach the tonemap's compiled variant, and
nothing between its inputs and its code is left open. What remains is what the code does AFTER the curve: every
translated pixel shader ends with `if (g_SesgosExpColor != 0u) oC0 *= exp2(signed byte 0 of g_SesgosExpColor)`
(shader_recompiler.cpp, the epilogue; ps_6671591252a5bce2.hlsl carries it), and the SDK fills that byte per draw from
the bound c0's RB_COLOR_INFO bits 20-25, signed, read live at the draw (dibujos.cpp, x360_sesgo_exp_color). The
tonemaps write the finished 0..1 picture into base 198's 8-bit f0 view - the same EDRAM base whose f3/f12 (7e3) words
carry color_exp_bias 2 (x4) from the lighting - so a bias byte on the f0 view's word would clamp the 8-bit output
white (positive) or crush it black (negative; the field is 6-bit signed, down to -32), the brightest pixels surviving
as speckles. The FXAA has had exactly this cleared since mp3_fxaa_sin_sesgo; the tonemaps never did.

What the offline logs say about the byte: never more than indirectly. The FXAA's own f3 bind reads 00030198 - bias 0 -
in every logged sample (920 lines across the runs), even though the same base's lighting word carries the 2: each
SetRenderTarget rewrites the whole dword, no staleness across views. And the SDK's per-session (bias, format) logger
(deko "destino de color con exp bias") printed only (2, f3) and (2, f12) pairs in every stored log - never a format-0
pair - while the tonemap lines printed in the same sessions: as far as the logs go, the f0 draws read bias 0. But the
tonemap's own draw never had its word printed (the [exposicion] line had no RB_COLOR_INFO; the [draws]/[frame] lines
print only base and format nibble), so the value at the draw is unproven, not zero-proven.

This commit: mp3_post_sin_sesgo (default true, the shape of mp3_fxaa_sin_sesgo) clears RB_COLOR_INFO bits 20-25 for
every IsTonemap draw (6671/FE22/CD94), restored after Dibujar exactly as the FXAA block restores; and the
[exposicion] tonemap line now prints the word it read - `RB_COLOR_INFO xxxxxxxx (formato N, exp bias N)` before the
constants. If a scene binds the f0 view with the byte set, the next run both shows it and draws past it; if every
line reads bias 0, the epilogue is excluded with evidence and the black 0654D000 is inside the shader's own math or
its inputs' content, not the ring state.

## The carry v5: the snapshot moves to the post DRAW (the close was one pass too late)

v4 (f399cd, v4.log) left gameplay black with the machinery working, and the logs say so:

- BOTH links fire in gameplay. The 300-frame [edram] line read 0.5 and then 1.0 "de 8 bits a flotante" in every
  gameplay window (19:42:41 on: 6.2-15.9 transferencias, 0.71-1.62 Mpixeles), and in v4 the ONLY increment of that
  counter is the install's Copiar3D (destinos.cpp), which cannot run unless a snapshot had succeeded
  (edram_posteado_valido_). (b) and (c) are excluded with evidence: it is (a) - what is installed is black.
- WHY, from mp3_030's traced frame (Swap 3652): the tonemap (draw 2285, vs 1924 ps 3237) and the frame's LATER
  8-bit writers are ONE renderer pass - identical claves (base 198, format 0, pitch 960, msaa 0): 24 prim-1
  no-VS probes (mask 0000, skipped), then twenty composite draws (vs 573 ps 1895: ten color-masked rounds, ten
  blending color, full-screen scissor, stencil refs 00/3F/01/02). The pass closes at the f0 resolve (0654D000),
  so v4's close-snapshot captured what the COMPOSITE left; v1's "correct colors" frames were those where the
  composite wrote nothing (the tonemap was the last 8-bit writer) - the tonemap's own output is the picture, the
  composite's color rounds are the blackener. The FXAA reads 0654D000 the instant after (v4.log: [exposicion] fxaa
  and "PS n2262 slot 0: base 0654D000" share a timestamp), which is how v1's bad frames went black on screen.
  Menus/cutscenes have no composite on the post base - v4 looked right there for the same reason.
- v5: Dibujar snapshots right after EACH post draw into an 8-bit target (EdramSnapshotPost - the same
  create/prep/Copiar3D the close did), then invalidates the pass binding (g_.pase_valido = false, the
  EdramEstencilPendiente pattern) and consumes edram_pase_post_ so the shared close cannot snapshot over it. The
  close keeps the snapshot as a fallback; the install is untouched. Cost: unchanged (2 copies/frame, one earlier).
- Instrumentation (always on, once per 10 s): "[edram] snapshot post <Vista> (WxH), version N: M imagenes de color
  de la base 198" plus three texel probes of the source and the snapshot ("[edram-diag] texel post fuente/snapshot",
  half floats, at the next Swap). One run decides: sane values + color gameplay closes the saga; all-zero probes
  exonerate the composite and put the black inside the tonemap's own draw (then the [verts-tonemap]/[ubo-tonemap]
  lines and the pass's other state are the frontier). The patch file 0320 was regenerated whole (the v3/v4 edits
  had never been folded in; it now carries all of v3+v4+v5 and reverse-checks on the tree).

### The carry saga, v1-v5 (2026-11-01, closes the day)

The frame map (mp3_030.log's traced frame + the [exposicion] timestamps): scene into 198/f12 -> bloom/luminance ->
THE TONEMAP (draw 2285, vs 1924 ps 3237) into 198/f0 -> 24 no-VS probes -> 20 COMPOSITE draws (vs 573, ps 1895,
full-screen, into the SAME 198/f0 view) -> resolve f0 -> 0654D000 -> FXAA into 198/f3 -> lights into 198/f12 ->
the final copy of f12 -> the Swap. The composite is the blackener: e725970's correct-color frames were exactly
those where the composite wrote nothing (the run log: 4.6-12.0 carries a frame in gameplay, colored when the
tonemap was the last 8-bit writer).

- v1 (e725970, ungated carry at every 8-bit pass close): colored frames + black frames, last writer decides.
- v2 (c1d817c, gated to post passes): the game's later f3/f12 passes undid the carry; the picture lived only in
  bullet time.
- v4 (f399cd, snapshot at the post pass's close + install at the frontal copy): menu and cutscenes COLORED (their
  post pass closes before anything else writes); gameplay black - the tonemap and the composite share ONE pass
  (identical claves), so the close-time snapshot ran after the composite and captured its black.
- v5 (ed100d0, snapshot right after each post draw, rebinding the pass): on console NEITHER the snapshot line NOR
  the install counter ever fired ("0.0 de 8 bits a flotante" in active windows, 1.2-1.4 Mpixeles a frame), and the
  picture reverted to the raw-white presentation - the after-draw wiring (the post-draw detection / the gate set in
  Dibujar) regressed between v4 and v5 and never engaged.

The spikes returned with the current library state too (the user's report; mp3_omitir_vs covers only the two
containers of 2026-10-31 - the wound variants of the newest shaders_unknown batch need folding or omitting).

NEXT SESSION, two concrete tasks:
1. DONE, superseded twice in one evening - v6 ran and went FULLY BLACK (below); v7 is the tree now.
2. Fold or omit the current costume variants (mp3/out/console_unknown has the 466-file set; tools/shaders.sh).

## The carry saga v6's post-mortem and v7 (2026-11-01, the late session)

v6 (433070f, v4's firing wiring + v5's after-draw moment, all cvar-gated) produced a FULLY BLACK screen on console -
even the cutscenes v4 had colored - and its own log lines were never read (the console was reverted immediately). The
v7 session rebuilt the model from the code and the four logs before touching anything:

- THE PASS MAP, verified in code this time: the tonemap (draw 2285), the 24 no-VS probes and the composite (draws
  2310-2329, vs 573 ps 1895) share ONE host pass. The probes are rejected at the VS lookup (dibujos.cpp ~567),
  BEFORE the claves comparison (~1840), so they never close the pass; it closes at the f0 resolve, after the
  composite. v4's close-only snapshot therefore ran post-composite: its 1.0/frame installs were the composite's
  output - black gameplay, exactly as observed.
- PUZZLE (ii) WAS A FALSE LEAD: the composite does NOT read 0654D000. Its only sampler is the DEPTH resolve
  0820A000 fmt 23 (mp3_030 draw 2310: "s0 tipo 12: base 0820A000 fmt 23 960x540") - it is the game's own
  stencil-directed effect layer (10 mask-0000 stencil rounds, then 10 mask-000F draws with stencil refs 01/02),
  rendered wrong in the port. Fixing "the f0 resolve's source" (the candidate (a) of the plan) would not touch it.
- PUZZLE (i) DISSOLVES: 0654D000 is resolved TWICE a frame - the G-buffer's c2 at base 288 first (mp3_030 line 635),
  then the tonemap's f0 (line 887) - and the resolve machinery itself reads the f0 image's own pixels correctly
  (v1's carries prove the swap/3D-copy source; the swap conditions all hold for the 8-bit resolve). The "f0 colored
  / 0654D000 black" disagreement compared different frames: in composite frames the black IS the composite's
  content, faithfully resolved. Nothing to fix in the resolve.
- WHY v6 BLACKENED EVERYTHING (its log was never read, so the exact draw cannot be named offline - what the tree
  shows): the after-draw snapshot INVALIDATED the pass (g_.pase_valido = false) after every snapshot, and the
  re-begin set edram_pase_post_ again from the next post draw - so the net widened from v4's "one snapshot per
  post-begun pass, at its close" to "every post draw of every post-begun pass, re-armed by the snapshot's own
  re-begin", while the install stayed keyed by dimensions alone. The installed content became whatever the LAST
  post draw anywhere wrote into any 960x960 8-bit target (effect/overlay passes included) - plus the mid-pass
  target-rebinding surgery (Copiar3D + re-begin + RestaurarContenido/CompletarBanda around each post draw) on
  shared paths the close had never touched. Both cut only in the scenes that depended on the install never firing.

v7 - the smallest footprint the model supports (still x360_edram_8bits_a_flotante, default false):
- THE SNAPSHOT moves to the copy engine, right after the FIRST post draw (the tonemap quad) of a post-begun pass,
  ONE per pass (edram_pase_post_ consumed on success): dkCmdBufCopyImage binds no render targets, sets no state,
  invalidates nothing - the pass and its recorded state g_ survive, no re-begin. No source preparation either (at
  that moment the target holds exactly what the draw wrote; v6's CompletarBanda here could clear the drawn rows
  mid-pass). Two barriers order the copy between the draw before it and the draws after it.
- THE CLOSE FALLBACK IS REMOVED: the tonemap's pass closes after the composite, so a close snapshot can only
  install a later writer's output (v4's black). The end-of-close flag reset stays.
- THE INSTALL is v4's verbatim firing plus one condition: only a snapshot of THIS frame installs
  (edram_posteado_fotograma_) - a stale side image must not paint a menu after gameplay.
- MENUS BY CONSTRUCTION: no 8-bit post draw runs there at all (v4.log's menu windows counted 0.0 installs and were
  colored by the default path) - nothing fires, nothing installs. CUTSCENES: their tonemap draw is snapshotted
  directly (the same content v4's close snapshot installed there, colored), the install lands after their whole
  post chain at the frontal copy, and v2's "later f3 passes undo it" cannot reach between the install and the copy.
  FAILURE IS SAFE: a refused snapshot logs its reason once per 10 s, leaves edram_posteado_valido_ false, and the
  frame presents exactly as with the cvar off.

One console run decides (the exact lines in patch 0320's header): "snapshot post ... tras el dibujo" + ~1.0
"de 8 bits a flotante" + colored gameplay closes the saga; "no tomado tras el dibujo, <razon>" names a death;
no line at all = the gate (huella/pass flag - the [exposicion] tonemap line says which); the line plus zero
"post fuente" probes = the tonemap's own draw writes nothing (the shader chain, not this machinery). The
composite's own defect (its depth-derived layer renders wrong) remains open and separate: v7 restores the picture
by installing around it, it does not fix it.

## The capture run (mp3_064, 2026-11-05, f13d52aa94): the tonemap quad caught, black-vs-burnt answered

The address-step dump paid for itself in one run: 23 new `_de_` files, among them
**vs_8e66f3e2621bb0fa_de_770f9c458637b2e8** - the tonemap quad's own re-patch (logged
"VS 8E66F3E2621BB0FA taken by address as n1931"). Word-diff against the base: 9 words, ALL inside the four
fetch instructions (instruction 3-6, words 10-20) - `00000A88` -> `00393A88` (the real stride/format patched
in), `05F81000` -> `2DF81000` (another fetch slot), `0000023F` -> `4025323F`: the classic D3D rebind patch.
So the quad was NEVER running its own layout: the base 770f9c's declaration TABLE matched the binding's shape
(which is what closed the layout question too early on 2026-11-05), but the translated code fetched with
another declaration's words - garbage vertices, nothing rasterized.

- THE USER'S REPORT decoded: "picture only in bullet time, black screen otherwise". The black screen is the
  carry presenting the EMPTY snapshot at full rate: the install windows read 0.5 -> 1.0 -> 0.9 -> 0.8
  (22:14:53-22:15:24, rooftop) - the tonemap wrote nothing (probes 00000000 again), the snapshot carries that
  emptiness, and at ~1 install/frame the Swap shows it. v7's run on the club read 0.4-0.6 on the same content
  and looked like the plain white wash - the install rate is scene/timing dependent (frontales_/same-frame),
  which is why the black was "never seen before". NOT caused by the dump change (it only writes files; the
  identification resolution is untouched).
- BLACK-VS-BURNT, ANSWERED BY THE DUMP: the tonemap's INPUT is healthy on the rooftop (08408000: mean 0.28,
  max 2.0, 1.7% white - the "scene 5-10x too dark" worry was the garage's, not a blocker here), and 0654D000
  (the resolve, post-composite) is BLOWN WHITE: mean 1.00, 51.6% pure white, 100% non-dark. The tonemap's own
  pre-composite output is ~nothing (the probes); the COMPOSITE fills f0 with white garbage on this level (it
  blackened the club in v4 - scene-dependent garbage, its own known-open defect). The deferred lighting
  finding stays deferred: with a healthy input a working tonemap will look right.
- BULLET TIME still shows the picture (the game's own composite-free path leaves the tonemap's output last -
  v1/v2's observation; it does not go through the broken quad variant's pass or presents before the composite).
- NEXT: fold the 23 (tools/shaders.sh) - the tonemap quad under its own fetch words should draw the real
  picture into f0, the v7 snapshot (proven faithful, right after the draw, before the composite) carries it,
  and the install (proven to reach the presentation - this run's black) shows it: normal gameplay colored, the
  same way bullet time already is. The costume re-patches among the 23 fold by the same mechanism.

## The v7 run read (mp3_063, 2026-11-02): the machinery works; the content is not the picture

The outcome branch was "the content itself" - and the run settled everything around it:

- THE MACHINERY: "snapshot post c198 f0 960 #1439 (960x540 de 960x960) tras el dibujo" fired once per frame in
  gameplay (the version counter 1 -> 664 in 80 s; 19-29/s in the good windows, 2-3/s in others - scene-dependent:
  the gate needs the tonemap draw to BEGIN its pass), zero "no tomado", installs 0.1-0.6 "de 8 bits a flotante"
  per frame (v4 proved 1.0 and that the install reaches the presentation - its black showed). The user's report:
  the picture unchanged (the white wash), the game playable - failure presented exactly as cvar-off, as designed.
  Installing what f0 held changed nothing on screen, so what f0 holds after the draw ~ what presents anyway.
- THE CONTENT: every probe set (one per 10 s, 7 windows, gameplay and cutscene-ish alike) reads "post fuente" AND
  "post snapshot" 00000000 at (480,270), (240,405), (720,162) - the copy is faithful; the f0 view right after the
  tonemap's draw holds no picture there. Alpha 0 too, and the tonemap's oC0.w is a luma: a drawn pixel would not
  read 0 at three spread points of every window.
- THE VS LAYOUT IS CLOSED (runs 36-38's theory, for the current state): [verts-tonemap] says "PS n3247 VS n1931"
  in v4 AND v7. The entry numbers are the dslib's XXH3-sorted order (n262 = vs_119028f2350aa4c0, the folded quad
  n1142 = vs_471aee299203383c): n1931 = vs_770f9c458637b2e8, a 36-word VS - NOT the 119028 family (48-word;
  that identification was another draw, the bloom/FXAA quads). Its own declaration (4 elements: position + 3
  texcoords, every fetch at slot 31) matches the current binding (one stream, slot 31, stride 52, 4 vertices,
  208 bytes) and the guest bytes are clean floats (quad corners -1..1, small UVs). The load runs under a layout
  that fits; the vertex data is clean.
- THE STATE AROUND THE DRAW: mp3_post_sin_stencil = true (default, not overridden in the toml) - the stencil test
  is already off at the tonemap; mp3_tonemapas_cd94 = true - the gameplay tonemaps run as CD94 (the [exposicion]
  lines say CD9469857A0C532A in the garage windows, gAdaptedLum 1.46 / Exposures 0.85, sane); the write bias is 0
  (RB_COLOR_INFO 00000198, formato 0, in every line).
- WHAT REMAINS, past every proven-clean gate (containers, vertex data, constants, textures, fetch scales,
  occlusion, gamma, alpha test, write bias, VS layout, stencil): the draw's own output. Two threads:
  1. Black-vs-burnt, one manual dump apart: run 20's offline reproduction said the translated tonemap + the mask
     reproduce the SCREEN pixel for pixel - if the console's f0 is the burnt picture (not black; the three probes
     could be dark points of it), the carry saga is DONE and the overexposure moves to the lighting passes.
     mp3_volcado_con_dibujos is already on: one gameplay window, ZL + ZR + Left, read 0654D000 and the f0 view of
     that same frame (the probes fire in the dump frame too).
  2. Runs 29-31's deferred finding, unchanged and now load-bearing: the gameplay HDR scene is 5-10x darker than
     the level expects (the lighting passes), the luminance always below AdaptedLumMin, the exposure pinned at
     its maximum - even a correctly carried tonemap output looks burnt in gameplay. The cutscenes (v4-colored)
     do not have this. If (1) shows burnt, this is the bug.
- THE SPIKES, this run - CORRECTED (2026-11-05): shaders_unknown/ held 14 files not in console_unknown, but the
  rebuild proved them STALE: every one of the 12 `_de_` variants' microcodes is word-for-word in the library
  already (some ARE the base containers' own code - dumped by the pre-step-0 runs of Nov 1, m.log and earlier),
  the rebuild from them came out byte-identical (md5 3e42e627, same 71,620,672 bytes as the console's), and the
  v7 run itself printed ZERO masked lines - step 0 matched everything it could. So the v7 spikes are NOT masked
  matches. The silent path left is the ADDRESS step: reaching its success branch already proves the load's code
  is no entry's own (step 0 would have returned it; and no two containers share a code with different
  declarations - checked offline over the whole dslib), so every load it TAKES is D3D's re-patch running on the
  container's own declaration - the jacket-VS mechanism verbatim (the weights into the indices' register), with
  no dump and no log. f13d52a adds the dump there: each distinct VS load the address step takes is written as a
  `_de_` variant of the entry it runs on (as the masked step does) plus a capped
  "[shaders] VS <hash> taken by address as n<N> ... patched, not its own code" line. One run captures the live
  re-patches; fold + rebuild + upload the run after. The 2 loads matching nothing at all
  (vs_82a73ba3cae509c4, vs_d2052d39bbb3b843 - skipped draws) stay: raw microcode without a base container
  cannot be folded.

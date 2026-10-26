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

# CPU optimization of the recompiled code

GTA IV's city is CPU-bound on the Switch: the Vulkan build of this repository (kabba536, on LibertyRecompAndroid)
measured 15-20 fps on foot and driving, ~11 in the heaviest places, 25-28 indoors, at CPU 1785 MHz. This build moves
the game to rexglue-sdk-nx (the runtime of ds2-nx, dantes-inferno-nx and gears2-nx) and brings the passes over the
generated code that made those ports' game threads faster: Dante's Inferno ~38 -> 44-46 fps, Gears of War 2 24.9 ->
27.3 fps on average. The full account of the main pass, its analyses and the pitfalls met is gears2-nx's
`gow2/CPU_OPTIMIZATION.md`; this file says what it does here and what is different for GTA IV.

## The passes (`tools/codegen.sh`, in this order)

| Pass | What it does | Switch |
| --- | --- | --- |
| `patch_barriers.py` | sync/lwsync/eieio/isync (comments in ReXGlue's x86-minded output) become ARM fences; db16cyc an ISB pause; dcbt/dcbtst a prefetch | `BARRIERS=0` |
| `patch_stack_access.py` | guest stack (r1) accesses without `volatile`: one thread's memory, GCC may keep spilled values in registers | - |
| `patch_inline_calls.py` | same-file calls to functions no hook replaces call the body (GCC may inline); the register save/restore helpers become always-inline copies | - |
| `patch_local_regs.py` | every guest register in a host local; `ctx` exact only at calls and returns, for the registers that matter | `LOCALREGS=0`, `PLR_CHECK=1` |

Plus `codegen/mp3_config.toml`'s `[rexcrt]`: memcpy, memset, memmove, strncmp, strncpy, strchr, strrchr, strstr and
_stricmp of the game's statically linked CRT run as native code (addresses verified by Liberty Recompiled).

## Why the register pass

Every guest register of the generated code is a field of `PPCContext& ctx`, every guest memory access goes through
`base`, and the build uses `-fno-strict-aliasing`: GCC cannot prove a guest store does not hit `ctx`, so after each store
it reloads every register it needs and stores each one back as soon as it is written. In a hot function roughly half of
the instructions were that traffic. The pass gives each function local copies (`L_r3`, `L_f1`, `L_cr6` ...) and works
out, for the whole program, which registers a call or a return must put back:

- `REF(F)`: what F (or its callees) may read before writing; `MOD(F)`: what may differ in ctx when F returns;
  `PRES(F)`: non-volatiles F saves at entry and restores before every return; `LIVEOUT(F)`: what callers may read
  after F returns. Each is a fixpoint over the call graph, taken from the code, not from the calling convention (the
  game's compiler does not always follow it between its own functions).
- Calls the code cannot see into (indirect calls, kernel imports, hooked functions, `rexcrt_*`) are taken at the
  convention's word; their targets get LIVEOUT = ALL where native code is on the other side.
- A function the pass cannot follow (another use of `ctx`) is left as it is with REF = MOD = ALL.

Liberty Recompiled gets part of this from its own codegen switches (`non_volatile_as_local`, `cr_as_local` ...,
plus 738 hand-kept `sync_registers`/`share_registers` marks): those trust the ABI and zero-initialize the locals, and
the volatile registers r0-r13/f0-f13 stay in ctx. Here the pass covers every register, from the code.

## The check build (`PLR_CHECK=1`)

`PLR_CHECK=1 mp3/tools/build_mac.sh <game>` builds a slower NRO that verifies the analysis on the console:

- before each call it writes a numbered poison `0x5EAD5EAD_5E<site>` into the dirty registers it did not write back
  (the callee must not read them), and at each return into the changed registers callers should not read back; a fault
  at `0x5Exxxxxx` names the site;
- it snapshots the live registers a callee should keep and logs
  `[regcheck] site NNNNNN (function): the call to X changed r31 A->B` when one changed;
- `mp3/out/codegen/patch_local_regs.check.map` lists the sites.

Play 10-15 minutes (city on foot, driving, a mission, an interior), then `grep regcheck` in the log: no lines and no
`0x5E` faults means the fast build can be trusted. Do it after any change to the pass.

## How to measure

- `logs/rex/rex_perfil.log`: clocks (`relojes: CPU ... GPU ... memoria ...`) and per 10 s window `juego N fps` and each
  thread's CPU %. The stack sampler costs ~8 %: compare like with like.
- The game log's `[deko3d] ultimos 10 s: N fotogramas/s ... GPU M ms por fotograma` and `GPU parada entre trabajos X`
  (small: GPU-bound; large: the CPU side is the limit).
- Same places as the Vulkan build's README (interiors / the pier, the city on foot and driving, the heaviest spots),
  same clocks (CPU 1785 / GPU 768 / RAM 1600, docked), `LOCALREGS=0` against the default.

## What to look at once the code exists

- `patch_local_regs.log`'s last line: functions rewritten, functions left as they were. Any left as they were turns
  LIVEOUT off globally (safe, slower): find out why from the `left as it is` lines.
- `PLR_DEBUG=<file>` lists functions whose REF exceeds the ABI's (the game's own conventions); `PLR_DUMP=<file.pkl>`
  pickles the sets.
- Hooks: any guest function the sources name (`sub_X`, `mp3_*`, `__imp__...`) keeps its calls through the alias and
  gets the ABI's REF/MOD; a new hook regenerates the code (codegen.sh's input hash).

## Further levers (not started)

1. Liberty Recompiled's runtime measured three game threads spending ~23 % of their time in futex calls on the
   mtmsrd lock: here the Switch PCH already keeps that state per thread (`make_nx_pch.py` point 3, from ds2-nx).
2. PRES for vector and CR saves; call-heavy functions (see gears2-nx's file, "Next steps").
3. Cross-file inlining of hot call chains (unity builds or LTO of related units).

## Max Payne 3: where the frame goes (runs 17-28, 2026-10-27)

Measured handheld with the 10 s reports (`[deko3d] ultimos 10 s`, `[ritmo]`, `rex_perfil.log`), A/B through
`x360_ab_estados`.

| | GPU busy / frame | GPU idle waiting for the CPU | frame (median) |
| --- | --- | --- | --- |
| 720p, CPU 1785, GPU 614 | 37-80 ms (scene 21-27, copies 15) | 1-2 ms | 40-55 ms |
| 540p, CPU 1785, GPU 614 | 26-30 ms (scene 14-17, copies 9-10) | 4-10 ms | 36-45 ms |
| 540p, CPU 1020, GPU 460.8 (stock + profile) | 25-29 ms (copies 12) | 9-25 ms | 41-80 ms |

At stock clocks the CPU is the limit: no thread is at 100 %, but the four cores are 85-90 % busy (CPU total 330-360 %)
and the frame's threads wait for each other and for a core. Per thread at stock (run 27): XAudio2 mixer ~82 % of a
core, main ~50 %, GPU ring ~47 %, XMA decoder ~33 %, the game's render thread ~29 %, job workers 7-10 % each. The audio
(~115 %) does not fit core 3 and runs on the game's cores; it also underruns (200-900 requests without data per 10 s).

### Done (branch claude/kind-kalam-7c40c9)

| Change | Result |
| --- | --- |
| 540p (`mp3_resolution.h`, menu ZL+ZR+Minus): -width/-height params and the wanted size 0x83139EB0 rewritten | GPU -35 % |
| Handheld GPU 460.8 MHz profile at stock CPU (`mp3_gpu_portatil_mhz`, as ds3-nx) | applied, logged in rex_stderr |
| SDK 0201: guest clock without a mutex | job threads' 10-15 % in the clock mutex gone |
| SDK 0203: color resolves without copy (`x360_resolver_color_sin_copia`, off) | 6.6 swaps / frame; 7e3 (f12) resolves keep copying |
| XAudio2 mono->5.1 mix in NEON (`mp3_mezcla_neon`, sub_8301B028) | A/B pending |
| `nfsmw_dk_envio_parcial` 64 vs 256 | no difference: dropped |
| Audio threads on core 3 only (`mp3_audio_solo_nucleo3`, SDK 0204) | 33 s freeze, worse underruns: stays off |

### Next, by expected gain (stock clocks)

1. Audio DSP natively, ported from gta4-nx's `mp3_audio_dsp_neon.cpp` (bit-exact, checked on the Mac against the
   generated code): Max Payne 3's RAGE audio has the same effects with other layouts. `sub_82C735E0` is the per-channel
   biquad (coefficients at +156..+172, states from +244, channel mask +276, two in series +277: GTA IV's
   sub_82936CE0); `sub_82C771D0`, `sub_82C745A8` are scalar FPU DSP of the same family; `sub_8303ECE0` is XAudio2's
   16-bit resampler (fixed-point position, VMX: GTA IV's own sub_821B0310 does the same job). Together ~30 % of a core at
   stock; gta4-nx got 2-3x on each.
2. Thread priorities (this branch): `mp3_RenderThreadLoop` (0x82200AB8, the render thread entry) and
   `mp3_JobWorkerMp3` (0x82DADC08) named, so `mp3_prioridad_render` / `mp3_prioridad_trabajos` (44-58, 0 = off, applied
   at each Swap) can be A/B'd: with the cores full, the main thread waits for these.
3. XMA: 11.7k contexts decoded per second (~11 us each at 1785 MHz); every kick and every lock goes through the MMIO
   handler (~3 us) and a lock waits for the worker's context mutex. `audio_xma_en_trabajador` 0 vs 1 is in the A/B.
4. GPU ring: 12-20 us to record a draw; `nfsmw_nativo_huellas_muestreo = 0` (full texture rechecks) is on until the
   render session's spike fix (ac52740) lets it go back to sampling.
5. 7e3 resolves (f12 -> RGBA8, the scene's tonemap input): still copied by 0203 (a conversion); 3-4 full-frame copies
   a frame at 540p.
6. Game-side load: find Max Payne 3's detail distance and ped density scales (gta4-nx's `gta4_distancia_lod`,
   `gta4_densidad_*`): fewer draws (ring, GPU) and fewer peds (main, jobs).

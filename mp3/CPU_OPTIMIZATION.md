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

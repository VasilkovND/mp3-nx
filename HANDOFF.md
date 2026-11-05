# HANDOFF — Max Payne 3 Switch port, the whiteness/black-screen saga

For the session taking over. Read `mp3/PORTING.md` (sections "Runs 23-35", the open tail, and "The carry saga,
v1-v5" + the v6/v7 addenda in git log) for the full history. This file is the operational state and the next step.

## The bug being chased

Gameplay picture wrong. Root structure (proven): the frame ends with the game copying EDRAM base 198's **f12 (7e3)
view** into the 8-bit texture the Swap presents → the user sees the raw lit scene clipped white. The **tonemap**
draws its finished 0..1 picture into base 198's **f0 (8-bit) view** EARLY in the frame; right after it, **20
composite draws (vs 573, ps 1895, blending; source = the depth resolve 0820A000)** draw into the SAME f0 view in
the SAME host pass and blacken it. Everything else is proven clean: exact shader containers, vertices, constants
(register == UBO), textures, fetch scales, occlusion, gamma, alpha test, write bias (0).

## Carry versions v1–v7 (all behind cvar `x360_edram_8bits_a_flotante`, default false)

- v1 `e725970`: carry at every 8-bit pass close. 4.6–12 carries/frame; **colored exactly when the tonemap was the
  last 8-bit writer** — the reference for "correct colors". Black otherwise.
- v2 `c1d817c`: gated to post passes; later f3/f12 passes undid it; picture only in bullet time.
- v4 `f399cd`: snapshot at post-pass close + install at the frontal copy. **Menus/cutscenes colored, install
  counter 1.0** (proven wiring), gameplay black — the snapshot ran after the composite (shared pass).
- v5 `ed100d0`: after-draw snapshot with pass rebinding — never fired (silent returns).
- v6 `433070f`: after-draw + close fallback — blackened even cutscenes (rebind side effects; log never read).
- **v7 `2da0fce` (current, on the console)**: zero-footprint — `dkCmdBufCopyImage` (copy engine, no rebind, no
  state) right after the FIRST post draw of a post-begun pass; no close fallback; install = v4's verbatim,
  only-this-frame. Menus safe by construction (no 8-bit post draws there). Every failure logs its reason once/10 s.

## THE NEXT STEP (v7 was run 2026-11-02; mp3_063 read; PORTING's "The v7 run read" has the detail)

The v7 outcome was "**the content itself**": snapshot ~1/frame in gameplay, installs 0.1-0.6/frame, ZERO refusals,
and every probe (fuente AND snapshot) reads 00000000 at three points right after the tonemap's draw - while the
user saw NO change (installing what f0 held = what presented anyway). The VS layout is now CLOSED clean
(vs_770f9c458637b2e8, its declaration matches the binding; runs 36-38's masked-layout theory was another quad),
the stencil is already off (mp3_post_sin_stencil default true), the tonemap runs CD94-substituted with sane
constants. What is left, in order:

1. **Black-vs-burnt, one observation run**: play to a gameplay window, ZL + ZR + Left (the manual dump;
   mp3_volcado_con_dibujos is on), quit, read that frame's 0654D000 + the f0 view + the probes (they fire in the
   dump frame). Burnt (not black) => the carry saga is DONE; the overexposure is the DEFERRED lighting finding
   (runs 29-31: the gameplay HDR scene 5-10x too dark, exposure pinned at max - the cutscenes don't have this).
   Black => the defect is inside the tonemap's draw, past every gate already proven clean.
2. **The spikes - CORRECTED (2026-11-05)**: the 14 "new" shaders_unknown files were STALE (pre-step-0 runs);
   their variants are already in the library, the rebuild came out byte-identical (md5 3e42e627), and the v7 run
   had ZERO masked matches. The live root is the ADDRESS step silently taking re-patched loads (reaching its
   success branch proves the code is no entry's own - step 0 would have returned it). f13d52a (NOW ON THE
   CONSOLE, verified by stamp + marker) dumps every distinct VS load the address step takes, as a `_de_` variant
   of the entry it runs on, plus a capped "taken by address as n<N>: patched, not its own code" log line. The
   next run (mp3_volcar_desconocidos stays true in the toml) captures them; fold + rebuild + upload the run
   after. The library itself needs NO upload - the console's is complete and byte-identical to the local
   rebuild.
3. Install counter 0.4-0.6 vs v4's 1.0 (the same-frame condition or frontales_ membership) matters only AFTER the
   content is right.

## The spikes (second bug, currently VISIBLE again)

Costume/cloth VS re-patches by D3D. Fixed once (word-for-word reload `49b9d61` re-applied; masked matching optional
`mp3_vs_por_mascara`; `mp3_vs_por_parecido = false` + `mp3_omitir_vs` with 2 containers in the toml; 466 variants
folded via `tools/shaders.sh`). Returned with later library state → next: harvest `shaders_unknown/` from the
console, diff against `mp3/out/console_unknown/`, fold + rebuild the library, and/or extend `mp3_omitir_vs` from
the `_de_ dump counts.

## Operations

- Console FTP: `192.168.3.26:5000`, root = SD (`/switch/mp3-nx/`). **ASK THE USER BEFORE EVERY UPLOAD** — their
  standing rule (they turn FTP off while playing). Upload in one foreground attempt, verify by re-reading the
  file (size + a marker string: the NRO's version stamp sits near `InformarSistema` ≈ offset 55.6 MB).
- Console now: NRO `f13d52aa94` (v7 + the address-step dump, uploaded and verified 2026-11-05), library
  byte-identical to the local rebuild (NO upload needed), `mp3.toml` with `x360_traza_fetch_escalado = true`,
  `mp3_vs_por_parecido = false`, `mp3_omitir_vs = "a186927bb5741b5c,f46d329fd9192dd1"`,
  `mp3_oclusion_real = false`, `x360_edram_8bits_a_flotante = true`, `nfsmw_dk_volcado_pase_base = 4095`,
  `mp3_volcado_con_dibujos = true`, `input_xbox_layout = true`, `user_language = 12`.
- Relief state if a build breaks rendering: remove `x360_edram_8bits_a_flotante` from the toml (cvar-gated code
  goes inert) → the white-but-playable presentation. That state is always acceptable to the user.
- Build: `cd ~/SwitchPorts/MaxPayne3_Switch/mp3-nx && cmake -S mp3 -B mp3/out/sw && cmake --build mp3/out/sw -j 8`.
  Reconfigure AFTER source edits (the version stamp is configure-time). sdk patches live as
  `mp3/sdk-patches/NNNN-*.patch`, applied in the dirty `sdk/` tree; regenerate the patch file when editing sdk
  sources (`git -C sdk diff` is the applied-patch state; `git -C sdk apply --reverse --check` must pass).
- Library rebuild: `mp3/tools/shaders.sh mp3/out/codegen/game` (~15 min) → `mp3/out/shaders/mp3_shaders.dslib` +
  `shadercache_dksh.bin`; upload both (~131 MB) when they change.
- Git: commit and merge ONLY as `git -c user.name=VasilkovND -c user.email=19706238+VasilkovND@users.noreply.github.com`,
  no Co-Authored-By, never the work identity. Repo public as VasilkovND/mp3-nx; push blocked until the user runs
  `gh auth login` as VasilkovND (gh is currently Vasilkoov, 403). 40+ commits await push.
- Token economy: the user pays per session and is sensitive to burn — one hypothesis per console run, written down
  BEFORE uploading; prefer offline analysis (logs under `/private/var/folders/.../opencode/mp3/` on this Mac:
  b.log=v1, g.log=v2, v4.log=v4, v5.log=v5, plus m*.log = the trace runs); subagents with fresh context are
  cheaper than long main-session iterations.

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

## THE NEXT STEP (0324 on the console, verified: NRO 0b41882464 + the toml with the omit back)

0323's f0-resolve install was read out of mp3_068 and reverted: it fired twice a frame (the f0 image resolves
twice), the additive lights overbrightened the raw scene they met through it (white again), and the stale tile
bookkeeping ghosted. 0322's frontal install is back (the state the user confirmed: THE WASH IS GONE, the picture
right). The toml carries mp3_omitir_vs again (the spikes; the shirt stays invisible until its variants are
folded - the address dump catches them every run, fold and re-enable without the omit when they are all in).
The library on the console is current (the ghost's two variants fold in mp3_068: no address takes). The deep
open question, now the only thing between this state and the finished look (HUD-over-scene, grain, the shirt):
NO tonemap draw (6671/FE22/CD94/009B) has ever written 198/f0 in normal gameplay - every gate around it is
proven clean; the next diagnostic is the draw itself (a probe inside its rasterization, its interpolants at the
draw, or the VS-PS linkage: the PS declares TEXCOORD0-12, the folded VS exports 0-10).

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
- Console now: NRO `b168903196` (0321 + the address-step dump + 0322's scene install, uploaded and md5-verified
  2026-11-05 night) and the FOLDED library (4055 containers, md5 7674383c - the 23 captured re-patches included,
  among them the tonemap quad's vs_42a1e87c725f14de); the DKSH cache is unchanged (the 23 new shaders compile on
  console at first use - one-off stutter, then dksh_variants.txt picks them up at the next cache rebuild);
  `mp3.toml` with `x360_traza_fetch_escalado = true`,
  `mp3_vs_por_parecido = false`, `mp3_omitir_vs = "a186927bb5741b5c,f46d329fd9192dd1"`,
  `mp3_oclusion_real = false`, `x360_edram_instalar_escena = true` (the snapshot's
  `x360_edram_8bits_a_flotante` REMOVED - its mid-pass surgery was the black menu), `nfsmw_dk_volcado_pase_base = 4095`,
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

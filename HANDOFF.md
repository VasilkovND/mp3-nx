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

## THE NEXT STEP (patch 0321 built, NRO pending upload): the fourth tonemap

mp3_065 proved the fold worked (the quad took its own container exact, n1088) and found the REAL miss: the
rooftop's gameplay tonemap is **ps_009b4466c815681b** (a fourth - the lists held the club's 6671/FE22, CD94,
FXAA). Ungated, its frames set no pase_post, snapshot nothing, install nothing - the Swap presents the raw copy
(THE WHITE WASH); the CD94 windows snapshot/install emptiness (THE BLACK). Patch 0321 + the mp3_gpu.cpp edits
add 0x009B4466C815681B to kPost, kPostDibujo, kTonemapas, kTm(via IsTonemap/kTonemapas), IsTonemap (the
sin_alpha/sin_sesgo epilogues, [exposicion], mp3_exposicion) and the bloom_juego list; the CD94 substitution
left alone (its own translation should run). With the quad folded and the gates knowing the shader, the v7
machinery has: healthy input (0.28 mean) -> own-layout quad -> snapshot after ITS draw -> install at the frontal
copy. One run: colored normal gameplay + [exposicion] tonemap 009B... lines + the probes sampling THIS draw.

1. Upload the rebuilt NRO (ASK FIRST), run, play the rooftop normally (and the club later - its 6671/FE22 path
   is covered as before).
2. Read: probes non-zero = the saga closes; still 00000000 = the tonemap's own draw (then the sin_alpha/sesgo
   coverage of 0321 is the first suspect it just gained, then its inputs at ITS draw via the new [exposicion]
   line).
3. Spikes: the 23 folds are in the library on the console - the costume verdict comes with this same run (the
   user has not reported them since; ask).

1. Fold + rebuild (running/done 2026-11-05): tools/shaders.sh; mp3_parecidos must report the new containers;
   upload mp3_shaders.dslib + shadercache_dksh.bin (~131 MB, ASK FIRST) - the console library is one fold
   behind.
2. The run after: normal gameplay colored + spikes gone is the expectation; the probes should stop reading
   00000000 and the install counter becomes secondary.
3. Still open after that: the composite's own garbage (its depth-derived layer, black on the club / white on
   the rooftop - bypassed by the carry's after-draw snapshot, not fixed) and the garage level's dark lighting
   (runs 29-31, deferred).

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
- Console now: NRO `afbf26cfe1` (v7 + the address-step dump + the fourth tonemap in every gate, uploaded and
  verified 2026-11-05) and the FOLDED library (4055 containers, md5 7674383c - the 23 captured re-patches
  included, among them the tonemap quad's vs_42a1e87c725f14de); the DKSH cache is unchanged (the 23 new shaders
  compile on console at first use - one-off stutter, then dksh_variants.txt picks them up at the next cache
  rebuild); `mp3.toml` with `x360_traza_fetch_escalado = true`,
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

# Backlog

State (2026-10-29): the game runs and can be played at 540p with a mild overclock. Two areas are not done: the
renderer (overexposure) and performance at stock clocks. Details of each item: `mp3/PORTING.md` (render) and
`mp3/CPU_OPTIMIZATION.md` (performance).

## Render

### Open

1. **Overexposure under lamps.** Lit areas are much brighter than on the Xbox 360. Dark beige surfaces glow white.
   Rooms without lamps look right.
   - Ruled out: the ped/player pass (the fog pass leaves pixels x1); G-buffer albedo and specular values; the tonemap's
     exposure path (applied); half-float overflow in scaled resolves (SDK patch 0310).
   - The 7e3 color exp bias: lights draw into format 12 (`k_2_10_10_10_FLOAT_AS_16_16_16_16`), the ambient/sun pass
     into format 3. Without the bias on format 12 (`mp3_sesgo_7e3 = 1`, default) the menu matches the original but
     scenes stay too bright. Without it on any 7e3 target (mode 2) the tonemap input is 16x darker than mode 1. The Xbox
     frame is about 2x darker than mode 1.
   - Tonemap: adaptation sits at the game's minimum (1.5). Current release settings: `mp3_exposicion = 0.5`,
     `mp3_gamma = 0.9`. They do not fix it.
   - Now on the console (NRO adaed8a): ZL+ZR+Right leaves out, in turn, each pixel shader drawing into the HDR scene
     (EDRAM 198, formats 3/12), with a capture each press. Next: find the press where the white goes away
     (`mp3_015.log`), then fix that shader's handling. After that, revisit exposure and gamma against the Xbox frames.
2. **Exposure and gamma back to identity** (1.0 / 1.0) once item 1 is fixed. They are workarounds now.

### Done

- Ghost silhouettes next to the player and objects: XenosRecomp dropped `getGradients` (fetch opcode 18). The lamp
  shadow casters' slope bias used the depth itself, so the maps held -10...-27 and inverted the shadows (7d22066).
- The lights' x4 exp bias on format 12 (bb81dd5): the menu matches the original.
- Scaled resolve copies no longer overflow half float (SDK patch 0310). Resolves written already scaled by the expected
  exp_adjust (SDK patch 0309).
- Alias color clears only at the scene's pitch (617a57c). Clears of c1-c3 through the depth path clear their color views
  (a779965, ~1.5 ms).
- The game's display gamma ramp applied (4eddef3).

### Diagnostics to remove before a release

- The shader search on ZL+ZR+Right (`mp3_busqueda_ps`, `mp3_busqueda_directa`).
- The 7e3 bias test toggle (`mp3_sesgo_prueba`) and the `[sombras]` constants log every 10 s.

## Performance

Measured handheld, the current build (runs 50-57, 2026-10-28; capped at 30 fps):

| Setting | Typical fps | Fights fps |
| --- | --- | --- |
| 540p, CPU 1785, GPU 614 (overclock) | 30 | 25-29 |
| 540p, CPU 1020, GPU 460.8 (stock) | 26-30 | 13-20 |
| 720p, CPU 1020, GPU 460.8 (stock) | 21 | not measured |

At stock clocks the CPU is the limit: the four cores are 85-90 % busy. Audio takes about 115 % of a core (XAudio2 mixer
~82 %, XMA decoder ~33 %) and underruns.

### Done

- 540p (ZL+ZR+Minus menu): GPU -35 %.
- Handheld GPU 460.8 MHz profile at stock CPU.
- SDK 0201 (guest clock without a mutex), 0205 (shader-read vertex buffers reused), 0206 (lock and wait syscalls
  counted), 0207 (events signal only their waiters), 0208 (RCAS sharpening of the stretched frame), 0209 (object lookups
  without the global lock).
- Native audio DSP, bit-identical to the generated code: reverb, peak limiter, biquad, mono-to-5.1 mix (NEON), S16
  resampler.
- Game detail settings (`mp3_detail`): no cloth, no face blend shapes, the game's own low-detail PARAMs. All off by
  default.

### Next, by expected gain

1. Thread priorities for the render thread and the job workers (`mp3_prioridad_render`, `mp3_prioridad_trabajos`): A/B.
2. XMA: every kick and lock goes through the MMIO handler (~3 us). A/B `audio_xma_en_trabajador`.
3. GPU ring: 12-20 us to record a draw. Texture rechecks back to sampling.
4. 7e3 resolves (format 12 to RGBA8): 3-4 full-frame copies a frame at 540p.
5. Game-side load: detail distance and ped density scales (as gta4-nx's `gta4_distancia_lod`, `gta4_densidad_*`).
6. Code generation levers not started: PRES for vector and CR saves; cross-file inlining of hot call chains.

## Repository

- Stays `mp3-nx`, as ds2-nx and ds3-nx: the game named, a Legal section in the README (not written yet).
- No prebuilt NRO or shader library in releases, ever.

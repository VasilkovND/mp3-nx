// mp3-nx - the XAudio2 mixer's hottest loop as native NEON code.
//
// Run 27 (stock clocks, CPU 1020 MHz) had the XAudio2 processing thread at ~82 % of a core, more than core 3 can give
// it next to the XMA decoder: the rest ran on the game's cores. Its busiest function, sub_8301B028 (13.5 % of a core),
// mixes one mono voice into a 5.1 bus with a per-channel gain ramp; the generated code runs its VMX128 vectors through
// per-instruction helpers. Here it is the same arithmetic on NEON.
//
// sub_8301B028 (r3 = a job of guest words):
//   +0  the input: n mono floats            +4  the output: n frames of 6 floats (interleaved 5.1)
//   +8  the 6 starting gains                +12 the 6 gain steps per frame (0: no ramp, the gains stay)
//   +20 n                                   +32 non-zero: add to what the output holds, else overwrite it
// For frame f and channel c it writes out[f][c] (+)= in[f] * gain_c, then gain_c += step_c. The guest multiplies and
// adds separately (vmulfp + vaddfp, and vmaddfp is a multiply then an add in the generated code too), so no fused
// multiply-add here; its 4-frame loop advances two frames' gains by twice the step, which rounds the ramp slightly
// differently (a few ulps over a 256-frame block, inaudible). The gains it is given are not written back.

#include "mp3_pch.h"

#include <rex/cvar.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

REXCVAR_DEFINE_BOOL(mp3_mezcla_neon, true, "MP3",
                    "La mezcla de una voz mono a 5.1 de XAudio2 (sub_8301B028) en NEON en vez del codigo generado");

namespace {

inline float LeerF(const uint8_t* p) {
  uint32_t u;
  std::memcpy(&u, p, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

#if defined(__aarch64__)
// Four big-endian guest floats to host order and back.
inline float32x4_t Cargar4(const uint8_t* p) { return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p))); }
inline void Guardar4(uint8_t* p, float32x4_t v) { vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v))); }
inline float32x2_t Cargar2(const uint8_t* p) { return vreinterpret_f32_u8(vrev32_u8(vld1_u8(p))); }
inline void Guardar2(uint8_t* p, float32x2_t v) { vst1_u8(p, vrev32_u8(vreinterpret_u8_f32(v))); }
#endif

}  // namespace

#ifdef MP3_TIENE_mp3_XAudioMixMonoTo6  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_XAudioMixMonoTo6);
REX_EXTERN(mp3_XAudioMixMonoTo6) {
#if defined(__aarch64__)
  if (!REXCVAR_GET(mp3_mezcla_neon)) {
    __imp__mp3_XAudioMixMonoTo6(ctx, base);
    return;
  }
  const uint32_t trabajo = ctx.r3.u32;
  const uint8_t* entrada = base + REX_LOAD_U32(trabajo + 0);
  uint8_t* salida = base + REX_LOAD_U32(trabajo + 4);
  const uint32_t ganancias = REX_LOAD_U32(trabajo + 8);
  const uint32_t pasos = REX_LOAD_U32(trabajo + 12);
  const uint32_t n = REX_LOAD_U32(trabajo + 20);
  const bool sumar = REX_LOAD_U32(trabajo + 32) != 0;

  float32x4_t g03 = Cargar4(base + ganancias);
  float32x2_t g45 = Cargar2(base + ganancias + 16);
  float32x4_t d03 = vdupq_n_f32(0.0f);
  float32x2_t d45 = vdup_n_f32(0.0f);
  if (pasos) {
    d03 = Cargar4(base + pasos);
    d45 = Cargar2(base + pasos + 16);
  }
  for (uint32_t f = 0; f < n; ++f) {
    const float x = LeerF(entrada + f * 4);
    uint8_t* o = salida + f * 24;
    float32x4_t r03 = vmulq_n_f32(g03, x);
    float32x2_t r45 = vmul_n_f32(g45, x);
    if (sumar) {
      r03 = vaddq_f32(r03, Cargar4(o));
      r45 = vadd_f32(r45, Cargar2(o + 16));
    }
    Guardar4(o, r03);
    Guardar2(o + 16, r45);
    g03 = vaddq_f32(g03, d03);
    g45 = vadd_f32(g45, d45);
  }
  // The guest leaves r3 pointing into its own stack frame; nobody reads it back (the callers reload it).
#else
  __imp__mp3_XAudioMixMonoTo6(ctx, base);
#endif
}
#endif  // MP3_TIENE_mp3_XAudioMixMonoTo6

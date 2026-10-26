// mp3-nx - GTA IV's audio mixer inner loop in native NEON.
//
// sub_82199BC8 (named mp3_AudioMixAddRamp in codegen/names.toml): dst[256] += src[256] * gain, 256 floats, with the
// gain ramping by `step` per 4-float vector. r3 = dst, r4 = src, r5 = the gain of the first vector, r6 = step (both
// guest vectors). As recompiled VMX every vector op goes through a stored PPC register; in the Vulkan build's city
// profile this one function was 12.7 % of all CPU time.
//
// From the Vulkan build of this repository (source/mp3nx-switch.patch, mp3_native_hooks.cpp; kabba536, GPL-3.0):
// same operations in the same order, so the samples are bit-identical to the generated code. vmaddfp is fused there
// as here (REX_VMADDFP = simde_mm_fmadd_ps = vfmaq_f32(c, b, a), tools/patch_vmx_exact.py), vaddfp is vaddq_f32 with
// the guest's operand order, the gains advance by repeated addition like the guest's, and flush-to-zero is the FPCR's
// (always on on the Switch, sdk/include/rex/ppc/context.h kFixedFlush). Unlike the Vulkan build's hook, this one also
// leaves in ctx every register the guest function leaves (r3-r11, v0-v13, cr6): the callers' register copies
// (tools/patch_local_regs.py) read back all the volatile registers after a call to a hooked function.

#include "mp3_pch.h"

#include <rex/cvar.h>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define MP3_AUDIO_NEON 1
#endif

REXCVAR_DEFINE_BOOL(mp3_mezclador_neon, true, "MP3",
                    "El bucle interno del mezclador de audio del juego (sub_82199BC8) en NEON nativo en vez del codigo "
                    "VMX recompilado (mismas muestras, bit a bit)");

REXCVAR_DEFINE_BOOL(mp3_mezclador_saltar_ceros, true, "MP3",
                    "Una mezcla con ganancia 0 y rampa 0 (un canal al que la voz no llega: 5.1 con panorama) no toca el "
                    "destino. No es bit a bit en el signo de un cero, los denormales y un NaN de la fuente: inaudible");

#ifdef MP3_TIENE_mp3_AudioMixAddRamp  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_AudioMixAddRamp);
REX_EXTERN(mp3_AudioMixAddRamp) {
#if MP3_AUDIO_NEON
  if (!REXCVAR_GET(mp3_mezclador_neon)) {
    __imp__mp3_AudioMixAddRamp(ctx, base);
    return;
  }
  ctx.fpscr.enableFlushMode();

  // Vectors here are in memory order (each word byte-swapped): lane k = guest element k. ctx keeps them as the
  // generated lvx128 does (VectorMaskL, the 16 bytes reversed): lane k = guest element 3 - k.
  auto load = [base](uint32_t ea) {
    return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(base + (ea & ~0xFu))));
  };
  auto store = [base](uint32_t ea, float32x4_t v) {
    vst1q_u8(base + (ea & ~0xFu), vrev32q_u8(vreinterpretq_u8_f32(v)));
  };
  auto to_ctx = [](PPCVRegister& r, float32x4_t v) {
    const float32x4_t pairs = vrev64q_f32(v);
    vst1q_f32(r.f32, vextq_f32(pairs, pairs, 2));
  };

  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;
  for (uint32_t i = 0; i < 1024; i += 128) __builtin_prefetch(base + uint32_t(dst + i), 0, 3);

  // The guest's register names, its operations and operand order.
  const float32x4_t step = load(ctx.r6.u32);
  const float32x4_t step2 = vaddq_f32(step, step);
  float32x4_t g0 = load(ctx.r5.u32);
  float32x4_t g1 = vaddq_f32(g0, step);
  const float32x4_t step3 = vaddq_f32(step2, step);
  const float32x4_t step4 = vaddq_f32(step2, step2);
  float32x4_t g2 = vaddq_f32(g0, step2);
  float32x4_t g3 = vaddq_f32(g0, step3);

  float32x4_t d0{}, d1{}, d2{}, s1{}, s2{}, r0{}, r1{}, r2{}, r3{};
  // Gain 0 and ramp 0: dst += src * 0 leaves dst as it is. Only the last iteration runs, without its stores, for the
  // registers the guest leaves (mp3_mezclador_saltar_ceros).
  static uint64_t llamadas = 0, saltadas = 0;
  ++llamadas;
  uint32_t primera = 0;
  bool guardar = true;
  if (REXCVAR_GET(mp3_mezclador_saltar_ceros) &&
      vmaxvq_u32(vorrq_u32(vreinterpretq_u32_f32(vabsq_f32(load(ctx.r5.u32))),
                           vreinterpretq_u32_f32(vabsq_f32(step)))) == 0) {
    primera = 15;
    guardar = false;
    ++saltadas;
  }
  if ((llamadas & 0xFFFFF) == 0) {
    REXLOG_INFO("[audio] mezclas con ganancia 0 saltadas: {} de {} ({:.0f} %)", saltadas, llamadas,
                100.0 * double(saltadas) / double(llamadas));
  }
  for (uint32_t i = primera; i < 16; ++i) {
    const uint32_t d = dst + i * 64;
    const uint32_t s = src + i * 64;
    // Every load of an iteration before its stores, as in the guest loop.
    const float32x4_t d3 = load(d + 48);  // v9
    const float32x4_t s3 = load(s + 48);  // v4
    const float32x4_t s0 = load(s + 0);   // v3
    d0 = load(d + 0);                     // v2
    s2 = load(s + 32);                    // v5
    s1 = load(s + 16);                    // v4
    d1 = load(d + 16);                    // v1
    d2 = load(d + 32);                    // v3
    r3 = vfmaq_f32(d3, g3, s3);
    r0 = vfmaq_f32(d0, g0, s0);
    r1 = vfmaq_f32(d1, g1, s1);
    r2 = vfmaq_f32(d2, g2, s2);
    g0 = vaddq_f32(g0, step4);
    g1 = vaddq_f32(g1, step4);
    g2 = vaddq_f32(g2, step4);
    g3 = vaddq_f32(g3, step4);
    if (guardar) {
      store(d + 48, r3);
      store(d + 0, r0);
      store(d + 16, r1);
      store(d + 32, r2);
    }
  }

  // What the guest leaves in its volatile registers (r31 it saves and restores; xer, ctr and the rest it leaves).
  const int64_t r10_last = ctx.r3.s64 + 48 + 15 * 64;
  const int64_t r11_last = ctx.r4.s64 + 32 + 15 * 64;
  ctx.r5.u64 = ctx.r3.u64 - ctx.r4.u64;
  ctx.r3.s64 = -32;
  ctx.r4.s64 = -16;
  ctx.r6.u64 = ctx.r5.u64 + uint64_t(r11_last);
  ctx.r7.s64 = r10_last - 32;
  ctx.r8.s64 = r10_last - 48;
  ctx.r9.s64 = 0;
  ctx.r10.s64 = r10_last + 64;
  ctx.r11.s64 = r11_last + 64;
  ctx.cr6.compare<uint32_t>(0, 0, ctx.xer);
  to_ctx(ctx.v0, step4);
  to_ctx(ctx.v1, d1);
  to_ctx(ctx.v2, d0);
  to_ctx(ctx.v3, d2);
  to_ctx(ctx.v4, s1);
  to_ctx(ctx.v5, s2);
  to_ctx(ctx.v6, r2);
  to_ctx(ctx.v7, r1);
  to_ctx(ctx.v8, r0);
  to_ctx(ctx.v9, r3);
  to_ctx(ctx.v10, g3);
  to_ctx(ctx.v11, g2);
  to_ctx(ctx.v12, g1);
  to_ctx(ctx.v13, g0);
#else
  __imp__mp3_AudioMixAddRamp(ctx, base);
#endif
}
#endif  // MP3_TIENE_mp3_AudioMixAddRamp

// A voice's effect chain (sub_8219AE20, r3 = the voice; its filters are mp3_AudioFilterSections and
// mp3_AudioBiquadChannels, ~16 % of a core in run54). XAudio2 as GTA IV links it: the voice's sends are at [voice + 72],
// [voice + 69] of them, 12 bytes each with the send object at +4 (its mix, sub_821997F8, through vtable + 24); a send's
// matrix entries are at [send + 20], [send + 16] of them, 12 bytes each with the current gain at +4 and the target at
// +8. A voice whose every gain is 0 now and next is mixed as silence (mp3_mezclador_saltar_ceros), so its filters are
// not run either (mp3_voces_mudas_sin_efectos). Only from sub_8219BA90: the mastering voice and its limiter always
// run. Their filter state stays where it was until the voice is heard again.
REXCVAR_DEFINE_BOOL(mp3_voces_mudas_sin_efectos, true, "MP3",
                    "Las voces de audio con todas las ganancias de sus envios a 0 (ahora y en el destino) no pasan "
                    "por sus efectos (filtros): se mezclan como silencio de todos modos");

#ifdef MP3_TIENE_mp3_AudioVoiceEffects  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_AudioVoiceEffects);
REX_EXTERN(mp3_AudioVoiceEffects) {
  static uint64_t llamadas = 0, mudas = 0;
  // Only a voice's chain run from sub_8219BA90 (the return address the generated code sets there): the mastering
  // voice's (sub_8219A258) has its one output pointer at +68, and +69 is a byte of it, not a send count (run57
  // crashed on the first audio frame reading 0x4).
  const uint32_t voz = ctx.r3.u32;
  const bool de_voz = uint32_t(ctx.lr) == 0x8219BAA8;
  const uint32_t envios = de_voz ? REX_LOAD_U8(voz + 69) : 0;
  bool muda = envios > 0 && REXCVAR_GET(mp3_voces_mudas_sin_efectos);
  if (muda) {
    const uint32_t lista = REX_LOAD_U32(voz + 72);
    for (uint32_t i = 0; i < envios && muda; ++i) {
      const uint32_t envio = REX_LOAD_U32(lista + i * 12 + 4);
      if (!envio) {
        muda = false;
        break;
      }
      const uint32_t entradas = REX_LOAD_U8(envio + 16);
      const uint32_t matriz = REX_LOAD_U32(envio + 20);
      for (uint32_t j = 0; j < entradas; ++j) {
        if ((REX_LOAD_U32(matriz + j * 12 + 4) | REX_LOAD_U32(matriz + j * 12 + 8)) & 0x7FFFFFFF) {
          muda = false;
          break;
        }
      }
    }
  }
  ++llamadas;
  if ((llamadas & 0x3FFFF) == 0) {
    REXLOG_INFO("[audio] voces mudas sin efectos: {} de {} ({:.0f} %)", mudas, llamadas,
                100.0 * double(mudas) / double(llamadas));
  }
  if (muda) {
    ++mudas;
    ctx.r3.u64 = 0;  // S_OK: the caller goes on to the sends
    return;
  }
  __imp__mp3_AudioVoiceEffects(ctx, base);
}
#endif  // MP3_TIENE_mp3_AudioVoiceEffects

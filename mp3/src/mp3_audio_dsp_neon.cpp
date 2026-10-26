// mp3-nx - DSP functions of GTA IV's audio mixer thread in native code, bit-identical to the recompiled guest code.
//
// The mixer thread (XThread3E6CA280) took ~73 % of a core in run37 (console_logs/run37/PERF_ANALYSIS.md); the voice
// processing it calls (sub_8219AE90 / sub_8219BA90) spends ~28 % of that thread in five DSP functions. This file
// replaces the ones whose native version is clearly faster and exactly reproducible. Conventions as in
// mp3_audio_neon.cpp: guest memory is big-endian; a guest vector in ctx keeps its elements in reverse lane order
// (lane k = guest element 3 - k, what the generated lvx128 leaves); vmaddfp is fused (REX_VMADDFP = vfmaq_f32, one
// rounding); flush-to-zero is the FPCR's, always on on the Switch (PPCFPSCRRegister::kFixedFlush), so each hook runs
// the original where it is not. Every volatile register the guest function changes is left in ctx with the guest's
// value: the callers' register copies (tools/patch_local_regs.py) read back all the volatile registers after a call
// to a hooked function.
//
// The scalar FPU arithmetic is written as the generated code computes it: in double, with every single-precision result
// rounded through float() (fadds, fmuls, frsp ...) and fmadds/fmsubs as std::fma in double rounded once more to float.
// That double rounding can differ from a single-precision fused multiply-add (fmaf) in rare cases, so it is kept
// exactly; only where single precision gives the same bits it is used instead (the biquads' fmuls and fadds, see
// there). A single the guest loads (lfs) and stores again (stfs) without arithmetic is copied as it is: the compiled
// generated code does that too (GCC folds the float -> double -> float round trip; on the console the scalar FPU keeps
// denormals), while a single that goes into ctx as a double goes through Widen(), the conversion that flushes a
// denormal (FZ), which the asm in it keeps from being folded away. This file is compiled with -ffp-contract=off
// (CMakeLists.txt) like the generated code: without it GCC would fuse the separate multiplies and adds here into FMAs
// (its default for GNU C++ is -ffp-contract=fast).
//
// What a hook does not reproduce: the guest's stack scratch below the caller's stack pointer that nothing reads
// after the return (the saved lr and non-volatile registers, spill slots). The stack data the functions it calls use
// (sub_8218FB98's copy of the block descriptor, the memcpy'd filter sections) is written as the guest writes it.
//
// Checked on the Mac against the generated code of these functions (built for the host with the same macros, every
// register written back at the return): the same memory and the same value in every register of ctx, over tens of
// thousands of random calls with denormal, zero and extreme values.

#include "mp3_pch.h"

#include <rex/cvar.h>

#include <bit>
#include <cmath>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define MP3_DSP_NEON 1
#endif

REXCVAR_DEFINE_BOOL(mp3_dsp_remuestreo_neon, true, "MP3",
                    "El remuestreo lineal de PCM de 16 bits del mezclador (sub_821B0310) en NEON nativo en vez del "
                    "codigo VMX recompilado (mismas muestras y registros, bit a bit)");

REX_EXTERN(__imp__mp3_AudioResampleS16);

// Guest functions the hooks call are called by address through the dispatch table, not by name: a name in the
// sources makes tools/patch_inline_calls.py and patch_local_regs.py treat that function as hooked, and every one of
// its callers in the game would then write back and reload all the volatile registers around the call.
#ifndef MP3_DSP_CALL_GUEST
#define MP3_DSP_CALL_GUEST(address) REX_CALL_INDIRECT_FUNC(address)
#endif

#if MP3_DSP_NEON
namespace {

constexpr bool kFlushFixed = PPCFPSCRRegister::kFixedFlush;

inline float LoadF32(uint8_t* base, uint32_t ea) {
  const uint32_t u = REX_LOAD_U32(ea);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline void StoreF32(uint8_t* base, uint32_t ea, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  REX_STORE_U32(ea, u);
}
// the empty asm keeps the compiler from folding float(double(f)) back to f, which would skip both.
inline double Widen(float f) {
  double d = f;
  __asm__("" : "+w"(d));
  return d;
}
inline double LoadF32AsF64(uint8_t* base, uint32_t ea) { return Widen(LoadF32(base, ea)); }
inline double LoadF64(uint8_t* base, uint32_t ea) {
  const uint64_t u = REX_LOAD_U64(ea);
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

// A guest vector (lvx128 / stvx128, 16-byte aligned) as a NEON register in memory order: lane k = guest element k.
inline uint32x4_t LoadVec(uint8_t* base, uint32_t ea) {
  return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(REX_RAW_ADDR(ea & ~0xFu))));
}
inline void StoreVec(uint8_t* base, uint32_t ea, uint32x4_t v) {
  vst1q_u8(REX_RAW_ADDR(ea & ~0xFu), vrev32q_u8(vreinterpretq_u8_u32(v)));
}

// Into ctx in the generated code's lane order (guest element k in lane 3 - k; halfword k in lane 7 - k).
inline void ToCtx(PPCVRegister& r, uint32x4_t v) {
  const uint32x4_t pairs = vrev64q_u32(v);
  vst1q_u32(r.u32, vextq_u32(pairs, pairs, 2));
}
inline void ToCtx(PPCVRegister& r, float32x4_t v) { ToCtx(r, vreinterpretq_u32_f32(v)); }
inline void ToCtx(PPCVRegister& r, int16x8_t v) {
  const int16x8_t quads = vrev64q_s16(v);
  vst1q_s16(r.s16, vextq_s16(quads, quads, 4));
}

// sub_82A04550, the CRT's unsigned 64-bit to double conversion (r3 -> f1 through the stack), as the PowerPC does it:
// it leaves r3 = the double's bits, r5 = 1086 - leading zeros (64 for 0), cr0 and xer.ca, and the double at r1 - 8.
inline void U64ToDouble(PPCContext& ctx, uint8_t* base) {
  const uint64_t value = ctx.r3.u64;
  ctx.r5.u64 = value == 0 ? 64 : __builtin_clzll(value);
  ctx.r3.u64 = (ctx.r5.u8 & 0x40) ? 0 : (value << (ctx.r5.u8 & 0x7F));
  ctx.cr0.compare<int64_t>(ctx.r3.s64, 0, ctx.xer);
  if (!ctx.cr0.eq) {
    ctx.xer.ca = ctx.r5.u32 <= 1086;
    ctx.r5.u64 = uint64_t(1086) - ctx.r5.u64;
    ctx.r3.u64 = __builtin_rotateleft64(ctx.r3.u64, 53) & 0xFFFFFFFFFFFFFull;
    ctx.r3.u64 = (__builtin_rotateleft64(ctx.r5.u64, 52) & 0x7FF0000000000000ull) |
                 (ctx.r3.u64 & 0x800FFFFFFFFFFFFFull);
  }
  const uint64_t bits = __builtin_bswap64(ctx.r3.u64);
  std::memcpy(base + uint32_t(ctx.r1.u32 - 8), &bits, 8);
  ctx.f1.u64 = ctx.r3.u64;
}

// sub_8218FB98: copies the 12-byte descriptor of an audio block (r3, preceded by an 8-byte header) to r4; leaves
// r3 = 0, r9-r11 the words copied (r11 the samples) and cr6 (r3 against 0).
inline void CopyBlockDescriptor(PPCContext& ctx, uint8_t* base) {
  ctx.cr6.compare<uint32_t>(ctx.r3.u32, 0, ctx.xer);
  const PPCRegister header = {.s64 = ctx.r3.u32 != 0 ? ctx.r3.s64 - 8 : 0};
  const uint32_t words = uint32_t(header.s64 + 20);
  const uint32_t dst = ctx.r4.u32;
  ctx.r3.u64 = 0;
  ctx.r9.u64 = REX_LOAD_U32(words);
  REX_STORE_U32(dst, ctx.r9.u32);
  ctx.r10.u64 = REX_LOAD_U32(words + 4);
  REX_STORE_U32(dst + 4, ctx.r10.u32);
  ctx.r11.u64 = REX_LOAD_U32(header.u32 + 28);
  REX_STORE_U32(dst + 8, ctx.r11.u32);
}

// fctidz as the generated code converts (NaN to INT64_MIN, saturating above).
inline int64_t Fctidz(double d) {
  return std::isnan(d) ? int64_t(0x8000000000000000ULL)
         : (d > double(LLONG_MAX)) ? LLONG_MAX
                                   : simde_mm_cvttsd_si64(simde_mm_load_sd(&d));
}

}  // namespace
#endif

// ---------------------------------------------------------------------------------------------------------------------
// sub_821B0310 (mp3_AudioResampleS16): a voice's 16-bit PCM source resampled with linear interpolation into float
// samples, with a linear gain ramp. Called by sub_821AD520 when the block is a whole number of 8-sample groups, the
// output is 16-byte aligned and the source has the samples the block needs (otherwise it calls the general path,
// sub_821A47C0).
//   r3 = source (big-endian int16 samples), r5 = output (float), r6 = output samples (counted down by 8),
//   r7 = the voice: +0 source start, +4 source length, +8 source position (written), +13 bytes per source sample / 2,
//        +20 output start, +24 output length, +28 output position (written), +36 gain of the last block (written:
//        +40), +40 gain of this block, +44 step in source samples per output sample, +48 fractional source position
//        (written).
// Position and step are 32.32 fixed point (both times 2^32, fctidz). Per output sample i, with source index n and
// fraction f (the low 32 bits, halved and converted, times 2^-31) and gain g = g_last + i * (g - g_last) / count:
//   out[i] = (s[n+1] - s[n]) * (f * g) + s[n] * g      (s = int16 * 2^-15; one vmaddfp, fused)
// Groups of 8: the guest gathers s[n] and s[n+1] one sample at a time with lvlx + vsldoi into two vectors of
// halfwords; here one 32-bit load per sample takes both (they are adjacent), and the same vector operations as the
// guest's (vupkhsh/vupklsh, vcfsx 15, vcfux 31 of the fraction shifted right by 1, vmulfp, vsubfp, vmaddfp, the gains
// and fractions advanced by vaddfp/vadduwm) follow in the same order with the same operands, so every output word is
// the guest's. The source is even-aligned in practice (each sample a halfword): with an odd source address, where
// bookkeeping at the end (source position = consumed bytes / (2 * +13), output position, the fraction through
// sub_82A04550, an unsigned 64-bit to double conversion, written out above as U64ToDouble) are the guest's
// operations.
// The guest's divwu by 2 * +13 traps on zero (twllei): with +13 = 0 the original runs, to trap the same way.
REX_EXTERN(mp3_AudioResampleS16) {
#if MP3_DSP_NEON
  const uint32_t voice = ctx.r7.u32;
  if (!kFlushFixed || !REXCVAR_GET(mp3_dsp_remuestreo_neon) || (ctx.r3.u32 & 1) != 0 ||
      REX_LOAD_U8(voice + 13) == 0) {
    __imp__mp3_AudioResampleS16(ctx, base);
    return;
  }

  // Scalar set-up (guest FPU: double, single results through float()).
  const float gain_last_bits = LoadF32(base, voice + 36);
  const double gain_last = Widen(gain_last_bits);
  const double k2_32 = LoadF64(base, 0x820BEE08);
  const double pos = LoadF32AsF64(base, voice + 48) * k2_32;
  const double step = LoadF32AsF64(base, voice + 44) * k2_32;
  const float gain_bits = LoadF32(base, voice + 40);
  const double gain = Widen(gain_bits);
  const double gain_delta = double(float(gain - gain_last));
  StoreF32(base, voice + 36, gain_bits);
  const double count = double(float(double(int64_t(ctx.r6.s32))));  // extsw, fcfid, frsp
  const int64_t pos_fx = Fctidz(pos);
  const int64_t step_fx = Fctidz(step);
  const double per_sample = double(float(gain_delta / count));
  const double k8 = LoadF32AsF64(base, 0x820B7AAC);
  const double per_group = double(float(per_sample * k8));

  // Vector set-up: the gains of samples 0-3 and 4-7 (vmaddfp of the ramp constants), the fractions of the 8 samples.
  const float32x4_t ramp_lo = vreinterpretq_f32_u32(LoadVec(base, 0x820BEC40));  // v6
  const float32x4_t ramp_hi = vreinterpretq_f32_u32(LoadVec(base, 0x820BECB0));  // v7
  const float32x4_t v_per_sample = vdupq_n_f32(float(per_sample));               // v10
  const float32x4_t v_gain_last = vdupq_n_f32(gain_last_bits);
  float32x4_t gain_hi = vfmaq_f32(v_gain_last, ramp_hi, v_per_sample);
  float32x4_t gain_lo = vfmaq_f32(v_gain_last, ramp_lo, v_per_sample);
  const float32x4_t v_per_group = vdupq_n_f32(float(per_group));                 // v5
  const uint32_t s = uint32_t(step_fx);
  const uint32_t p = uint32_t(pos_fx);
  const uint32x4_t s0123 = {0, s, s + s, s + (s + s)};                        // v8 (vsldoi chain)
  const uint32x4_t s4 = vdupq_n_u32((s + s) + (s + s));                       // v0
  const uint32x4_t step8 = vaddq_u32(s4, s4);                                 // v4
  uint32x4_t frac_lo = vaddq_u32(s0123, vdupq_n_u32(p));                      // v8 += v9
  uint32x4_t frac_hi = vaddq_u32(vaddq_u32(s0123, s4), vdupq_n_u32(p));       // v0 = (v8 + v0) + v9
  const float32x4_t k2_m15 = vreinterpretq_f32_u32(vdupq_n_u32(0x38000000));  // vcfsx scale 2^-15
  const float32x4_t k2_m31 = vreinterpretq_f32_u32(vdupq_n_u32(0x30000000));  // vcfux scale 2^-31

  uint64_t src = ctx.r3.u64;
  uint64_t dst = ctx.r5.u64;
  uint64_t remaining = ctx.r6.u64;
  uint64_t acc = uint64_t(pos_fx);  // r11: 32.32 source position relative to src
  const uint64_t inc = uint64_t(step_fx);
  int32x4_t words_lo, words_hi;
  float32x4_t a_lo_g, a_hi_g, f_lo_g, f_hi_g, b_hi, out_lo, out_hi;
  do {
    // The 8 source pairs (s[n], s[n+1]) as one big-endian word each: s[n] in the high halfword.
    alignas(16) uint32_t words[8];
    for (int k = 0; k < 8; ++k) {
      const uint32_t ea = uint32_t(src + ((uint32_t(acc >> 32) << 1) & 0xFFFFFFFEu));
      acc += inc;
      uint32_t w;
      std::memcpy(&w, REX_RAW_ADDR(ea), 4);
      words[k] = __builtin_bswap32(w);
    }
    src += (uint32_t(acc >> 32) << 1) & 0xFFFFFFFEu;
    acc &= 0xFFFFFFFFu;
    __builtin_prefetch(REX_RAW_ADDR(uint32_t(src + 128)), 0, 3);

    words_lo = vld1q_s32(reinterpret_cast<const int32_t*>(words));
    words_hi = vld1q_s32(reinterpret_cast<const int32_t*>(words + 4));
    const float32x4_t a_lo = vmulq_f32(vcvtq_f32_s32(vshrq_n_s32(words_lo, 16)), k2_m15);                    // v9
    const float32x4_t b_lo = vmulq_f32(vcvtq_f32_s32(vshrq_n_s32(vshlq_n_s32(words_lo, 16), 16)), k2_m15);  // v2
    const float32x4_t a_hi = vmulq_f32(vcvtq_f32_s32(vshrq_n_s32(words_hi, 16)), k2_m15);                    // v7
    const float32x4_t f_lo =
        vmulq_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(vshrq_n_u32(frac_lo, 1))), k2_m31);  // v3 (vsrw, vcfux)
    const float32x4_t f_hi =
        vmulq_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(vshrq_n_u32(frac_hi, 1))), k2_m31);  // v1
    b_hi = vmulq_f32(vcvtq_f32_s32(vshrq_n_s32(vshlq_n_s32(words_hi, 16), 16)), k2_m15);   // v31
    const float32x4_t a_lo_gain = vmulq_f32(a_lo, gain_lo);
    const float32x4_t d_lo = vsubq_f32(b_lo, a_lo);
    a_hi_g = vmulq_f32(a_hi, gain_hi);
    f_lo_g = vmulq_f32(f_lo, gain_lo);
    f_hi_g = vmulq_f32(f_hi, gain_hi);
    const float32x4_t d_hi = vsubq_f32(b_hi, a_hi);
    out_lo = vfmaq_f32(a_lo_gain, f_lo_g, d_lo);
    out_hi = vfmaq_f32(a_hi_g, f_hi_g, d_hi);
    a_lo_g = a_lo_gain;
    StoreVec(base, uint32_t(dst), vreinterpretq_u32_f32(out_lo));
    StoreVec(base, uint32_t(dst + 16), vreinterpretq_u32_f32(out_hi));
    remaining -= 8;
    gain_hi = vaddq_f32(gain_hi, v_per_group);
    gain_lo = vaddq_f32(gain_lo, v_per_group);
    dst += 32;
    frac_hi = vaddq_u32(frac_hi, step8);
    frac_lo = vaddq_u32(frac_lo, step8);
  } while (int32_t(uint32_t(remaining)) > 0);

  // Bookkeeping: source position in samples (bytes / (2 * +13), at most +4) and output position (at most +24).
  const uint32_t per_sample_bytes = __builtin_rotateleft32(REX_LOAD_U8(voice + 13), 1);
  const uint64_t src_length = REX_LOAD_U32(voice + 4);
  const uint64_t src_pos = uint32_t(uint32_t(src - REX_LOAD_U32(voice + 0)) / per_sample_bytes);  // divwu
  const uint64_t src_pos_clamped = uint32_t(src_pos) < uint32_t(src_length) ? src_pos : src_length;
  const uint64_t dst_length = REX_LOAD_U32(voice + 24);
  const uint64_t dst_pos = uint32_t(dst - REX_LOAD_U32(voice + 20)) >> 2;
  REX_STORE_U32(voice + 8, uint32_t(src_pos_clamped));
  ctx.cr6.compare<uint32_t>(uint32_t(dst_pos), uint32_t(dst_length), ctx.xer);
  const uint64_t dst_pos_clamped = uint32_t(dst_pos) < uint32_t(dst_length) ? dst_pos : dst_length;
  REX_STORE_U32(voice + 28, uint32_t(dst_pos_clamped));

  // The fraction left (low 32 bits of the position) as a float in [0, 1): sub_82A04550 converts it to double (it
  // sets r3, r5, f1, cr0 and xer), then a multiply by 2^-32 and a rounding to single.
  PPCRegister frame = ctx.r1;
  frame.u32 = frame.u32 - 160;
  ctx.r1 = frame;
  ctx.r3.u64 = acc;
  ctx.r5.u64 = dst;
  U64ToDouble(ctx, base);
  ctx.r1.s64 = frame.s64 + 160;
  const double frac = double(float(ctx.f1.f64 * LoadF64(base, 0x820BEE00)));
  StoreF32(base, voice + 48, float(frac));

  // The rest of what the guest leaves in its registers.
  ctx.r4.u64 = src;
  ctx.r6.u64 = remaining;
  ctx.r7.s64 = 2;
  ctx.r8.u64 = src_pos_clamped;
  ctx.r9.u64 = dst_pos;
  ctx.r10.u64 = dst_pos_clamped;
  ctx.r11.s64 = -2113142784;
  ctx.r12.u64 = uint32_t(ctx.lr);  // the saved lr, reloaded with lwz
  ctx.f0.f64 = frac;
  ctx.f11.f64 = step;
  ctx.f12.s64 = step_fx;
  ctx.f13.f64 = k8;
  ToCtx(ctx.v0, frac_hi);
  ToCtx(ctx.v1, f_hi_g);
  ToCtx(ctx.v2, a_hi_g);
  ToCtx(ctx.v3, f_lo_g);
  ToCtx(ctx.v4, step8);
  ToCtx(ctx.v5, v_per_group);
  ToCtx(ctx.v6, vdupq_n_u32(1));
  ToCtx(ctx.v7, out_hi);
  ToCtx(ctx.v8, frac_lo);
  ToCtx(ctx.v9, out_lo);
  ToCtx(ctx.v10, vcombine_s16(vmovn_s32(vshrq_n_s32(words_lo, 16)), vmovn_s32(vshrq_n_s32(words_hi, 16))));
  ToCtx(ctx.v11, gain_lo);
  ToCtx(ctx.v12, vcombine_s16(vmovn_s32(words_lo), vmovn_s32(words_hi)));
  ToCtx(ctx.v13, gain_hi);
  ToCtx(ctx.v30, a_lo_g);
  ToCtx(ctx.v31, b_hi);
#else
  __imp__mp3_AudioResampleS16(ctx, base);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// The biquads of sub_82936CE0 and sub_829321A0: transposed direct form II, the guest's arithmetic step by step.
//   y  = fmadds(x, b0, s1)
//   s1 = fadds(fmsubs(x, b1, fmuls(y, a1)), s2)
//   s2 = fmsubs(x, b2, fmuls(y, a2))
// Both functions keep the coefficients and states as singles (lfs) and every result rounded to single. The fused
// steps are computed as the generated code does them: std::fma in double, then rounded to float. fmuls and fadds are
// computed here in single precision directly: the guest's double product of two singles is exact (48 bits), and a
// double sum of two singles rounded again to single is the correctly rounded single sum (53 >= 2 * 24 + 2 bits, the
// double rounding is innocuous); with FZ both flush the same values, the ones whose exact result is below 2^-126 (a
// sum of two singles that small is exact in double). That shortens the loop-carried chain by the two conversions per
// operation the double form needs. The operand order of every operation is the guest's.
namespace {
#if MP3_DSP_NEON
struct BiquadCoefs {
  double b0, b1, b2;  // as the guest holds them (lfs)
  float a1, a2;       // the same values as singles (flushed like the guest's lfs)
};

struct BiquadState {
  float s1, s2;
};

// One sample through one biquad: returns y; p1/p2 = fmuls(y, a1/a2), u = fmsubs(x, b1, p1) for the caller's
// register bookkeeping.
struct BiquadStep {
  float y, p2, u;
};
inline BiquadStep Biquad(double x, const BiquadCoefs& c, BiquadState& st) {
  const float y = float(std::fma(x, c.b0, double(st.s1)));
  const float p1 = y * c.a1;
  const float p2 = y * c.a2;
  const float u = float(std::fma(x, c.b1, -double(p1)));    // fmsubs u,x,b1,p1
  const float s2 = float(std::fma(x, c.b2, -double(p2)));   // fmsubs s2,x,b2,p2
  st.s1 = u + st.s2;
  st.s2 = s2;
  return {y, p2, u};
}

// The bits a state is checked by before it is stored back: an exponent field of 0 (zero or denormal) makes the guest
// store its constant (0.0) instead.
inline uint32_t ExponentBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u & 0x7F800000u;
}

inline bool Overlap(uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len) {
  return uint64_t(a) < uint64_t(b) + b_len && uint64_t(b) < uint64_t(a) + a_len;
}
#endif
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// sub_82936CE0 (mp3_AudioBiquadChannels): an effect object's biquad over each channel of a 256-sample block, in
// place, then a gain. Reached through a vtable only (no direct calls).
//   r3 = the object: +12..+28 b0 b1 b2 a1 a2, +32/+36/+40/+44 arrays of the per-channel states s1 s2 (first biquad)
//        and t1 t2 (second), +60 output gain, +68 bypass (nonzero: nothing done), +74 channel mask, +75 two
//        biquads in series (nonzero) or one;
//   r4 = the block (sub_8218FB98 copies its descriptor to the stack: +1 channel count, +8 samples, 256 floats per
//        channel). Returns r3 = 0.
// Per active channel (r9 counts them, 4 bytes per state) the states are loaded, the 256 samples filtered (out =
// its exponent field is 0. The guest runs the channels one after the other; here up to four active channels run
// together in one loop, so that their independent recurrences overlap: the per-sample chain (fmadds, fmuls, fmsubs,
// stores are to disjoint addresses (checked: when the block, the object and the state arrays overlap in any way the
// original runs; when only state arrays overlap each other the channels run one at a time, in the guest's order). The
// gain at +60 is reloaded by the guest for every sample, and loaded once here: with the block outside the object it
// cannot change. The states stay in locals for the whole block (the guest keeps them in registers too).
REXCVAR_DEFINE_BOOL(mp3_dsp_biquad_neon, true, "MP3",
                    "Los filtros biquad por canal del mezclador (sub_82936CE0) en codigo nativo en vez del recompilado "
                    "(mismas muestras y registros, bit a bit)");
REX_EXTERN(__imp__mp3_AudioBiquadChannels);

#if MP3_DSP_NEON
namespace {

// The registers the guest's loop leaves for its last active channel (f1, f3, f8, f9; f4-f7 are the states).
struct BiquadChannelTail {
  double f1, f3, f8, f9;
};

struct BiquadChannel {
  uint32_t samples;  // guest address of the channel's 256 floats
  BiquadState first, second;
  BiquadChannelTail tail;
};

// The states live in locals for the whole block (in ch[] the guest-memory stores would make the compiler reload
// them every sample: it cannot tell they do not alias).
template <int kChannels, bool kTwo>
void BiquadChannels(uint8_t* base, const BiquadCoefs& c, float gain, double gain_f64, BiquadChannel* ch) {
  BiquadState first[kChannels], second[kChannels];
  uint32_t samples[kChannels];
  for (int k = 0; k < kChannels; ++k) {
    first[k] = ch[k].first;
    second[k] = ch[k].second;
    samples[k] = ch[k].samples;
  }
  for (uint32_t i = 0; i < 254; ++i) {
    for (int k = 0; k < kChannels; ++k) {
      const uint32_t ea = samples[k] + i * 4;
      const BiquadStep s = Biquad(LoadF32AsF64(base, ea), c, first[k]);
      float y = s.y;
      if constexpr (kTwo) y = Biquad(Widen(s.y), c, second[k]).y;
      StoreF32(base, ea, y * gain);
    }
  }
  // Samples 254 and 255 with the values the guest's registers keep: f1 = fmuls(y, a2) of the last biquad (of sample
  // 254 with two biquads), f3 = fmuls(y2, a2) or the gain, f8 = the last fmsubs(x, b1, p1), f9 = the last biquad's x.
  for (int k = 0; k < kChannels; ++k) {
    float p2_254 = 0;
    for (uint32_t i = 254; i < 256; ++i) {
      const uint32_t ea = samples[k] + i * 4;
      const double x = LoadF32AsF64(base, ea);
      const BiquadStep s = Biquad(x, c, first[k]);
      if constexpr (kTwo) {
        const double y1 = Widen(s.y);
        const BiquadStep t = Biquad(y1, c, second[k]);
        StoreF32(base, ea, t.y * gain);
        if (i == 254) p2_254 = t.p2;
        else ch[k].tail = {double(p2_254), double(t.p2), double(t.u), y1};
      } else {
        StoreF32(base, ea, s.y * gain);
        ch[k].tail = {double(s.p2), gain_f64, double(s.u), x};
      }
    }
    ch[k].first = first[k];
    ch[k].second = second[k];
  }
}

}  // namespace
#endif

REX_EXTERN(mp3_AudioBiquadChannels) {
#if MP3_DSP_NEON
  const uint32_t obj = ctx.r3.u32;
  const uint32_t desc = ctx.r4.u32;
  if (!kFlushFixed || !REXCVAR_GET(mp3_dsp_biquad_neon) || REX_LOAD_U8(obj + 68) != 0) {
    __imp__mp3_AudioBiquadChannels(ctx, base);
    return;
  }
  // What sub_8218FB98 will copy: the channel count (byte 1 of the descriptor's word at +20 of the header 8 bytes
  // before it) and the samples (+28). Checked before anything is written, so that the original can still run.
  const uint32_t header = desc != 0 ? desc - 8 : 0;
  const uint32_t channels = REX_LOAD_U8(header + 21);
  const uint32_t samples = REX_LOAD_U32(header + 28);
  const uint32_t mask = REX_LOAD_U8(obj + 74);
  const bool two = REX_LOAD_U8(obj + 75) != 0;
  uint32_t active = 0;
  for (uint32_t k = 0; k < channels; ++k) {
    const uint32_t bit = (k & 0x20) ? 0u : (1u << (k & 0x1F));
    active += (bit & mask) != 0;
  }
  const uint32_t arrays[4] = {REX_LOAD_U32(obj + 32), REX_LOAD_U32(obj + 36), REX_LOAD_U32(obj + 40),
                              REX_LOAD_U32(obj + 44)};
  PPCRegister frame = ctx.r1;
  frame.u32 = frame.u32 - 208;
  bool overlap = channels == 0 || Overlap(samples, channels * 1024, obj, 76) ||
                 Overlap(frame.u32 + 96, 12, samples, channels * 1024);
  bool arrays_overlap = false;
  for (int a = 0; a < 4; ++a) {
    overlap |= Overlap(arrays[a], active * 4, samples, channels * 1024) || Overlap(arrays[a], active * 4, obj, 76);
    for (int b = a + 1; b < 4; ++b) arrays_overlap |= Overlap(arrays[a], active * 4, arrays[b], active * 4);
  }
  if (overlap) {
    __imp__mp3_AudioBiquadChannels(ctx, base);
    return;
  }

  // The frame and the descriptor copy, as the guest makes them (sub_8218FB98 sets r3, r9, r10, r11 and cr6).
  ctx.r3.u64 = ctx.r4.u64;
  ctx.r4.s64 = frame.s64 + 96;
  CopyBlockDescriptor(ctx, base);

  BiquadCoefs c;
  c.b0 = LoadF32AsF64(base, obj + 12);
  c.b1 = LoadF32AsF64(base, obj + 16);
  c.b2 = LoadF32AsF64(base, obj + 20);
  const double a1 = LoadF32AsF64(base, obj + 24);
  const double a2 = LoadF32AsF64(base, obj + 28);
  c.a1 = float(a1);
  c.a2 = float(a2);
  const double gain_f64 = LoadF32AsF64(base, obj + 60);
  const float gain = float(gain_f64);
  const float denormal_state_bits = LoadF32(base, 0x82000A34);
  const double denormal_state = Widen(denormal_state_bits);

  // The active channels in order, up to four at a time when their states cannot alias.
  constexpr int kBatch = 4;
  BiquadChannel batch[kBatch];
  uint32_t state_index = 0;  // r9 / 4
  bool any = false;
  BiquadChannel last{};
  float last_t2_bits_src = 0;
  auto load = [&](uint32_t channel, BiquadChannel& b) {
    const uint32_t r9 = state_index * 4;
    b.samples = samples + channel * 1024;
    b.first.s1 = LoadF32(base, arrays[0] + r9);
    b.first.s2 = LoadF32(base, arrays[1] + r9);
    b.second.s1 = LoadF32(base, arrays[2] + r9);
    b.second.s2 = LoadF32(base, arrays[3] + r9);
    ++state_index;
  };
  auto store = [&](BiquadChannel& b, uint32_t index) {
    const uint32_t r9 = index * 4;
    // The guest's f6 = s1, f5 = s2, f7 = t1, f4 = t2 (doubles); with one biquad f7 and f4 are the loaded t1 and t2,
    // stored back as they were loaded. Each is checked by its bits on the stack (stfs, lwz, rlwinm) and replaced by
    // the constant (fmr fN,f2) when its exponent field is 0.
    const float bits[4] = {b.first.s1, b.first.s2, b.second.s1, b.second.s2};
    double reg[4];
    for (int j = 0; j < 4; ++j) {
      const bool replace = ExponentBits(bits[j]) == 0;
      reg[j] = replace ? denormal_state : Widen(bits[j]);
      StoreF32(base, arrays[j] + r9, replace ? denormal_state_bits : bits[j]);  // stfsx
    }
    last = b;
    last_t2_bits_src = bits[3];
    ctx.f6.f64 = reg[0];
    ctx.f5.f64 = reg[1];
    ctx.f7.f64 = reg[2];
    ctx.f4.f64 = reg[3];
    any = true;
  };
  auto run = [&](int n) {
    switch (n * 2 + int(two)) {
      case 2: BiquadChannels<1, false>(base, c, gain, gain_f64, batch); break;
      case 3: BiquadChannels<1, true>(base, c, gain, gain_f64, batch); break;
      case 4: BiquadChannels<2, false>(base, c, gain, gain_f64, batch); break;
      case 5: BiquadChannels<2, true>(base, c, gain, gain_f64, batch); break;
      case 6: BiquadChannels<3, false>(base, c, gain, gain_f64, batch); break;
      case 7: BiquadChannels<3, true>(base, c, gain, gain_f64, batch); break;
      case 8: BiquadChannels<4, false>(base, c, gain, gain_f64, batch); break;
      default: BiquadChannels<4, true>(base, c, gain, gain_f64, batch); break;
    }
  };
  int pending = 0;
  uint32_t pending_index[kBatch];
  uint32_t last_channel_bit = 0;
  for (uint32_t k = 0; k < channels; ++k) {
    const uint32_t bit = (k & 0x20) ? 0u : (1u << (k & 0x1F));
    last_channel_bit = bit;
    if ((bit & mask) == 0) continue;
    pending_index[pending] = state_index;
    load(k, batch[pending]);
    ++pending;
    if (pending == kBatch || arrays_overlap) {
      run(pending);
      for (int j = 0; j < pending; ++j) store(batch[j], pending_index[j]);
      pending = 0;
    }
  }
  if (pending) {
    run(pending);
    for (int j = 0; j < pending; ++j) store(batch[j], pending_index[j]);
  }

  // What the guest leaves in its registers.
  const bool last_active = (last_channel_bit & mask) != 0;
  ctx.r3.u64 = 0;
  if (any) {
    ctx.r4.u64 = arrays[2];
    ctx.r5.u64 = arrays[1];
    ctx.f1.f64 = last.tail.f1;
    ctx.f3.f64 = last.tail.f3;
    ctx.f8.f64 = last.tail.f8;
    ctx.f9.f64 = last.tail.f9;
  }
  ctx.r6.s64 = 1;
  ctx.r7.u64 = channels;
  ctx.r8.u64 = uint64_t(channels) * 1024;
  ctx.r9.u64 = uint64_t(active) * 4;
  ctx.r10.u64 = last_active ? ExponentBits(last_t2_bits_src) : last_channel_bit;
  ctx.r11.u64 = channels;
  ctx.r12.u64 = uint32_t(ctx.lr);
  ctx.cr6.compare<uint32_t>(channels, channels, ctx.xer);
  ctx.f0.f64 = c.b0;
  ctx.f2.f64 = denormal_state;
  ctx.f10.f64 = a2;
  ctx.f11.f64 = a1;
  ctx.f12.f64 = c.b2;
  ctx.f13.f64 = c.b1;
#else
  __imp__mp3_AudioBiquadChannels(ctx, base);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// sub_829321A0 (mp3_AudioFilterSections): an effect object's two filter sections over one 256-sample block, in
// place; each section is two identical biquads in series. Reached through a vtable only.
//   r3 = the object: +4..+20 section 1 b0 b1 b2 a1 a2, +24/+28 and +32/+36 its biquads' s1 s2; +40..+56 section 2
//        coefficients, +60/+64 and +68/+72 its states; +76 section 2 parameter (on when nonzero), +80 section 1
//        parameter (on below 23900), +84/+88 the parameters the coefficients were made for, +100/+101 reset the states
//        of section 1/2 when it comes on;
//   r4 = the block (sub_8218FB98: +8 the 256 floats). Returns r3 = 0.
// The guest copies each section to its stack (memcpy, sub_82A11940), resets the states of a section that comes on,
// remakes the coefficients when a parameter changed (sub_82932090, copied back to the object), filters the block
// (section 1, then section 2; the states and coefficients are reloaded from its stack for every sample) and stores the
// eight states back, each replaced by the constant at 0x82000A34 when its exponent field is 0. All of that up to the
// filtering is done here as the guest does it, calling the same functions (by address) with the same registers. The
// filtering itself runs as a software pipeline: in one step biquad k takes sample n - k, so the two or four recurrences
// of a step are independent and overlap (in the guest each sample goes through all of them before the next starts).
// Each biquad sees the same inputs in the same order as in the guest, so every value is the guest's; the loads run
// ahead of the stores, which is safe because the block is checked to be apart from the object and the stack frame (or
// the original runs). The last 8 samples run one at a time, keeping what the guest's registers hold at the end.
REXCVAR_DEFINE_BOOL(mp3_dsp_secciones_neon, true, "MP3",
                    "Las dos secciones de filtros (biquads en serie) del mezclador (sub_829321A0) en codigo nativo en "
                    "vez del recompilado (mismas muestras y registros, bit a bit)");
REX_EXTERN(__imp__mp3_AudioFilterSections);

#if MP3_DSP_NEON
namespace {

// Biquads in series over samples [begin, end) of a block, skewed: at step t biquad k runs sample t - k. The block's
// earlier samples have gone through every biquad already (or begin is 0).
template <int kStages>
void FilterSeries(uint8_t* base, uint32_t block, uint32_t begin, uint32_t end, const BiquadCoefs* const* coefs,
                  BiquadState* st_in) {
  BiquadState st[kStages];
  for (int k = 0; k < kStages; ++k) st[k] = st_in[k];
  double pipe[kStages] = {};  // pipe[k]: the input of biquad k for the next step (k > 0)
  auto stage = [&](int k, uint32_t n) {
    const double x = k == 0 ? LoadF32AsF64(base, block + n * 4) : pipe[k];
    const float y = Biquad(x, *coefs[k], st[k]).y;
    if (k == kStages - 1) {
      StoreF32(base, block + n * 4, y);
    } else {
      pipe[k + 1] = Widen(y);
    }
  };
  const uint32_t steps = end - begin + kStages - 1;
  uint32_t t = 0;
  // Fill: only the first biquads have a sample.
  for (; t < kStages - 1 && t < steps; ++t) {
    for (int k = kStages - 1; k >= 0; --k) {
      if (t >= uint32_t(k) && t - k < end - begin) stage(k, begin + t - k);
    }
  }
  // Every biquad busy.
  for (; t + 1 < end - begin + 1 && t < steps; ++t) {
    for (int k = kStages - 1; k >= 0; --k) stage(k, begin + t - k);
  }
  // Drain.
  for (; t < steps; ++t) {
    for (int k = kStages - 1; k >= 0; --k) {
      if (t >= uint32_t(k) && t - k < end - begin) stage(k, begin + t - k);
    }
  }
  for (int k = 0; k < kStages; ++k) st_in[k] = st[k];
}

}  // namespace
#endif

REX_EXTERN(mp3_AudioFilterSections) {
#if MP3_DSP_NEON
  const PPCRegister self = ctx.r3;  // r31
  const uint32_t obj = self.u32;
  const uint32_t desc = ctx.r4.u32;
  const uint32_t param1 = REX_LOAD_U32(obj + 80);
  const uint32_t param2 = REX_LOAD_U32(obj + 76);
  const bool on1 = param1 < 23900;  // subfc, subfe, clrlwi r30
  const bool on2 = param2 != 0;     // subfic, subfe, clrlwi r27
  PPCRegister frame = ctx.r1;
  frame.u32 = frame.u32 - 240;
  const uint32_t header = desc != 0 ? desc - 8 : 0;
  const uint32_t block = REX_LOAD_U32(header + 28);  // what sub_8218FB98 copies to +88 of the frame
  if (!kFlushFixed || !REXCVAR_GET(mp3_dsp_secciones_neon) || (!on1 && !on2) || Overlap(block, 1024, obj, 104) ||
      Overlap(block, 1024, frame.u32, 240) || Overlap(block, 1024, header + 20, 12)) {
    __imp__mp3_AudioFilterSections(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr);  // saved with stw, reloaded with lwz

  // The guest's set-up, call for call, with the registers it passes.
  ctx.xer.ca = param2 == 0;  // the carry the last subfe leaves
  ctx.r3 = ctx.r4;
  ctx.r4.s64 = frame.s64 + 80;
  ctx.r9.u64 = param2;
  CopyBlockDescriptor(ctx, base);
  ctx.r1 = frame;
  ctx.r3.s64 = frame.s64 + 144;
  ctx.r4.s64 = self.s64 + 4;  // r29
  ctx.r5.s64 = 36;
  ctx.lr = 0x8293220C;
  MP3_DSP_CALL_GUEST(0x82A11940);  // memcpy(frame + 144, obj + 4, 36)
  ctx.r3.s64 = ctx.r1.s64 + 96;
  ctx.r4.s64 = self.s64 + 40;  // r28
  ctx.r5.s64 = 36;
  ctx.lr = 0x82932220;
  MP3_DSP_CALL_GUEST(0x82A11940);  // memcpy(frame + 96, obj + 40, 36)
  const uint32_t sp = ctx.r1.u32;
  const float reset_state_bits = LoadF32(base, 0x82000A34);
  const double reset_state = Widen(reset_state_bits);
  ctx.r11.s64 = -2113929216;
  ctx.cr6.compare<uint32_t>(on1, 0, ctx.xer);
  if (on1) {
    ctx.r11.u64 = REX_LOAD_U8(obj + 100);
    ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
    if (ctx.r11.u32 != 0) {
      for (uint32_t o : {176u, 168u, 172u, 164u}) StoreF32(base, sp + o, reset_state_bits);
    }
  }
  ctx.cr6.compare<uint32_t>(on2, 0, ctx.xer);
  if (on2) {
    ctx.r11.u64 = REX_LOAD_U8(obj + 101);
    ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
    if (ctx.r11.u32 != 0) {
      for (uint32_t o : {128u, 120u, 124u, 116u}) StoreF32(base, sp + o, reset_state_bits);
    }
  }
  // Coefficients remade when a parameter changed, and copied back to the object (lfs/stfs).
  ctx.r5.u64 = REX_LOAD_U32(obj + 80);
  ctx.r11.u64 = REX_LOAD_U32(obj + 88);
  ctx.cr6.compare<uint32_t>(ctx.r11.u32, ctx.r5.u32, ctx.xer);
  if (ctx.r11.u32 != ctx.r5.u32) {
    ctx.r6.s64 = ctx.r1.s64 + 144;
    ctx.r4.s64 = 1;
    ctx.r3 = self;
    ctx.lr = 0x82932294;
    MP3_DSP_CALL_GUEST(0x82932090);  // the coefficients (sub_82932090)
    const uint32_t to[5] = {uint32_t(self.s64 + 4), obj + 8, obj + 12, obj + 16, obj + 20};
    for (int k = 0; k < 5; ++k) {
      const float bits = LoadF32(base, ctx.r1.u32 + 144 + 4 * k);
      StoreF32(base, to[k], bits);
      ctx.f0.f64 = Widen(bits);
    }
  }
  ctx.r5.u64 = REX_LOAD_U32(obj + 76);
  ctx.r11.u64 = REX_LOAD_U32(obj + 84);
  ctx.cr6.compare<uint32_t>(ctx.r11.u32, ctx.r5.u32, ctx.xer);
  if (ctx.r11.u32 != ctx.r5.u32) {
    ctx.r6.s64 = ctx.r1.s64 + 96;
    ctx.r4.s64 = 0;
    ctx.r3 = self;
    ctx.lr = 0x829322DC;
    MP3_DSP_CALL_GUEST(0x82932090);  // the coefficients (sub_82932090)
    const uint32_t to[5] = {uint32_t(self.s64 + 40), obj + 44, obj + 48, obj + 52, obj + 56};
    for (int k = 0; k < 5; ++k) StoreF32(base, to[k], LoadF32(base, ctx.r1.u32 + 96 + 4 * k));
  }
  ctx.r10.u64 = REX_LOAD_U32(obj + 80);
  ctx.r8.u64 = REX_LOAD_U32(obj + 76);
  ctx.r7.u64 = on2 ? 0 : 1;
  ctx.r9.u64 = on1 ? 0 : 1;
  REX_STORE_U32(obj + 88, ctx.r10.u32);
  REX_STORE_U32(obj + 84, ctx.r8.u32);
  REX_STORE_U8(obj + 101, ctx.r7.u8);
  REX_STORE_U8(obj + 100, ctx.r9.u8);
  const uint32_t samples = REX_LOAD_U32(ctx.r1.u32 + 88);

  // The coefficients and states as the guest reloads them from its stack.
  auto coefs_at = [&](uint32_t at) {
    BiquadCoefs c;
    c.b0 = LoadF32AsF64(base, at);
    c.b1 = LoadF32AsF64(base, at + 4);
    c.b2 = LoadF32AsF64(base, at + 8);
    c.a1 = float(LoadF32AsF64(base, at + 12));
    c.a2 = float(LoadF32AsF64(base, at + 16));
    return c;
  };
  const BiquadCoefs c1 = coefs_at(sp + 144), c2 = coefs_at(sp + 96);
  // Biquads A and B (section 1: s1/s2 at +164/+168 and +172/+176), C and D (section 2: +116/+120, +124/+128).
  BiquadState st[4] = {{LoadF32(base, sp + 164), LoadF32(base, sp + 168)},
                       {LoadF32(base, sp + 172), LoadF32(base, sp + 176)},
                       {LoadF32(base, sp + 116), LoadF32(base, sp + 120)},
                       {LoadF32(base, sp + 124), LoadF32(base, sp + 128)}};
  const BiquadCoefs* coefs[4] = {&c1, &c1, &c2, &c2};
  BiquadState* series = on1 ? &st[0] : &st[2];
  const BiquadCoefs* const* series_coefs = on1 ? &coefs[0] : &coefs[2];
  const int stages = (on1 && on2) ? 4 : 2;
  if (stages == 4) {
    FilterSeries<4>(base, samples, 0, 248, series_coefs, series);
  } else {
    FilterSeries<2>(base, samples, 0, 248, series_coefs, series);
  }

  // Samples 248-255 one at a time, keeping what the guest's last blocks leave in f1-f13: for section 1, f1 = the s2
  // of A loaded for sample 248 and f2 = its fmuls(y, a2), f3/f4 = its b1/b2; for the last section of sample 255
  // (P then Q), f5 = P's s2 before it, f6 = P's fmuls(y, a2), f7 = P's new s1, f8/f9 = Q's fmuls(y, a2)/(y, a1),
  // f10 = b2, f11 = Q's s2 before it, f12/f13 = Q's new s1/s2.
  for (uint32_t n = 248; n < 256; ++n) {
    const uint32_t ea = samples + n * 4;
    double x = LoadF32AsF64(base, ea);
    for (int sec = 0; sec < 2; ++sec) {
      if (!(sec == 0 ? on1 : on2)) continue;
      const BiquadCoefs& c = sec == 0 ? c1 : c2;
      BiquadState& p = st[sec * 2];
      BiquadState& q = st[sec * 2 + 1];
      const float p_s2 = p.s2, q_s2 = q.s2;
      const BiquadStep ps = Biquad(x, c, p);
      const double yp = Widen(ps.y);
      const float yq = Biquad(yp, c, q).y;
      if (sec == 0 && n == 248) {
        ctx.f1.f64 = Widen(p_s2);
        ctx.f2.f64 = double(ps.p2);
        ctx.f3.f64 = c.b1;
        ctx.f4.f64 = c.b2;
      }
      if (n == 255) {
        ctx.f5.f64 = Widen(p_s2);
        ctx.f6.f64 = double(ps.p2);
        ctx.f7.f64 = double(p.s1);
        ctx.f8.f64 = double(yq * c.a2);
        ctx.f9.f64 = double(yq * c.a1);
        ctx.f10.f64 = c.b2;
        ctx.f11.f64 = Widen(q_s2);
        ctx.f12.f64 = double(q.s1);
        ctx.f13.f64 = double(q.s2);
      }
      x = Widen(yq);
    }
    StoreF32(base, ea, float(x));
  }

  // The states back to the object, in the guest's order, each through the stack (stfs, lwz, rlwinm) and replaced by
  // the constant when its exponent field is 0.
  const struct {
    const float* state;
    uint32_t to;
  } back[8] = {{&st[0].s1, 24}, {&st[1].s1, 32}, {&st[0].s2, 28}, {&st[1].s2, 36},
               {&st[2].s1, 60}, {&st[3].s1, 68}, {&st[2].s2, 64}, {&st[3].s2, 72}};
  uint32_t bits = 0;
  for (const auto& b : back) {
    // An off section keeps the stack copy of the object's state, stored back as it is.
    bits = ExponentBits(*b.state);
    ctx.f0.f64 = bits == 0 ? reset_state : Widen(*b.state);
    StoreF32(base, obj + b.to, bits == 0 ? reset_state_bits : *b.state);
  }

  ctx.r3.u64 = 0;
  ctx.r10.u64 = 0;  // the loop counter
  ctx.r11.u64 = bits;
  ctx.cr6.compare<uint32_t>(bits, 0, ctx.xer);
  ctx.r12.u64 = lr;
  ctx.lr = lr;
  ctx.r1.s64 = ctx.r1.s64 + 240;
#else
  __imp__mp3_AudioFilterSections(ctx, base);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// Helpers for the hooks below, which keep guest vectors in ctx's lane order (lane k = guest element 3 - k) throughout,
// so that every vector operation is the generated code's on the same lanes; in particular the horizontal sums of
// vmsum4fp128 (simde's _mm_dp_ps: vmulq_f32, then vaddvq_f32 = two faddp, lanes (0 + 1) + (2 + 3)) add the same
// operands in the same order, NaN payloads included.
#if MP3_DSP_NEON
namespace {

inline float32x4_t LoadVecR(uint8_t* base, uint32_t ea) {
  const uint8x16_t reverse = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
  return vreinterpretq_f32_u8(vqtbl1q_u8(vld1q_u8(REX_RAW_ADDR(ea & ~0xFu)), reverse));
}
inline void StoreVecR(uint8_t* base, uint32_t ea, float32x4_t v) {
  const uint8x16_t reverse = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
  vst1q_u8(REX_RAW_ADDR(ea & ~0xFu), vqtbl1q_u8(vreinterpretq_u8_f32(v), reverse));
}
inline void ToCtxR(PPCVRegister& r, float32x4_t v) { vst1q_f32(r.f32, v); }
inline float32x4_t FromCtxR(const PPCVRegister& r) { return vld1q_f32(r.f32); }

inline PPCVRegister Lvlx(uint8_t* base, uint32_t ea) {
  PPCVRegister r;
  simde_mm_store_si128((simde__m128i*)r.u8,
                       simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea & ~0xFu)),
                                             simde_mm_load_si128((simde__m128i*)&VectorMaskL[(ea & 0xF) * 16])));
  return r;
}
inline float32x4_t SplatElement0(const PPCVRegister& r) { return vdupq_laneq_f32(FromCtxR(r), 3); }

// A guest address range for the overlap checks: kind 0 is only read, kind 1 written (or the guest's own stack frame),
// kind 2 a delay line (written). Ranges of kind 0 may overlap each other and delay lines may overlap each other (they
// are read and written in the guest's order); any other overlap makes the hook run the original.
struct GuestRange {
  uint64_t lo, hi;
  int kind;
};
bool RangesApart(const GuestRange* fixed, size_t n_fixed, const GuestRange* lines, size_t n_lines) {
  bool clash = false;
  for (size_t i = 0; i < n_fixed; ++i) {
    for (size_t j = i + 1; j < n_fixed; ++j) {
      clash |= (fixed[i].kind | fixed[j].kind) != 0 && fixed[i].lo < fixed[j].hi && fixed[j].lo < fixed[i].hi;
    }
    for (size_t j = 0; j < n_lines; ++j) clash |= fixed[i].lo < lines[j].hi && lines[j].lo < fixed[i].hi;
  }
  return !clash;
}

// The 16-byte blocks a delay line of `length` words at `buffer` is read and written in, starting at word `position`
// and advancing by 4 modulo the length (lvx128 / stvx128 drop the low 4 bits): the line, and the first block when the
// position is outside it. Returns the number of ranges, 0 when the line wraps around the 4 GB guest space.
size_t DelayLineRanges(uint32_t buffer, uint32_t position, uint32_t length, GuestRange* out) {
  const uint64_t last = uint64_t(buffer) + 4ull * (length - 1);
  if (length == 0 || last > 0xFFFFFFFFull) return 0;
  out[0] = {buffer & ~0xFu, (last & ~0xFull) + 16, 2};
  if (position < length) return 1;
  const uint32_t first = (position * 4 + buffer) & ~0xFu;
  out[1] = {first, uint64_t(first) + 16, 2};
  return 2;
}

}  // namespace
#endif

// ---------------------------------------------------------------------------------------------------------------------
// sub_82935BD0 (mp3_AudioReverb): an effect object's reverb over each active channel of a 256-sample block, in place:
// eight comb filters with a damping filter (in parallel), then four all-pass filters (in series), mixed with the dry
// signal. Reached through a vtable only. The guest processes 4 samples per vector:
//   r3 = the object: +8/+12/+16 per active channel arrays of the comb delay lines (8 pointers), damping states (8
//        floats) and positions (8 words), +20 feedback, +28 the damping vector, +36 the damping coefficients (four
//        gain, +60 dry gain, +78 channel mask;
//   r4 = the block (sub_8218FB98 copies its descriptor to the stack: +1 channel count, +8 samples). Returns r3 = 0.
// Lengths of the delay lines: 8 + 4 words at 0x820A1468 (the guest's divwu by them traps on 0: the original runs).
// Per 4 samples and comb k (x = the line's 4 words at its position):
//   d   = {row5 . x, row6 . x, row7 . x, row8 . x}   (vmsum4fp128 each, then vperm / vmrghw into one vector)
//   out = (d * feedback + damping * state_k) + in * input_gain   (two vmaddfp)   state_k = row8 . x
// the sum of the 8 x is the comb output, then per all-pass: y = ap + buf * g (written back), x = buf - x; at the end
// block = x * wet + dry * dry_gain. Here each comb step computes the four dot products with three faddp of the four
// products (the same pairwise sums, in the same order, as the four vaddvq_f32 of the generated code), keeps the
// positions, states and pointers in registers and replaces the divwu modulo by a compare (the same value). The guest
// re-reads the pointers, positions and lengths for every step and keeps the states and the comb output on its stack:
// that is the same here only when nothing the hook writes overlaps them, which is checked (with every delay line,
// position and state array of every active channel, the block, the object and the stack frame; delay lines may
// overlap each other, as they are processed in the guest's order); otherwise the original runs.
REXCVAR_DEFINE_BOOL(mp3_dsp_reverb_neon, true, "MP3",
                    "La reverberacion del mezclador (sub_82935BD0: 8 filtros peine y 4 pasa todo por canal) en NEON "
                    "nativo en vez del codigo VMX recompilado (mismas muestras y registros, bit a bit)");
REX_EXTERN(__imp__mp3_AudioReverb);

REX_EXTERN(mp3_AudioReverb) {
#if MP3_DSP_NEON
  const PPCRegister self = ctx.r3;  // r31
  const uint32_t obj = self.u32;
  const uint32_t desc = ctx.r4.u32;
  if (!kFlushFixed || !REXCVAR_GET(mp3_dsp_reverb_neon) || obj > 0xFFFFFF00u) {
    __imp__mp3_AudioReverb(ctx, base);
    return;
  }
  PPCRegister frame = ctx.r1;
  frame.u32 = frame.u32 - 1488;
  // What sub_8218FB98 will copy (checked before anything is written, so that the original can still run).
  const uint32_t header = desc != 0 ? desc - 8 : 0;
  const uint32_t channels = REX_LOAD_U8(header + 21);
  const uint32_t samples = REX_LOAD_U32(header + 28);
  const uint32_t mask = REX_LOAD_U8(obj + 78);
  constexpr uint32_t kCombLengths = 0x820A1468, kAllPassLengths = 0x820A1488;
  uint32_t comb_length[8], ap_length[4];
  bool zero_length = false;
  for (int k = 0; k < 8; ++k) zero_length |= (comb_length[k] = REX_LOAD_U32(kCombLengths + 4 * k)) == 0;
  for (int k = 0; k < 4; ++k) zero_length |= (ap_length[k] = REX_LOAD_U32(kAllPassLengths + 4 * k)) == 0;
  const uint32_t comb_lines = REX_LOAD_U32(obj + 8), comb_states = REX_LOAD_U32(obj + 12),
                 comb_positions = REX_LOAD_U32(obj + 16), ap_lines = REX_LOAD_U32(obj + 44),
                 ap_positions = REX_LOAD_U32(obj + 48);

  // Every active channel's ranges (up to 8 active channels; more and the original runs).
  constexpr int kMaxActive = 8;
  GuestRange fixed[9 + 6 * kMaxActive], lines_at[2 * 12 * kMaxActive];
  size_t n_fixed = 0, n_lines = 0;
  fixed[n_fixed++] = {obj, uint64_t(obj) + 80, 0};
  fixed[n_fixed++] = {kCombLengths, kAllPassLengths + 16, 0};
  fixed[n_fixed++] = {frame.u32, uint64_t(frame.u32) + 1488, 1};
  fixed[n_fixed++] = {header + 20, uint64_t(header + 20) + 12, 0};  // read before the frame is written
  bool fallback = false;
  uint32_t active = 0;
  for (uint32_t c = 0; c < channels && !fallback; ++c) {
    const uint32_t bit = (c & 0x20) ? 0u : (1u << (c & 0x1F));
    if ((bit & mask) == 0) continue;
    if (active == kMaxActive || zero_length) {
      fallback = true;
      break;
    }
    const uint32_t a4 = active * 4;  // r11
    const uint32_t lines = REX_LOAD_U32(comb_lines + a4), aps = REX_LOAD_U32(ap_lines + a4);
    const uint32_t positions = REX_LOAD_U32(comb_positions + a4), states = REX_LOAD_U32(comb_states + a4),
                   ap_pos = REX_LOAD_U32(ap_positions + a4);
    fixed[n_fixed++] = {lines, uint64_t(lines) + 32, 0};
    fixed[n_fixed++] = {aps, uint64_t(aps) + 16, 0};
    fixed[n_fixed++] = {positions, uint64_t(positions) + 32, 1};
    fixed[n_fixed++] = {states, uint64_t(states) + 32, 1};
    fixed[n_fixed++] = {ap_pos, uint64_t(ap_pos) + 16, 1};
    const uint32_t block = samples + 1024 * c;
    if (uint64_t(block) + 1024 > 0xFFFFFFFFull) fallback = true;
    fixed[n_fixed++] = {block & ~0xFu, uint64_t((block + 1008) & ~0xFu) + 16, 1};
    for (int k = 0; k < 12 && !fallback; ++k) {
      const size_t added = k < 8 ? DelayLineRanges(REX_LOAD_U32(lines + 4 * k), REX_LOAD_U32(positions + 4 * k),
                                                   comb_length[k], &lines_at[n_lines])
                                 : DelayLineRanges(REX_LOAD_U32(aps + 4 * (k - 8)), REX_LOAD_U32(ap_pos + 4 * (k - 8)),
                                                   ap_length[k - 8], &lines_at[n_lines]);
      fallback = added == 0;
      n_lines += added;
    }
    ++active;
  }
  // The pointer tables' entries of the active channels (read for every step).
  for (uint32_t table : {comb_lines, comb_states, comb_positions, ap_lines, ap_positions})
    fixed[n_fixed++] = {table, uint64_t(table) + 4 * active, 0};
  if (fallback || !RangesApart(fixed, n_fixed, lines_at, n_lines)) {
    __imp__mp3_AudioReverb(ctx, base);
    return;
  }

  // The set-up, as the guest leaves it in its registers.
  ctx.r3 = ctx.r4;
  ctx.r4.s64 = frame.s64 + 88;
  CopyBlockDescriptor(ctx, base);
  const float input_gain_bits = LoadF32(base, 0x820A143C);
  const float allpass_gain_bits = LoadF32(base, 0x82000D74);
  const uint32_t damping_rows = REX_LOAD_U32(obj + 36);
  const uint32_t damping_vector = REX_LOAD_U32(obj + 28);
  const PPCVRegister lv60 = Lvlx(base, obj + 60), lv56 = Lvlx(base, obj + 56), lv20 = Lvlx(base, obj + 20);
  const PPCVRegister row8 = Lvlx(base, damping_rows), row7 = Lvlx(base, damping_rows + 4),
                     row6 = Lvlx(base, damping_rows + 8), row5 = Lvlx(base, damping_rows + 12);
  const float32x4_t v30 = SplatElement0(lv60), v29 = SplatElement0(lv56), v10 = SplatElement0(lv20);
  const float32x4_t v9 = LoadVecR(base, damping_vector);
  const float32x4_t v8 = FromCtxR(row8), v7 = FromCtxR(row7), v6 = FromCtxR(row6), v5 = FromCtxR(row5);
  const float32x4_t v4 = vreinterpretq_f32_u32(vdupq_n_u32(std::bit_cast<uint32_t>(input_gain_bits)));
  const float32x4_t v31 = vreinterpretq_f32_u32(vdupq_n_u32(std::bit_cast<uint32_t>(allpass_gain_bits)));
  // The vperm masks at +160 and +128 of the frame (ctx lane order of {0x00010203, 0x18191A1B, 0, 0} and
  // {0x04050607, 0x1C1D1E1F, 0, 0}).
  PPCVRegister v3, v2;
  v3.u32[3] = 0x00010203u, v3.u32[2] = 0x18191A1Bu, v3.u32[1] = 0, v3.u32[0] = 0;
  v2.u32[3] = 0x04050607u, v2.u32[2] = 0x1C1D1E1Fu, v2.u32[1] = 0, v2.u32[0] = 0;

  // What the last active channel leaves in the registers.
  float32x4_t out_v0 = vdupq_n_f32(0), out_v11 = vdupq_n_f32(0), out_v12 = vdupq_n_f32(0), out_v13 = vdupq_n_f32(0);
  float32x4_t out_v25 = out_v0, out_v26 = out_v0, out_v28 = out_v0, last_d = out_v0;
  float last_states[8] = {};
  uint64_t out_r5 = 0, out_r6 = 0, out_r8 = 0;
  uint32_t last_bit = 0;
  bool any = false;
  alignas(16) float32x4_t comb_sum[64];

  uint32_t a4 = 0;  // r11: 4 * the active channels so far
  for (uint32_t c = 0; c < channels; ++c) {
    const uint32_t bit = (c & 0x20) ? 0u : (1u << (c & 0x1F));
    last_bit = bit;
    if ((bit & mask) == 0) continue;
    const uint32_t block = samples + 1024 * c;  // r16 + [r1+96]
    const uint32_t lines = REX_LOAD_U32(comb_lines + a4), positions = REX_LOAD_U32(comb_positions + a4),
                   states = REX_LOAD_U32(comb_states + a4);
    uint32_t line[8], pos[8];
    float32x4_t state[8];
    for (int k = 0; k < 8; ++k) {
      line[k] = REX_LOAD_U32(lines + 4 * k);
      pos[k] = REX_LOAD_U32(positions + 4 * k);
      state[k] = SplatElement0(Lvlx(base, states + 4 * k));
    }

    // The combs: per 4 samples, the 8 lines in order.
    for (uint32_t q = 0; q < 64; ++q) {
      if ((q & 7) == 0) {
        for (int k = 0; k < 8; ++k) {  // dcbt: the guest's prefetch of the next 32 samples of each line
          const uint32_t ahead = pos[k] + 32 < comb_length[k] ? pos[k] + 32 : (pos[k] + 32) % comb_length[k];
          __builtin_prefetch(REX_RAW_ADDR(ahead * 4 + line[k]), 1, 3);
        }
      }
      const float32x4_t in = LoadVecR(base, block + 16 * q);  // v11
      float32x4_t sum = vdupq_n_f32(0);
      for (int k = 0; k < 8; ++k) {
        const uint32_t ea = pos[k] * 4 + line[k];
        const float32x4_t x = LoadVecR(base, ea);
        const float32x4_t damped = vmulq_f32(v9, state[k]);
        const float32x4_t p8 = vmulq_f32(v8, x);                    // vmsum4fp128 v13,v8,v0
        sum = vaddq_f32(sum, x);
        const float32x4_t p6 = vmulq_f32(v6, x);                    // vmsum4fp128 v27,v6,v0
        const float32x4_t p7 = vmulq_f32(v7, x);                    // vmsum4fp128 v26,v7,v0
        const float32x4_t p5 = vmulq_f32(v5, x);                    // vmsum4fp128 v25,v5,v0
        const float32x4_t d = vpaddq_f32(vpaddq_f32(p8, p7), vpaddq_f32(p6, p5));  // lanes: row8, 7, 6, 5 . x
        state[k] = vdupq_laneq_f32(d, 0);
        const float32x4_t out = vfmaq_f32(vfmaq_f32(damped, v10, d), v4, in);
        StoreVecR(base, ea, out);
        const uint32_t next = pos[k] + 4;                           // (pos + 4) divwu / mullw / subf by the length
        pos[k] = next < comb_length[k] ? next : next % comb_length[k];
        if (q == 63 && k == 7) {
          last_d = d;
          out_v28 = damped;
        }
      }
      comb_sum[q] = sum;
    }
    for (int k = 0; k < 8; ++k) {
      REX_STORE_U32(positions + 4 * k, pos[k]);
      last_states[k] = vgetq_lane_f32(state[k], 0);
      StoreF32(base, states + 4 * k, last_states[k]);
    }
    out_v25 = vdupq_laneq_f32(last_d, 3);
    out_v26 = vdupq_laneq_f32(last_d, 1);

    // The all-passes in series over the comb output, then the mix into the block.
    const uint32_t aps = REX_LOAD_U32(ap_lines + a4), ap_pos_at = REX_LOAD_U32(ap_positions + a4);
    uint32_t ap_line[4], ap_pos[4];
    for (int k = 0; k < 4; ++k) {
      ap_line[k] = REX_LOAD_U32(aps + 4 * k);
      ap_pos[k] = REX_LOAD_U32(ap_pos_at + 4 * k);
    }
    float32x4_t x, y = vdupq_n_f32(0), dry = y, wet_dry = y;
    for (uint32_t q = 0; q < 64; ++q) {
      x = comb_sum[q];
      dry = LoadVecR(base, block + 16 * q);
      for (int k = 0; k < 4; ++k) {
        const uint32_t ea = ap_pos[k] * 4 + ap_line[k];
        const float32x4_t delayed = LoadVecR(base, ea);
        y = vfmaq_f32(x, v31, delayed);
        x = vsubq_f32(delayed, x);
        StoreVecR(base, ea, y);
        const uint64_t next = uint64_t(ap_pos[k]) + 4;
        const uint32_t quotient = uint32_t(next) / ap_length[k];
        if (q == 63 && k == 3) {
          out_r5 = quotient;
          out_r6 = uint64_t(int64_t(int32_t(quotient)) * int64_t(int32_t(ap_length[k])));
          out_r8 = next - out_r6;
        }
        ap_pos[k] = uint32_t(next) - quotient * ap_length[k];
      }
      wet_dry = vmulq_f32(dry, v30);
      x = vfmaq_f32(wet_dry, v29, x);
      StoreVecR(base, block + 16 * q, x);
    }
    for (int k = 0; k < 4; ++k) REX_STORE_U32(ap_pos_at + 4 * k, ap_pos[k]);
    out_v0 = x;
    out_v11 = y;
    out_v12 = dry;
    out_v13 = wet_dry;
    any = true;
    a4 += 4;
  }

  // What the guest leaves in its registers.
  ctx.r3.u64 = 0;
  ToCtxR(ctx.v5, v5);
  ToCtxR(ctx.v6, v6);
  ToCtxR(ctx.v7, v7);
  ToCtxR(ctx.v8, v8);
  ToCtxR(ctx.v9, v9);
  ToCtxR(ctx.v10, v10);
  ToCtxR(ctx.v29, v29);
  ToCtxR(ctx.v30, v30);
  ctx.cr6.compare<uint32_t>(channels, channels, ctx.xer);
  if (channels == 0) {
    ctx.r4.s64 = frame.s64 + 88;
    ctx.r5.s64 = 12;
    ctx.r6.s64 = 4;
    ctx.r7.s64 = 20;
    ctx.r8.u64 = damping_vector;
    ctx.r9.s64 = 8;
    ctx.r10.u64 = 0;
    ctx.r11.u64 = damping_rows;
    ctx.f0.f64 = Widen(input_gain_bits);
    ctx.v0 = lv20;
    ctx.v13 = lv56;
  } else {
    ToCtxR(ctx.v2, FromCtxR(v2));
    ToCtxR(ctx.v3, FromCtxR(v3));
    ToCtxR(ctx.v4, v4);
    ToCtxR(ctx.v31, v31);
    ctx.r9.u64 = channels;
    ctx.r10.u64 = channels;
    ctx.r11.u64 = a4;
    if (!any) {
      ctx.r4.s64 = frame.s64 + 88;
      ctx.r5.s64 = 12;
      ctx.r6.s64 = 4;
      ctx.r7.s64 = 20;
      ctx.f0.f64 = Widen(input_gain_bits);
      ctx.v0 = lv20;
      ctx.v13 = lv56;
    } else {
      ctx.r4.s64 = frame.s64 + 304 + 1024;
      ctx.r5.u64 = out_r5;
      ctx.r6.u64 = out_r6;
      ctx.r7.u64 = 0;
      ctx.f0.f64 = Widen(last_states[0]);
      ctx.f13.f64 = Widen(last_states[1]);
      ctx.f12.f64 = Widen(last_states[2]);
      ctx.f11.f64 = Widen(last_states[3]);
      ctx.f10.f64 = Widen(last_states[4]);
      ctx.f9.f64 = Widen(last_states[5]);
      ctx.f8.f64 = Widen(last_states[6]);
      ctx.f7.f64 = Widen(last_states[7]);
      ToCtxR(ctx.v0, out_v0);
      ToCtxR(ctx.v1, vdupq_n_f32(0));
      ToCtxR(ctx.v11, out_v11);
      ToCtxR(ctx.v12, out_v12);
      ToCtxR(ctx.v13, out_v13);
      ToCtxR(ctx.v25, out_v25);
      ToCtxR(ctx.v26, out_v26);
      ToCtxR(ctx.v28, out_v28);
      // The last step's permute of v27, with the generated code's vperm.
      PPCVRegister row6_dot, row8_dot;
      ToCtxR(row6_dot, vdupq_laneq_f32(last_d, 2));
      ToCtxR(row8_dot, vdupq_laneq_f32(last_d, 0));
      simde_mm_store_si128((simde__m128i*)ctx.v27.u8,
                           rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*)row6_dot.u8),
                                                         simde_mm_load_si128((simde__m128i*)row8_dot.u8),
                                                         simde_mm_load_si128((simde__m128i*)v2.u8)));
    }
    // r8: the last channel's 1 << index when it is not active, else what its last all-pass step left.
    ctx.r8.u64 = (any && (last_bit & mask) != 0) ? out_r8 : last_bit;
  }
  ctx.r12.u64 = uint32_t(ctx.lr);  // the saved lr, reloaded with lwz
  ctx.r1.s64 = frame.s64 + 1488;
#else
  __imp__mp3_AudioReverb(ctx, base);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// sub_82934F00 (mp3_AudioLimiter): an effect object's look-ahead peak limiter over each active channel of a
// 256-sample block (+652 nonzero; with +652 = 0 it is an RMS compressor with vector logarithms, and the original runs).
// Reached through a vtable only. Between two timing calls (sub_8286A2E8 / sub_8286A308 on the structure at 0x831CC888,
// then sub_8286A338 when a counter at 0x831CC880 wraps to 1), per active channel (mask +658):
//   r3 = the object: +12 + 80 * channel the channel's last 20 samples (history), +492 + 4 * channel its envelope,
//        +608 threshold, +612 output gain, +644/+648 attack/release coefficients;
//   r4 = the block (sub_8218FB98 copies its descriptor to the stack: +1 channel count, +8 samples).
// With S = the history followed by the block, per sample n (the guest's FPU arithmetic, singles in doubles):
//   peak = |S[n+20]| folded with |S[n]| ... |S[n+19]| in that order by  p = (fsubs(p, v) >= 0) ? p : v   (fsel)
//   c = (fsubs(|S[n+20]|, env) >= 0) ? attack : release;  env = fmadds(fsubs(1, c), env, fmuls(peak, c))
//   out[n] = fmuls(fmuls(gain(env), +612), S[n])  with gain(env) = fdivs(threshold, env) clamped (two fsel)
// written to the buffer at 0x831CC480, then the last 20 samples go to the history and the buffer to the block (both
// with the game's memcpy, sub_82A11940). The guest's peak fold is a chain of 20 dependent fsubs + fsel per sample;
// here the folds of 16 samples run side by side in NEON (each lane a sample, the same values in the same order). A
// single-precision subtraction gives what the guest's double subtraction rounded to single gives (the double rounding
// is innocuous, and a difference small enough to be flushed is exact in both), so the fsel picks the same value; the
// values folded are |lfs| as the guest has them (a denormal flushed, a NaN quieted), as singles. The envelope, the
// division and the output keep the guest's double arithmetic with every result rounded to single. The last sample runs
// as the guest's own blocks of four, keeping what its registers hold. Checked before anything is written: the buffer,
// counter and timing structure at 0x831CC480-0x831CC8B0 apart from what a channel reads (block, history, envelope,
// the object's parameters, the stack frame) and the blocks and histories apart from the frame; otherwise the original
// runs.
REXCVAR_DEFINE_BOOL(mp3_dsp_limitador_neon, true, "MP3",
                    "El limitador de picos con anticipacion del mezclador (sub_82934F00, modo de pico) en codigo nativo "
                    "en vez del recompilado (mismas muestras y registros, bit a bit)");
REX_EXTERN(__imp__mp3_AudioLimiter);

#if MP3_DSP_NEON
namespace {

// |lfs| as a single: the guest's fabs of the loaded double (a denormal flushed, a signalling NaN quieted).
inline float32x4_t AbsLoaded(float32x4_t v) {
  const float64x2_t lo = vcvt_f64_f32(vget_low_f32(v));  // fcvtl: as lfs (FZ, NaN quieted)
  const float64x2_t hi = vcvt_high_f64_f32(v);
  return vabsq_f32(vcvt_high_f32_f64(vcvt_f32_f64(lo), hi));  // exact back to single; fabs
}

// The guest's fold step: fsubs p,v then fsel (>= 0: keep p).
inline float32x4_t PeakStep(float32x4_t p, float32x4_t v) {
  return vbslq_f32(vcgezq_f32(vsubq_f32(p, v)), p, v);
}

}  // namespace
#endif

REX_EXTERN(mp3_AudioLimiter) {
#if MP3_DSP_NEON
  const PPCRegister self = ctx.r3;  // r31
  const uint32_t obj = self.u32;
  const uint32_t desc = ctx.r4.u32;
  constexpr uint32_t kGlobal = 0x831CC480, kGlobalSize = 0x831CC8B0 - kGlobal;  // buffer, counter, timing
  constexpr uint32_t kBuffer = 0x831CC480;
  PPCRegister frame = ctx.r1;
  frame.u32 = frame.u32 - 272;
  const uint32_t header = desc != 0 ? desc - 8 : 0;
  bool fallback = !kFlushFixed || !REXCVAR_GET(mp3_dsp_limitador_neon) || obj > 0xFFFFF000u ||
                  Overlap(obj + 12, 648, kGlobal, kGlobalSize) || Overlap(frame.u32, 272, kGlobal, kGlobalSize) ||
                  Overlap(header + 20, 12, kGlobal, kGlobalSize) || REX_LOAD_U8(obj + 652) == 0;
  if (!fallback) {
    const uint32_t channels = REX_LOAD_U8(header + 21);
    const uint32_t samples = REX_LOAD_U32(header + 28);
    const uint32_t mask = REX_LOAD_U8(obj + 658);
    for (uint32_t c = 0; c < channels && !fallback; ++c) {
      const uint32_t bit = (c & 0x20) ? 0u : (1u << (c & 0x1F));
      if ((bit & mask) == 0) continue;
      const uint32_t block = samples + 1024 * c, history = obj + 12 + 80 * c, envelope = obj + 492 + 4 * c;
      fallback = Overlap(block, 1024, kGlobal, kGlobalSize) || Overlap(history, 80, kGlobal, kGlobalSize) ||
                 Overlap(envelope, 4, kGlobal, kGlobalSize) || Overlap(block, 1024, frame.u32, 272) ||
                 Overlap(history, 80, frame.u32, 272) || Overlap(envelope, 4, frame.u32, 272) ||
                 uint64_t(block) + 1024 > 0xFFFFFFFFull;
    }
  }
  if (fallback) {
    __imp__mp3_AudioLimiter(ctx, base);
    return;
  }
  const uint64_t lr = ctx.lr;

  ctx.r3 = ctx.r4;
  ctx.r4.s64 = frame.s64 + 80;
  CopyBlockDescriptor(ctx, base);
  ctx.r1 = frame;
  ctx.r11.s64 = -2095251456;
  const int64_t timing = ctx.r11.s64 - 14200;    // r22 = 0x831CC888
  ctx.r3.s64 = timing;
  ctx.lr = 0x82934F34;
  MP3_DSP_CALL_GUEST(0x8286A2E8);
  ctx.r11.u64 = REX_LOAD_U8(obj + 652);
  ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
  ctx.r11.u64 = REX_LOAD_U8(frame.u32 + 81);
  const double attack = LoadF32AsF64(base, obj + 644);   // f28
  const double release = LoadF32AsF64(base, obj + 648);  // f27
  const double threshold = LoadF32AsF64(base, obj + 608);  // f30
  ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
  if (ctx.r11.u32 != 0) {
    ctx.r9.s64 = -2113863680;
    ctx.r10.s64 = -2113929216;
    ctx.r11.s64 = -2095251456;
    const double floor_level = LoadF32AsF64(base, 0x8200BA9C);  // f29
    const double one = LoadF32AsF64(base, 0x82000D48);          // f31
    const double one_minus_attack = double(float(one - attack)), one_minus_release = double(float(one - release));
    const int64_t buffer = int64_t(-2095251456) - 15232;  // r26 = 0x831CC480
    alignas(16) float folded[276 + 4];  // |S[k]| as singles
    alignas(16) float peak[256];
    uint32_t c = 0;
    do {
      ctx.r11.u64 = REX_LOAD_U8(obj + 658);
      ctx.r10.u64 = (c & 0x20) ? 0u : (1u << (c & 0x1F));
      ctx.r11.u64 = ctx.r10.u64 & ctx.r11.u64;
      ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
      if (ctx.r11.u32 != 0) {
        const PPCRegister channel_base = {.u64 = uint64_t(int64_t(c) * 1024) + REX_LOAD_U32(frame.u32 + 88)};  // r30
        const uint32_t block = channel_base.u32;
        const uint32_t history = obj + 12 + 80 * c;
        const uint32_t envelope_at = uint32_t(self.s64 + 492 + 4 * int64_t(c));  // r28
        const double gain = LoadF32AsF64(base, obj + 612);  // f9 (reloaded per sample; nothing writes it)
        double env = LoadF32AsF64(base, envelope_at);        // f11

        // |S[k]| for k = 0..275, and the 256 folds, 16 samples at a time.
        for (uint32_t k = 0; k < 20; k += 4) {
          vst1q_f32(folded + k, AbsLoaded(vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(REX_RAW_ADDR(history + 4 * k))))));
        }
        for (uint32_t k = 0; k < 256; k += 4) {
          vst1q_f32(folded + 20 + k,
                    AbsLoaded(vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(REX_RAW_ADDR(block + 4 * k))))));
        }
        for (uint32_t n = 0; n < 256; n += 16) {
          float32x4_t p0 = vld1q_f32(folded + n + 20), p1 = vld1q_f32(folded + n + 24),
                      p2 = vld1q_f32(folded + n + 28), p3 = vld1q_f32(folded + n + 32);
          for (uint32_t j = 0; j < 20; ++j) {
            p0 = PeakStep(p0, vld1q_f32(folded + n + j));
            p1 = PeakStep(p1, vld1q_f32(folded + n + 4 + j));
            p2 = PeakStep(p2, vld1q_f32(folded + n + 8 + j));
            p3 = PeakStep(p3, vld1q_f32(folded + n + 12 + j));
          }
          vst1q_f32(peak + n, p0);
          vst1q_f32(peak + n + 4, p1);
          vst1q_f32(peak + n + 8, p2);
          vst1q_f32(peak + n + 12, p3);
        }

        // Sample 255's fold as the guest's last five blocks of four (S[255..274] = the block's 235..254), for the
        // registers it leaves: f6 = the first fsubs of the last block, f7 = sample 254 as loaded.
        double p = double(folded[275]), f6 = 0, f7 = 0;
        for (uint32_t g = 0; g < 5; ++g) {
          const uint32_t at = block + 4 * (235 + 4 * g);
          const double e1 = LoadF32AsF64(base, at + 4), e2 = LoadF32AsF64(base, at + 8);
          const double e3 = LoadF32AsF64(base, at + 12);
          double v = std::fabs(LoadF32AsF64(base, at));
          f6 = double(float(p - v));
          p = f6 >= 0.0 ? p : v;
          for (double e : {e1, e2, e3}) {
            v = std::fabs(e);
            p = double(float(p - v)) >= 0.0 ? p : v;
          }
          f7 = e3;
        }

        // The envelope and the output, sample by sample (the guest's double arithmetic).
        double t8 = 0, t12 = 0, out = 0, delayed = 0;
        for (uint32_t n = 0; n < 256; ++n) {
          delayed = n < 20 ? LoadF32AsF64(base, history + 4 * n) : LoadF32AsF64(base, block + 4 * (n - 20));  // f10
          const double level = double(folded[n + 20]);
          const bool attacking = double(float(level - env)) >= 0.0;
          const double coef = attacking ? attack : release;
          const double held = n == 255 ? p : double(peak[n]);
          const double scaled = double(float(held * coef));
          env = double(float(std::fma(attacking ? one_minus_attack : one_minus_release, env, scaled)));
          const double t13 = double(float(env - floor_level));
          t12 = double(float(threshold - env));
          const double ratio = double(float(threshold / (t13 >= 0.0 ? env : one)));  // fsel f13; fdivs
          t8 = double(float(one - ratio));
          double g = t8 >= 0.0 ? ratio : one;
          g = t12 >= 0.0 ? one : g;
          out = double(float(double(float(g * gain)) * delayed));
          StoreF32(base, uint32_t(buffer) + 4 * n, float(out));
        }
        StoreF32(base, envelope_at, float(env));

        // The registers of the last sample (n = 255: the history loops skipped, five blocks of four of the block).
        ctx.f0.f64 = env;
        ctx.f6.f64 = f6;
        ctx.f7.f64 = f7;
        ctx.f8.f64 = t8;
        ctx.f9.f64 = gain;
        ctx.f10.f64 = delayed;
        ctx.f11.f64 = env;
        ctx.f12.f64 = t12;
        ctx.f13.f64 = out;
        ctx.r6.u64 = channel_base.u64 + 1024;
        ctx.r7.s64 = 20;
        ctx.r8.s64 = 0;
        ctx.r9.u64 = channel_base.u64 + 1024;
        ctx.r10.s64 = 256;
        ctx.r11.s64 = 20;
        ctx.xer.ca = 1;
        ctx.cr6.compare<uint32_t>(256, 256, ctx.xer);

        // The history and the block, with the game's memcpy.
        ctx.r5.s64 = 80;
        ctx.r4.s64 = channel_base.s64 + 944;
        ctx.r3.s64 = self.s64 + 12 + 80 * int64_t(c);  // r25
        ctx.lr = 0x829351CC;
        MP3_DSP_CALL_GUEST(0x82A11940);
        ctx.r5.s64 = 1024;
        ctx.r4.s64 = buffer;
        ctx.r3 = channel_base;
        ctx.lr = 0x829351DC;
        MP3_DSP_CALL_GUEST(0x82A11940);
      }
      ctx.r11.u64 = REX_LOAD_U8(frame.u32 + 81);
      ++c;
      ctx.cr6.compare<uint32_t>(c, ctx.r11.u32, ctx.xer);
    } while (c < ctx.r11.u32);
  }

  ctx.r3.s64 = timing;
  ctx.lr = 0x829354F0;
  MP3_DSP_CALL_GUEST(0x8286A308);
  ctx.r11.u64 = uint64_t(REX_LOAD_U32(0x831CC880)) + 1;
  ctx.cr6.compare<uint32_t>(ctx.r11.u32, 1, ctx.xer);
  REX_STORE_U32(0x831CC880, ctx.r11.u32);
  if (ctx.r11.u32 == 1) {
    ctx.r3.s64 = timing;
    ctx.lr = 0x82935510;
    MP3_DSP_CALL_GUEST(0x8286A338);
    ctx.r11.u64 = 0;
    REX_STORE_U32(0x831CC880, 0);
  }
  ctx.r3.u64 = 0;
  ctx.r1.s64 = ctx.r1.s64 + 272;
  ctx.r12.u64 = uint32_t(lr);
  ctx.lr = lr;
#else
  __imp__mp3_AudioLimiter(ctx, base);
#endif
}

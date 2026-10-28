// mp3-nx - DSP functions of Max Payne 3's audio mixer in native code, bit-identical to the recompiled guest code.
//
// At stock clocks the game's XAudio2 mixer thread takes ~77 % of a core; its effect chain (the game's own DSP, called
// from the mixer's effect processing) is a large part of it. The conventions are those of mp3_audio_dsp_neon.cpp (the
// GTA IV port's version of the same work, whose reverb is the same algorithm with another layout): guest memory is
// big-endian; a guest vector in ctx keeps its elements in reverse lane order (lane k = guest element 3 - k, what the
// generated lvx128 leaves), and the hooks below keep their vectors in that order throughout, so that each vector
// operation is the generated code's on the same lanes; vmaddfp is fused (REX_VMADDFP = simde_mm_fmadd_ps(a, b, c) =
// vfmaq_f32(c, b, a), the operands in that order, which decides the NaN a lane gets when several are NaN); flush-to-zero
// is the FPCR's, always on on the Switch (PPCFPSCRRegister::kFixedFlush), so each hook runs the original where it is not.
// This file is compiled with -ffp-contract=off (CMakeLists.txt) like the generated code.
//
// What a hook leaves in ctx: exactly the registers the generated function writes back at its return (patch_local_regs.py
// writes back only the registers a caller may read), with the guest's values; the others keep what they had. What it
// does not reproduce: the guest's stack scratch below the caller's stack pointer, which nothing reads after the return.
//
// Checked on the Mac against the generated code of each function (built for the host with the same macros, FPCR.FZ on
// as on the Switch): the same guest memory and the same bytes in the whole PPCContext, over tens of thousands of random
// calls with denormal, zero, NaN and extreme values (the harness lives outside the repository: it is built from the
// generated code).

#include "mp3_pch.h"

#include <rex/cvar.h>

#include <bit>
#include <cmath>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define MP3_DSP_NEON 1
#endif

// Guest functions the hooks call are called by address through the dispatch table, not by name: a name in the
// sources makes tools/patch_inline_calls.py and patch_local_regs.py treat that function as hooked, and every one of
// its callers in the game would then write back and reload all the volatile registers around the call.
#ifndef MP3_DSP_CALL_GUEST
#define MP3_DSP_CALL_GUEST(address) REX_CALL_INDIRECT_FUNC(address)
#endif

#if MP3_DSP_NEON
// Helpers for the hooks below; a hook whose name codegen/names.toml does not give is not compiled.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
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
// float -> double as the generated lfs does it (a denormal flushed under FZ); the empty asm keeps the compiler from
// folding a later float(double(f)) back to f.
inline double Widen(float f) {
  double d = f;
  __asm__("" : "+w"(d));
  return d;
}
inline double LoadF32AsF64(uint8_t* base, uint32_t ea) { return Widen(LoadF32(base, ea)); }
// fsubs / fmuls ... as the generated code computes them: in double, rounded to single.
inline double Single(double d) { return double(float(d)); }
inline bool Overlap(uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len) {
  return uint64_t(a) < uint64_t(b) + b_len && uint64_t(b) < uint64_t(a) + a_len;
}

// Guest vectors in ctx lane order (lane k = guest element 3 - k).
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

// lvlx128 as the generated code does it.
inline PPCVRegister Lvlx(uint8_t* base, uint32_t ea) {
  PPCVRegister r;
  simde_mm_store_si128((simde__m128i*)r.u8,
                       simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea & ~0xFu)),
                                             simde_mm_load_si128((simde__m128i*)&VectorMaskL[(ea & 0xF) * 16])));
  return r;
}
inline float32x4_t SplatElement0(const PPCVRegister& r) { return vdupq_laneq_f32(FromCtxR(r), 3); }

// vperm128 as the generated code does it (ctx byte order).
inline PPCVRegister Vperm(const PPCVRegister& a, const PPCVRegister& b, const PPCVRegister& c) {
  PPCVRegister r;
  simde_mm_store_si128((simde__m128i*)r.u8,
                       rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*)a.u8),
                                                     simde_mm_load_si128((simde__m128i*)b.u8),
                                                     simde_mm_load_si128((simde__m128i*)c.u8)));
  return r;
}
inline PPCVRegister AsReg(float32x4_t v) {
  PPCVRegister r;
  ToCtxR(r, v);
  return r;
}

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
// The 16-byte blocks a range of bytes [lo, lo + len) touches with lvx/stvx/lvlx; false when it wraps around 4 GB.
inline bool Blocks(uint64_t lo, uint64_t len, int kind, GuestRange* out) {
  const uint64_t hi = ((lo + len - 1) | 0xF) + 1;
  *out = {lo & ~0xFull, hi, kind};
  return hi <= 0x100000000ull;
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
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------------------------------------------------
// sub_82C76208 (mp3_AudioReverbMp3): the reverb effect (Freeverb: eight damped comb filters in parallel, then four
// all-pass filters in series, mixed with the dry signal) over each active channel of a block, in place. The guest
// processes 4 samples per vector.
//   r3 = the object: +144/+148/+152 per active channel pointers to its 8 comb delay lines, 8 comb states (floats) and 8
//        positions (words), +156 feedback, +160 the damping vector (lvx), +164 the damping coefficients (four lvlx at
//        +0, +4, +8, +12: rows a0..a3), +168/+172 per active channel pointers to its 4 all-pass lines and positions,
//        +180 wet gain, +184 dry gain, +200 channel mask (channel c active when bit rotl32(1, c) is set);
//   r5 = the samples (channel c at r5 + 4 * c * r6), r6 = samples per channel, r7 = channels.
// Lengths: the 8 comb lines' at 0x820E3730 (the guest's divwu by them traps on 0: the original runs then); the
// all-passes' are 556, 440, 340, 224 (constants in the code, divided by multiplication). The input gain vector is at
// 0x820E3760, the all-pass gain vector at 0x820D9F00. Per 4 samples (in) and comb k, x = the line's 4 words at its
// position:
//   d = {a3 . x, a2 . x, a1 . x, a0 . x}  (vmsum4fp128 each, merged by two vperm and a vmrghw)
//   out = fma(in, gain_in, fma(d, feedback, damping * state_k));  state_k = a0 . x;  position += 4 modulo the length
// the sum of the 8 x (from 0, in order) is the comb output; per all-pass y = fma(buf, g, x) (written back),
// x = buf - x; the block gets fma(x, wet, dry * dry_gain). The guest processes ceil(r6 / 32) * 32 samples per channel.
// Here each comb step computes the four dot products with three faddp of the four products (the same pairwise sums, in
// the same order, as the four vaddvq_f32 of the generated code's _mm_dp_ps), keeps positions, states and pointers in
// registers and replaces the divisions by a compare (the same value). The guest re-reads pointers, positions and
// lengths for every step: that is the same here only when nothing the hook writes overlaps them, which is checked (with
// every delay line, position and state array of every active channel, the blocks, the object, the constants and the
// stack frame; delay lines may overlap each other, they are processed in the guest's order); otherwise the original
// runs, as it does for r6 = 0 or r6 > 256 (the comb output buffer on the guest's stack holds 296 samples), r7 = 0 or
// r7 > 64.
REXCVAR_DEFINE_BOOL(mp3_dsp_reverb_mp3, true, "MP3",
                    "La reverberacion del mezclador (sub_82C76208: 8 filtros peine y 4 pasa todo por canal) en NEON "
                    "nativo en vez del codigo VMX recompilado (mismas muestras y registros, bit a bit)");

#ifdef MP3_TIENE_mp3_AudioReverbMp3  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_AudioReverbMp3);
REX_EXTERN(mp3_AudioReverbMp3) {
#if MP3_DSP_NEON
  const uint32_t obj = ctx.r3.u32;
  const uint32_t n = ctx.r6.u32;
  const uint32_t channels = ctx.r7.u32;
  if (!kFlushFixed || !REXCVAR_GET(mp3_dsp_reverb_mp3) || n == 0 || n > 256 || channels == 0 || channels > 64 ||
      obj > 0xFFFFFF00u) {
    __imp__mp3_AudioReverbMp3(ctx, base);
    return;
  }
  const uint32_t sp = ctx.r1.u32;
  const uint32_t frame = sp - 1424;
  const uint32_t samples = ctx.r5.u32;
  const uint32_t quads = ((n - 1) >> 5 << 3) + 8;  // 4-sample groups per channel: ceil(n / 32) * 8
  constexpr uint32_t kCombLengths = 0x820E3730, kInputGain = 0x820E3760, kAllPassGain = 0x820D9F00;
  constexpr uint32_t kApLength[4] = {556, 440, 340, 224};
  uint32_t comb_length[8];
  bool fallback = false;
  for (int k = 0; k < 8; ++k) fallback |= (comb_length[k] = REX_LOAD_U32(kCombLengths + 4 * k)) == 0;
  const uint32_t mask = REX_LOAD_U8(obj + 200);
  const uint32_t t_lines = REX_LOAD_U32(obj + 144), t_states = REX_LOAD_U32(obj + 148),
                 t_positions = REX_LOAD_U32(obj + 152), t_ap_lines = REX_LOAD_U32(obj + 168),
                 t_ap_positions = REX_LOAD_U32(obj + 172);
  const uint32_t damping_at = REX_LOAD_U32(obj + 160), rows_at = REX_LOAD_U32(obj + 164);

  // Every range the guest reads or writes (up to 16 active channels: at most 8 per 32 channels).
  constexpr int kMaxActive = 16;
  GuestRange fixed[10 + 10 * kMaxActive], lines_at[2 * 12 * kMaxActive];
  size_t n_fixed = 0, n_lines = 0;
  bool ok = true;
  ok &= Blocks(obj + 144, 60, 0, &fixed[n_fixed++]);
  ok &= Blocks(rows_at, 16, 0, &fixed[n_fixed++]);
  ok &= Blocks(damping_at, 16, 0, &fixed[n_fixed++]);
  fixed[n_fixed++] = {kCombLengths, kCombLengths + 32, 0};
  fixed[n_fixed++] = {kInputGain, kInputGain + 16, 0};
  fixed[n_fixed++] = {kAllPassGain, kAllPassGain + 16, 0};
  fixed[n_fixed++] = {frame, uint64_t(sp) + 48, 1};  // the frame and the caller's word at +44 (r6 saved there)
  // The blocks of all the channels (each processes `quads` groups from its start; they may overlap each other when n
  // is not a multiple of 4 or 32, and are processed in the guest's order).
  ok &= Blocks(samples, 4ull * (channels - 1) * n + 16ull * quads, 1, &fixed[n_fixed++]);
  ok &= sp >= 1424 && uint64_t(sp) + 48 <= 0x100000000ull;
  uint32_t active = 0;
  for (uint32_t c = 0; c < channels && ok && !fallback; ++c) {
    if ((mask & __builtin_rotateleft32(1u, c)) == 0) continue;
    if (active == kMaxActive) {
      fallback = true;
      break;
    }
    const uint32_t a4 = active * 4;
    for (uint32_t table : {t_lines, t_states, t_positions, t_ap_lines, t_ap_positions})
      ok &= Blocks(uint64_t(table) + a4 > 0xFFFFFFFFull ? 0xFFFFFFFFull : table + a4, 4, 0, &fixed[n_fixed++]);
    if (!ok) break;
    const uint32_t lines = REX_LOAD_U32(t_lines + a4), states = REX_LOAD_U32(t_states + a4),
                   positions = REX_LOAD_U32(t_positions + a4), aps = REX_LOAD_U32(t_ap_lines + a4),
                   ap_positions = REX_LOAD_U32(t_ap_positions + a4);
    ok &= Blocks(lines, 32, 0, &fixed[n_fixed++]);
    ok &= Blocks(aps, 16, 0, &fixed[n_fixed++]);
    ok &= Blocks(states, 32, 1, &fixed[n_fixed++]);
    ok &= Blocks(positions, 32, 1, &fixed[n_fixed++]);
    ok &= Blocks(ap_positions, 16, 1, &fixed[n_fixed++]);
    for (int k = 0; k < 12 && ok; ++k) {
      const size_t added =
          k < 8 ? DelayLineRanges(REX_LOAD_U32(lines + 4 * k), REX_LOAD_U32(positions + 4 * k), comb_length[k],
                                  &lines_at[n_lines])
                : DelayLineRanges(REX_LOAD_U32(aps + 4 * (k - 8)), REX_LOAD_U32(ap_positions + 4 * (k - 8)),
                                  kApLength[k - 8], &lines_at[n_lines]);
      ok &= added != 0;
      n_lines += added;
    }
    ++active;
  }
  if (fallback || !ok || !RangesApart(fixed, n_fixed, lines_at, n_lines)) {
    __imp__mp3_AudioReverbMp3(ctx, base);
    return;
  }

  // The set-up, as the guest's registers hold it.
  REX_STORE_U32(sp + 44, n);  // stw r6,1468(r1): the caller's parameter area
  const float32x4_t v62 = LoadVecR(base, damping_at);
  const PPCVRegister lv63 = Lvlx(base, rows_at), lv61 = Lvlx(base, rows_at + 4), lv60 = Lvlx(base, rows_at + 8),
                     lv59 = Lvlx(base, rows_at + 12);
  const float32x4_t v63 = FromCtxR(lv63), v61 = FromCtxR(lv61), v60 = FromCtxR(lv60), v59 = FromCtxR(lv59);
  const PPCVRegister lv58 = Lvlx(base, obj + 184), lv57 = Lvlx(base, obj + 180), lv56 = Lvlx(base, obj + 156);
  const float32x4_t v50 = SplatElement0(lv58);  // dry gain
  const float32x4_t v5 = SplatElement0(lv57);   // wet gain
  const float32x4_t v12 = SplatElement0(lv56);  // feedback
  const float32x4_t v13 = LoadVecR(base, kInputGain);
  const float32x4_t v8 = LoadVecR(base, kAllPassGain);
  const float32x4_t zero = vdupq_n_f32(0);
  // The vperm masks the guest builds on its stack: {0x00010203, 0x18191A1B, 0, 0} (v7), {0x04050607, 0x1C1D1E1F, 0, 0}
  // (v6), in ctx lane order.
  PPCVRegister mask7, mask6;
  mask7.u32[3] = 0x00010203u, mask7.u32[2] = 0x18191A1Bu, mask7.u32[1] = 0, mask7.u32[0] = 0;
  mask6.u32[3] = 0x04050607u, mask6.u32[2] = 0x1C1D1E1Fu, mask6.u32[1] = 0, mask6.u32[0] = 0;

  // What the last active channel leaves in the registers: the last group's combs 5, 6, 7 (x, the dot products, the
  // running sums, comb 6's state before it) and the last all-pass group's dry * dry gain; and the last all-pass step's
  // quotient times 224 (r7).
  float32x4_t x5 = zero, x6 = zero, d5 = zero, d6 = zero, d7 = zero, sum5 = zero, sum6 = zero, sum7 = zero;
  float32x4_t state6_before = zero, dry_scaled = zero;
  float last_states[8] = {};
  uint64_t last_r7 = 0;
  alignas(16) float32x4_t comb_sum[64];

  uint32_t a4 = 0;  // r11
  uint32_t last_active_channel = ~0u;
  for (uint32_t c = 0; c < channels; ++c) {
    if ((mask & __builtin_rotateleft32(1u, c)) == 0) continue;
    last_active_channel = c;
    const uint32_t block = samples + 4 * c * n;
    const uint32_t lines = REX_LOAD_U32(t_lines + a4), positions = REX_LOAD_U32(t_positions + a4),
                   states = REX_LOAD_U32(t_states + a4);
    uint32_t line[8], pos[8];
    float32x4_t state[8];
    for (int k = 0; k < 8; ++k) {
      line[k] = REX_LOAD_U32(lines + 4 * k);
      pos[k] = REX_LOAD_U32(positions + 4 * k);
      state[k] = SplatElement0(Lvlx(base, states + 4 * k));
    }

    // The combs: per 4 samples, the 8 lines in order.
    for (uint32_t q = 0; q < quads; ++q) {
      if ((q & 7) == 0) {
        for (int k = 0; k < 8; ++k) {  // dcbt: the guest's prefetch of the next 32 samples of each line
          const uint32_t ahead = pos[k] + 32 < comb_length[k] ? pos[k] + 32 : (pos[k] + 32) % comb_length[k];
          __builtin_prefetch(REX_RAW_ADDR(ahead * 4 + line[k]), 1, 3);
        }
      }
      const float32x4_t in = LoadVecR(base, block + 16 * q);  // v0
      float32x4_t sum = zero;                                  // v57 = v56
      for (int k = 0; k < 8; ++k) {
        const uint32_t ea = pos[k] * 4 + line[k];
        const float32x4_t x = LoadVecR(base, ea);
        const float32x4_t damped = vmulq_f32(v62, state[k]);
        sum = vaddq_f32(sum, x);
        // lanes: a0 . x, a1 . x, a2 . x, a3 . x (v63, v61, v60, v59): the guest's d in ctx lane order
        const float32x4_t d = vpaddq_f32(vpaddq_f32(vmulq_f32(v63, x), vmulq_f32(v61, x)),
                                         vpaddq_f32(vmulq_f32(v60, x), vmulq_f32(v59, x)));
        if (k == 6) state6_before = state[k];
        state[k] = vdupq_laneq_f32(d, 0);
        const float32x4_t out = vfmaq_f32(vfmaq_f32(damped, v12, d), v13, in);
        StoreVecR(base, ea, out);
        const uint32_t next = pos[k] + 4;  // divwu / mullw / subf by the length
        pos[k] = next < comb_length[k] ? next : next % comb_length[k];
        if (k == 5) x5 = x, d5 = d, sum5 = sum;
        if (k == 6) x6 = x, d6 = d, sum6 = sum;
        if (k == 7) d7 = d, sum7 = sum;
      }
      comb_sum[q] = sum;
    }
    for (int k = 0; k < 8; ++k) {
      REX_STORE_U32(positions + 4 * k, pos[k]);
      last_states[k] = vgetq_lane_f32(state[k], 0);
      StoreF32(base, states + 4 * k, last_states[k]);  // lfs + stfs: the same bits (not denormal, a VMX result)
    }

    // The all-passes in series over the comb output, then the mix into the block.
    const uint32_t aps = REX_LOAD_U32(t_ap_lines + a4), ap_positions = REX_LOAD_U32(t_ap_positions + a4);
    uint32_t ap_line[4], ap_pos[4];
    for (int k = 0; k < 4; ++k) {
      ap_line[k] = REX_LOAD_U32(aps + 4 * k);
      ap_pos[k] = REX_LOAD_U32(ap_positions + 4 * k);
    }
    for (uint32_t q = 0; q < quads; ++q) {
      float32x4_t x = comb_sum[q];
      const float32x4_t dry = LoadVecR(base, block + 16 * q);  // v34
      for (int k = 0; k < 4; ++k) {
        const uint32_t ea = ap_pos[k] * 4 + ap_line[k];
        const float32x4_t delayed = LoadVecR(base, ea);
        const float32x4_t y = vfmaq_f32(x, v8, delayed);
        x = vsubq_f32(delayed, x);
        StoreVecR(base, ea, y);
        const uint32_t next = ap_pos[k] + 4;
        const uint32_t quotient = next / kApLength[k];
        ap_pos[k] = next - quotient * kApLength[k];
        if (k == 3) last_r7 = uint64_t(quotient) * 224;
      }
      dry_scaled = vmulq_f32(dry, v50);
      StoreVecR(base, block + 16 * q, vfmaq_f32(dry_scaled, v5, x));
    }
    for (int k = 0; k < 4; ++k) REX_STORE_U32(ap_positions + 4 * k, ap_pos[k]);
    a4 += 4;
  }

  // What the guest leaves in its registers (only those the generated function writes back).
  ctx.r4.u64 = 12;
  ctx.r8.u64 = 4;
  ctx.r9.u64 = 8;
  ctx.r10.u64 = uint32_t(n << 2);
  ctx.r11.u64 = a4;
  ctx.r7.u64 = last_active_channel == channels - 1 ? last_r7 : 0;
  ctx.cr0.lt = 0;
  ctx.cr0.gt = 0;
  ctx.cr0.eq = 1;
  ctx.cr0.so = ctx.xer.so;
  ctx.xer.ca = 1;
  ToCtxR(ctx.v8, v8);
  ToCtxR(ctx.v12, v12);
  ToCtxR(ctx.v50, v50);
  ToCtxR(ctx.v59, v59);
  ToCtxR(ctx.v61, v61);
  ToCtxR(ctx.v62, v62);
  ToCtxR(ctx.v63, v63);
  if (a4 == 0) {
    ctx.v56 = lv56;
    ctx.v57 = lv57;
    ctx.v58 = lv58;
    return;
  }
  ctx.r6.u64 = n;  // reloaded with lwz
  ctx.f0.f64 = Widen(last_states[0]);
  ctx.f13.f64 = Widen(last_states[1]);
  ctx.f12.f64 = Widen(last_states[2]);
  ctx.f11.f64 = Widen(last_states[3]);
  ctx.f10.f64 = Widen(last_states[4]);
  ctx.f9.f64 = Widen(last_states[5]);
  ctx.f8.f64 = Widen(last_states[6]);
  ctx.f7.f64 = Widen(last_states[7]);
  ToCtxR(ctx.v10, dry_scaled);
  // The guest's vmsum4fp128 results (splats; lane 0 = a0, 1 = a1, 2 = a2, 3 = a3 of d) and its vperm merges.
  const PPCVRegister a0_5 = AsReg(vdupq_laneq_f32(d5, 0)), a1_5 = AsReg(vdupq_laneq_f32(d5, 1)),
                     a2_5 = AsReg(vdupq_laneq_f32(d5, 2)), a3_5 = AsReg(vdupq_laneq_f32(d5, 3));
  const PPCVRegister a1_6 = AsReg(vdupq_laneq_f32(d6, 1)), a3_6 = AsReg(vdupq_laneq_f32(d6, 3));
  const PPCVRegister a1_7 = AsReg(vdupq_laneq_f32(d7, 1)), a3_7 = AsReg(vdupq_laneq_f32(d7, 3));
  ctx.v35 = Vperm(a3_7, a1_7, mask7);
  ctx.v37 = a3_7;
  ctx.v38 = a1_7;
  ToCtxR(ctx.v39, vdupq_laneq_f32(d7, 2));
  ctx.v41 = Vperm(a3_6, a1_6, mask7);
  ToCtxR(ctx.v43, sum6);
  ctx.v44 = a3_6;
  ctx.v46 = a1_6;
  ToCtxR(ctx.v47, vdupq_laneq_f32(d6, 2));
  ToCtxR(ctx.v48, state6_before);
  ToCtxR(ctx.v49, x6);
  ctx.v51 = Vperm(a3_5, a1_5, mask7);
  ctx.v52 = Vperm(a2_5, a0_5, mask6);
  ctx.v53 = a3_5;
  ctx.v54 = a1_5;
  ToCtxR(ctx.v55, sum5);
  ToCtxR(ctx.v56, zero);
  ToCtxR(ctx.v57, sum7);
  ToCtxR(ctx.v58, vdupq_laneq_f32(d7, 0));
  (void)x5;
#else
  __imp__mp3_AudioReverbMp3(ctx, base);
#endif
}
#endif  // MP3_TIENE_mp3_AudioReverbMp3

// ---------------------------------------------------------------------------------------------------------------------
// sub_82C745A8 (mp3_AudioLimiterMp3): the master limiter over each active channel of a block, in place. With the mode byte
// (+32 of the parameters) nonzero it is a look-ahead peak limiter, done here; with it zero an RMS compressor with
// vector logarithms (the original runs). Between two timing calls (sub_82CD0280 / sub_82CD02A0 on the structure at
// 0x8373DF28, then sub_82CD02D0 when the counter at 0x8373DF20 wraps to 1):
//   r3 = the object: +192 + 80 * channel the channel's last 20 samples (history), +672 + 4 * channel its envelope,
//        +804 channel mask (bit rotl32(1, channel));
//   r4 = the parameters: +0 output gain and +4 threshold (each clamped to the limits at 0x831C5CE0 +0/+4 and +12/+16,
//        then through sub_8222D2E8, a dB to linear conversion), +24/+28 attack/release coefficients, +32 mode,
//        +33 bypass (nonzero: nothing done);
//   r5 = the samples (channel c at r5 + 4 * c * r6), r6 = samples per channel, r7 = channels.
// With S = the history followed by the block, per sample n (the guest's FPU arithmetic, singles in doubles):
//   peak = |S[n+20]| folded with |S[n]| ... |S[n+19]| in that order by  p = (fsubs(p, v) >= 0) ? p : v   (fsel)
//   c = (fsubs(|S[n+20]|, env) >= 0) ? attack : release;  env = fmadds(fsubs(1, c), env, fmuls(peak, c))
//   out[n] = fmuls(fmuls(gain(env), S[n]), output gain)  with gain(env) = fdivs(threshold, env) clamped (three fsel)
// written to the buffer at 0x8373DB20, then the last 20 samples go to the history and the buffer to the block (both
// with the game's memcpy, sub_8309B8B8). The guest's peak fold is a chain of 20 dependent fsubs + fsel per sample;
// here the folds of 16 samples run side by side in NEON (each lane a sample, the same values in the same order). A
// single-precision subtraction gives what the guest's double subtraction rounded to single gives (the double rounding
// is innocuous, and a difference small enough to be flushed is exact in both), so the fsel picks the same value; the
// values folded are |lfs| as the guest has them (a denormal flushed, a signalling NaN quieted), as singles. The
// envelope, the division and the output keep the guest's double arithmetic with every result rounded to single. The
// set-up and every call (timing, the two conversions, the memcpys) are the guest's, with the registers it passes.
// Checked before anything is written: the buffer apart from what a channel reads (block, history); r6 from 21 to 256
// (the buffer holds 258 floats; with fewer than 21 samples the last sample's fold takes another path); r7 from 1 to 64;
// otherwise the original runs.
REXCVAR_DEFINE_BOOL(mp3_dsp_limiter_mp3, true, "MP3",
                    "El limitador de picos con anticipacion del mezclador (sub_82C745A8, modo de pico) en codigo "
                    "nativo en vez del recompilado (mismas muestras y registros, bit a bit)");

#ifdef MP3_TIENE_mp3_AudioLimiterMp3  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_AudioLimiterMp3);

#if MP3_DSP_NEON
namespace {

// |lfs| as a single: the guest's fabs of the loaded double (a denormal flushed, a signalling NaN quieted).
inline float32x4_t AbsLoaded(float32x4_t v) {
  const float64x2_t lo = vcvt_f64_f32(vget_low_f32(v));  // fcvtl: as lfs (FZ, NaN quieted)
  const float64x2_t hi = vcvt_high_f64_f32(v);
  return vabsq_f32(vcvt_high_f32_f64(vcvt_f32_f64(lo), hi));  // exact back to single; fabs
}

// The guest's fold step: fsubs p,v then fsel (>= 0: keep p).
inline float32x4_t PeakStep(float32x4_t p, float32x4_t v) { return vbslq_f32(vcgezq_f32(vsubq_f32(p, v)), p, v); }

// Four big-endian guest floats (any alignment) in memory order.
inline float32x4_t Load4(uint8_t* base, uint32_t ea) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(REX_RAW_ADDR(ea))));
}

}  // namespace
#endif

REX_EXTERN(mp3_AudioLimiterMp3) {
#if MP3_DSP_NEON
  const PPCRegister self = ctx.r3;     // r31
  const PPCRegister params = ctx.r4;   // r29
  const PPCRegister samples = ctx.r5;  // r30
  const uint32_t obj = self.u32;
  const uint32_t n = ctx.r6.u32;  // r24
  const uint32_t channels = ctx.r7.u32;
  constexpr uint32_t kBuffer = 0x8373DB20;
  const uint64_t stride = uint32_t(n << 2);  // r23
  bool fallback = !kFlushFixed || !REXCVAR_GET(mp3_dsp_limiter_mp3) || REX_LOAD_U8(params.u32 + 33) != 0 ||
                  REX_LOAD_U8(params.u32 + 32) == 0 || n < 21 || n > 256 || channels == 0 || channels > 64 ||
                  obj > 0xFFFFF000u;
  for (uint32_t c = 0; c < channels && !fallback; ++c) {
    // Any channel may become active (the mask is read again per channel, after the memcpys of the earlier ones).
    const uint32_t block = uint32_t(samples.u64 + c * stride);
    const uint32_t history = obj + 192 + 80 * c;
    fallback = Overlap(kBuffer, 4 * n, block, 4 * n) || Overlap(kBuffer, 4 * n, history, 80) ||
               uint64_t(block) + 4 * n > 0x100000000ull || uint64_t(history) + 80 > 0x100000000ull;
  }
  if (fallback) {
    __imp__mp3_AudioLimiterMp3(ctx, base);
    return;
  }
  const PPCRegister caller_sp = ctx.r1;
  PPCRegister frame = ctx.r1;
  frame.u32 = caller_sp.u32 - 304;  // stwu: the low word only
  REX_STORE_U32(frame.u32, caller_sp.u32);  // the back chain (stwu)
  constexpr int64_t kTiming = int64_t(-2089549824) - 8408;  // r19 = 0x8373DF28

  // The timing start, and the two dB conversions (clamped first; f-registers as the guest leaves them).
  ctx.r3.s64 = kTiming;
  MP3_DSP_CALL_GUEST(0x82CD0280);
  constexpr uint32_t kLimits = 0x831C5CE0;  // r26
  const double threshold_db = LoadF32AsF64(base, params.u32 + 4);  // f12
  const double attack = LoadF32AsF64(base, params.u32 + 24);       // f27
  const double release = LoadF32AsF64(base, params.u32 + 28);      // f26
  double f0 = LoadF32AsF64(base, kLimits + 12);
  const double f11_setup = Single(threshold_db - f0);
  const double f10_setup = f11_setup >= 0.0 ? threshold_db : f0;
  double f13 = LoadF32AsF64(base, kLimits + 16);
  const double f9_setup = Single(f10_setup - f13);
  ctx.r1 = frame;
  ctx.f1.f64 = f9_setup >= 0.0 ? f13 : f10_setup;
  ctx.f12.f64 = threshold_db;
  MP3_DSP_CALL_GUEST(0x8222D2E8);
  const double threshold = ctx.f1.f64;  // f30
  f0 = LoadF32AsF64(base, kLimits);
  const double gain_db = LoadF32AsF64(base, params.u32 + 0);  // f8
  const double f7_setup = Single(gain_db - f0);
  f13 = LoadF32AsF64(base, kLimits + 4);
  const double f6_setup = f7_setup >= 0.0 ? gain_db : f0;
  const double f5_setup = Single(f6_setup - f13);
  ctx.f0.f64 = f0;
  ctx.f1.f64 = f5_setup >= 0.0 ? f13 : f6_setup;
  ctx.f13.f64 = f13;
  MP3_DSP_CALL_GUEST(0x8222D2E8);
  const double out_gain = ctx.f1.f64;                         // f29
  const double one = LoadF32AsF64(base, 0x820C2234);           // f31
  const double floor_level = LoadF32AsF64(base, 0x82003180);  // f28

  // The FPRs the return hands back when no channel ran.
  double fr[14] = {ctx.f0.f64, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64, f5_setup, f6_setup,
                   f7_setup,   gain_db,    f9_setup,   f10_setup,  f11_setup,  ctx.f12.f64, ctx.f13.f64};

  alignas(16) float folded[276 + 4];  // |S[k]| as singles
  alignas(16) float peak[256];
  for (uint32_t c = 0; c < channels; ++c) {
    if ((REX_LOAD_U8(obj + 804) & __builtin_rotateleft32(1u, c)) == 0) continue;
    const PPCRegister channel_base = {.u64 = samples.u64 + c * stride};  // r30
    const uint32_t block = channel_base.u32;
    const uint32_t history = obj + 192 + 80 * c;
    const uint32_t envelope_at = uint32_t(self.s64 + 672 + 4 * int64_t(c));  // r27
    double env = LoadF32AsF64(base, envelope_at);                           // f12

    // |S[k]| for k = 0..275, and the n folds, 16 samples at a time (the tail of `folded` past n + 20 is unused).
    for (uint32_t k = 0; k < 20; k += 4) vst1q_f32(folded + k, AbsLoaded(Load4(base, history + 4 * k)));
    for (uint32_t k = 0; k < n; k += 4) vst1q_f32(folded + 20 + k, AbsLoaded(Load4(base, block + 4 * k)));
    for (uint32_t i = 0; i < n; i += 16) {
      float32x4_t p0 = vld1q_f32(folded + i + 20), p1 = vld1q_f32(folded + i + 24), p2 = vld1q_f32(folded + i + 28),
                  p3 = vld1q_f32(folded + i + 32);
      for (uint32_t j = 0; j < 20; ++j) {
        p0 = PeakStep(p0, vld1q_f32(folded + i + j));
        p1 = PeakStep(p1, vld1q_f32(folded + i + 4 + j));
        p2 = PeakStep(p2, vld1q_f32(folded + i + 8 + j));
        p3 = PeakStep(p3, vld1q_f32(folded + i + 12 + j));
      }
      vst1q_f32(peak + i, p0);
      vst1q_f32(peak + i + 4, p1);
      vst1q_f32(peak + i + 8, p2);
      vst1q_f32(peak + i + 12, p3);
    }

    // The envelope and the output, sample by sample (the guest's double arithmetic).
    for (uint32_t i = 0; i < n; ++i) {
      const double delayed = i < 20 ? LoadF32AsF64(base, history + 4 * i) : LoadF32AsF64(base, block + 4 * (i - 20));
      const double level = double(folded[i + 20]);
      const double coef = Single(level - env) >= 0.0 ? attack : release;  // f13
      const double scaled = Single(double(peak[i]) * coef);              // f9
      const double f10 = Single(one - coef);
      env = Single(std::fma(f10, env, scaled));
      const double f8 = Single(env - floor_level);
      const double f7 = Single(threshold - env);
      const double f6 = f8 >= 0.0 ? env : one;
      const double f5 = Single(threshold / f6);
      const double f4 = Single(one - f5);
      const double f3 = f4 >= 0.0 ? f5 : one;
      const double f2 = f7 >= 0.0 ? one : f3;
      const double f1 = Single(f2 * delayed);
      const double out = Single(f1 * out_gain);
      StoreF32(base, kBuffer + 4 * i, float(out));
      if (i == n - 1) {
        const double last[14] = {out, f1, f2, f3, f4, f5, f6, f7, f8, scaled, f10, delayed, env, coef};
        std::memcpy(fr, last, sizeof(fr));
      }
    }
    StoreF32(base, envelope_at, float(env));

    // The history and the block, with the game's memcpy; the registers the last sample's fold leaves.
    ctx.r3.s64 = self.s64 + 192 + 80 * int64_t(c);  // r25
    ctx.r4.u64 = uint32_t((n - 20) << 2) + channel_base.u64;
    ctx.r5.s64 = 80;
    ctx.r6.u64 = uint32_t((n - 2) << 2);
    ctx.r7.s64 = int64_t(n) - 18;
    ctx.r8.s64 = int64_t(n) - 20;
    ctx.r9.s64 = int64_t(n) - 21;
    ctx.r10.s64 = 20;
    ctx.xer.ca = 1;
    ctx.lr = 0x82C748F4;
    MP3_DSP_CALL_GUEST(0x8309B8B8);
    ctx.r3 = channel_base;
    ctx.r4.s64 = int64_t(-2089549824) - 9440;  // r22 = the buffer
    ctx.r5.u64 = stride;
    ctx.lr = 0x82C74904;
    MP3_DSP_CALL_GUEST(0x8309B8B8);
  }

  // The timing end (addic. r28 left ca set and cr0 eq), the counter.
  ctx.xer.ca = 1;
  ctx.r3.s64 = kTiming;
  MP3_DSP_CALL_GUEST(0x82CD02A0);
  constexpr uint32_t kCounter = 0x8373DF20;
  ctx.r11.u64 = uint64_t(REX_LOAD_U32(kCounter)) + 1;
  REX_STORE_U32(kCounter, ctx.r11.u32);
  if (ctx.r11.u32 == 1) {
    ctx.r3.s64 = kTiming;
    MP3_DSP_CALL_GUEST(0x82CD02D0);
    ctx.r11.u64 = 0;
    REX_STORE_U32(kCounter, 0);
  }
  ctx.cr0.lt = 0;
  ctx.cr0.gt = 0;
  ctx.cr0.eq = 1;
  ctx.cr0.so = ctx.xer.so;
  ctx.r1 = caller_sp;
  PPCRegister* const f = &ctx.f0;
  for (int k = 0; k < 14; ++k) f[k].f64 = fr[k];
#else
  __imp__mp3_AudioLimiterMp3(ctx, base);
#endif
}
#endif  // MP3_TIENE_mp3_AudioLimiterMp3

// ---------------------------------------------------------------------------------------------------------------------
// sub_82C735E0 (mp3_AudioBiquadMp3): an effect object's biquad over each active channel of a block, in place, one or two
// in series (the same coefficients). Transposed direct form II, the guest's arithmetic step by step:
//   y  = fmadds(x, b0, s1)
//   s1 = fadds(fmsubs(x, b1, fmuls(y, a1)), s2)
//   s2 = fmsubs(x, b2, fmuls(y, a2))
//   r3 = the object: +156..+172 b0 b1 b2 a1 a2, per active channel a the states s1 +176 + 4a, s2 +200 + 4a (first
//        biquad), t1 +224 + 4a, t2 +248 + 4a (second), +276 channel mask (bit rotl32(1, channel)), +277 two biquads in
//        series (nonzero) or one;
//   r4 = the parameters: +20 bypass (nonzero: nothing done);
//   r5 = the samples (channel c at r5 + 4 * c * r6), r6 = samples per channel, r7 = channels.
// Each state is stored back through the stack (stfs, lwz, rlwinm) and replaced by the constant at 0x820A8E0C when its
// exponent field is 0 (with one biquad t1 and t2 go back as they were loaded, checked the same way). The guest runs the
// channels one after the other; here up to four active channels run together, so that their independent recurrences
// overlap (the per-sample chain is the latency, not the arithmetic). fmuls and fadds are computed in single precision
// directly: the guest's double product of two singles is exact (48 bits), and a double sum of two singles rounded again
// to single is the correctly rounded single sum (53 >= 2 * 24 + 2 bits, the double rounding is innocuous); with FZ
// both flush the same values. The fused steps are std::fma in double rounded to float, as the generated code does them.
// The guest's loop takes 4 samples per iteration and leaves its last iteration's values in f1-f4: the last iteration
// and the remainder run here as the guest's code, for every channel (the registers are the last active channel's).
// Checked before anything is written: the blocks apart from the object's coefficients, states and flags; at most 6
// active channels (a 7th's states would be the next array's first: the guest's order matters then); otherwise the
// original runs, as it does for a bypass or r7 = 0 (nothing to gain).
REXCVAR_DEFINE_BOOL(mp3_dsp_biquad_mp3, true, "MP3",
                    "Los filtros biquad por canal del mezclador (sub_82C735E0) en codigo nativo en vez del recompilado "
                    "(mismas muestras y registros, bit a bit)");

#ifdef MP3_TIENE_mp3_AudioBiquadMp3  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_AudioBiquadMp3);

#if MP3_DSP_NEON
namespace {

struct BqCoefs {
  double b0, b1, b2;  // as the guest holds them (lfs)
  float a1, a2;       // the same values as singles
};
struct BqState {
  float s1, s2;
};
inline float Bq(double x, const BqCoefs& c, BqState& st) {
  const float y = float(std::fma(x, c.b0, double(st.s1)));
  const float p1 = y * c.a1;
  const float p2 = y * c.a2;
  const float u = float(std::fma(x, c.b1, -double(p1)));
  const float s2 = float(std::fma(x, c.b2, -double(p2)));
  st.s1 = u + st.s2;
  st.s2 = s2;
  return y;
}

// Samples [0, end) of k channels side by side.
template <int kChannels, bool kTwo>
void BqChannels(uint8_t* base, const BqCoefs& c, const uint32_t* block, BqState* first, BqState* second,
                uint32_t end) {
  BqState f[kChannels], s[kChannels];
  for (int k = 0; k < kChannels; ++k) f[k] = first[k], s[k] = second[k];
  for (uint32_t i = 0; i < end; ++i) {
    for (int k = 0; k < kChannels; ++k) {
      const uint32_t ea = block[k] + i * 4;
      float y = Bq(LoadF32AsF64(base, ea), c, f[k]);
      if constexpr (kTwo) y = Bq(Widen(y), c, s[k]);
      StoreF32(base, ea, y);
    }
  }
  for (int k = 0; k < kChannels; ++k) first[k] = f[k], second[k] = s[k];
}

// The guest's last loop iteration (`group`: 4 samples at r11) and its remainder loop (`rest` samples), register for
// register (f[n] = fn; f29-f31 are the guest's scratch).
struct BqRegs {
  double f[14];
  double f29, f30, f31;
};
void BqTail(uint8_t* base, bool two, uint32_t r11, bool group, uint32_t rest, BqRegs& R) {
  double &f0 = R.f[0], &f1 = R.f[1], &f2 = R.f[2], &f3 = R.f[3], &f4 = R.f[4], &f6 = R.f[6], &f7 = R.f[7],
         &f8 = R.f[8], &f9 = R.f[9], &f10 = R.f[10], &f11 = R.f[11], &f12 = R.f[12], &f13 = R.f[13];
  double &f29 = R.f29, &f30 = R.f30, &f31 = R.f31;
  auto ld = [&](uint32_t ea) { return LoadF32AsF64(base, ea); };
  auto st = [&](uint32_t ea, double v) { StoreF32(base, ea, float(v)); };
  if (two) {
    if (group) {
      f4 = ld(r11);
      f3 = Single(std::fma(f4, f0, f9));
      f2 = ld(r11 + 4);
      f1 = ld(r11 + 8);
      f9 = ld(r11 + 12);
      f7 = Single(std::fma(f3, f0, f7));
      st(r11, f7);
      f31 = Single(f3 * f11);
      f30 = Single(f3 * f10);
      f29 = Single(f7 * f11);
      f31 = Single(std::fma(f4, f13, -f31));
      f4 = Single(std::fma(f4, f12, -f30));
      f7 = Single(f7 * f10);
      f30 = Single(std::fma(f3, f13, -f29));
      f8 = Single(f31 + f8);
      f7 = Single(std::fma(f3, f12, -f7));
      f6 = Single(f30 + f6);
      f3 = Single(std::fma(f2, f0, f8));
      f8 = Single(std::fma(f3, f0, f6));
      st(r11 + 4, f8);
      f6 = Single(f3 * f11);
      f31 = Single(f3 * f10);
      f30 = Single(f8 * f11);
      f6 = Single(std::fma(f2, f13, -f6));
      f2 = Single(std::fma(f2, f12, -f31));
      f8 = Single(f8 * f10);
      f31 = Single(std::fma(f3, f13, -f30));
      f6 = Single(f6 + f4);
      f4 = Single(std::fma(f3, f12, -f8));
      f3 = Single(f31 + f7);
      f8 = Single(std::fma(f1, f0, f6));
      f7 = Single(std::fma(f8, f0, f3));
      st(r11 + 8, f7);
      f6 = Single(f8 * f11);
      f3 = Single(f8 * f10);
      f31 = Single(f7 * f11);
      f6 = Single(std::fma(f1, f13, -f6));
      f3 = Single(std::fma(f1, f12, -f3));
      f7 = Single(f7 * f10);
      f1 = Single(std::fma(f8, f13, -f31));
      f6 = Single(f6 + f2);
      f2 = Single(std::fma(f8, f12, -f7));
      f1 = Single(f1 + f4);
      f7 = Single(std::fma(f9, f0, f6));
      f6 = Single(std::fma(f7, f0, f1));
      st(r11 + 12, f6);
      f1 = Single(f7 * f10);
      r11 += 16;
      f4 = Single(f7 * f11);
      f31 = Single(f6 * f11);
      f8 = Single(std::fma(f9, f12, -f1));
      f6 = Single(f6 * f10);
      f4 = Single(std::fma(f9, f13, -f4));
      f1 = Single(std::fma(f7, f13, -f31));
      f6 = Single(std::fma(f7, f12, -f6));
      f9 = Single(f4 + f3);
      f7 = Single(f1 + f2);
    }
    for (uint32_t i = 0; i < rest; ++i, r11 += 4) {
      f4 = ld(r11);
      f3 = Single(std::fma(f4, f0, f9));
      f2 = Single(std::fma(f3, f0, f7));
      st(r11, f2);
      f1 = Single(f3 * f11);
      f7 = Single(f3 * f10);
      f9 = Single(f2 * f11);
      f1 = Single(std::fma(f4, f13, -f1));
      f2 = Single(f2 * f10);
      f31 = Single(std::fma(f3, f13, -f9));
      f9 = Single(f1 + f8);
      f8 = Single(std::fma(f4, f12, -f7));
      f7 = Single(f31 + f6);
      f6 = Single(std::fma(f3, f12, -f2));
    }
  } else {
    if (group) {
      f4 = ld(r11);
      f3 = Single(std::fma(f4, f0, f9));
      st(r11, f3);
      f2 = ld(r11 + 4);
      f1 = ld(r11 + 8);
      f9 = ld(r11 + 12);
      f31 = Single(f3 * f11);
      f3 = Single(f3 * f10);
      f31 = Single(std::fma(f4, f13, -f31));
      f4 = Single(std::fma(f4, f12, -f3));
      f3 = Single(f31 + f8);
      f8 = Single(std::fma(f2, f0, f3));
      st(r11 + 4, f8);
      f3 = Single(f8 * f11);
      f8 = Single(f8 * f10);
      f3 = Single(std::fma(f2, f13, -f3));
      f2 = Single(std::fma(f2, f12, -f8));
      f8 = Single(f3 + f4);
      f4 = Single(std::fma(f1, f0, f8));
      st(r11 + 8, f4);
      f3 = Single(f4 * f11);
      f8 = Single(f4 * f10);
      f4 = Single(std::fma(f1, f13, -f3));
      f3 = Single(std::fma(f1, f12, -f8));
      f2 = Single(f4 + f2);
      f1 = Single(std::fma(f9, f0, f2));
      st(r11 + 12, f1);
      r11 += 16;
      f8 = Single(f1 * f11);
      f4 = Single(f1 * f10);
      f2 = Single(std::fma(f9, f13, -f8));
      f8 = Single(std::fma(f9, f12, -f4));
      f9 = Single(f2 + f3);
    }
    for (uint32_t i = 0; i < rest; ++i, r11 += 4) {
      f4 = ld(r11);
      f3 = Single(std::fma(f4, f0, f9));
      st(r11, f3);
      f2 = Single(f3 * f11);
      f1 = Single(f3 * f10);
      f9 = Single(std::fma(f4, f13, -f2));
      f9 = Single(f9 + f8);
      f8 = Single(std::fma(f4, f12, -f1));
    }
  }
}

}  // namespace
#endif

REX_EXTERN(mp3_AudioBiquadMp3) {
#if MP3_DSP_NEON
  const uint32_t obj = ctx.r3.u32;
  const uint32_t n = ctx.r6.u32;
  const uint32_t channels = ctx.r7.u32;
  const uint64_t stride = uint32_t(n << 2);  // r31
  if (!kFlushFixed || !REXCVAR_GET(mp3_dsp_biquad_mp3) || REX_LOAD_U8(ctx.r4.u32 + 20) != 0 || channels == 0 ||
      channels > 64 || obj > 0xFFFFFE00u || n > 0x10000) {
    __imp__mp3_AudioBiquadMp3(ctx, base);
    return;
  }
  const uint32_t mask = REX_LOAD_U8(obj + 276);
  uint32_t active = 0;
  bool fallback = false;
  for (uint32_t c = 0; c < channels && !fallback; ++c) {
    const uint32_t block = uint32_t(ctx.r5.u64 + c * stride);
    fallback = (n != 0 && (Overlap(block, 4 * n, obj + 156, 124) || uint64_t(block) + 4 * n > 0x100000000ull));
    active += (mask & __builtin_rotateleft32(1u, c)) != 0;
  }
  if (fallback || active > 6) {
    __imp__mp3_AudioBiquadMp3(ctx, base);
    return;
  }
  const bool two = REX_LOAD_U8(obj + 277) != 0;

  BqRegs R;
  for (int k = 0; k < 14; ++k) R.f[k] = (&ctx.f0)[k].f64;
  R.f29 = R.f30 = R.f31 = 0;
  R.f[0] = LoadF32AsF64(base, obj + 156);
  R.f[13] = LoadF32AsF64(base, obj + 160);
  R.f[12] = LoadF32AsF64(base, obj + 164);
  R.f[11] = LoadF32AsF64(base, obj + 168);
  R.f[10] = LoadF32AsF64(base, obj + 172);
  R.f[5] = LoadF32AsF64(base, 0x820A8E0C);
  const BqCoefs coefs = {R.f[0], R.f[13], R.f[12], float(R.f[11]), float(R.f[10])};

  // The active channels, their blocks and states (as the guest loads them: lfs, a denormal flushed).
  uint32_t blocks[6];
  BqState first[6], second[6];
  for (uint32_t c = 0, a = 0; c < channels; ++c) {
    if ((mask & __builtin_rotateleft32(1u, c)) == 0) continue;
    blocks[a] = uint32_t(ctx.r5.u64 + c * stride);
    first[a] = {float(LoadF32AsF64(base, obj + 176 + 4 * a)), float(LoadF32AsF64(base, obj + 200 + 4 * a))};
    second[a] = {float(LoadF32AsF64(base, obj + 224 + 4 * a)), float(LoadF32AsF64(base, obj + 248 + 4 * a))};
    ++a;
  }

  // The guest's loop: groups of 4 while 4 more samples remain past the next 3, then one at a time.
  const uint32_t groups = n / 4;  // ceil((n - 3) / 4) for n >= 4
  const uint32_t batched = groups ? 4 * (groups - 1) : 0;
  const uint32_t rest = n - 4 * groups;
  for (uint32_t a = 0; a < active && batched; a += 4) {
    const uint32_t k = active - a < 4 ? active - a : 4;
    switch (k * 2 + int(two)) {
      case 2: BqChannels<1, false>(base, coefs, blocks + a, first + a, second + a, batched); break;
      case 3: BqChannels<1, true>(base, coefs, blocks + a, first + a, second + a, batched); break;
      case 4: BqChannels<2, false>(base, coefs, blocks + a, first + a, second + a, batched); break;
      case 5: BqChannels<2, true>(base, coefs, blocks + a, first + a, second + a, batched); break;
      case 6: BqChannels<3, false>(base, coefs, blocks + a, first + a, second + a, batched); break;
      case 7: BqChannels<3, true>(base, coefs, blocks + a, first + a, second + a, batched); break;
      case 8: BqChannels<4, false>(base, coefs, blocks + a, first + a, second + a, batched); break;
      default: BqChannels<4, true>(base, coefs, blocks + a, first + a, second + a, batched); break;
    }
  }

  // Per active channel: the guest's last iteration and remainder, the states back.
  uint32_t r11 = 0, r10 = 0;
  for (uint32_t a = 0; a < active; ++a) {
    R.f[9] = first[a].s1;
    R.f[8] = first[a].s2;
    R.f[7] = second[a].s1;
    R.f[6] = second[a].s2;
    BqTail(base, two, blocks[a] + 4 * batched, groups != 0, rest, R);
    const uint32_t at[4] = {obj + 176 + 4 * a, obj + 200 + 4 * a, obj + 224 + 4 * a, obj + 248 + 4 * a};
    const int reg[4] = {9, 8, 7, 6};
    for (int j = 0; j < 4; ++j) {
      // stfs to the stack, lwz. A state no sample changed (t1 and t2 with one biquad, all four with no samples) is
      // stored as loaded: the compiled lfs + stfs pair copies the bits (the float -> double -> float round trip is
      // folded away), so a denormal or signalling NaN shows there.
      r11 = (n == 0 || (!two && j >= 2)) ? REX_LOAD_U32(at[j]) : std::bit_cast<uint32_t>(float(R.f[reg[j]]));
      r10 = r11 & 0x7F800000u;
      if (r10 == 0) R.f[reg[j]] = R.f[5];
      StoreF32(base, at[j], float(R.f[reg[j]]));  // the constant (fmr) when replaced
    }
  }

  // What the guest leaves in its registers.
  const bool last_active = (mask & __builtin_rotateleft32(1u, channels - 1)) != 0;
  for (int k = 0; k < 14; ++k) (&ctx.f0)[k].f64 = R.f[k];
  PPCRegister r8 = {.s64 = ctx.r3.s64 + 244};
  r8.u32 += 4 * active;  // stfsu: the low word
  ctx.r8 = r8;
  if (active && n >= 4) ctx.r9.s64 = ctx.r6.s64 - 3;
  ctx.r10.u64 = last_active ? r10 : 0;
  ctx.r11.u64 = last_active ? r11 : mask;
  ctx.r4.u64 = __builtin_rotateleft32(1u, channels);
  ctx.r5.u64 = ctx.r5.u64 + channels * stride;
  ctx.r7.u64 = ctx.r7.u64 - channels;
  ctx.cr0.lt = 0;
  ctx.cr0.gt = 0;
  ctx.cr0.eq = 1;
  ctx.cr0.so = ctx.xer.so;
  ctx.xer.ca = 1;
#else
  __imp__mp3_AudioBiquadMp3(ctx, base);
#endif
}
#endif  // MP3_TIENE_mp3_AudioBiquadMp3

// ---------------------------------------------------------------------------------------------------------------------
// sub_8303ECE0 (mp3_XAudioResampleMp3): XAudio2's resampler of 16-bit PCM to float by linear interpolation, ~5 % of a
// core at stock clocks.
//   f1 = the rate (source samples per output sample, double), r3 = the source (big-endian int16), r4 = the output
//   (floats), r5 = the number of output samples, r6 = a float: the fractional position in the source, read at the
//   start and written back at the end. Returns in r3 the source samples it advanced (int32, the difference of the
//   source pointers halved by srawi: XER.CA from it).
// The position is 32.32 fixed point: step = fctidz(f1 * 2^32), pos = fctidz(*r6 * 2^32) (the constants at 0x821139C8
// and 0x821139C0 are 2^32 and 2^-32, read from guest memory as the guest does). Output i is
//   s0 = src[pos >> 32] / 32768, s1 = src[(pos >> 32) + 1] / 32768, f = float(lo32(pos) >> 1) * 2^-31,
//   out = fma(s1 - s0, f, s0)   (vcsxwfp / vcuxwfp, vsubfp, vmaddfp: single precision, one rounding in the fma)
// where lo32(pos) advances by lo32(step) in a vector of its own. The guest's loops: 16 outputs per iteration while 16
// remain (two chains of 8 samples gathered with lvlx + vsldoi: outputs 0-7 from the source pointer, 8-15 from that
// pointer plus 2 * hi32(pos + 8 * step), the pointer then advanced by the integer part and the position left with its
// fraction), then 4 at a time (stvlx / stvrx), then 1 at a time (stvewx: the word at r4 & ~3). The integer parts are
// taken as 32-bit words and doubled within 32 bits (rlwinm), added to the 64-bit pointer: the same here. The final
// fraction goes back to *r6 through the u64 -> double helper at 0x82FFE980 (its f1, r5, cr0 are left as it leaves
// them) times 2^-32, rounded to single.
// Here the 16-output iterations but the last load each sample with the next one (4 bytes) straight into a vector lane
// and compute 4 outputs per NEON operation; the last iteration of each loop is done register by register (on ctx's
// vector registers in place), so that every register the guest writes ends as the guest leaves it. The stores to the
// stack the guest does (saved LR at sp - 8, the back chain, step and position at frame + 80/88 that two lvlx read back,
// the helper's word at frame - 8) are done too, and loads and stores keep the guest's order per iteration (all the
// gathers of an iteration before its stores), so an output that overlaps the source, *r6 or the stack gives the same
// bytes. Written back: every volatile register the function writes (r3-r12, f0, f1, f9-f13, v0-v13, v27-v63, cr0,
// cr6, XER, CTR, LR), with the guest's values: the set the generated function writes back once codegen/names.toml
// names it (without the name it writes back fewer; the others are volatile, dead after the call). The original runs
// for an odd source pointer (an lvlx at the last byte of a block would read half a sample), an output range that wraps
// around 4 GB or crosses 0xE0000000 (the physical mirror's host offset), and with FPCR.FZ not fixed.
REXCVAR_DEFINE_BOOL(mp3_dsp_remuestreo_mp3, true, "MP3",
                    "El remuestreo PCM de 16 bits de XAudio2 (sub_8303ECE0: interpolacion lineal a float) en NEON "
                    "nativo en vez del codigo VMX recompilado (mismas muestras y registros, bit a bit)");

#ifdef MP3_TIENE_mp3_XAudioResampleMp3  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_XAudioResampleMp3);
#if MP3_DSP_NEON
namespace {

// Vector operations of the resampler on guest vectors in ctx lane order.
template <int N>
inline PPCVRegister RsSld(const PPCVRegister& a, const PPCVRegister& b) {  // vsldoi a, b, N
  PPCVRegister r;
  vst1q_u8(r.u8, vextq_u8(vld1q_u8(b.u8), vld1q_u8(a.u8), 16 - N));
  return r;
}
inline PPCVRegister RsUnpackHigh(const PPCVRegister& x) {  // halfwords 0-3, sign-extended
  PPCVRegister r;
  vst1q_s32(r.s32, vmovl_s16(vget_high_s16(vld1q_s16(x.s16))));
  return r;
}
inline PPCVRegister RsUnpackLow(const PPCVRegister& x) {  // halfwords 4-7
  PPCVRegister r;
  vst1q_s32(r.s32, vmovl_s16(vget_low_s16(vld1q_s16(x.s16))));
  return r;
}
inline PPCVRegister RsSample(const PPCVRegister& x) {  // vcsxwfp x, 15
  PPCVRegister r;
  vst1q_f32(r.f32, vmulq_f32(vcvtq_f32_s32(vld1q_s32(x.s32)), vdupq_n_f32(0x1p-15f)));
  return r;
}
inline PPCVRegister RsShift1(const PPCVRegister& x) {  // vsrw x, splat(1)
  PPCVRegister r;
  vst1q_u32(r.u32, vshrq_n_u32(vld1q_u32(x.u32), 1));
  return r;
}
inline PPCVRegister RsFraction(const PPCVRegister& x) {  // vcuxwfp x, 31 (x below 2^31: the signed conversion)
  PPCVRegister r;
  vst1q_f32(r.f32, vmulq_f32(vcvtq_f32_s32(vld1q_s32(x.s32)), vdupq_n_f32(0x1p-31f)));
  return r;
}
inline PPCVRegister RsAdd(const PPCVRegister& a, const PPCVRegister& b) {  // vadduwm
  PPCVRegister r;
  vst1q_u32(r.u32, vaddq_u32(vld1q_u32(a.u32), vld1q_u32(b.u32)));
  return r;
}
inline PPCVRegister RsSub(const PPCVRegister& a, const PPCVRegister& b) {  // vsubfp
  PPCVRegister r;
  vst1q_f32(r.f32, vsubq_f32(vld1q_f32(a.f32), vld1q_f32(b.f32)));
  return r;
}
inline PPCVRegister RsMadd(const PPCVRegister& a, const PPCVRegister& b, const PPCVRegister& c) {  // vmaddfp a*b+c
  PPCVRegister r;
  vst1q_f32(r.f32, vfmaq_f32(vld1q_f32(c.f32), vld1q_f32(b.f32), vld1q_f32(a.f32)));
  return r;
}
inline PPCVRegister RsSplatWord(const PPCVRegister& x, uint32_t word) {  // vspltisw
  PPCVRegister r;
  vst1q_u32(r.u32, vdupq_n_u32(word));
  return r;
}
inline PPCVRegister RsSplat0(const PPCVRegister& x) {  // vspltw x, 0
  PPCVRegister r;
  vst1q_u32(r.u32, vdupq_laneq_u32(vld1q_u32(x.u32), 3));
  return r;
}
// stvlx + stvrx at ea: the 16 bytes from ea (no wrap and no 0xE0000000 crossing: checked).
inline void RsStore16(uint8_t* base, uint32_t ea, const PPCVRegister& v) {
  const uint8x16_t reverse = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
  vst1q_u8(REX_RAW_ADDR(ea), vqtbl1q_u8(vld1q_u8(v.u8), reverse));
}
// The doubled integer part of a 32.32 position, as rldicl + rlwinm leave it.
inline uint32_t RsTwiceHigh(uint64_t pos) { return uint32_t(pos >> 32) << 1; }
// A source sample and the next one, as their 4 bytes in guest order (even addresses: each sample's two bytes are in the
// block its lvlx reads). One load unless the pair straddles 0xE0000000 or 4 GB (and some harmless multiples of 512 MB).
inline uint32_t RsPair(uint8_t* base, uint32_t ea) {
  uint32_t w;
  if (((ea + 2) & 0x1FFFFFFFu) != 0) [[likely]] {
    std::memcpy(&w, REX_RAW_ADDR(ea), 4);
  } else {
    uint16_t a, b;
    std::memcpy(&a, REX_RAW_ADDR(ea), 2);
    std::memcpy(&b, REX_RAW_ADDR(uint32_t(ea + 2)), 2);
    w = uint32_t(a) | uint32_t(b) << 16;
  }
  return w;
}
inline uint32x4_t RsReverse(uint32x4_t v) { return vextq_u32(vrev64q_u32(v), vrev64q_u32(v), 2); }
inline int64_t RsFctidz(double d) {
  return std::isnan(d) ? int64_t(0x8000000000000000ULL)
         : (d > double(LLONG_MAX)) ? LLONG_MAX
                                   : simde_mm_cvttsd_si64(simde_mm_load_sd(&d));
}

}  // namespace
#endif

REX_EXTERN(mp3_XAudioResampleMp3) {
#if MP3_DSP_NEON
  const uint32_t n = ctx.r5.u32;
  const uint32_t out_at = ctx.r4.u32;
  const uint64_t out_end = uint64_t(out_at) + 4ull * n;
  const uint64_t out_limit = (out_at & ~3u) < 0xE0000000u ? 0xE0000000ull : 0x100000000ull;
  if (!kFlushFixed || !REXCVAR_GET(mp3_dsp_remuestreo_mp3) || (ctx.r3.u32 & 1) != 0 || out_end > out_limit) {
    __imp__mp3_XAudioResampleMp3(ctx, base);
    return;
  }

  // Prologue: LR saved, the frame (208 bytes; stwu sets the low word of r1).
  const uint64_t r1 = ctx.r1.u64;
  const uint32_t sp = uint32_t(r1);
  const uint32_t frame = sp - 208;
  const uint64_t r1_frame = (r1 & 0xFFFFFFFF00000000ull) | frame;
  REX_SSTORE_U32(sp - 8, uint32_t(ctx.lr));
  REX_STORE_U32(frame, sp);
  const uint64_t source = ctx.r3.u64;  // r22, r31 at the start
  const uint32_t position_at = ctx.r6.u32;
  const double f13_in = LoadF32AsF64(base, position_at);
  const double two32 = std::bit_cast<double>(REX_LOAD_U64(0x821139C8));
  const double f12 = ctx.f1.f64 * two32;
  const double f11 = f13_in * two32;
  const int64_t f10 = RsFctidz(f12);
  const int64_t f9 = RsFctidz(f11);
  REX_SSTORE_U64(frame + 80, uint64_t(f10));
  const uint64_t step = REX_SLOAD_U64(frame + 80);  // r11
  REX_SSTORE_U64(frame + 80, step);
  REX_SSTORE_U64(frame + 88, uint64_t(f9));
  ctx.v56 = Lvlx(base, frame + 84);
  uint64_t r3 = REX_SLOAD_U64(frame + 88);  // the position
  REX_SSTORE_U64(frame + 88, r3);
  ctx.v55 = Lvlx(base, frame + 92);
  const uint64_t step8 = __builtin_rotateleft64(step, 3) & ~7ull;  // r26
  uint64_t r31 = source, r4 = ctx.r4.u64;
  uint64_t r6 = r1_frame + 88, r7 = r1_frame + 80, r8 = 4, r9 = n & 0xFFFFFFF0u, r10 = 2;
  uint64_t ctr = ctx.ctr.u64;
  const uint32_t r25 = n & 0xC, r24 = n & 3;
  // The fractions of the first 16 outputs (v0, v12, v11, v10) and their increments.
  ctx.v63 = RsSplatWord(ctx.v63, 0);
  ctx.v27 = RsSplat0(ctx.v56);
  ctx.v12 = RsSplat0(ctx.v55);
  ctx.v0 = RsAdd(ctx.v27, ctx.v27);
  ctx.v54 = RsSld<4>(ctx.v63, ctx.v27);
  ctx.v13 = RsAdd(ctx.v27, ctx.v0);
  ctx.v53 = RsSld<4>(ctx.v54, ctx.v0);
  ctx.v28 = RsAdd(ctx.v0, ctx.v0);
  ctx.v0 = RsSld<4>(ctx.v53, ctx.v13);
  ctx.v10 = RsAdd(ctx.v28, ctx.v28);
  ctx.v11 = RsAdd(ctx.v0, ctx.v28);
  ctx.v0 = RsAdd(ctx.v0, ctx.v12);
  ctx.v13 = RsAdd(ctx.v10, ctx.v10);
  ctx.v12 = RsAdd(ctx.v11, ctx.v12);
  ctx.v11 = RsAdd(ctx.v0, ctx.v10);
  ctx.v10 = RsAdd(ctx.v12, ctx.v10);

  if (r9 != 0) {  // 16 outputs per iteration
    ctx.v63 = RsSplatWord(ctx.v63, 1);
    uint64_t r30 = r4 + 32;
    const uint32_t iterations = uint32_t(r9) >> 4;
    // All but the last: NEON.
    if (iterations > 1) {
      uint32x4_t frac[4] = {RsReverse(vld1q_u32(ctx.v0.u32)), RsReverse(vld1q_u32(ctx.v12.u32)),
                            RsReverse(vld1q_u32(ctx.v11.u32)), RsReverse(vld1q_u32(ctx.v10.u32))};
      const uint32x4_t advance = vld1q_u32(ctx.v13.u32);
      const uint8x16_t reverse = {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12};
      uint32x4_t pairs[4] = {};  // per lane: sample | next sample << 16 (after the vrev16)
      for (uint32_t it = 0; it + 1 < iterations; ++it) {
        uint64_t a = r3;
        const uint32_t base_a = uint32_t(r31);
        const uint64_t q0 = r3 + step8;
        const uint64_t b_base = r31 + RsTwiceHigh(q0);
        const uint32_t base_b = uint32_t(b_base);
        uint64_t q = uint32_t(q0);
#define MP3_RS_GATHER(m, k, ea) pairs[m] = vsetq_lane_u32(RsPair(base, ea), pairs[m], k)
        MP3_RS_GATHER(0, 0, base_a + RsTwiceHigh(a));
        MP3_RS_GATHER(0, 1, base_a + RsTwiceHigh(a += step));
        MP3_RS_GATHER(0, 2, base_a + RsTwiceHigh(a += step));
        MP3_RS_GATHER(0, 3, base_a + RsTwiceHigh(a += step));
        MP3_RS_GATHER(1, 0, base_a + RsTwiceHigh(a += step));
        MP3_RS_GATHER(1, 1, base_a + RsTwiceHigh(a += step));
        MP3_RS_GATHER(1, 2, base_a + RsTwiceHigh(a += step));
        MP3_RS_GATHER(1, 3, base_a + RsTwiceHigh(a += step));
        MP3_RS_GATHER(2, 0, base_b + RsTwiceHigh(q));
        MP3_RS_GATHER(2, 1, base_b + RsTwiceHigh(q += step));
        MP3_RS_GATHER(2, 2, base_b + RsTwiceHigh(q += step));
        MP3_RS_GATHER(2, 3, base_b + RsTwiceHigh(q += step));
        MP3_RS_GATHER(3, 0, base_b + RsTwiceHigh(q += step));
        MP3_RS_GATHER(3, 1, base_b + RsTwiceHigh(q += step));
        MP3_RS_GATHER(3, 2, base_b + RsTwiceHigh(q += step));
        MP3_RS_GATHER(3, 3, base_b + RsTwiceHigh(q += step));
#undef MP3_RS_GATHER
        q += step;
        r31 = b_base + RsTwiceHigh(q);
        r3 = uint32_t(q);
        for (int m = 0; m < 4; ++m) {
          const int32x4_t both = vreinterpretq_s32_u8(vrev16q_u8(vreinterpretq_u8_u32(pairs[m])));
          const float32x4_t x0 =
              vmulq_f32(vcvtq_f32_s32(vshrq_n_s32(vshlq_n_s32(both, 16), 16)), vdupq_n_f32(0x1p-15f));
          const float32x4_t x1 = vmulq_f32(vcvtq_f32_s32(vshrq_n_s32(both, 16)), vdupq_n_f32(0x1p-15f));
          const float32x4_t f = vmulq_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(vshrq_n_u32(frac[m], 1))),
                                          vdupq_n_f32(0x1p-31f));
          const float32x4_t out = vfmaq_f32(x0, vsubq_f32(x1, x0), f);
          vst1q_u8(REX_RAW_ADDR(uint32_t(r4 + 16 * m)), vqtbl1q_u8(vreinterpretq_u8_f32(out), reverse));
          frac[m] = vaddq_u32(frac[m], advance);
        }
        r4 += 64;
        r30 += 64;
      }
      // The guest's sample windows after that iteration (halfword h = sample h of the chain: v60 / v59 the samples and
      // the next ones of outputs 0-7, v62 / v61 of 8-15) and fractions.
      for (int c = 0; c < 2; ++c) {
        const uint16x8_t both = vreinterpretq_u16_u8(vrev16q_u8(vreinterpretq_u8_u32(pairs[2 * c])));
        const uint16x8_t both_hi = vreinterpretq_u16_u8(vrev16q_u8(vreinterpretq_u8_u32(pairs[2 * c + 1])));
        // lane order: samples 0..7 as uzp1 (low halves) / uzp2 (high halves); the window holds sample h in lane 7 - h.
        const uint16x8_t s0 = vuzp1q_u16(both, both_hi), s1 = vuzp2q_u16(both, both_hi);
        const uint8x16_t rev16 = {14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1};
        vst1q_u8((c ? ctx.v62 : ctx.v60).u8, vqtbl1q_u8(vreinterpretq_u8_u16(s0), rev16));
        vst1q_u8((c ? ctx.v61 : ctx.v59).u8, vqtbl1q_u8(vreinterpretq_u8_u16(s1), rev16));
      }
      vst1q_u32(ctx.v0.u32, RsReverse(frac[0]));
      vst1q_u32(ctx.v12.u32, RsReverse(frac[1]));
      vst1q_u32(ctx.v11.u32, RsReverse(frac[2]));
      vst1q_u32(ctx.v10.u32, RsReverse(frac[3]));
    }
    // The last, register by register.
    {
      const uint64_t base_a = r31;
      const uint64_t q0 = r3 + step8;
      const uint64_t b_base = r31 + RsTwiceHigh(q0);
      const uint64_t q = uint32_t(q0);
      for (int j = 0; j < 2; ++j) {
        const uint64_t a = r3 + 4 * j * step, b = q + 4 * j * step;
        uint32_t ea[4], eb[4];
        for (int k = 0; k < 4; ++k) {
          ea[k] = uint32_t(base_a) + RsTwiceHigh(a + k * step);
          eb[k] = uint32_t(b_base) + RsTwiceHigh(b + k * step);
        }
        ctx.v52 = Lvlx(base, ea[0]);
        ctx.v51 = Lvlx(base, ea[0] + 2);
        ctx.v50 = RsSld<2>(ctx.v60, ctx.v52);
        ctx.v49 = RsSld<2>(ctx.v59, ctx.v51);
        ctx.v48 = Lvlx(base, eb[0]);
        ctx.v47 = Lvlx(base, eb[0] + 2);
        ctx.v46 = Lvlx(base, eb[1]);
        ctx.v45 = Lvlx(base, eb[1] + 2);
        ctx.v43 = RsSld<2>(ctx.v61, ctx.v47);
        ctx.v44 = RsSld<2>(ctx.v62, ctx.v48);
        ctx.v42 = Lvlx(base, eb[2]);
        ctx.v41 = Lvlx(base, eb[2] + 2);
        ctx.v40 = Lvlx(base, eb[3]);
        ctx.v39 = RsSld<2>(ctx.v44, ctx.v46);
        ctx.v38 = RsSld<2>(ctx.v43, ctx.v45);
        ctx.v37 = Lvlx(base, eb[3] + 2);
        ctx.v36 = Lvlx(base, ea[1]);
        ctx.v35 = Lvlx(base, ea[1] + 2);
        ctx.v34 = RsSld<2>(ctx.v50, ctx.v36);
        ctx.v33 = RsSld<2>(ctx.v49, ctx.v35);
        ctx.v32 = RsSld<2>(ctx.v39, ctx.v42);
        ctx.v61 = RsSld<2>(ctx.v38, ctx.v41);
        ctx.v60 = Lvlx(base, ea[2]);
        ctx.v59 = Lvlx(base, ea[2] + 2);
        ctx.v56 = RsSld<2>(ctx.v34, ctx.v60);
        ctx.v55 = RsSld<2>(ctx.v33, ctx.v59);
        ctx.v54 = Lvlx(base, ea[3]);
        ctx.v53 = Lvlx(base, ea[3] + 2);
        ctx.v62 = RsSld<2>(ctx.v32, ctx.v40);
        ctx.v61 = RsSld<2>(ctx.v61, ctx.v37);
        ctx.v60 = RsSld<2>(ctx.v56, ctx.v54);
        ctx.v59 = RsSld<2>(ctx.v55, ctx.v53);
      }
      const uint64_t end = q + 8 * step;
      r6 = base_a + RsTwiceHigh(r3 + 6 * step);
      r7 = end >> 32;
      r3 = uint32_t(end);
      r31 = b_base + RsTwiceHigh(end);
      ctx.v52 = RsUnpackHigh(ctx.v60);
      ctx.v51 = RsShift1(ctx.v0);
      ctx.v50 = RsUnpackHigh(ctx.v59);
      ctx.v49 = RsShift1(ctx.v12);
      ctx.v48 = RsUnpackLow(ctx.v60);
      ctx.v47 = RsShift1(ctx.v11);
      ctx.v46 = RsUnpackLow(ctx.v59);
      ctx.v45 = RsShift1(ctx.v10);
      ctx.v44 = RsUnpackHigh(ctx.v62);
      ctx.v9 = RsSample(ctx.v52);
      ctx.v43 = RsUnpackHigh(ctx.v61);
      ctx.v42 = RsSample(ctx.v50);
      ctx.v41 = RsUnpackLow(ctx.v62);
      ctx.v8 = RsSample(ctx.v48);
      ctx.v40 = RsUnpackLow(ctx.v61);
      ctx.v39 = RsSample(ctx.v46);
      ctx.v7 = RsSample(ctx.v44);
      ctx.v38 = RsSample(ctx.v43);
      ctx.v6 = RsSample(ctx.v41);
      ctx.v0 = RsAdd(ctx.v0, ctx.v13);
      ctx.v37 = RsSample(ctx.v40);
      ctx.v12 = RsAdd(ctx.v12, ctx.v13);
      ctx.v4 = RsFraction(ctx.v51);
      ctx.v11 = RsAdd(ctx.v11, ctx.v13);
      ctx.v2 = RsFraction(ctx.v49);
      ctx.v10 = RsAdd(ctx.v10, ctx.v13);
      ctx.v31 = RsFraction(ctx.v47);
      ctx.v29 = RsFraction(ctx.v45);
      ctx.v5 = RsSub(ctx.v42, ctx.v9);
      ctx.v3 = RsSub(ctx.v39, ctx.v8);
      ctx.v1 = RsSub(ctx.v38, ctx.v7);
      ctx.v30 = RsSub(ctx.v37, ctx.v6);
      ctx.v9 = RsMadd(ctx.v5, ctx.v4, ctx.v9);
      ctx.v8 = RsMadd(ctx.v3, ctx.v2, ctx.v8);
      ctx.v7 = RsMadd(ctx.v1, ctx.v31, ctx.v7);
      ctx.v6 = RsMadd(ctx.v30, ctx.v29, ctx.v6);
      r9 = r30 - 16;
      r8 = r30 + 16;
      RsStore16(base, uint32_t(r4), ctx.v9);
      RsStore16(base, uint32_t(r9), ctx.v8);
      RsStore16(base, uint32_t(r30), ctx.v7);
      RsStore16(base, uint32_t(r8), ctx.v6);
      r4 += 64;
      r30 += 64;
      ctr = 0;
    }
  }

  if (r25 != 0) {  // 4 outputs per iteration
    ctx.v63 = RsSplatWord(ctx.v63, 1);
    r7 = 32;
    for (uint32_t g = 0; g < r25 / 4; ++g) {
      uint32_t e[4];
      for (int k = 0; k < 4; ++k) e[k] = uint32_t(r31) + RsTwiceHigh(r3 + k * step);
      const uint64_t next = r3 + 4 * step;
      ctx.v36 = Lvlx(base, e[0]);
      ctx.v35 = Lvlx(base, e[0] + 2);
      ctx.v34 = RsSld<2>(ctx.v58, ctx.v36);
      ctx.v33 = RsSld<2>(ctx.v57, ctx.v35);
      ctx.v32 = Lvlx(base, e[1]);
      ctx.v62 = Lvlx(base, e[1] + 2);
      ctx.v61 = RsSld<2>(ctx.v34, ctx.v32);
      ctx.v60 = RsSld<2>(ctx.v33, ctx.v62);
      ctx.v58 = Lvlx(base, e[2] + 2);
      ctx.v59 = Lvlx(base, e[2]);
      ctx.v56 = RsSld<2>(ctx.v60, ctx.v58);
      ctx.v57 = RsSld<2>(ctx.v61, ctx.v59);
      ctx.v55 = Lvlx(base, e[3]);
      ctx.v54 = Lvlx(base, e[3] + 2);
      ctx.v58 = RsSld<2>(ctx.v57, ctx.v55);
      ctx.v57 = RsSld<2>(ctx.v56, ctx.v54);
      ctx.v53 = RsUnpackLow(ctx.v58);
      ctx.v52 = RsShift1(ctx.v0);
      ctx.v51 = RsUnpackLow(ctx.v57);
      ctx.v0 = RsAdd(ctx.v0, ctx.v28);
      ctx.v13 = RsSample(ctx.v53);
      ctx.v50 = RsSample(ctx.v51);
      ctx.v11 = RsFraction(ctx.v52);
      ctx.v12 = RsSub(ctx.v50, ctx.v13);
      ctx.v13 = RsMadd(ctx.v12, ctx.v11, ctx.v13);
      RsStore16(base, uint32_t(r4), ctx.v13);
      r4 += 16;
      r6 = (r3 + 3 * step) >> 32;
      r8 = r31 + RsTwiceHigh(r3 + 3 * step);
      r9 = RsTwiceHigh(next);
      r31 += r9;
      r3 = uint32_t(next);
    }
    ctr = 0;
  }

  if (r24 != 0) {  // 1 output per iteration
    ctx.v63 = RsSplatWord(ctx.v63, 1);
    for (uint32_t i = 0; i < r24; ++i) {
      ctx.v49 = RsShift1(ctx.v0);
      ctx.v0 = RsAdd(ctx.v0, ctx.v27);
      ctx.v11 = RsFraction(ctx.v49);
      const uint64_t next = r3 + step;
      r8 = r31 + RsTwiceHigh(r3);
      r7 = next >> 32;
      r9 = RsTwiceHigh(next);
      r3 = uint32_t(next);
      r31 += r9;
      ctx.v48 = Lvlx(base, uint32_t(r8));
      ctx.v47 = Lvlx(base, uint32_t(r8) + 2);
      ctx.v46 = RsUnpackHigh(ctx.v48);
      ctx.v45 = RsUnpackHigh(ctx.v47);
      ctx.v13 = RsSample(ctx.v46);
      ctx.v44 = RsSample(ctx.v45);
      ctx.v12 = RsSub(ctx.v44, ctx.v13);
      ctx.v13 = RsMadd(ctx.v12, ctx.v11, ctx.v13);
      ctx.v43 = RsSplat0(ctx.v13);
      REX_STORE_U32(uint32_t(r4) & ~3u, ctx.v43.u32[0]);
      r4 += 4;
    }
    ctr = 0;
  }

  // The fraction left to *r6: the u64 -> double helper (its registers), times 2^-32, single.
  {
    uint64_t r5 = r3 == 0 ? 64 : __builtin_clzll(r3);
    uint64_t x = (r5 & 0x40) ? 0 : (r3 << (r5 & 0x7F));
    ctx.cr0.compare<int64_t>(int64_t(x), 0, ctx.xer);
    if (!ctx.cr0.eq) {
      ctx.xer.ca = uint32_t(r5) <= 1086;
      r5 = 1086 - r5;
      x = __builtin_rotateleft64(x, 53) & 0xFFFFFFFFFFFFFull;
      x = (__builtin_rotateleft64(r5, 52) & 0x7FF0000000000000ull) | (x & 0x800FFFFFFFFFFFFFull);
    }
    REX_SSTORE_U64(frame - 8, x);
    ctx.f1.u64 = REX_SLOAD_U64(frame - 8);
    ctx.r5.u64 = r5;
  }
  const uint64_t r10_end = r31 - source;
  ctx.xer.ca = (int32_t(r10_end) < 0) & ((r10_end & 1) != 0);
  ctx.r3.s64 = int32_t(r10_end) >> 1;
  ctx.f0.f64 = ctx.f1.f64 * std::bit_cast<double>(REX_LOAD_U64(0x821139C0));
  ctx.f13.f64 = double(float(ctx.f0.f64));
  StoreF32(base, position_at, float(ctx.f13.f64));
  ctx.r1.u64 = r1_frame + 208;
  ctx.r12.u64 = REX_SLOAD_U32(sp - 8);
  ctx.lr = ctx.r12.u64;
  (void)r10;

  ctx.r4.u64 = r4;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = r9;
  ctx.r10.u64 = r10_end;
  ctx.r11.s64 = -2112815104;
  ctx.f9.s64 = f9;
  ctx.f10.s64 = f10;
  ctx.f11.f64 = f11;
  ctx.f12.f64 = f12;
  ctx.cr6.compare<uint32_t>(r24, 0, ctx.xer);
  ctx.ctr.u64 = ctr;
#else
  __imp__mp3_XAudioResampleMp3(ctx, base);
#endif
}
#endif  // MP3_TIENE_mp3_XAudioResampleMp3

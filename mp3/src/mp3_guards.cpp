// mp3-nx - diagnostics of the recompiled code: where the main guest thread is (the [main] line every 10 s), and the
// report of tools/patch_local_regs.py's check build (PLR_CHECK=1). From gears2-nx's gow2_guards.cpp, without its
// Unreal object walks and its stack guard (Gears of War 2 only).

#include "mp3_pch.h"

#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

// Where the main guest thread is, for the 10 s statistics (src/mp3_gpu.cpp): its last call site (lr), stack pointer
// and r3, r30, r31. A hang shows up as the same place repeated, without a rebuild to find it. With
// tools/patch_local_regs.py the context holds the values last written back at a call, not the live ones.
void mp3_log_main_thread() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  for (auto& thread : kernel->object_table()->GetObjectsByType<rex::system::XThread>()) {
    if (!thread->main_thread() || !thread->thread_state()) continue;
    const PPCContext& ctx = *thread->thread_state()->context();
    REXLOG_INFO("[main] lr {:08X} r1 {:08X} r3 {:08X} r30 {:08X} r31 {:08X}", uint32_t(ctx.lr), ctx.r1.u32,
                ctx.r3.u32, ctx.r30.u32, ctx.r31.u32);
  }
}

// The check build of tools/patch_local_regs.py (PLR_CHECK=1): a call changed registers the analysis said it keeps.
// Logged once per call site (its number, mp3/out/codegen/patch_local_regs.check.map), at most 256 sites, with each
// register that changed (before -> after). A poisoned value a callee read shows up as a fault at 0x5Exxxxxx instead:
// the low 24 bits are the site's number too.
extern "C" void mp3_regcheck_fail(const char* function, const char* callee, int site, const char* regs,
                                   const uint64_t* before, const PPCContext* ctx) {
  static std::mutex mutex;
  static std::unordered_set<int> seen;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() >= 256 || !seen.insert(site).second) return;
  }
  std::string changed;
  const char* p = regs;
  for (int k = 0; *p; ++k) {
    const char kind = *p++;
    int n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    while (*p == ' ') ++p;
    uint64_t now = 0;
    if (kind == 'r') {
      static const PPCRegister PPCContext::*const kR[32] = {
          &PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,  &PPCContext::r4,  &PPCContext::r5,
          &PPCContext::r6,  &PPCContext::r7,  &PPCContext::r8,  &PPCContext::r9,  &PPCContext::r10, &PPCContext::r11,
          &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17,
          &PPCContext::r18, &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
          &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29,
          &PPCContext::r30, &PPCContext::r31};
      now = (ctx->*kR[n & 31]).u64;
    } else {
      static const PPCRegister PPCContext::*const kF[32] = {
          &PPCContext::f0,  &PPCContext::f1,  &PPCContext::f2,  &PPCContext::f3,  &PPCContext::f4,  &PPCContext::f5,
          &PPCContext::f6,  &PPCContext::f7,  &PPCContext::f8,  &PPCContext::f9,  &PPCContext::f10, &PPCContext::f11,
          &PPCContext::f12, &PPCContext::f13, &PPCContext::f14, &PPCContext::f15, &PPCContext::f16, &PPCContext::f17,
          &PPCContext::f18, &PPCContext::f19, &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23,
          &PPCContext::f24, &PPCContext::f25, &PPCContext::f26, &PPCContext::f27, &PPCContext::f28, &PPCContext::f29,
          &PPCContext::f30, &PPCContext::f31};
      now = (ctx->*kF[n & 31]).u64;
    }
    if (now != before[k]) changed += fmt::format(" {}{} {:X}->{:X}", kind, n, before[k], now);
  }
  REXLOG_WARN("[regcheck] site {:06X} ({}): the call to {} changed{}", site, function, callee, changed);
}

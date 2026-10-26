#!/usr/bin/env python3
"""Make the Switch PCH for the generated code.

generated/default/mp3_pch.h is rendered by the host codegen (ReXGlue v0.10.0). The Switch
needs the changes below (the first three came from Dead Space 2's port, ds2-nx, through Dante's
Inferno's; the second is a no-op here unless the codegen emits that macro):

  1. DEFINE_REX_FUNC gets a GCC branch (attributes after the declarator). Without it GCC
     drops the weak alias and the link fails with every sub_ symbol undefined.
  2. REX_RAW_ADDR_W(x, n): DS2's codegen writes it for stores; outside the official-SDK
     NX memory model it is REX_RAW_ADDR(x).
  3. The interrupt locks, per thread (below).
  4. __builtin_arm_isb for GCC (clang's builtin; tools/patch_barriers.py writes it for the Xenon
     delay hint db16cyc of spin loops; devkitA64's GCC has no such builtin, the same ISB inline).
  6. Guest stack accesses without volatile (tools/patch_stack_access.py).
  7. The check build's report function (tools/patch_local_regs.py, PLR_CHECK=1).
  8. The memory barriers and prefetches tools/patch_barriers.py writes.
  9. An include guard around everything (below).
From gears2-nx (Gears of War 2's port), without its stack guard (its point 5).

Usage: make_nx_pch.py <generated/default/mp3_pch.h> <out.h>
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()

GCC_BRANCH = '''#elif defined(__GNUC__) && !defined(__clang__)
// GCC (devkitA64): attributes go after the declarator, or the weak alias is dropped.
#define DEFINE_REX_FUNC(name)                                       \\
  extern "C" REX_FUNC(name)                                         \\
      __attribute__((alias("__imp__" #name), weak, noinline));      \\
  REX_EXTERN(__imp__##name)
#else
#define DEFINE_REX_FUNC(name)                                       \\
  __attribute__((alias("__imp__" #name)))'''

anchor = '''#else
#define DEFINE_REX_FUNC(name)                                       \\
  __attribute__((alias("__imp__" #name)))'''
if text.count(anchor) != 1:
    sys.exit("DEFINE_REX_FUNC anchor not found exactly once")
text = text.replace(anchor, GCC_BRANCH)

if "REX_RAW_ADDR_W" not in text:
    text = text.rstrip() + '''

//=============================================================================
// Switch (rexglue-sdk-nx): DS2's codegen emits REX_RAW_ADDR_W for stores.
//=============================================================================
#ifndef REX_RAW_ADDR_W
#define REX_RAW_ADDR_W(x, n) REX_RAW_ADDR(x)
#endif
'''
# 3. The interrupt locks. Guest code brackets its lwarx/stwcx. sequences with mtmsrd (interrupts off), and the codegen
#    turns each of those into one process-wide recursive mutex. The atomic itself is already a host compare-and-swap;
#    the mutex only serialized every thread doing atomics: Dead Space 2's job scheduler (the main thread queuing
#    skinning and culling jobs, four EARSJob workers taking them) spent up to a quarter of the main thread blocked on
#    it with 3 cores for 9 busy threads. Now the "interrupts off" state is per thread (what mfmsr reads back), and the
#    old global lock stays available (ppc_msr_cerrojo_global_(), set at startup by mp3_cpu.cpp from a cvar).
OLD_LOCKS_START = "#define REX_CHECK_GLOBAL_LOCK()"
OLD_LOCKS_END = "rex::thread::global_critical_region::mutex().unlock();                                \\\n  } while (0)\n"
i = text.find(OLD_LOCKS_START)
j = text.find(OLD_LOCKS_END, i)
if i < 0 or j < 0 or text.count(OLD_LOCKS_START) != 1:
    sys.exit("interrupt lock macros not found exactly once")
j += len(OLD_LOCKS_END)
NEW_LOCKS = """// Switch (make_nx_pch.py): the interrupt locks per thread unless ppc_msr_cerrojo_global_() (see the script).
inline bool& ppc_msr_cerrojo_global_() {
  static bool global = false;
  return global;
}

inline i32& ppc_msr_profundidad_() {
  static thread_local i32 profundidad = 0;
  return profundidad;
}

#define REX_CHECK_GLOBAL_LOCK()                                          \\
  (ppc_msr_cerrojo_global_() ? ([&]() -> u64 {                           \\
    auto lock_ = rex::thread::global_critical_region::AcquireDirect();   \\
    return ppc_global_lock_count_().load() ? 0 : 0x8000;                 \\
  }())                                                                   \\
                             : (ppc_msr_profundidad_() ? u64(0) : u64(0x8000)))

#define REX_ENTER_GLOBAL_LOCK()                              \\
  do {                                                       \\
    if (ppc_msr_cerrojo_global_()) {                         \\
      rex::thread::global_critical_region::mutex().lock();   \\
      ppc_global_lock_count_().fetch_add(1);                 \\
    } else {                                                 \\
      ++ppc_msr_profundidad_();                              \\
    }                                                        \\
  } while (0)

#define REX_LEAVE_GLOBAL_LOCK()                              \\
  do {                                                       \\
    if (ppc_msr_cerrojo_global_()) {                         \\
      ppc_global_lock_count_().fetch_sub(1);                 \\
      rex::thread::global_critical_region::mutex().unlock(); \\
    } else {                                                 \\
      --ppc_msr_profundidad_();                              \\
    }                                                        \\
  } while (0)
"""
text = text[:i] + NEW_LOCKS + text[j:]

# 5b. REX_FUNC_PROLOGUE_LEAF: gears2-nx's prologue without its stack guard, for functions that call nothing. This port
#     has no stack guard (point 5 of gears2-nx: a few instructions in every call, for a Gears-only recursion), so it is
#     only defined, in case a pass from there writes it.
if "REX_FUNC_PROLOGUE_LEAF" not in text:
    text = text.rstrip() + """

// Switch (make_nx_pch.py, point 5b): the prologue of functions that call nothing, without the stack guard.
#if defined(__GNUC__)
#define REX_FUNC_PROLOGUE_LEAF()        \\
  do {                                  \\
    if (((size_t)base & 0x1F) != 0)     \\
      __builtin_unreachable();          \\
  } while (0)
#else
#define REX_FUNC_PROLOGUE_LEAF() ((void)0)
#endif
"""

# 6. The guest stack accesses tools/patch_stack_access.py marks: plain (memcpy, alias-safe) instead of volatile, so the
#    compiler may keep a spilled local in a register or pair the helpers' stores. The stack belongs to one thread.
if "REX_SLOAD_U32" not in text:
    text = text.rstrip() + """

// Switch (make_nx_pch.py, point 6): guest stack accesses (r1), without volatile.
#define REX_SLOAD_U8(x) ({ u8 rex_sv_; __builtin_memcpy(&rex_sv_, base + (u32)(x), 1); rex_sv_; })
#define REX_SLOAD_U16(x) ({ u16 rex_sv_; __builtin_memcpy(&rex_sv_, base + (u32)(x), 2); __builtin_bswap16(rex_sv_); })
#define REX_SLOAD_U32(x) ({ u32 rex_sv_; __builtin_memcpy(&rex_sv_, base + (u32)(x), 4); __builtin_bswap32(rex_sv_); })
#define REX_SLOAD_U64(x) ({ u64 rex_sv_; __builtin_memcpy(&rex_sv_, base + (u32)(x), 8); __builtin_bswap64(rex_sv_); })
#define REX_SSTORE_U8(x, y) ({ u8 rex_sv_ = (u8)(y); __builtin_memcpy(base + (u32)(x), &rex_sv_, 1); })
#define REX_SSTORE_U16(x, y) ({ u16 rex_sv_ = __builtin_bswap16(y); __builtin_memcpy(base + (u32)(x), &rex_sv_, 2); })
#define REX_SSTORE_U32(x, y) ({ u32 rex_sv_ = __builtin_bswap32(y); __builtin_memcpy(base + (u32)(x), &rex_sv_, 4); })
#define REX_SSTORE_U64(x, y) ({ u64 rex_sv_ = __builtin_bswap64(y); __builtin_memcpy(base + (u32)(x), &rex_sv_, 8); })
"""

# 7. The check build of tools/patch_local_regs.py (PLR_CHECK=1) reports through this (src/mp3_guards.cpp).
if "mp3_regcheck_fail" not in text:
    text = text.rstrip() + """

// Switch (make_nx_pch.py, point 7): tools/patch_local_regs.py's check build.
extern "C" void mp3_regcheck_fail(const char* function, const char* callee, int site, const char* regs,
                                   const uint64_t* before, const PPCContext* ctx);
"""

if "__builtin_arm_isb" not in text:
    text = text.rstrip() + '''

//=============================================================================
// Switch (make_nx_pch.py): the spin-loop pause patch_spin_hints.py writes (clang's builtin) for GCC.
//=============================================================================
#if defined(__GNUC__) && !defined(__clang__) && defined(__aarch64__) && !defined(__builtin_arm_isb)
#define __builtin_arm_isb(x) __asm__ __volatile__("isb" ::: "memory")
#endif
'''

# 8. tools/patch_barriers.py: the PowerPC barriers as ARM ones, and the cache hints as prefetches. The codegen turns
#    sync, lwsync, eieio and isync into comments (x86 orders memory by itself); the Switch's Cortex-A57 does not.
if "REX_PPC_SYNC" not in text:
    text = text.rstrip() + """

// Switch (make_nx_pch.py, point 8): tools/patch_barriers.py.
#define REX_PPC_SYNC() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define REX_PPC_LWSYNC() __atomic_thread_fence(__ATOMIC_ACQ_REL)
#define REX_PPC_EIEIO() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define REX_PPC_ISYNC() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define REX_PPC_DCBT(ea) __builtin_prefetch(base + (u32)(ea), 0, 3)
#define REX_PPC_DCBTST(ea) __builtin_prefetch(base + (u32)(ea), 1, 3)
"""

# 10. tools/patch_vmx_exact.py: vmaddfp/vnmsubfp as the console's VMX computes them, with one rounding (fmla/fmls on
#     the Switch). Liberty Recompiled and the Vulkan build (source/) build GTA IV that way (REX_FUSED_VMADDFP).
if "REX_VMADDFP" not in text:
    text = text.rstrip() + """

// Switch (make_nx_pch.py, point 10): tools/patch_vmx_exact.py.
#include <simde/x86/fma.h>
#define REX_VMADDFP(a, b, c) simde_mm_fmadd_ps((a), (b), (c))
#define REX_VNMSUBFP(a, b, c) simde_mm_fnmadd_ps((a), (b), (c))
"""

# 9. An include guard around the whole header. The generated files include "mp3_pch.h" again after the PCH
#    (cmake_pch.hxx) has: #pragma once alone did not stop that second inclusion with devkitA64's GCC 16 and the
#    precompiled header (every recompiled unit failed with redefinitions on GTA IV's first build), a guard macro does.
if "MP3_NX_PCH_H" not in text:
    text = "#ifndef MP3_NX_PCH_H\n#define MP3_NX_PCH_H\n" + text.rstrip() + "\n\n#endif  // MP3_NX_PCH_H\n"

# Only when it changes: every unit of the recompiled tree depends on this header through the PCH, so rewriting it
# with the same text on each configure would make the next build recompile all of them.
try:
    previous = open(dst, encoding="utf-8").read()
except OSError:
    previous = None
if previous == text:
    print("unchanged", dst)
else:
    open(dst, "w", encoding="utf-8").write(text)
    print("wrote", dst)

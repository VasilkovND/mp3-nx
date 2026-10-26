#!/usr/bin/env python3
"""The PowerPC memory barriers and cache hints the codegen leaves as comments, as ARM code.

ReXGlue v0.10.0's codegen was written for x86, which orders memory by itself: sync, lwsync, eieio and isync become
only their disassembly comment (`// lwsync `). The Switch's Cortex-A57 reorders loads and stores, so the game's own
lock-free code (job queues, reference counts, the streaming request lists) needs the fences back. Each comment gets
its macro on the next line (make_nx_pch.py, point 8): sync and eieio a full fence, lwsync an acquire-release one,
isync (the end of an acquire sequence after lwarx/stwcx.) an acquire one.

Two hints as well:
  - db16cyc (the Xenon's 16-cycle pause in spin loops) becomes an ISB, as gears-of-war-2-pc-port's
    patch_spin_hints.py does on Android: the waiting thread gives the core's pipeline a rest instead of hammering the
    cache line the other thread is about to write.
  - dcbt/dcbtst (the game's prefetches: the programmers put them where the data is not in the cache) become
    __builtin_prefetch (PRFM, which never faults). GTA IV's Switch port (kabba536, otimizar_codigo_gerado.py) measured
    one in the audio reverb: a load waiting on memory held 6.5 % of the audio thread right after eight dcbt that did
    nothing.

Runs right after codegen, on the code with ctx. names (before tools/patch_local_regs.py renames them). Idempotent.

Usage: patch_barriers.py generated/default/mp3_recomp.*.cpp
"""

import re
import sys

FENCES = {"sync": "REX_PPC_SYNC", "lwsync": "REX_PPC_LWSYNC", "eieio": "REX_PPC_EIEIO", "isync": "REX_PPC_ISYNC"}
FENCE = re.compile(r"^(\t+)// (sync|lwsync|eieio|isync)\b[^\n]*\n(?!\t+REX_PPC_)", re.M)
PAUSE = re.compile(r"^(\t+)// db16cyc\b[^\n]*\n(?!\t+__builtin_arm_isb)", re.M)
DCBT = re.compile(r"^(\t+)// (dcbt|dcbtst) r(\d+),r(\d+)[ \t]*\n(?!\t+REX_PPC_DCBT)", re.M)


def ea(ra, rb):
    return f"ctx.r{rb}.u32" if ra == "0" else f"ctx.r{ra}.u32 + ctx.r{rb}.u32"


def main(paths):
    fences = pauses = prefetches = 0
    for path in paths:
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        text, a = FENCE.subn(lambda m: m.group(0) + f"{m.group(1)}{FENCES[m.group(2)]}();\n", text)
        text, b = PAUSE.subn(lambda m: m.group(0) + f"{m.group(1)}__builtin_arm_isb(15);\n", text)
        text, c = DCBT.subn(lambda m: m.group(0) + "%sREX_PPC_%s(%s);\n" % (
            m.group(1), m.group(2).upper(), ea(m.group(3), m.group(4))), text)
        if a or b or c:
            open(path, "w", encoding="utf-8", errors="surrogateescape").write(text)
        fences, pauses, prefetches = fences + a, pauses + b, prefetches + c
    print(f"patch_barriers: {fences} fences, {pauses} spin pauses, {prefetches} prefetches")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])

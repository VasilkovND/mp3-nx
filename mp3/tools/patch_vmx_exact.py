#!/usr/bin/env python3
"""Vector instructions of ReXGlue v0.10.0's code made to compute what the console computes.

- vmaddfp / vnmsubfp: the codegen writes a*b + c and -(a*b - c) with two roundings; the console's VMX fuses them
  (one rounding). Liberty Recompiled and this repository's Vulkan build (source/) build GTA IV with the fused form
  (REX_FUSED_VMADDFP, simde's fmadd / fnmadd: fmla / fmls on the Switch). Written as REX_VMADDFP / REX_VNMSUBFP,
  defined in the PCH (tools/make_nx_pch.py, point 10).
- vpkuwus: the codegen writes it as eight scalar stores into vD, so when vD is also vA or vB a lane is overwritten
  before it is read. One vector expression instead (Liberty Recompiled's fix): clamp to 0xFFFF, then pack.

usage: patch_vmx_exact.py <generated .cpp>...
"""
import re
import sys

LOAD = r"simde_mm_load_ps\(([A-Za-z_0-9.]+)\)"
MADD = re.compile(r"simde_mm_add_ps\(simde_mm_mul_ps\(%s, %s\), %s\)" % (LOAD, LOAD, LOAD))
NMSUB = re.compile(
    r"simde_mm_xor_ps\(simde_mm_sub_ps\(simde_mm_mul_ps\(%s, %s\), %s\), "
    r"simde_mm_castsi128_ps\(simde_mm_set1_epi32\(int\(0x80000000\)\)\)\)" % (LOAD, LOAD, LOAD))
# Eight lines: vD.u16[7-i] from vA.u32[3-i] and vD.u16[3-i] from vB.u32[3-i], i = 0..3.
LANE = r"\t(?P<d{n}>[A-Za-z_0-9.]+)\.u16\[{h}\] = (?P<s{n}>[A-Za-z_0-9.]+)\.u32\[{w}\] > 0xFFFF \? 0xFFFF : \(uint16_t\)(?P=s{n})\.u32\[{w}\];\n"
PKUWUS = re.compile("".join(LANE.format(n=2 * i, h=7 - i, w=3 - i) + LANE.format(n=2 * i + 1, h=3 - i, w=3 - i)
                            for i in range(4)))


def pkuwus(m):
    d = {m.group("d%d" % k) for k in range(8)}
    a = {m.group("s%d" % k) for k in range(0, 8, 2)}
    b = {m.group("s%d" % k) for k in range(1, 8, 2)}
    if len(d) != 1 or len(a) != 1 or len(b) != 1:
        return m.group(0)
    d, a, b = d.pop(), a.pop(), b.pop()
    return ("\tsimde_mm_store_si128((simde__m128i*){d}.u16, simde_mm_packus_epi32("
            "simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*){b}.u32), simde_mm_set1_epi32(0xFFFF)), "
            "simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*){a}.u32), simde_mm_set1_epi32(0xFFFF))));\n"
            ).format(d=d, a=a, b=b)


totals = [0, 0, 0]
for path in sys.argv[1:]:
    text = open(path, encoding="utf-8").read()
    text, n1 = MADD.subn(r"REX_VMADDFP(simde_mm_load_ps(\1), simde_mm_load_ps(\2), simde_mm_load_ps(\3))", text)
    text, n2 = NMSUB.subn(r"REX_VNMSUBFP(simde_mm_load_ps(\1), simde_mm_load_ps(\2), simde_mm_load_ps(\3))", text)
    text, n3 = PKUWUS.subn(pkuwus, text)
    if n1 or n2 or n3:
        open(path, "w", encoding="utf-8").write(text)
    totals = [totals[0] + n1, totals[1] + n2, totals[2] + n3]
print("patch_vmx_exact: %d vmaddfp, %d vnmsubfp fused; %d vpkuwus as one expression" % tuple(totals))

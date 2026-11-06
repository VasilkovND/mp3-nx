#!/usr/bin/env python3
# mp3/tools/check_log.py <log files...>
# The regression suite built from every failure signature of the 0320-0341 era. Each check encodes a bug that
# SHIPPED and was read out of a console log: a build passes when every window of every phase matches the
# invariants below. Run over the historical logs first: the good eras pass, the broken ones fail by name.
import sys, re, collections

def windows(path):
    """300-frame [edram] windows -> list of dicts."""
    out = []
    for line in open(path, errors='replace'):
        m = re.search(r'\[edram\] 300 fotogramas: ([0-9.]+) transferencias.*?([0-9.]+) de 8 bits a flotante', line)
        if m:
            out.append({'transfers': float(m.group(1)), 'installs': float(m.group(2))})
    return out

def phases(path):
    """Classify windows into phases by their transfer rate (0.0 = menu/loading, active = gameplay)."""
    ws = windows(path)
    ph = {'idle': [w for w in ws if w['transfers'] == 0.0], 'active': [w for w in ws if w['transfers'] > 0.0]}
    return ph

def check(path):
    fails, warns = [], []
    txt = open(path, errors='replace').read()
    ph = phases(path)
    act, idle = ph['active'], ph['idle']
    # 1. mp3_085: two-plus plants a frame in gameplay broke the EDRAM cycle whole (2.x installs).
    for w in act:
        if w['installs'] > 1.05:
            fails.append(f"gameplay installs {w['installs']}/frame > 1.0 (the multi-plant collapse of 0340)")
    # 2. mp3_063-066: the empty-snapshot black (installs of zero content sustained).
    if act and all(w['installs'] == 0.0 for w in act[-3:]):
        warns.append("active windows end at 0.0 installs (the empty-content era signature)")
    # 3. The probes: tonemap inputs must be non-zero where they appear (the scene; 0331+).
    probes = re.findall(r'texel tonemap entrada slot 6 base \w+ \(\d+,\d+\): ([0-9A-F. ]+)', txt)
    zero_probes = sum(1 for p in probes if p.strip('0 .') == '')
    if probes and zero_probes == len(probes):
        fails.append("every scene probe reads zero at the tonemap's draw")
    # 4. mp3_071/072: the dead light skip (counter present but zero while additive draws ran).
    if 'luz aditiva saltada' not in txt and 'exposicion] fxaa' in txt:
        warns.append("fxaa draws but no light-skip lines (the dead-arming signature of 0326/0327)")
    # 5. The exp trace: a screen texture scaled by 2^-13 raw (the /8192 copies of 080's era).
    if re.search(r'exp_adjust -13 \(x0\.0001', txt):
        warns.append("a 2^-13 raw scaled copy of a screen texture")
    # 6. Shaders: address-step takes in steady state = unfunded re-patches = the spikes' canary.
    takes = len(re.findall(r'taken by address as n\d+', txt))
    if takes > 8:
        warns.append(f"{takes} address-step takes (the spikes' variants unfunded)")
    # 7. mp3_082: the crutch firing where no FXAA drew (the menu burial).
    fxaa = len(re.findall(r'exposicion\] fxaa', txt))
    if idle and not fxaa:
        for w in idle[-3:]:
            if w['installs'] > 0.0:
                fails.append(f"idle/menu window with {w['installs']} installs/frame and no fxaa anywhere (the menu burial)")
    # 8. The menu phase needs the f0 plants (0340+): some installs in idle windows that have resolves.
    #    (Only a warning: pre-0340 logs legitimately read 0.0 here.)
    if idle and not any(w['installs'] > 0 for w in idle):
        warns.append("no installs in any idle window (menus unpainted - pre-0340 behavior)")
    return fails, warns

def main():
    ok = True
    print(f"{'log':<14} {'verdict':<8} notes")
    for path in sys.argv[1:]:
        try:
            fails, warns = check(path)
        except Exception as e:
            print(f"{path:<14} ERROR   {e}")
            ok = False
            continue
        name = path.split('/')[-1]
        if fails:
            ok = False
            print(f"{name:<14} FAIL    " + "; ".join(fails))
            for w in warns: print(f"{'':<14} warn    {w}")
        else:
            print(f"{name:<14} PASS    " + ("; ".join(warns) if warns else "clean"))
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()

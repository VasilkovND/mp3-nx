#!/usr/bin/env python3
"""Make the SD card folder for mp3-nx from your own Max Payne 3 (disc 1) and Title Update 6.

    python3 stage_sd.py --tu tu00000006_00000000 -o <out>/mp3-nx   (a Games on Demand package goes in god/ as it is)
    python3 stage_sd.py --disc "Max Payne 3 Disc 1.iso" --tu tu00000006_00000000 -o <out>/mp3-nx
    python3 stage_sd.py --check <game folder>            (what tools/codegen.sh runs: is this a Max Payne 3 folder?)

Writes, beside where the NRO goes (src/mp3_app.h reads this layout):
    mp3-nx/update/ the files of Title Update 6's disc001/ folder (default.xexp, update.rpf, commonup.rpf,
                    platformup.rpf, platform/levels/.../script_final_patch.rpf ...): the game's update: device
    mp3-nx/game/   with --disc: the extracted disc (no file over FAT32's 4 GB) and TU6's default.xexp next to its
                    default.xex (the runtime and the code generator apply it, as for Dante's Inferno's TU2)

Title Update 6 holds one folder per disc (disc001/, disc002/), each with its own default.xexp: --disc-number picks one
(1 until disc 2 is ported). The same game/ folder is what tools/build_mac.sh takes to generate the code; with a Games
on Demand package, dantes-inferno-nx's god_xex.py takes default.xex out of the package's pieces.
Only the standard library (the title update package is read by the small STFS reader below, from dantes-inferno-nx).
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

TITLE_ID = 0x5454086B  # Max Payne 3
# Tested inputs: TU6 default.xexp hash -> (edition, disc default.xex hash). Kept empty: no hashes of the game in the
# repository.
EDITIONS = {}
SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Disc files the game does not need: the console's system update.
SKIP = ("$systemupdate/",)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --- XDVDFS (Xbox 360 disc) -------------------------------------------------------------------------------
def partition(f) -> int:
    for base in (0x0, 0x2080000, 0xFD90000, 0x18300000):
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    sys.exit("Not an Xbox 360 disc image (no XDVDFS volume found)")


def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    pending, seen = [0], set()
    while pending:
        o = pending.pop()
        p = o * 4
        if o in seen or p + 14 > len(table):
            continue
        seen.add(o)
        left, right, start, length, attr, nlen = struct.unpack("<HHIIBB", table[p:p + 14])
        if left == 0xFFFF:
            continue
        name = table[p + 14:p + 14 + nlen].decode("latin1")
        if left:
            pending.append(left)
        if right:
            pending.append(right)
        if "/" in name or "\\" in name or name in ("", ".", ".."):
            sys.exit(f"Unsafe name on disc: {name!r}")
        path = f"{prefix}{name}"
        if attr & 0x10:
            if length:
                yield from walk(f, base, start, length, path + "/")
        else:
            yield path, start, length


def extract(iso: Path, out: Path) -> None:
    with iso.open("rb") as f:
        base = partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root, root_size = struct.unpack("<II", f.read(8))
        files = sorted(e for e in walk(f, base, root, root_size) if not e[0].lower().startswith(SKIP))
        total = sum(n for _, _, n in files)
        done = 0
        for path, start, length in files:
            dest = out / path
            if dest.is_file() and dest.stat().st_size == length:
                done += length
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            f.seek(base + start * SECTOR)
            left = length
            with dest.open("wb") as o:
                while left:
                    chunk = f.read(min(8 << 20, left))
                    if not chunk:
                        sys.exit(f"{iso}: image ends inside {path}")
                    o.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
                    print(f"\r  {iso.name}: {done * 100 // max(total, 1)}%", end="", flush=True)
        print(f"\r  {iso.name}: {len(files)} files, {total / 2**30:.1f} GiB -> {out}")


# --- Title update ------------------------------------------------------------------------------------------
class Stfs:
    """The parts of an Xbox 360 STFS package (CON/LIVE/PIRS) needed to read one file from it."""

    BLOCK = 0x1000

    def __init__(self, path: Path):
        self.f = open(path, "rb")
        head = self.read(0, 0x3A0)
        if head[:4] not in (b"CON ", b"LIVE", b"PIRS"):
            sys.exit(f"{path} is not an Xbox 360 package (STFS)")
        self.title_id = struct.unpack_from(">I", head, 0x360)[0]
        header_size = struct.unpack_from(">I", head, 0x340)[0]
        self.data = (header_size + 0xFFF) & ~0xFFF
        volume = head[0x379:0x379 + 0x24]
        # Read-only packages keep one hash table per level, the others two.
        self.tables = 1 if volume[2] & 1 else 2
        self.table_blocks = struct.unpack_from("<H", volume, 3)[0]
        self.table_start = int.from_bytes(volume[5:8], "little")

    def read(self, offset: int, n: int) -> bytes:
        self.f.seek(offset)
        return self.f.read(n)

    def block(self, b: int) -> bytes:
        # Data blocks are numbered without the hash table blocks between them: one level 0 table every 0xAA blocks,
        # one level 1 every 0x70E4, one level 2 every 0x4AF768.
        backing = b + (b + 0xAA) // 0xAA * self.tables
        if b >= 0xAA:
            backing += (b + 0x70E4) // 0x70E4 * self.tables
            if b >= 0x70E4:
                backing += (b + 0x4AF768) // 0x4AF768 * self.tables
        return self.read(self.data + backing * self.BLOCK, self.BLOCK)

    def entries(self) -> list:
        out = []
        for k in range(self.table_blocks):
            table = self.block(self.table_start + k)
            for i in range(0, self.BLOCK, 0x40):
                e = table[i:i + 0x40]
                if not e[0]:
                    continue
                flags = e[0x28]
                out.append({"name": e[:flags & 0x3F].decode("latin-1"), "dir": bool(flags & 0x80),
                            "contig": bool(flags & 0x40), "blocks": int.from_bytes(e[0x29:0x2C], "little"),
                            "start": int.from_bytes(e[0x2F:0x32], "little"),
                            "parent": struct.unpack_from(">h", e, 0x32)[0],
                            "size": struct.unpack_from(">I", e, 0x34)[0]})
        return out


def extract_tu(path: Path, out: Path, disc: int) -> bytes:
    """The files of Title Update 6's disc<NNN>/ folder into out/ (the game reads them as update:), and its default.xexp."""
    if path.suffix.lower() == ".xexp":
        sys.exit("--tu needs the whole Title Update 6 package (its update.rpf and the other files are used too), "
                 "not only default.xexp")
    pkg = Stfs(path)
    try:
        if pkg.title_id != TITLE_ID:
            sys.exit(f"{path} is not a Max Payne 3 package (title {pkg.title_id:08X})")
        entries = pkg.entries()

        def where(e):
            parts = [e["name"]]
            while 0 <= e["parent"] < len(entries):
                e = entries[e["parent"]]
                parts.append(e["name"])
            for p in parts:
                if "/" in p or "\\" in p or p in ("", ".", ".."):
                    sys.exit(f"Unsafe name in the update: {p!r}")
            return out.joinpath(*reversed(parts))

        folder = f"disc{disc:03d}"
        xexp, written = None, 0
        for e in entries:
            if e["dir"]:
                continue
            parts = where(e).relative_to(out).parts
            if len(parts) < 2 or parts[0].casefold() != folder:
                continue
            if not e["contig"]:
                sys.exit(f"{e['name']} is fragmented in this package: not supported by this reader")
            data = b"".join(pkg.block(e["start"] + k) for k in range(e["blocks"]))[:e["size"]]
            dest = out.joinpath(*parts[1:])
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            written += 1
            if len(parts) == 2 and e["name"].casefold() == "default.xexp":
                xexp = data
        print(f"  {path.name}: {written} files of {folder}/ -> {out}")
        if xexp is None:
            sys.exit(f"{path} has no {folder}/default.xexp")
        return xexp
    finally:
        pkg.f.close()


def check(game: Path) -> None:
    """What codegen.sh needs: default.xex (and, for this port, TU6's default.xexp) in the folder."""
    xex, xexp = game / "default.xex", game / "default.xexp"
    if not xex.is_file():
        sys.exit(f"{xex} not found")
    if xex.read_bytes()[:4] != b"XEX2":
        sys.exit(f"{xex} is not an Xbox 360 executable")
    if not xexp.is_file():
        sys.exit(f"{xexp} not found: this port runs Max Payne 3 with Title Update 6 (tools/stage_sd.py --tu)")
    known = EDITIONS.get(sha256(xexp.read_bytes()))
    if not known:
        print(f"note: default.xexp {sha256(xexp.read_bytes())[:16]}... (no list of tested Title Update 6 builds)")
    elif known[1] != sha256(xex.read_bytes()):
        print(f"warning: default.xex is not the {known[0]} disc's")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", type=Path, help="the disc image (ISO); leave it out with a Games on Demand package")
    ap.add_argument("--tu", type=Path, help="Title Update 6 package (tu00000006_00000000 from 5454086B/000B0000/)")
    ap.add_argument("--disc-number", type=int, default=1, help="which disc's folder of the update (default 1)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--check", type=Path, help="check a game folder instead")
    a = ap.parse_args()
    if a.check:
        check(a.check)
        return
    if not (a.tu and a.out):
        ap.error("--tu and -o are needed (or --check)")
    for name, path in (("--disc", a.disc), ("--tu", a.tu)):
        if path and not path.is_file():
            sys.exit(f"{name}: {path} not found")

    xexp = extract_tu(a.tu, a.out / "update", a.disc_number)
    print(f"  TU default.xexp: {len(xexp)} bytes, sha256 {sha256(xexp)}")
    if a.disc:
        game = a.out / "game"
        extract(a.disc, game)
        xex = (game / "default.xex").read_bytes()
        print(f"  disc default.xex: sha256 {sha256(xex)}")
        (game / "default.xexp").write_bytes(xexp)
        check(game)
        print(f"Ready: copy {a.out} to sdmc:/switch/ (the NRO and the shader files go in it, beside game/ and update/).")
    else:
        print(f"Ready: copy {a.out} to sdmc:/switch/, and your Games on Demand package (the header file and its .data "
              f"folder, from the Xbox 360's Content/0000000000000000/5454086B/00007000/) into its god/ folder.")


if __name__ == "__main__":
    main()

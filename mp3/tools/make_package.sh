#!/bin/bash
# The SD card package for one's own use, from a finished build: mp3/out/sw/mp3.nro and the shader files, the
# settings in mp3/dist/mp3.toml, tools/stage_sd.py and the Russian install guide (mp3/dist/INSTRUKCIYA_RU.txt).
# The NRO and the shaders are made from the game: the zip stays on your computer, it is never committed or published.
#
#   mp3/tools/make_package.sh [out.zip]      (default: mp3/out/mp3-nx-switch.zip)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/mp3/out/mp3-nx-switch.zip}"
for f in mp3/out/sw/mp3.nro mp3/out/shaders/mp3_shaders.dslib mp3/out/shaders/shadercache_dksh.bin; do
  [ -f "$ROOT/$f" ] || { echo "missing $f: build first (mp3/tools/build_mac.sh)" >&2; exit 1; }
done
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
D="$TMP/switch/mp3-nx"
mkdir -p "$D/god" "$D/update" "$TMP/tools"
cp "$ROOT/mp3/out/sw/mp3.nro" "$D/mp3-nx.nro"
cp "$ROOT/mp3/out/shaders/mp3_shaders.dslib" "$ROOT/mp3/out/shaders/shadercache_dksh.bin" "$D/"
cp "$ROOT/mp3/dist/mp3.toml" "$D/"
cp "$ROOT/mp3/tools/stage_sd.py" "$TMP/tools/"
cp "$ROOT/mp3/dist/INSTRUKCIYA_RU.txt" "$TMP/"
rm -f "$OUT"
(cd "$TMP" && zip -qr -X "$OUT" INSTRUKCIYA_RU.txt switch tools)
echo "$OUT ($(du -h "$OUT" | cut -f1))"

#!/bin/bash
# The deko3d shader library and DKSH cache, from your own copy, translated ahead of time (shaders/build_library.sh) and
# compiled for the console's GPU, so the game never compiles a shader while it runs (the Vulkan build compiled them on
# the console at first start: minutes of stutter). Nothing here is committed.
#
# GTA IV keeps its shaders in RAGE effect files (.fxc, "rgxa" containers holding Xenos shader containers 0x102A11xx)
# inside the game's RPF archives. Until the RPF extractor exists (it needs the disc to be written and tested), the
# library is built from the containers the console dumps (mp3_shader_dump.cpp: shaders_dump/ beside the NRO), copied
# to mp3/out/console_dumps/ (or EXTRA=<folder>), and from any folder of .fxc/.bin files given as FXC=<folder>.
#
#   mp3/tools/shaders.sh <game folder>   -> mp3/out/shaders/mp3_shaders.dslib and shadercache_dksh.bin (beside the NRO)
#
# Needs the Vulkan SDK's dxc and spirv-val (VULKAN_BIN, see shaders/README.md).
set -euo pipefail
[ $# -eq 1 ] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/mp3/out"
mkdir -p "$OUT/shaders_extracted"
for d in "$OUT/console_dumps" ${EXTRA:+"$EXTRA"} ${FXC:+"$FXC"}; do
  for f in "$d"/*.bin; do
    [ -f "$f" ] && cp -n "$f" "$OUT/shaders_extracted/" || true
  done
done
echo "shaders: $(ls "$OUT/shaders_extracted" | grep -c '^vs_') VS, $(ls "$OUT/shaders_extracted" | grep -c '^ps_') PS"
# The variants the console compiled in the background in earlier sessions (tools/dksh_variants.py) go in the cache too.
DKSH=1 VARIANTES="$ROOT/mp3/dksh_variants.txt" "$ROOT/shaders/build_library.sh" "$OUT/shaders_extracted" "$OUT/shaders"
ls -la "$OUT/shaders/mp3_shaders.dslib" "$OUT/shaders/shadercache_dksh.bin"

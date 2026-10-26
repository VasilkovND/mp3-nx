#!/bin/bash
# Generate the recompiled C++ of Max Payne 3 from your own default.xex and Title Update 6's default.xexp, and the
# executable's memory image. Nothing here is committed: the output is mp3/out/codegen/.
#
#   mp3/tools/codegen.sh <folder with default.xex and default.xexp>     (tools/stage_sd.py's game/ folder)
#
# Steps (each skipped when already done):
#   1. ReXGlue SDK v0.10.0 for the host with our patch (host-patches/: dump the loaded image), built with the Mac's
#      clang: only its `rexglue` tool is used. It applies default.xexp to default.xex as it loads it.
#   2. The manifest: codegen/mp3_config.toml (analysis settings, native CRT functions and the function entries
#      discovery misses) and codegen/names.toml (names for the functions we hook).
#   3. rexglue codegen, then our passes over the generated code:
#      patch_vmx_exact.py     vmaddfp/vnmsubfp fused as on the console, vpkuwus with vD = vA or vB (VMX_EXACT=0 skips it)
#      patch_barriers.py      the PowerPC barriers as ARM fences, spin pauses, prefetches (BARRIERS=0 skips it)
#      patch_stack_access.py  guest stack accesses without volatile
#      patch_inline_calls.py  same-file calls GCC may inline, save/restore helpers inline
#      patch_local_regs.py    guest registers in host locals (mp3/CPU_OPTIMIZATION.md; LOCALREGS=0 skips it,
#                             PLR_CHECK=1 makes the check version)
set -euo pipefail

if [ $# -ne 1 ]; then
  sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi
GAME=$(cd "$1" && pwd)
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/mp3/out"
CG="$OUT/codegen"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
BARRIERS="${BARRIERS:-1}"
LOCALREGS="${LOCALREGS:-1}"
VMX_EXACT="${VMX_EXACT:-1}"
REXGLUE_TAG="v0.10.0"

python3 "$ROOT/mp3/tools/stage_sd.py" --check "$GAME"

# 1. Host ReXGlue
HOST="$OUT/rexglue-host"
REXGLUE="$HOST/out/$(uname -s | tr '[:upper:]' '[:lower:]' | sed 's/darwin/mac/')-$(uname -m | sed 's/x86_64/amd64/')/rexglue"
if [ ! -x "$REXGLUE" ]; then
  if [ ! -d "$HOST/.git" ]; then
    echo "== ReXGlue $REXGLUE_TAG (host)"
    git clone -q --branch "$REXGLUE_TAG" --depth 1 https://github.com/rexglue/rexglue-sdk.git "$HOST"
    git -C "$HOST" submodule update --init --recursive --depth 1 -j 8
  fi
  for p in "$ROOT"/mp3/host-patches/*.patch; do
    if git -C "$HOST" apply --reverse --check "$p" 2>/dev/null; then continue; fi
    echo "== host patch: $(basename "$p")"
    git -C "$HOST" apply "$p"
  done
  echo "== build rexglue (host)"
  mkdir -p "$OUT"
  cmake -S "$HOST" -B "$HOST/out/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXGLUE_USE_VULKAN=ON > "$OUT/rexglue-host.log"
  cmake --build "$HOST/out/host" --target rexglue -j "$JOBS" >> "$OUT/rexglue-host.log"
  if [ "$(uname -s)" = Darwin ]; then
    # Its libraries sit beside it: find them there, so the folder can move.
    for p in $(otool -l "$REXGLUE" | awk '/LC_RPATH/{getline; getline; print $2}'); do
      install_name_tool -rpath "$p" @executable_path "$REXGLUE" 2>/dev/null || true
    done
    codesign -f -s - "$REXGLUE" 2>/dev/null || true
  fi
fi
[ -x "$REXGLUE" ] || { echo "rexglue was not built (see $OUT/rexglue-host.log)"; exit 1; }

# 2. Project folder: the executable and its update (copied: codegen refuses a link out of its game root), the manifest.
mkdir -p "$CG/game"
for f in default.xex default.xexp; do
  cmp -s "$GAME/$f" "$CG/game/$f" || cp "$GAME/$f" "$CG/game/$f"
done
# CODEGEN_CONFIG=<file> stands in for codegen/mp3_config.toml: only to try the tools on another title's executable
# (Max Payne 3's addresses mean nothing there).
cp "${CODEGEN_CONFIG:-$ROOT/mp3/codegen/mp3_config.toml}" "$CG/mp3_config.toml"
cp "$ROOT/mp3/codegen/names.toml" "$CG/"
cat > "$CG/mp3_manifest.toml.new" <<'TOML'
# Written by mp3/tools/codegen.sh.
[project]
name = "mp3"
sdk_version = "0.10.0"
game_root = "game"

[entrypoint]
file_path = "game/default.xex"
out_directory_path = "generated/default"
includes = ["mp3_config.toml", "names.toml"]
TOML
cmp -s "$CG/mp3_manifest.toml.new" "$CG/mp3_manifest.toml" || mv "$CG/mp3_manifest.toml.new" "$CG/mp3_manifest.toml"
rm -f "$CG/mp3_manifest.toml.new"
# A name the SDK already defines as __imp__<name> (its export stubs) would replace the game's function with that stub.
for n in $(sed -n 's/.*name = "\([A-Za-z_0-9]*\)".*/\1/p' "$ROOT/mp3/codegen/mp3_config.toml" "$ROOT/mp3/codegen/names.toml"); do
  if grep -rqE "__imp__$n\b" "$ROOT/sdk/src" "$ROOT/sdk/include"; then
    echo "codegen: the SDK already defines __imp__$n; rename it in mp3/codegen/"; exit 1
  fi
done

# 3. Codegen (re-run only when an input changed) and our passes.
STAMP="$CG/inputs.sha"
INPUTS=$( (cat "$CG/game/default.xex" "$CG/game/default.xexp" "$CG"/*.toml "$REXGLUE" \
  "$ROOT"/mp3/tools/patch_vmx_exact.py \
  "$ROOT"/mp3/tools/patch_barriers.py "$ROOT"/mp3/tools/patch_stack_access.py \
  "$ROOT"/mp3/tools/patch_inline_calls.py "$ROOT"/mp3/tools/patch_local_regs.py
  echo "barriers=$BARRIERS localregs=$LOCALREGS check=${PLR_CHECK:-0} vmx_exact=$VMX_EXACT"
  # The functions the sources name (patch_inline_calls.py and patch_local_regs.py keep their calls): a new hook
  # regenerates the code.
  grep -rhoE "\b(__imp__)?(sub_[0-9A-Fa-f]{8}|mp3_[A-Za-z0-9_]+|rexcrt_[A-Za-z0-9_]+)\b" \
    "$ROOT/mp3/src" "$ROOT/sdk/src" "$ROOT/sdk/include" | sort -u) \
  | shasum -a 256 | cut -d' ' -f1)
if [ "$(cat "$STAMP" 2>/dev/null)" != "$INPUTS" ] || [ ! -f "$CG/generated/default/sources.cmake" ]; then
  # Generated and patched in a staging folder, then only the files whose text changed are copied over generated/:
  # the others keep their dates, so after a small change Ninja rebuilds a few units.
  echo "== codegen"
  ST="$CG/staging"
  rm -rf "$ST"
  mkdir -p "$ST"
  cp "$CG"/*.toml "$ST/"
  ln -s ../game "$ST/game"
  (cd "$ST" && REX_DUMP_IMAGE="$CG/image.bin" "$REXGLUE" --force codegen mp3_manifest.toml > "$CG/codegen.log" 2>&1) || {
    tail -20 "$CG/codegen.log"; exit 1; }
  GEN="generated/default"
  if [ "$VMX_EXACT" = 1 ]; then
    (cd "$ST" && python3 "$ROOT/mp3/tools/patch_vmx_exact.py" $GEN/mp3_recomp.*.cpp) > "$CG/patch_vmx_exact.log"
    tail -1 "$CG/patch_vmx_exact.log"
  fi
  if [ "$BARRIERS" = 1 ]; then
    (cd "$ST" && python3 "$ROOT/mp3/tools/patch_barriers.py" $GEN/mp3_recomp.*.cpp) > "$CG/patch_barriers.log"
    tail -1 "$CG/patch_barriers.log"
  fi
  (cd "$ST" && python3 "$ROOT/mp3/tools/patch_stack_access.py" $GEN/*.cpp) > "$CG/patch_stack_access.log"
  tail -1 "$CG/patch_stack_access.log"
  (cd "$ST" && python3 "$ROOT/mp3/tools/patch_inline_calls.py" "$ROOT/mp3/src" "$ROOT/sdk/src" "$ROOT/sdk/include" \
    -- $GEN/mp3_recomp.*.cpp) > "$CG/patch_inline_calls.log"
  tail -1 "$CG/patch_inline_calls.log" | cut -c1-160
  if [ "$LOCALREGS" = 1 ]; then
    (cd "$ST" && PLR_MAP="$CG/patch_local_regs.check.map" python3 "$ROOT/mp3/tools/patch_local_regs.py" \
      $GEN/mp3_inline_helpers.h "$ROOT/mp3/src" "$ROOT/sdk/src" "$ROOT/sdk/include" -- $GEN/mp3_recomp.*.cpp) \
      > "$CG/patch_local_regs.log" 2>&1 || { tail -20 "$CG/patch_local_regs.log"; exit 1; }
    tail -1 "$CG/patch_local_regs.log"
  fi
  python3 - "$ST/$GEN" "$CG/$GEN" <<'PY'
import filecmp, os, shutil, sys
new, old = sys.argv[1:]
os.makedirs(old, exist_ok=True)
names = set(os.listdir(new))
changed = 0
for n in sorted(names):
    a, b = os.path.join(new, n), os.path.join(old, n)
    if not os.path.isfile(b) or not filecmp.cmp(a, b, shallow=False):
        shutil.copy2(a, b)
        changed += 1
for n in set(os.listdir(old)) - names:
    os.remove(os.path.join(old, n))
print(f"generated/default: {changed} of {len(names)} files changed")
PY
  rm -rf "$ST"
  echo "$INPUTS" > "$STAMP"
fi
echo "codegen: $(ls "$CG"/generated/default/*.cpp | wc -l | tr -d ' ') files in $CG/generated/default, image $CG/image.bin"

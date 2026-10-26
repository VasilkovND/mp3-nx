// mp3-nx - GTA IV's scene without predicated tiling.
//
// The Xbox 360 renders a 1280x720 frame with several targets in tiles, because its EDRAM holds 10 MB: Direct3D's
// BeginTiling (sub_82A3E910, mp3_D3DDevice_BeginTiling in codegen/names.toml) takes the tile rectangles, and the
// GPU replays the frame's commands once per tile under a bin predicate. The deko3d renderer keeps each EDRAM target
// as a whole image and has no 10 MB to fit; with GTA IV's three tiles only the first band of the G-buffer was ever
// drawn and the world came out black. Here the game asks for one tile, the union of its rectangles (as
// gears-of-war-2-pc-port turns tiling off for Gears of War 2: GUseTilingMode = FALSE), and Direct3D does the rest:
// it records the count in the device, so EndTiling resolves that one tile.

#include "mp3_pch.h"

#include <algorithm>
#include <string>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mp3_sin_tiling, true, "MP3",
                    "La escena en un solo tile (la union de los del juego): sin tiling predicado. false = como en la "
                    "Xbox 360 (el renderizador solo dibuja la primera banda)");

#ifdef MP3_TIENE_mp3_D3DDevice_BeginTiling  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_D3DDevice_BeginTiling);
REX_EXTERN(mp3_D3DDevice_BeginTiling) {
  const uint32_t count = ctx.r5.u32;
  const uint32_t rects = ctx.r6.u32;
  if (REXCVAR_GET(mp3_sin_tiling) && count > 1 && count <= 16 && rects) {
    auto rd = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
    auto wr = [&](uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v); };
    uint32_t x1 = rd(rects), y1 = rd(rects + 4), x2 = rd(rects + 8), y2 = rd(rects + 12);
    for (uint32_t i = 1; i < count; ++i) {
      const uint32_t r = rects + i * 16;
      x1 = std::min(x1, rd(r));
      y1 = std::min(y1, rd(r + 4));
      x2 = std::max(x2, rd(r + 8));
      y2 = std::max(y2, rd(r + 12));
    }
    // Each new layout once (the cutscenes may tile otherwise than the city).
    static std::unordered_set<uint64_t> vistos;
    const uint64_t forma = (uint64_t(count) << 48) | (uint64_t(rd(rects + 12)) << 32) | (uint64_t(x2) << 16) | y2;
    if (vistos.size() < 32 && vistos.insert(forma).second) {
      std::string t;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t r = rects + i * 16;
        t += fmt::format(" ({},{})-({},{})", rd(r), rd(r + 4), rd(r + 8), rd(r + 12));
      }
      REXLOG_INFO("[gpu] BeginTiling: {} tiles{} -> one, ({},{})-({},{})", count, t, x1, y1, x2, y2);
    }
    wr(rects, x1);
    wr(rects + 4, y1);
    wr(rects + 8, x2);
    wr(rects + 12, y2);
    ctx.r5.u64 = 1;
  }
  __imp__mp3_D3DDevice_BeginTiling(ctx, base);
}
#endif  // MP3_TIENE_mp3_D3DDevice_BeginTiling

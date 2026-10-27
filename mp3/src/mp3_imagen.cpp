// mp3-nx - the picture's final adjustment: brightness, contrast and gamma applied in the renderer's output pass, after
// the game's own gamma ramp (sdk/src/x360/deko/destinos.cpp, RecalcularRampa). Max Payne 3's frame came out a little
// too bright once its exposure was right; mp3_gamma darkens the mid-tones, the user can change all three in mp3.toml.

#include "mp3_pch.h"

#include "mp3_gpu.h"

#include <rex/cvar.h>
#include <rex/x360/render_hooks.h>

#include <cstring>

REXCVAR_DEFINE_DOUBLE(mp3_brillo, 0.0, "MP3",
                      "Brillo de la imagen final: se suma a cada canal (-0.2 a 0.2; 0 = el del juego)");
REXCVAR_DEFINE_DOUBLE(mp3_contraste, 1.0, "MP3", "Contraste de la imagen final (0.8 a 1.3; 1 = el del juego)");
REXCVAR_DEFINE_DOUBLE(mp3_gamma, 0.9, "MP3",
                      "Gamma de la imagen final: menos de 1 oscurece los tonos medios (0.7 a 1.3; 1 = el del juego)");

namespace mp3::gpu {
namespace {

class Ganchos final : public rex::x360::RenderHooks {
 public:
  rex::x360::PostProcess Posproceso() override {
    rex::x360::PostProcess p;
    p.brillo = float(REXCVAR_GET(mp3_brillo));
    p.contraste = float(REXCVAR_GET(mp3_contraste));
    p.gamma = float(REXCVAR_GET(mp3_gamma));
    p.activo = p.brillo != 0.0f || p.contraste != 1.0f || p.gamma != 1.0f;
    return p;
  }

  // Changes with the three settings, so the output table is computed again when one of them does.
  uint64_t VersionPosproceso() override {
    const float v[3] = {float(REXCVAR_GET(mp3_brillo)), float(REXCVAR_GET(mp3_contraste)),
                        float(REXCVAR_GET(mp3_gamma))};
    uint32_t b[3];
    std::memcpy(b, v, sizeof(v));
    return (uint64_t(b[0]) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(b[1]) << 21) ^ b[2];
  }
};

}  // namespace

rex::x360::RenderHooks& GanchosImagen() {
  static Ganchos ganchos;
  return ganchos;
}

}  // namespace mp3::gpu

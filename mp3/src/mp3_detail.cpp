// mp3-nx - the game's own detail settings, lowered for stock clocks (runs 29-31: 6-14 fps in heavy scenes, the four
// cores full and the GPU at 60-110 ms a frame).
//
// RAGE's command-line parameters (PARAMs) are 0x14-byte objects: +0 the name, +4 the value text (null when not given),
// registered by sub_82CB10C8 from each one's static constructor, which zeroes +4. A flag parameter is on when +4 is not
// null; a number is read from the text with strtol (sub_82CB1710). Max Payne 3 on the 360 still reads some of them:
//   removetopmips      0x8384FE38  sub_82E0B320, as textures load: their top mip level is dropped (bandwidth, memory)
//   cheapaudioeffects  0x8373CEA0  sub_82C54DE8, the audio engine's constructor (byte +591 of the engine): cheaper
//                                  effects in the mixer
//   noptfx             0x834A2210  tested by about 38 particle (RMPTFX) functions every frame: no particles
//   temporalrender     0x8352FF38  sub_826741C8 (to measure)
// The first two are read once, at start: they are set right after their registration (mp3_ParamRegister). The last two
// are tested while the game runs and follow their settings at every Swap, so an A/B run (x360_ab_estados) can switch
// them. The LOD distance of the high-detail ped models (ped.lod.distance.high at 0x83574A7C, pedincar at 0x831763F0,
// floats from visualsettings.dat, read by sub_8288CF70) is scaled the same way.

#include "mp3_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(mp3_quitar_mips, false, "MP3",
                    "Parametro removetopmips del juego: cada textura pierde su nivel de mip mas grande al cargarse (menos "
                    "ancho de banda de la GPU; texturas algo mas borrosas). Al arrancar");
REXCVAR_DEFINE_BOOL(mp3_audio_efectos_baratos, false, "MP3",
                    "Parametro cheapaudioeffects del juego: efectos de audio mas baratos en el mezclador. Al arrancar");
REXCVAR_DEFINE_BOOL(mp3_sin_particulas, false, "MP3",
                    "Parametro noptfx del juego: sin particulas (humo, chispas, sangre, cristales). Cambia en marcha");
REXCVAR_DEFINE_BOOL(mp3_render_temporal, false, "MP3",
                    "Parametro temporalrender del juego (efecto por medir). Cambia en marcha");
REXCVAR_DEFINE_DOUBLE(mp3_lod_peds, 1.0, "MP3",
                      "Escala de la distancia del modelo detallado de los personajes (ped.lod.distance.high y "
                      "pedincar.lod.distance.high). 1 = la del juego; menos = pasan antes al modelo sencillo")
    .range(0.1, 1.0);

namespace {

constexpr uint32_t kQuitarMips = 0x8384FE38;
constexpr uint32_t kAudioBarato = 0x8373CEA0;
constexpr uint32_t kSinParticulas = 0x834A2210;
constexpr uint32_t kRenderTemporal = 0x8352FF38;
// "1" in the image: the value text a flag gets.
constexpr uint32_t kTextoUno = 0x82002904;

constexpr uint32_t kLodPeds = 0x83574A7C;
constexpr uint32_t kLodPedsEnCoche = 0x831763F0;

uint32_t Leer(uint8_t* base, uint32_t direccion) {
  uint32_t v;
  std::memcpy(&v, base + direccion, 4);
  return __builtin_bswap32(v);
}
void Escribir(uint8_t* base, uint32_t direccion, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + direccion, &v, 4);
}
float LeerF(uint8_t* base, uint32_t direccion) {
  const uint32_t u = Leer(base, direccion);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void EscribirF(uint8_t* base, uint32_t direccion, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  Escribir(base, direccion, u);
}

// A runtime flag to its setting; logs a change.
void Bandera(uint8_t* base, uint32_t objeto, bool encendida, const char* nombre) {
  const uint32_t antes = Leer(base, objeto + 4);
  if ((antes != 0) == encendida) return;
  Escribir(base, objeto + 4, encendida ? kTextoUno : 0);
  REXLOG_INFO("[detalle] {} {}", nombre, encendida ? "ENCENDIDO" : "apagado");
}

// One LOD distance: the game's value (taken again whenever the game writes another one) times the scale.
struct Lod {
  uint32_t direccion;
  float del_juego = 0.0f;
  float escrita = -1.0f;
};
Lod g_lods[2] = {{kLodPeds}, {kLodPedsEnCoche}};
double g_escala_aplicada = 1.0;

}  // namespace

#ifdef MP3_TIENE_mp3_ParamRegister  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_ParamRegister);
REX_EXTERN(mp3_ParamRegister) {
  const uint32_t objeto = ctx.r3.u32;
  __imp__mp3_ParamRegister(ctx, base);
  const bool mips = objeto == kQuitarMips && REXCVAR_GET(mp3_quitar_mips);
  const bool audio = objeto == kAudioBarato && REXCVAR_GET(mp3_audio_efectos_baratos);
  if (mips || audio) {
    Escribir(base, objeto + 4, kTextoUno);
    REXLOG_INFO("[detalle] {} ENCENDIDO al arrancar", mips ? "removetopmips" : "cheapaudioeffects");
  }
}
#endif  // MP3_TIENE_mp3_ParamRegister

// Called by the ring thread at each Swap (mp3_gpu.cpp): the settings that change while the game runs.
void Mp3AplicarDetalle(uint8_t* base) {
  if (!base) return;
  Bandera(base, kSinParticulas, REXCVAR_GET(mp3_sin_particulas), "noptfx");
  Bandera(base, kRenderTemporal, REXCVAR_GET(mp3_render_temporal), "temporalrender");
  const double escala = REXCVAR_GET(mp3_lod_peds);
  for (Lod& l : g_lods) {
    const float actual = LeerF(base, l.direccion);
    if (actual <= 0.0f) continue;  // not loaded yet
    if (actual != l.escrita) l.del_juego = actual;  // the game's own (first time, or the game wrote it again)
    const float deseada = float(l.del_juego * escala);
    if (actual != deseada) {
      EscribirF(base, l.direccion, deseada);
    }
    l.escrita = deseada;
  }
  if (escala != g_escala_aplicada) {
    g_escala_aplicada = escala;
    REXLOG_INFO("[detalle] distancia LOD de los personajes x{:.2f}: {:.1f} y {:.1f}", escala, g_lods[0].escrita,
                g_lods[1].escrita);
  }
}

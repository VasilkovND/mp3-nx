// mp3-nx - the frame's resolution: 1280x720, 960x540 or 768x432 (the same 16:9 shape; the presenter stretches it to the
// screen). 960x540 is for stock clocks: the GPU draws about 56 % of the pixels; 768x432 (run 38: the GPU still at
// 32-43 ms a frame at stock) 36 %. Max Payne 3 keeps the size it
// wants in two words at 0x83139EB0/B4 (1280, 720 in the image); sub_822BB928 compares them with the device's size
// (0x831D0954/58, taken from XGetVideoMode by sub_82DEEA18) and, when they differ, resets the device and resizes every
// render target to them. Run 19 changed only the video mode: the game put everything back to 1280x720 and the frame
// was presented cropped. So 540p writes 960x540 into both, before the game runs (Aplicar and AplicarMemoria, from
// mp3_app.h); a change takes effect at the next start.
//
// The choice is in mp3_resolucion.txt beside the NRO ("720" or "540"), written by the options menu (mp3_menu.cpp,
// ZL + ZR + Minus). Without it: 720 (the CPU clock read at start is not the one sys-clk sets a moment later, so it
// cannot tell an overclocked console).

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace mp3::resolucion {

inline std::filesystem::path Fichero() { return rex::filesystem::GetExecutableFolder() / "mp3_resolucion.txt"; }

// The sizes of each choice (by its height).
inline uint32_t Ancho(uint32_t alto) { return alto == 432 ? 768 : alto == 540 ? 960 : 1280; }
inline uint32_t Alto(uint32_t alto) { return alto == 432 || alto == 540 ? alto : 720; }
inline bool Valida(uint32_t v) { return v == 720 || v == 540 || v == 432; }

// The saved choice (720, 540 or 432), 0 if there is none.
inline uint32_t Guardada() {
  FILE* f = std::fopen(Fichero().string().c_str(), "r");
  if (!f) return 0;
  unsigned valor = 0;
  const bool leido = std::fscanf(f, "%u", &valor) == 1;
  std::fclose(f);
  return leido && Valida(valor) ? valor : 0;
}

inline bool Guardar(uint32_t valor) {
  FILE* f = std::fopen(Fichero().string().c_str(), "w");
  if (!f) return false;
  const bool escrito = std::fprintf(f, "%u\n", unsigned(valor)) > 0;
  return std::fclose(f) == 0 && escrito;
}

// The CPU clock in MHz (0 if it cannot be read).
inline uint32_t CpuMhz() {
#if defined(__SWITCH__)
  uint32_t hz = 0;
  if (R_SUCCEEDED(clkrstInitialize())) {
    ClkrstSession sesion;
    if (R_SUCCEEDED(clkrstOpenSession(&sesion, PcvModuleId_CpuBus, 3))) {
      clkrstGetClockRate(&sesion, &hz);
      clkrstCloseSession(&sesion);
    }
    clkrstExit();
  }
  return hz / 1000000;
#else
  return 0;
#endif
}

inline uint32_t PorDefecto() { return 720; }

inline uint32_t Elegida() {
  const uint32_t guardada = Guardada();
  return guardada ? guardada : PorDefecto();
}

// The size the game was started with (Aplicar), for the menu.
inline uint32_t& Aplicada() {
  static uint32_t valor = 720;
  return valor;
}

// The chosen size into the video mode the game reads; call after the configuration is loaded, before the game runs.
inline void Aplicar() {
  const uint32_t guardada = Guardada();
  const uint32_t elegida = guardada ? guardada : PorDefecto();
  Aplicada() = elegida;
  const bool reducida = elegida != 720;
  rex::cvar::SetFlagByName("video_mode_width", std::to_string(Ancho(elegida)));
  rex::cvar::SetFlagByName("video_mode_height", std::to_string(Alto(elegida)));
  // The SDK reports a mode below 1280x720 as standard definition, and the game then lays its interface out for SD
  // (bigger, with wide safe margins): a reduced size is still told high definition (SDK patch 0202).
  rex::cvar::SetFlagByName("video_mode_alta_definicion", reducida ? "true" : "false");
  REXLOG_INFO("mp3: fotograma {}x{} ({}p, {}; CPU {} MHz)", Ancho(elegida), Alto(elegida), elegida,
              guardada ? "guardada" : "por defecto", CpuMhz());
}

// Where the game keeps the size it wants (big-endian words), read by sub_822BB928.
constexpr uint32_t kAnchoDeseado = 0x83139EB0;
constexpr uint32_t kAltoDeseado = 0x83139EB4;
// The device's size (grcDevice): 1 and 1 in the image, which sub_82DEEA18 replaces with XGetVideoMode's size, or keeps
// when the -width / -height parameters set them. Run 20 at 540p still had a 1280x720 device (its copy for other threads
// at 0x8381C810 / 0x8381C800): 960x540 is written here before the game runs, as those parameters would.
constexpr uint32_t kAnchoDispositivo = 0x831D0954;
constexpr uint32_t kAltoDispositivo = 0x831D0958;

// The same size into the game's wanted size; call once the executable is loaded, before it runs.
inline void AplicarMemoria(rex::Runtime& runtime) {
  if (Aplicada() == 720 || !runtime.memory()) return;
  const uint32_t w = Ancho(Aplicada()), h = Alto(Aplicada());
  uint8_t* ancho = runtime.memory()->TranslateVirtual<uint8_t*>(kAnchoDeseado);
  uint8_t* alto = runtime.memory()->TranslateVirtual<uint8_t*>(kAltoDeseado);
  const uint32_t antes_ancho = __builtin_bswap32(*reinterpret_cast<uint32_t*>(ancho));
  const uint32_t antes_alto = __builtin_bswap32(*reinterpret_cast<uint32_t*>(alto));
  if (antes_ancho != 1280 || antes_alto != 720) {
    REXLOG_WARN("mp3: el tamano deseado no es 1280x720 ({}x{}): {}p sin aplicar", antes_ancho, antes_alto, h);
    return;
  }
  *reinterpret_cast<uint32_t*>(ancho) = __builtin_bswap32(w);
  *reinterpret_cast<uint32_t*>(alto) = __builtin_bswap32(h);
  uint32_t* disp_ancho = runtime.memory()->TranslateVirtual<uint32_t*>(kAnchoDispositivo);
  uint32_t* disp_alto = runtime.memory()->TranslateVirtual<uint32_t*>(kAltoDispositivo);
  const uint32_t antes_disp_ancho = __builtin_bswap32(*disp_ancho), antes_disp_alto = __builtin_bswap32(*disp_alto);
  if (antes_disp_ancho == 1 && antes_disp_alto == 1) {
    *disp_ancho = __builtin_bswap32(w);
    *disp_alto = __builtin_bswap32(h);
  }
  REXLOG_INFO("mp3: tamano deseado del juego {}x{} (era {}x{}), dispositivo {} (era {}x{})", w, h, antes_ancho,
              antes_alto, antes_disp_ancho == 1 && antes_disp_alto == 1 ? "cambiado" : "sin cambiar", antes_disp_ancho,
              antes_disp_alto);
}

// Diagnostic, called at each Swap: the sizes the game holds, logged after 300 Swaps and again after 3000 (the device's at
// 0x831D0954/58, the copy other threads read at 0x8381C810/0x8381C800, the wanted size at 0x83139EB0/B4).
inline void InformarTamanos(rex::memory::Memory* memoria) {
  static uint32_t swaps = 0;
  ++swaps;
  if (!memoria || (swaps != 300 && swaps != 3000)) return;
  auto leer = [&](uint32_t direccion) {
    return __builtin_bswap32(*memoria->TranslateVirtual<const uint32_t*>(direccion));
  };
  REXLOG_INFO("mp3: tamanos tras {} Swaps: dispositivo {}x{}, copia {}x{}, deseado {}x{}", swaps, leer(kAnchoDispositivo),
              leer(kAltoDispositivo), leer(0x8381C810), leer(0x8381C800), leer(kAnchoDeseado), leer(kAltoDeseado));
}

}  // namespace mp3::resolucion

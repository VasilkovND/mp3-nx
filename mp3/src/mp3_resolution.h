// mp3-nx - the frame's resolution: 1280x720 or 960x540 (three quarters, the same 16:9 shape; the presenter stretches it
// to the screen). 960x540 is for stock clocks: the GPU draws about 56 % of the pixels. Max Payne 3 sizes its device
// from XGetVideoMode (sub_82DEEA18, grcDevice's setup, once at start), so the choice goes into video_mode_width/height
// before the game runs (Aplicar, from mp3_app.h) and a change takes effect at the next start.
//
// The choice is in mp3_resolucion.txt beside the NRO ("720" or "540"), written by the options menu (mp3_menu.cpp,
// ZL + ZR + Minus). Without it: 720 when the CPU runs at 1.5 GHz or more (an overclocked console), else 540. As in
// ds3-nx.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace mp3::resolucion {

inline std::filesystem::path Fichero() { return rex::filesystem::GetExecutableFolder() / "mp3_resolucion.txt"; }

// The saved choice (720 or 540), 0 if there is none.
inline uint32_t Guardada() {
  FILE* f = std::fopen(Fichero().string().c_str(), "r");
  if (!f) return 0;
  unsigned valor = 0;
  const bool leido = std::fscanf(f, "%u", &valor) == 1;
  std::fclose(f);
  return leido && (valor == 720 || valor == 540) ? valor : 0;
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

inline uint32_t PorDefecto() {
  const uint32_t mhz = CpuMhz();
  return mhz == 0 || mhz >= 1500 ? 720 : 540;
}

inline uint32_t Elegida() {
  const uint32_t guardada = Guardada();
  return guardada ? guardada : PorDefecto();
}

// The chosen size into the video mode the game reads; call after the configuration is loaded, before the game runs.
inline void Aplicar() {
  const uint32_t guardada = Guardada();
  const uint32_t elegida = guardada ? guardada : PorDefecto();
  const bool p540 = elegida == 540;
  rex::cvar::SetFlagByName("video_mode_width", p540 ? "960" : "1280");
  rex::cvar::SetFlagByName("video_mode_height", p540 ? "540" : "720");
  REXLOG_INFO("mp3: fotograma {} ({}p, {}; CPU {} MHz)", p540 ? "960x540" : "1280x720", elegida,
              guardada ? "guardada" : "por defecto", CpuMhz());
}

}  // namespace mp3::resolucion

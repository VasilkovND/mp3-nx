// mp3-nx - the port's options menu: ZL + ZR + Minus opens it over the game (the presenter paints it, SDK patch 0088:
// no ImGui, which would move presentation to the UI thread), and while it is open the game sees the controller idle.
// Left / Right change the frame's resolution (mp3_resolution.h: 720p for overclocked consoles, 540p for stock clocks),
// A saves it in mp3_resolucion.txt beside the NRO, B closes. The game sizes its frame once at start, so the choice
// applies at the next start. As in ds3-nx.

#include "mp3_menu.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/filtro_mando.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/superposicion.h>

#include "mp3_resolution.h"

REXCVAR_DECLARE(double, mp3_exposicion);  // defined in mp3_gpu.cpp: the tonemap's Exposures scale

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace mp3::menu {
namespace {

#if defined(__SWITCH__)
constexpr uint64_t kCombinacion = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus;
constexpr uint64_t kIzquierda = HidNpadButton_Left | HidNpadButton_StickLLeft;
constexpr uint64_t kDerecha = HidNpadButton_Right | HidNpadButton_StickLRight;
constexpr uint64_t kArriba = HidNpadButton_Up | HidNpadButton_StickLUp;
constexpr uint64_t kAbajo = HidNpadButton_Down | HidNpadButton_StickLDown;
constexpr uint64_t kAceptar = HidNpadButton_A;
constexpr uint64_t kCerrar = HidNpadButton_B;
#else
constexpr uint64_t kCombinacion = 0, kIzquierda = 0, kDerecha = 0, kArriba = 0, kAbajo = 0, kAceptar = 0, kCerrar = 0;
#endif

struct Estado {
  std::mutex candado;
  bool abierto = false;
  uint32_t resolucion = 720;        // the value shown (720 or 540)
  double exposicion = 1.0;          // the exposure shown (x2 steps; 1/512 to 8)
  bool fila_exposicion = false;     // the selected row: false resolution, true exposure
  std::string aviso;                // the last line: what happened
  std::array<uint64_t, 8> antes{};  // buttons held at each slot's previous poll
};
// The exposure picked in the menu and saved beside the NRO (mp3_exposicion.txt), applied at start over mp3.toml's.
double ExposicionGuardada() {
  FILE* f = std::fopen((rex::filesystem::GetExecutableFolder() / "mp3_exposicion.txt").string().c_str(), "r");
  if (!f) return 0.0;
  double v = 0.0;
  const bool leido = std::fscanf(f, "%lf", &v) == 1;
  std::fclose(f);
  return leido && v >= 1.0 / 512.0 && v <= 8.0 ? v : 0.0;
}
bool GuardarExposicion(double v) {
  FILE* f = std::fopen((rex::filesystem::GetExecutableFolder() / "mp3_exposicion.txt").string().c_str(), "w");
  if (!f) return false;
  const bool escrito = std::fprintf(f, "%g\n", v) > 0;
  return std::fclose(f) == 0 && escrito;
}

Estado& ElEstado() {
  static Estado e;
  return e;
}

void Pintar(const Estado& e) {
  const uint32_t guardada = resolucion::Guardada();
  const std::string ahora =
      std::to_string(resolucion::Ancho(resolucion::Aplicada())) + "x" + std::to_string(resolucion::Alto(resolucion::Aplicada()));
  const char* opcion = e.resolucion == 720 ? "720P  1280x720  (OVERCLOCK)" : "540P   960x540  (STOCK CLOCKS)";
  char expo[32];
  std::snprintf(expo, sizeof expo, "%g", e.exposicion);
  std::vector<std::string> lineas = {
      "MAX PAYNE 3 - OPTIONS",
      std::string(e.fila_exposicion ? "  RESOLUTION:  " : "> RESOLUTION:  < ") +
          opcion + (e.fila_exposicion ? "" : " >"),
      std::string("    NOW ") + ahora + ", SAVED " +
          (guardada ? std::to_string(guardada) + "P" : std::string("AUTO")) + " (APPLIES AT THE NEXT START)",
      std::string(e.fila_exposicion ? "> EXPOSURE:    < " : "  EXPOSURE:    ") + "x" + expo +
          (e.fila_exposicion ? " >" : "") + "  (BRIGHTNESS; LEFT/RIGHT X2, A SAVES)",
      "",
      "LEFT/RIGHT: CHANGE   A: SAVE   B: CLOSE",
      e.aviso,
  };
  rex::ui::FijarSuperposicion(std::move(lineas));
}

bool Filtro(size_t ranura, uint64_t botones) {
  Estado& e = ElEstado();
  std::lock_guard<std::mutex> cerrojo(e.candado);
  const size_t s = ranura < e.antes.size() ? ranura : e.antes.size() - 1;
  const uint64_t nuevos = botones & ~e.antes[s];
  e.antes[s] = botones;
  if (!e.abierto) {
    if ((botones & kCombinacion) != kCombinacion || !(nuevos & kCombinacion)) return false;
    e.abierto = true;
    e.resolucion = resolucion::Elegida();
    e.fila_exposicion = false;
    e.exposicion = REXCVAR_GET(mp3_exposicion);
    e.aviso.clear();
    REXLOG_INFO("mp3: menu de opciones abierto");
    Pintar(e);
    return true;
  }
  if (nuevos & kCerrar) {
    e.abierto = false;
    rex::ui::FijarSuperposicion({});
    REXLOG_INFO("mp3: menu de opciones cerrado");
    return true;
  }
  if (nuevos & (kArriba | kAbajo)) {
    e.fila_exposicion = !e.fila_exposicion;
    e.aviso.clear();
    Pintar(e);
  } else if (nuevos & (kIzquierda | kDerecha)) {
    const bool derecha = (nuevos & kDerecha) != 0;
    if (e.fila_exposicion) {
      // The brightness row: x2 up, /2 down, 1/512 to 8; applied at once, so the change is visible behind the menu.
      e.exposicion = std::min(8.0, std::max(1.0 / 512.0, e.exposicion * (derecha ? 2.0 : 0.5)));
      REXCVAR_SET(mp3_exposicion, e.exposicion);
      e.aviso.clear();
      REXLOG_INFO("mp3: exposicion {}", e.exposicion);
    } else {
      e.resolucion = e.resolucion == 720 ? 540 : 720;
      e.aviso.clear();
    }
    Pintar(e);
  } else if (nuevos & kAceptar) {
    if (e.fila_exposicion) {
      const bool guardado = GuardarExposicion(e.exposicion);
      e.aviso = guardado ? "SAVED. USED AT EVERY START" : "COULD NOT SAVE (SD CARD?)";
      REXLOG_INFO("mp3: exposicion {} {}", e.exposicion, guardado ? "guardada" : "NO guardada");
    } else {
      const bool guardado = resolucion::Guardar(e.resolucion);
      e.aviso = guardado ? "SAVED. RESTART THE GAME TO APPLY IT" : "COULD NOT SAVE (SD CARD?)";
      REXLOG_INFO("mp3: resolucion {}p {}", e.resolucion, guardado ? "guardada" : "NO guardada");
    }
    Pintar(e);
  }
  return true;  // the game does not see the controller while the menu is open
}

}  // namespace

void Instalar(rex::Runtime& runtime) {
  (void)runtime;
  if (const double guardada = ExposicionGuardada()) {
    REXCVAR_SET(mp3_exposicion, guardada);
    REXLOG_INFO("mp3: exposicion guardada {}", guardada);
  }
  rex::input::FijarFiltroMando(&Filtro);
  REXLOG_INFO("mp3: menu de opciones con ZL + ZR + Menos (resolucion guardada: {}, por defecto {}p)",
              resolucion::Guardada(), resolucion::PorDefecto());
}

}  // namespace mp3::menu

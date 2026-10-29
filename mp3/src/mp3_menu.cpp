// mp3-nx - the port's options menu: ZL + ZR + Minus opens it over the game (the presenter paints it, SDK patch 0088:
// no ImGui, which would move presentation to the UI thread), and while it is open the game sees the controller idle.
// Left / Right change the frame's resolution (mp3_resolution.h: 720p for overclocked consoles, 540p for stock clocks),
// A saves it in mp3_resolucion.txt beside the NRO, B closes. The game sizes its frame once at start, so the choice
// applies at the next start. As in ds3-nx.

#include "mp3_menu.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/filtro_mando.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/superposicion.h>

#include "mp3_resolution.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace mp3::menu {
namespace {

#if defined(__SWITCH__)
constexpr uint64_t kCombinacion = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus;
constexpr uint64_t kIzquierda = HidNpadButton_Left | HidNpadButton_StickLLeft;
constexpr uint64_t kDerecha = HidNpadButton_Right | HidNpadButton_StickLRight;
constexpr uint64_t kAceptar = HidNpadButton_A;
constexpr uint64_t kCerrar = HidNpadButton_B;
#else
constexpr uint64_t kCombinacion = 0, kIzquierda = 0, kDerecha = 0, kAceptar = 0, kCerrar = 0;
#endif

struct Estado {
  std::mutex candado;
  bool abierto = false;
  uint32_t resolucion = 720;        // the value shown (720 or 540)
  std::string aviso;                // the last line: what happened
  std::array<uint64_t, 8> antes{};  // buttons held at each slot's previous poll
};
Estado& ElEstado() {
  static Estado e;
  return e;
}

void Pintar(const Estado& e) {
  const uint32_t guardada = resolucion::Guardada();
  const std::string ahora =
      std::to_string(resolucion::Ancho(resolucion::Aplicada())) + "x" + std::to_string(resolucion::Alto(resolucion::Aplicada()));
  const char* opcion = e.resolucion == 720 ? "720P  1280x720  (OVERCLOCK)" : "540P   960x540  (STOCK CLOCKS)";
  std::vector<std::string> lineas = {
      "MAX PAYNE 3 - OPTIONS",
      std::string("> RESOLUTION:  < ") +
          opcion + " >",
      std::string("    NOW ") + ahora + ", SAVED " +
          (guardada ? std::to_string(guardada) + "P" : std::string("AUTO")) + " (APPLIES AT THE NEXT START)",
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
  if (nuevos & (kIzquierda | kDerecha)) {
    // Left or right switches between 720 and 540.
    e.resolucion = e.resolucion == 720 ? 540 : 720;
    e.aviso.clear();
    Pintar(e);
  } else if (nuevos & kAceptar) {
    const bool guardado = resolucion::Guardar(e.resolucion);
    e.aviso = guardado ? "SAVED. RESTART THE GAME TO APPLY IT" : "COULD NOT SAVE (SD CARD?)";
    REXLOG_INFO("mp3: resolucion {}p {}", e.resolucion, guardado ? "guardada" : "NO guardada");
    Pintar(e);
  }
  return true;  // the game does not see the controller while the menu is open
}

}  // namespace

void Instalar(rex::Runtime& runtime) {
  (void)runtime;
  rex::input::FijarFiltroMando(&Filtro);
  REXLOG_INFO("mp3: menu de opciones con ZL + ZR + Menos (resolucion guardada: {}, por defecto {}p)",
              resolucion::Guardada(), resolucion::PorDefecto());
}

}  // namespace mp3::menu

// mp3-nx - the port's options menu: ZL + ZR + Minus opens it over the game (the presenter paints it, SDK patch 0088:
// no ImGui, which would move presentation to the UI thread), and while it is open the game sees the controller idle.
// Up / Down choose a line, Left / Right change its value at once, A saves the three values, B closes. It holds what
// the game has no setting for on the console: the detail distance (mp3_distancia_lod) and the car and ped density
// (mp3_densidad_coches / mp3_densidad_peatones), all applied by the ring thread at the next Swap (mp3_gpu.cpp).
// The saved values are in mp3_opciones.txt beside the NRO and replace mp3.toml's at start.

#include "mp3_menu.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/filtro_mando.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/superposicion.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DECLARE(double, mp3_distancia_lod);      // mp3_gpu.cpp
REXCVAR_DECLARE(double, mp3_densidad_coches);    // mp3_gpu.cpp
REXCVAR_DECLARE(double, mp3_densidad_peatones);  // mp3_gpu.cpp

namespace mp3::menu {
namespace {

#if defined(__SWITCH__)
constexpr uint64_t kCombinacion = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus;
constexpr uint64_t kArriba = HidNpadButton_Up | HidNpadButton_StickLUp;
constexpr uint64_t kAbajo = HidNpadButton_Down | HidNpadButton_StickLDown;
constexpr uint64_t kIzquierda = HidNpadButton_Left | HidNpadButton_StickLLeft;
constexpr uint64_t kDerecha = HidNpadButton_Right | HidNpadButton_StickLRight;
constexpr uint64_t kAceptar = HidNpadButton_A;
constexpr uint64_t kCerrar = HidNpadButton_B;
#else
constexpr uint64_t kCombinacion = 0, kArriba = 0, kAbajo = 0, kIzquierda = 0, kDerecha = 0, kAceptar = 0,
                   kCerrar = 0;
#endif

struct Opcion {
  const char* nombre;  // the menu's text (the overlay's font: upper case)
  const char* cvar;
  std::vector<double> valores;
};
const std::array<Opcion, 3> kOpciones = {{
    {"DRAW DISTANCE", "mp3_distancia_lod", {0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0}},
    {"TRAFFIC (CARS)", "mp3_densidad_coches", {0.2, 0.4, 0.6, 0.8, 1.0}},
    {"PEDESTRIANS", "mp3_densidad_peatones", {0.2, 0.4, 0.6, 0.8, 1.0}},
}};

double Actual(size_t i) {
  switch (i) {
    case 0: return REXCVAR_GET(mp3_distancia_lod);
    case 1: return REXCVAR_GET(mp3_densidad_coches);
    default: return REXCVAR_GET(mp3_densidad_peatones);
  }
}

// The index of the listed value nearest to the setting's.
size_t Indice(size_t i) {
  const double v = Actual(i);
  size_t mejor = 0;
  for (size_t k = 1; k < kOpciones[i].valores.size(); ++k) {
    if (std::fabs(kOpciones[i].valores[k] - v) < std::fabs(kOpciones[i].valores[mejor] - v)) mejor = k;
  }
  return mejor;
}

void Fijar(size_t i, double valor) {
  char texto[16];
  std::snprintf(texto, sizeof(texto), "%.2f", valor);
  rex::cvar::SetFlagByName(kOpciones[i].cvar, texto);
}

std::filesystem::path Fichero() { return rex::filesystem::GetExecutableFolder() / "mp3_opciones.txt"; }

bool Guardar() {
  FILE* f = std::fopen(Fichero().string().c_str(), "w");
  if (!f) return false;
  bool escrito = true;
  for (size_t i = 0; i < kOpciones.size(); ++i) {
    escrito &= std::fprintf(f, "%s %.2f\n", kOpciones[i].cvar, Actual(i)) > 0;
  }
  return std::fclose(f) == 0 && escrito;
}

// The saved values replace the configuration's (mp3.toml). Lines "name value"; unknown names are ignored.
void Cargar() {
  FILE* f = std::fopen(Fichero().string().c_str(), "r");
  if (!f) return;
  char nombre[64];
  double valor = 0;
  while (std::fscanf(f, "%63s %lf", nombre, &valor) == 2) {
    for (size_t i = 0; i < kOpciones.size(); ++i) {
      if (std::string(nombre) == kOpciones[i].cvar && valor >= 0.0 && valor <= 2.0) {
        Fijar(i, valor);
        REXLOG_INFO("mp3: opcion guardada {} = {}", nombre, valor);
      }
    }
  }
  std::fclose(f);
}

struct Estado {
  std::mutex candado;
  bool abierto = false;
  size_t linea = 0;
  std::string aviso;                // the last line: what happened
  std::array<uint64_t, 8> antes{};  // buttons held at each slot's previous poll
};
Estado& ElEstado() {
  static Estado e;
  return e;
}

void Pintar(const Estado& e) {
  std::vector<std::string> lineas = {"GTA IV - OPTIONS"};
  for (size_t i = 0; i < kOpciones.size(); ++i) {
    char texto[48];
    std::snprintf(texto, sizeof(texto), "%s%-16s < %.2f >", e.linea == i ? "> " : "  ", kOpciones[i].nombre,
                  Actual(i));
    lineas.emplace_back(texto);
  }
  lineas.emplace_back("");
  lineas.emplace_back("UP/DOWN: CHOOSE   LEFT/RIGHT: CHANGE");
  lineas.emplace_back("A: SAVE   B: CLOSE");
  lineas.push_back(e.aviso);
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
  if (nuevos & kArriba) {
    e.linea = (e.linea + kOpciones.size() - 1) % kOpciones.size();
    Pintar(e);
  } else if (nuevos & kAbajo) {
    e.linea = (e.linea + 1) % kOpciones.size();
    Pintar(e);
  } else if (nuevos & (kIzquierda | kDerecha)) {
    const auto& valores = kOpciones[e.linea].valores;
    size_t k = Indice(e.linea);
    if ((nuevos & kIzquierda) && k > 0) --k;
    if ((nuevos & kDerecha) && k + 1 < valores.size()) ++k;
    Fijar(e.linea, valores[k]);
    e.aviso = "APPLIED. A: SAVE FOR THE NEXT START";
    REXLOG_INFO("mp3: {} = {}", kOpciones[e.linea].cvar, valores[k]);
    Pintar(e);
  } else if (nuevos & kAceptar) {
    const bool guardado = Guardar();
    e.aviso = guardado ? "SAVED." : "COULD NOT SAVE (SD CARD?)";
    REXLOG_INFO("mp3: opciones {}", guardado ? "guardadas" : "NO guardadas");
    Pintar(e);
  }
  return true;  // the game does not see the controller while the menu is open
}

}  // namespace

void Instalar(rex::Runtime& runtime) {
  (void)runtime;
  Cargar();
  rex::input::FijarFiltroMando(&Filtro);
  REXLOG_INFO("mp3: menu de opciones con ZL + ZR + Menos (distancia {}, coches {}, peatones {})",
              REXCVAR_GET(mp3_distancia_lod), REXCVAR_GET(mp3_densidad_coches),
              REXCVAR_GET(mp3_densidad_peatones));
}

}  // namespace mp3::menu

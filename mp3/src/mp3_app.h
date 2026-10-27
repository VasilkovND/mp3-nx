// mp3-nx - Max Payne 3 (Xbox 360, Games on Demand, Title Update 6) on the Nintendo Switch, on rexglue-sdk-nx with the
// deko3d renderer.
//
// Where the game is, next to the NRO (sdmc:/switch/mp3-nx/ on the console):
//   game/     the extracted disc 1 (default.xex, common.rpf, xbox360.rpf, audio*.rpf ...), or
//   god/      disc 1's Games on Demand package as it is on the Xbox 360's drive (mp3_god.cpp), or the package's
//             header file anywhere on the card (mp3_god)
//   update/   the files of Title Update 6's disc001/ folder: default.xexp, update.rpf, commonup.rpf, platformup.rpf ...
//             (the game's update:)
// tools/stage_sd.py makes update/ from the TU6 package. The runtime applies default.xexp as it loads default.xex: the
// recompiled code is TU6's.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include "mp3_cpu.h"
#include "mp3_god.h"
#include "mp3_gpu.h"
#include "mp3_menu.h"
#include "mp3_resolution.h"

#if defined(__SWITCH__)
#include <rex/ui/switch_apm.h>
#endif

#if defined(__SWITCH__)
// The SDK's FPS counter (ZL + ZR + Up) and debug shortcuts (ZL + ZR + R: the profiler's stack sampling, captures ...).
REXCVAR_DECLARE(bool, switch_contador_fps);
REXCVAR_DECLARE(bool, switch_atajos_depuracion);
#endif
// While the port is being brought up: the FPS counter on screen from the start, and the debug shortcuts on.
REXCVAR_DEFINE_BOOL(mp3_fps_al_arrancar, true, "MP3",
                    "Contador de FPS en pantalla desde el arranque (ZL + ZR + arriba lo apaga) y atajos de depuracion "
                    "encendidos (ZL + ZR + R: muestreo de pilas del perfil)");
REXCVAR_DEFINE_STRING(mp3_game, "", "MP3", "Carpeta del juego (disco extraido + default.xexp). Vacio: game/ junto al ejecutable.");
REXCVAR_DEFINE_STRING(mp3_god, "", "MP3",
                      "El paquete Games on Demand del disco 1: su archivo de cabecera (junto a su carpeta .data) o la "
                      "carpeta que lo tiene. Vacio: god/ junto al ejecutable.");

// Handheld GPU clock through Horizon's own performance profiles (the ones commercial games pick; no overclock), as in
// ds3-nx: at stock the handheld GPU runs at 307.2 MHz, and at 540p Max Payne 3 needs ~26 ms of GPU per frame at 614 MHz
// (run 25), ~52 at 307. 460 asks for GPU 460.8 MHz with the CPU at 1020 and the memory at 1331.2 as before
// (sdk/src/ui/switch_apm.cpp tries the pairs that keep the memory clock first). Docked, or with the CPU already above
// 1020 MHz at start (sys-clk), nothing is requested.
REXCVAR_DEFINE_INT32(mp3_gpu_portatil_mhz, 460, "MP3",
                     "GPU en modo portatil por los perfiles de rendimiento de Horizon: 460 o 384 MHz; 0 = no tocar "
                     "(307,2 MHz de serie)")
    .range(0, 460);

class Mp3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Mp3App>(new Mp3App(ctx, "mp3", PPCImageConfig));
  }

 protected:
  // No Xenos emulation: mp3_gpu.cpp reads the game's command ring and draws with the deko3d renderer.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!config.graphics) config.graphics = mp3::gpu::CreateGraphicsSystem();
  }

  // game_data_root is the folder with default.xex (ConstructRuntime checks it).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto folder = rex::filesystem::GetExecutableFolder();
    std::error_code ec;
    // Title Update 6's files for disc 1 (default.xexp, update.rpf ...): the game reads them as update:.
    if (paths.update_data_root.empty() && !folder.empty() && std::filesystem::is_directory(folder / "update", ec)) {
      paths.update_data_root = folder / "update";
    }
    if (!paths.game_data_root.empty()) return;
    const std::string flag = REXCVAR_GET(mp3_game);
    if (!flag.empty()) {
      paths.game_data_root = flag;
      return;
    }
    if (folder.empty()) return;
    if (!PaqueteGod().empty()) {
      // A Games on Demand package: its executable is loaded from god_xex/ (mp3_god.cpp fills it).
      std::filesystem::create_directories(folder / "god_xex", ec);
      paths.game_data_root = folder / "god_xex";
      return;
    }
    paths.game_data_root = folder / "game";
  }

  void OnPostInitLogging() override {
#if defined(__SWITCH__)
    if (REXCVAR_GET(mp3_fps_al_arrancar)) {
      REXCVAR_SET(switch_contador_fps, true);
      REXCVAR_SET(switch_atajos_depuracion, true);
    }
#endif
    mp3::cpu::InformarSistema();
    mp3::resolucion::Aplicar();  // 720p or 540p (mp3_resolution.h), before the game reads its video mode
  }

  void OnLoadXexImage(std::string& xex_image) override {
    std::error_code ec;
    const auto root = game_data_root();
    const auto folder = rex::filesystem::GetExecutableFolder();
    bool god = false;
    try {
      god = mp3::god::Montar(*runtime(), PaqueteGod(), root, folder / "update");
    } catch (const std::exception& error) {
      Fail(std::string("Max Payne 3: ") + error.what());
    }
    for (const char* name : {"default.xex", "default.xexp"}) {
      if (!std::filesystem::is_regular_file(root / name, ec)) {
        Fail(fmt::format("Max Payne 3: {} is missing from {}. Make the folder with tools/stage_sd.py (your disc or "
                         "Games on Demand package, and Title Update 6).",
                         name, root.string()));
      }
    }
    if (!std::filesystem::is_regular_file(folder / "update" / "update.rpf", ec)) {
      REXLOG_WARN("mp3: update/update.rpf not found: Title Update 6's data files are missing (tools/stage_sd.py --tu)");
    }
    REXLOG_INFO("mp3: game at {}{}; loading {}", root.string(), god ? " and god/" : "", xex_image);
  }

  void OnPreLaunchModule() override {
    mp3::cpu::Configurar();
    mp3::god::DespuesDeCargar(*runtime());
    mp3::resolucion::AplicarMemoria(*runtime());  // the game's wanted size too (mp3_resolution.h)
    mp3::menu::Instalar(*runtime());
#if defined(__SWITCH__)
    const uint32_t cpu_mhz = mp3::resolucion::CpuMhz();
    if (const int32_t mhz = REXCVAR_GET(mp3_gpu_portatil_mhz); mhz > 0 && cpu_mhz <= 1020) {
      RexSwitchApmPedirGpuMhz(mhz);
      RexSwitchApmAplicar();
    } else if (mhz > 0) {
      REXLOG_INFO("mp3: CPU a {} MHz (overclock): sin perfil de GPU portatil", cpu_mhz);
    }
#endif
  }

 private:
  // Disc 1's Games on Demand package: mp3_god, or god/ beside the NRO; empty when there is none.
  static std::filesystem::path PaqueteGod() {
    std::error_code ec;
    const std::string flag = REXCVAR_GET(mp3_god);
    if (!flag.empty()) return std::filesystem::exists(flag, ec) ? std::filesystem::path(flag) : std::filesystem::path();
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (!folder.empty() && std::filesystem::is_directory(folder / "god", ec)) return folder / "god";
    return {};
  }

  [[noreturn]] static void Fail(const std::string& msg) {
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    rex::FlushLogging();
    std::_Exit(1);
  }
};

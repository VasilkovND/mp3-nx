// mp3-nx - the game straight from a Games on Demand package (an Xbox 360 hard drive install), from dantes-inferno-nx
// through gta4-nx. Max Payne 3 is two discs, two packages (Media IDs 1DFE6863 and 1FE2BB66 in the user's copy): disc 1
// is mounted, disc 2 is not yet.
//
// A GOD package is the disc image cut into 162.56 MB pieces (<name>.data/Data0000, Data0001, ...) with hash tables
// between the blocks, plus a header file <name> (STFS header, content type 0x7000, SVOD volume). The SDK's
// StfsContainerDevice (from Xenia) reads it as a disc with plain file reads, so the console can use it: god/ next to the
// NRO, copied from the hard drive's Content/0000000000000000/5454086B/00007000/ as it is, or the header file wherever
// it already is on the card (mp3_god).
//
// How it is mounted: with a package in god/, the runtime's game: and d: start at god_xex/ next to the NRO, and:
//   - d: and \Device\Cdrom0 are the package;
//   - the package's default.xex is copied into god_xex/ (when it differs), with update/default.xexp (Title Update 6)
//     beside it, for the runtime to load and patch it from there;
//   - after the executable is loaded, game: is the package too, as it is the disc on the console.
// Title Update 6's other files (update.rpf, commonup.rpf ...) are update: (mp3_app.h). Extracted disc files in game/
// still work as before when there is no god/.

#include "mp3_pch.h"

#include "mp3_god.h"

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xcontent.h>
#include <rex/thread.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using rex::X_STATUS;  // the SDK's status macros use this unqualified typedef

namespace mp3::god {
namespace {

constexpr const char* kDispositivo = "\\Device\\Mp3God";
constexpr uint32_t kTitleId = 0x5454086B;  // Max Payne 3
bool g_montado = false;

struct Cerrar {
  void operator()(rex::filesystem::File* f) const {
    if (f) f->Destroy();
  }
};

// default.xex of the package, whole.
std::vector<uint8_t> LeerXex(rex::filesystem::Device& paquete) {
  rex::filesystem::Entry* entrada = paquete.ResolvePath("default.xex");
  rex::filesystem::File* bruto = nullptr;
  if (!entrada || entrada->Open(rex::filesystem::FileAccess::kGenericRead, &bruto) != X_STATUS_SUCCESS) {
    throw std::runtime_error("the Games on Demand package has no default.xex");
  }
  std::unique_ptr<rex::filesystem::File, Cerrar> archivo(bruto);
  std::vector<uint8_t> datos(entrada->size());
  size_t leidos = 0;
  if (archivo->ReadSync(datos, 0, &leidos) != X_STATUS_SUCCESS || leidos != datos.size()) {
    throw std::runtime_error("cannot read default.xex from the Games on Demand package");
  }
  return datos;
}

// The build field of the XEX execution info's version (major 4, minor 4, build 16, qfe 8 bits); 0 when absent.
uint32_t VersionCompilacion(const std::vector<uint8_t>& xex) {
  auto u32 = [&](size_t o) -> uint32_t {
    return o + 4 <= xex.size() ? (uint32_t(xex[o]) << 24) | (uint32_t(xex[o + 1]) << 16) | (uint32_t(xex[o + 2]) << 8) | xex[o + 3]
                               : 0;
  };
  const uint32_t cabeceras = u32(20);
  for (uint32_t i = 0; i < cabeceras && i < 64; ++i) {
    if (u32(24 + 8 * i) == 0x00040006) {
      return (u32(u32(28 + 8 * i) + 4) >> 8) & 0xFFFF;
    }
  }
  return 0;
}

bool Igual(const std::filesystem::path& ruta, const std::vector<uint8_t>& datos) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(ruta, ec) || std::filesystem::file_size(ruta, ec) != datos.size()) return false;
  std::vector<uint8_t> actual(datos.size());
  FILE* f = std::fopen(ruta.string().c_str(), "rb");
  if (!f) return false;
  const bool ok = std::fread(actual.data(), 1, actual.size(), f) == actual.size();
  std::fclose(f);
  return ok && actual == datos;
}

void Enlazar(rex::filesystem::VirtualFileSystem& vfs, const char* alias) {
  vfs.UnregisterSymbolicLink(alias);
  if (!vfs.RegisterSymbolicLink(alias, kDispositivo)) {
    throw std::runtime_error(std::string("cannot map ") + alias + " to the Games on Demand package");
  }
}

}  // namespace

bool Montar(rex::Runtime& runtime, const std::filesystem::path& carpeta_god, const std::filesystem::path& carpeta_xex,
            const std::filesystem::path& carpeta_update) {
  std::error_code ec;
  if (carpeta_god.empty() || !std::filesystem::exists(carpeta_god, ec)) return false;
  auto paquete = std::make_unique<rex::filesystem::StfsContainerDevice>(kDispositivo, carpeta_god);
  if (!paquete->Initialize()) {
    throw std::runtime_error(carpeta_god.string() + ": no readable Games on Demand package (the header file and its "
                             ".data folder with every DataNNNN piece are needed)");
  }
  const auto& meta = paquete->header().metadata;
  const uint32_t tipo = uint32_t(meta.content_type.get());
  const uint32_t titulo = meta.execution_info.title_id, medio = meta.execution_info.media_id;
  if (titulo != kTitleId) {
    throw std::runtime_error(fmt::format("{}: title {:08X}, not Max Payne 3 ({:08X})", carpeta_god.string(), titulo,
                                         kTitleId));
  }
  REXLOG_INFO("mp3: paquete Games on Demand en {}: tipo {:08X}, titulo {:08X}, Media ID {:08X}", carpeta_god.string(),
              tipo, titulo, medio);

  const std::vector<uint8_t> xex = LeerXex(*paquete);
  const auto ruta_xex = carpeta_xex / "default.xex";
  if (!Igual(ruta_xex, xex)) {
    std::filesystem::create_directories(carpeta_xex, ec);
    FILE* f = std::fopen(ruta_xex.string().c_str(), "wb");
    if (!f || std::fwrite(xex.data(), 1, xex.size(), f) != xex.size()) {
      if (f) std::fclose(f);
      throw std::runtime_error("cannot write " + ruta_xex.string());
    }
    std::fclose(f);
    REXLOG_INFO("mp3: default.xex copiado del paquete a {} ({} bytes)", ruta_xex.string(), xex.size());
  }
  // The package carries the disc's executable; Title Update 6's default.xexp goes beside it (from update/, where
  // tools/stage_sd.py puts the update's files).
  const auto xexp = carpeta_xex / "default.xexp";
  const auto origen = carpeta_update / "default.xexp";
  // Read and written by hand: std::filesystem::copy_file failed on the console's SD card (first run).
  std::vector<uint8_t> parche;
  if (FILE* f = std::fopen(origen.string().c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
      parche.resize(size_t(n));
      if (std::fread(parche.data(), 1, parche.size(), f) != parche.size()) parche.clear();
    }
    std::fclose(f);
  }
  if (parche.empty()) {
    throw std::runtime_error("update/default.xexp is missing or unreadable: the code is Title Update 6's "
                             "(tools/stage_sd.py --tu)");
  }
  if (!Igual(xexp, parche)) {
    FILE* f = std::fopen(xexp.string().c_str(), "wb");
    if (!f || std::fwrite(parche.data(), 1, parche.size(), f) != parche.size()) {
      if (f) std::fclose(f);
      throw std::runtime_error("cannot write " + xexp.string());
    }
    std::fclose(f);
  }
  REXLOG_INFO("mp3: Title Update 6 (update/default.xexp, {} bytes) junto al default.xex del paquete (build {})",
              parche.size(), VersionCompilacion(xex));

  rex::filesystem::VirtualFileSystem& vfs = *runtime.file_system();
  if (!vfs.RegisterDevice(std::move(paquete))) throw std::runtime_error("cannot register the Games on Demand package");
  auto cerrojo = rex::thread::global_critical_region::AcquireDirect();
  Enlazar(vfs, "d:");
  Enlazar(vfs, "\\Device\\Cdrom0");
  g_montado = true;
  return true;
}

void DespuesDeCargar(rex::Runtime& runtime) {
  if (!g_montado) return;
  auto cerrojo = rex::thread::global_critical_region::AcquireDirect();
  Enlazar(*runtime.file_system(), "game:");
  REXLOG_INFO("mp3: game: y d: son el paquete Games on Demand");
}

bool Montado() { return g_montado; }

}  // namespace mp3::god

// ds_parecidos: shaders the console took "by similarity" (mp3_shaders.cpp, VolcarDesconocido) made library entries
// of their own.
//
//   ds_parecidos <shaders_unknown folder> <containers folder>
//
// Direct3D patches a vertex shader's microcode when it binds it to a vertex declaration (the fetches, sometimes a few
// exports), so some loads match no container of the library exactly and the console takes the closest one, which
// may draw wrong (Max Payne 3's black spikes). The console writes such a load as {vs,ps}_<microcode fingerprint>_de_
// <container fingerprint>.bin (the microcode as loaded, big-endian words). Here the container named after `_de_` is
// copied with its microcode replaced by those words (same length) and written to the containers folder as
// {vs,ps}_<XXH3 of the new container>.bin: the next library build translates the shader as the game really loads it.
// Containers already there are left alone.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> Leer(const fs::path& ruta) {
  std::ifstream f(ruta, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

uint32_t U32(const std::vector<uint8_t>& d, size_t o) {
  return o + 4 <= d.size() ? uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3] : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "uso: ds_parecidos <shaders_unknown> <contenedores>\n");
    return 1;
  }
  const fs::path desconocidos = argv[1], contenedores = argv[2];
  const std::regex nombre(R"(^(vs|ps)_([0-9a-f]{16})_de_([0-9a-f]{16})\.bin$)");
  int nuevos = 0, presentes = 0, fallidos = 0;
  for (const auto& e : fs::directory_iterator(desconocidos)) {
    std::smatch m;
    const std::string n = e.path().filename().string();
    if (!std::regex_match(n, m, nombre)) continue;
    const fs::path origen = contenedores / (m[1].str() + "_" + m[3].str() + ".bin");
    std::vector<uint8_t> c = Leer(origen);
    const std::vector<uint8_t> micro = Leer(e.path());
    // Container (big-endian): flags, virtualSize, physicalSize, ...; the word at 24 points at a header in the virtual
    // part whose first word is the microcode's offset in the physical part, which follows the virtual one.
    const uint32_t virtuales = U32(c, 4), cabecera = U32(c, 24);
    const size_t inicio = size_t(virtuales) + U32(c, cabecera);
    if (c.empty() || micro.empty() || micro.size() % 4 || inicio + micro.size() > c.size()) {
      std::fprintf(stderr, "%s: contenedor %s ausente o el microcodigo no cabe\n", n.c_str(), origen.filename().c_str());
      ++fallidos;
      continue;
    }
    std::copy(micro.begin(), micro.end(), c.begin() + std::ptrdiff_t(inicio));
    char salida[64];
    std::snprintf(salida, sizeof(salida), "%s_%016llx.bin", m[1].str().c_str(),
                  static_cast<unsigned long long>(XXH3_64bits(c.data(), c.size())));
    const fs::path destino = contenedores / salida;
    if (fs::exists(destino)) {
      ++presentes;
      continue;
    }
    std::ofstream(destino, std::ios::binary).write(reinterpret_cast<const char*>(c.data()), std::streamsize(c.size()));
    ++nuevos;
  }
  std::printf("ds_parecidos: %d contenedores nuevos, %d ya estaban, %d fallidos\n", nuevos, presentes, fallidos);
  return fallidos ? 1 : 0;
}

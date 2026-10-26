// mp3-nx - the game straight from a Games on Demand package (mp3_god.cpp).

#pragma once

#include <filesystem>

namespace rex {
class Runtime;
}

namespace mp3::god {

// Before the executable is loaded: if `carpeta_god` is (or holds) the game's Games on Demand package, mounts it as d:, copies
// its default.xex into `carpeta_xex` (the runtime's game data root in this mode) when that one differs, puts
// `carpeta_update`/default.xexp (Title Update 6) beside it, and returns true. Throws
// with a message for the player when the folder is there but unusable.
bool Montar(rex::Runtime& runtime, const std::filesystem::path& carpeta_god, const std::filesystem::path& carpeta_xex,
            const std::filesystem::path& carpeta_update);

// After the executable is loaded: game: becomes the package too.
void DespuesDeCargar(rex::Runtime& runtime);

bool Montado();

}  // namespace mp3::god

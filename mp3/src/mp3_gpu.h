// mp3-nx - the game's graphics system on the Switch (see mp3_gpu.cpp; copied from halo3/src/halo3_gpu.cpp, game-neutral).

#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace mp3::gpu {

// Stands in for the Xbox 360 GPU: answers what the game's Direct3D waits for, and reads its command ring.
std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

}  // namespace mp3::gpu

namespace rex::x360 {
class RenderHooks;
}

namespace mp3::gpu {

// The renderer's hooks: the final picture's brightness, contrast and gamma (mp3_imagen.cpp).
rex::x360::RenderHooks& GanchosImagen();

}  // namespace mp3::gpu

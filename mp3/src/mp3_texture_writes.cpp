// mp3-nx - the game's bulk writes to GPU-visible memory reported to the texture cache.
//
// Max Payne 3 streams textures into memory other textures used before (a ped's diffuse replaced by another's, a mip
// level arriving after its placeholder). The renderer keeps converted copies and only learned of a change on its
// periodic recheck: Max wore an enemy's black jacket for seconds, textures flickered and went missing. The XDK's copy and
// decompression functions the game puts texture data in place with (found with tools/fuzzy_find.py against Dead Space
// 2's named tree, ratio 1.0) now mark the physical range they wrote (sdk escrituras.h); the texture cache rechecks a
// texture at its next use after such a write (x360_texturas_por_escritura).

#include "mp3_pch.h"

#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/x360/escrituras.h>

namespace {

// A guest virtual range written: marked if it lies in a physical heap (the GPU's memory).
void Marcar(uint32_t direccion, uint32_t bytes) {
  if (!bytes) return;
  auto* memoria = rex::system::kernel_memory();
  if (!memoria) return;
  const uint32_t fisica = memoria->GetPhysicalAddress(direccion);
  if (fisica == UINT32_MAX) return;
  rex::x360::MarcarEscritura(fisica, bytes);
}

}  // namespace

// XMemCpyStreaming (r3 destination, r4 source, r5 bytes): the copy into write-combined memory.
#ifdef MP3_TIENE_mp3_XMemCpyStreaming
REX_EXTERN(__imp__mp3_XMemCpyStreaming);
REX_EXTERN(mp3_XMemCpyStreaming) {
  const uint32_t destino = ctx.r3.u32, bytes = ctx.r5.u32;
  __imp__mp3_XMemCpyStreaming(ctx, base);
  Marcar(destino, bytes);
}
#endif

// XMemCpy (r3 destination, r4 source, r5 bytes).
#ifdef MP3_TIENE_mp3_XMemCpy
REX_EXTERN(__imp__mp3_XMemCpy);
REX_EXTERN(mp3_XMemCpy) {
  const uint32_t destino = ctx.r3.u32, bytes = ctx.r5.u32;
  __imp__mp3_XMemCpy(ctx, base);
  Marcar(destino, bytes);
}
#endif

// XMemDecompress and its stream variant (r3 context, r4 destination, r5 address of the destination size, which the
// call sets to the bytes written).
#ifdef MP3_TIENE_mp3_XMemDecompress
REX_EXTERN(__imp__mp3_XMemDecompress);
REX_EXTERN(mp3_XMemDecompress) {
  const uint32_t destino = ctx.r4.u32, tam = ctx.r5.u32;
  __imp__mp3_XMemDecompress(ctx, base);
  if (tam) Marcar(destino, rex::memory::load_and_swap<uint32_t>(base + tam));
}
#endif

#ifdef MP3_TIENE_mp3_XMemDecompressStream
REX_EXTERN(__imp__mp3_XMemDecompressStream);
REX_EXTERN(mp3_XMemDecompressStream) {
  const uint32_t destino = ctx.r4.u32, tam = ctx.r5.u32;
  __imp__mp3_XMemDecompressStream(ctx, base);
  if (tam) Marcar(destino, rex::memory::load_and_swap<uint32_t>(base + tam));
}
#endif

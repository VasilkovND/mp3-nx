// mp3-nx - where the device's size comes from (diagnostic for 540p). Run 21 wrote 960x540 into the device's size
// (0x831D0954/58) before the game ran, and 300 frames later it was 1280x720 again. Each function below that sets or
// copies that size is wrapped: its first calls log the device's size, the copy other threads read
// (0x8381C810 / 0x8381C800) and, for the texture factory, its default target size (+44 / +48), before and after.
// sub_82DF3FA0 (a render target released: the device's size goes back to the factory's) runs every frame and logs
// only when it changes the device's size.

#include "mp3_pch.h"

#include <atomic>

#include <rex/logging.h>

namespace {

struct Tamanos {
  uint32_t disp_ancho, disp_alto, copia_ancho, copia_alto;
};

Tamanos Leer(uint8_t* base) {
  return {REX_LOAD_U32(0x831D0954), REX_LOAD_U32(0x831D0958), REX_LOAD_U32(0x8381C810), REX_LOAD_U32(0x8381C800)};
}

void Informar(const char* que, const Tamanos& a, const Tamanos& d, uint32_t lr) {
  REXLOG_INFO("[540p] {} (lr {:08X}): dispositivo {}x{} -> {}x{}, copia {}x{} -> {}x{}", que, lr, a.disp_ancho,
              a.disp_alto, d.disp_ancho, d.disp_alto, a.copia_ancho, a.copia_alto, d.copia_ancho, d.copia_alto);
}

}  // namespace

#define MP3_ENVOLVER(nombre, texto, veces)                     \
  REX_EXTERN(__imp__##nombre);                                 \
  REX_EXTERN(nombre) {                                         \
    static std::atomic<int> llamadas{0};                       \
    const uint32_t lr = uint32_t(ctx.lr);                      \
    const Tamanos antes = Leer(base);                          \
    __imp__##nombre(ctx, base);                                \
    if (llamadas.fetch_add(1) < (veces)) Informar(texto, antes, Leer(base), lr); \
  }

#ifdef MP3_TIENE_mp3_DeviceSetup
MP3_ENVOLVER(mp3_DeviceSetup, "sub_82DEEA18 (configuracion del dispositivo)", 4)
#endif
#ifdef MP3_TIENE_mp3_DevicePresentParams
MP3_ENVOLVER(mp3_DevicePresentParams, "sub_82DECDA8 (parametros de presentacion)", 8)
#endif
#ifdef MP3_TIENE_mp3_DeviceResize
MP3_ENVOLVER(mp3_DeviceResize, "sub_822BB928 (cambio de tamano)", 4)
#endif
#ifdef MP3_TIENE_mp3_DeviceReset
MP3_ENVOLVER(mp3_DeviceReset, "sub_82DEE698 (reinicio del dispositivo)", 8)
#endif

#ifdef MP3_TIENE_mp3_TextureFactoryCreate
REX_EXTERN(__imp__mp3_TextureFactoryCreate);
REX_EXTERN(mp3_TextureFactoryCreate) {
  static std::atomic<int> llamadas{0};
  const uint32_t lr = uint32_t(ctx.lr);
  const Tamanos antes = Leer(base);
  __imp__mp3_TextureFactoryCreate(ctx, base);
  if (llamadas.fetch_add(1) < 4) {
    Informar("sub_82DF5EA8 (fabrica de texturas)", antes, Leer(base), lr);
  }
}
#endif

#ifdef MP3_TIENE_mp3_ReleaseRenderTarget
REX_EXTERN(__imp__mp3_ReleaseRenderTarget);
REX_EXTERN(mp3_ReleaseRenderTarget) {
  static std::atomic<int> cambios{0};
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t fabrica = ctx.r3.u32;
  const Tamanos antes = Leer(base);
  __imp__mp3_ReleaseRenderTarget(ctx, base);
  const Tamanos despues = Leer(base);
  if ((antes.disp_ancho != despues.disp_ancho || antes.disp_alto != despues.disp_alto) && cambios.fetch_add(1) < 8) {
    Informar("sub_82DF3FA0 (render target soltado)", antes, despues, lr);
    REXLOG_INFO("[540p]   fabrica {:08X}: tamano por defecto {}x{}", fabrica, REX_LOAD_U32(fabrica + 44),
                REX_LOAD_U32(fabrica + 48));
  }
}
#endif

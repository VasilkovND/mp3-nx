// mp3-nx - the game's graphics system on the Switch.
//
// Copied from halo3/src/halo3_gpu.cpp: nothing in it is about Halo 3. Once a second game uses it, it belongs in sdk/.
//
// What it does
//   Stands in for the Xbox 360 GPU, so the game runs past its first frame. The game's own Direct3D keeps
//   running unchanged and writes its list of GPU commands (the PM4 ring). A thread reads that list and does
//   what the game waits for:
//     1. MMIO registers at 0x7FC80000, with the fixed values the SDK's emulation returns;
//     2. the ring read pointer, written back where the game watches it (it spins on it to know how much
//        room is left);
//     3. interrupts: vblank, and PM4_INTERRUPT packets (the GPU reached that point of the ring);
//     4. GPU writes to memory (MEM_WRITE, COND_WRITE, REG_TO_MEM, EVENT_WRITE_*, scratch registers), as in
//        the SDK's graphics/command_processor.cpp. Occlusion queries report 1000 samples, like the SDK.
//   5. (deko3d build) the minimal renderer: IM_LOAD / IM_LOAD_IMMEDIATE tell which translated shader is loaded
//      (mp3_shaders.h), and each draw goes to the SDK's deko3d backend (rex::x360::RenderDk) as a PeticionDibujo
//      with the register mirror; resolves and clears go to its Copiar, and each Swap presents the frontbuffer
//      that fetch constant 0 describes. A draw whose VS or PS is not identified is skipped and counted.
//
// Presentation (the deko3d build): the SDK's deko3d provider and presenter (rex/ui/deko3d/presentador.h). ReXApp
// sees them and sets up the window and the ImGui overlay as usual; until the renderer refreshes the guest output,
// the presenter paints black under the overlay.
//
// Without it the runtime drops the game's Vd* calls ("no GPU emulation loaded") and the render thread waits
// forever for a GPU that never reads its commands.

#include "mp3_gpu.h"

void mp3_log_main_thread();  // src/mp3_guards.cpp

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/types.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

#if MP3_DEKO3D
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <string>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/ui/deko3d/presentador.h>
#include <rex/x360/decodificador.h>
#include <rex/x360/deko/destinos.h>
#include <rex/x360/destinos.h>
#include <rex/x360/peticion.h>
#include <rex/x360/xenos.h>

#include "mp3_shaders.h"

// xxHash reads with memcpy under GCC's strict aliasing (see shaders/ds_shader_library.cpp).
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DECLARE(bool, switch_captura_pedida);  // ZL + ZR + Down (sdk/src/input/switch/switch_input_driver.cpp)
REXCVAR_DECLARE(bool, switch_volcado_pedido);  // ZL + ZR + Left: every resolved texture of the next frame
REXCVAR_DECLARE(bool, nfsmw_dk_todas_las_salidas);  // skip mode 9 (SDK patch 0033)
REXCVAR_DECLARE(bool, nfsmw_dk_gamma_lineal);       // skip mode 10 (SDK patch 0033)
REXCVAR_DECLARE(std::string, rex_archivos_ocultos);  // SDK patch 0048: files the game does not find
namespace {
// The frame's size. GTA IV keeps its render targets at 1280x720 whatever the video mode says (run46: with
// video_mode_width/height = 1024x576 every resolve was still 1280x720; the width and height it derives from
// XGetVideoMode, capped at 1280x720, at 0x82B0B45C/60, size something else). Taking the size from the video mode made the
// one-tile widening miss the G-buffer's 1280x256 band and the presentation show a 1024x576 corner of the frame.
constexpr uint32_t AnchoJuego() { return 1280; }
constexpr uint32_t AltoJuego() { return 720; }
}  // namespace
REXCVAR_DECLARE(bool, switch_omitir_pedido);   // ZL + ZR + Right: next draw-skipping mode (SkipMode)
// The SDK renderer's shadow-pass shortcuts (sdk/src/x360/deko/destinos.cpp; the SDK keeps their nfsmw_ names): both
// drop colorless draws that alpha-test or discard, which here are ordinary depth-only draws. Off for Dead Space 2.
REXCVAR_DECLARE(bool, nfsmw_sombras_sin_vegetacion);
REXCVAR_DECLARE(bool, nfsmw_dk_tiles_en_pantalla);  // SDK patch 0053
REXCVAR_DECLARE(bool, nfsmw_nativo_ps_solo_alfa);
// The renderer's shadow-map depth bias, given when the game sets none: Dead Space 2's shadow maps are 960 wide.
REXCVAR_DECLARE(int32_t, nfsmw_dk_sombras_sesgo_pitch);
// The renderer's sampled change checks of geometry and textures stay at the SDK's defaults (sampled). Full fingerprints
// were tried against the streaks (which were the vertex index and the computed-index fetches, not stale vertices):
// they hashed 14-20 MB of vertices a frame and the ring thread went from 9 % to 40 % of a core.
REXCVAR_DECLARE(bool, nfsmw_dk_geometria_muestreo);
REXCVAR_DECLARE(int32_t, nfsmw_dk_perfil_gpu_s);  // the renderer's periodic GPU profile (telemetry only)
// Safe mode (ZL + ZR + Right, mode 1): every shortcut the renderer took for NFS Most Wanted off at once, to tell whether
// one of them breaks Dead Space 2 (each changes what is drawn when its guess about the game is wrong). The SDK reads
// them on every use, so they switch while the game runs.
REXCVAR_DECLARE(bool, nfsmw_nativo_saltar_invisibles);
REXCVAR_DECLARE(bool, nfsmw_nativo_sin_ps_sin_color);
REXCVAR_DECLARE(bool, nfsmw_nativo_sin_desenfoque);
REXCVAR_DECLARE(bool, nfsmw_nativo_pcf_barato);
REXCVAR_DECLARE(bool, nfsmw_nativo_sombra_minimo);
REXCVAR_DECLARE(bool, nfsmw_nativo_z_temprana);
REXCVAR_DECLARE(bool, nfsmw_nativo_cielo_aplazado);
REXCVAR_DECLARE(bool, nfsmw_dk_constantes_por_diferencias);
REXCVAR_DECLARE(bool, nfsmw_nativo_saltar_borrados_repetidos);
REXCVAR_DECLARE(bool, nfsmw_nativo_resolver_sin_copia);
REXCVAR_DECLARE(bool, nfsmw_nativo_intercambiar_sin_borrado);
REXCVAR_DECLARE(bool, nfsmw_nativo_frontal_perezoso);
REXCVAR_DECLARE(bool, nfsmw_dk_borrar_area_util);
REXCVAR_DECLARE(bool, nfsmw_dk_borrado_color_aplazado);
REXCVAR_DECLARE(bool, nfsmw_dk_borrado_z_aplazado);

// Automatic data collection while the renderer is being brought up: nothing to press on the console.
// Set by the GetData hook (mp3_perf_hooks.cpp) while Direct3D answers "not yet": Unreal's rendering thread is waiting
// for a query, so OcclusionPoll answers the ones still being counted instead of letting it wait for the GPU.
std::atomic<bool> g_mp3_query_waited{false};

REXCVAR_DEFINE_INT32(mp3_auto_captura_s, 15, "MP3",
                     "Cada cuantos segundos se guarda una captura reducida (640x360) en capturas/auto_NNNN.bmp; 0 = nunca");
REXCVAR_DEFINE_INT32(mp3_auto_volcado_primero_s, 120, "MP3",
                     "Segundo de juego del primer volcado automatico de las texturas resueltas de un fotograma");
REXCVAR_DEFINE_INT32(mp3_auto_volcado_cada_s, 300, "MP3", "Segundos entre volcados automaticos; 0 = solo el primero");
REXCVAR_DEFINE_DOUBLE(mp3_distancia_lod, 0.6, "MP3",
                      "Escala de la distancia de detalle del juego (0x82A931BC, 1.0 en la consola): un objeto se dibuja "
                      "mientras su distancia LOD por esta escala supera la de la camara; con menos, los modelos lejanos "
                      "pasan antes a su version de bajo detalle (menos dibujos en la escena, las sombras y los reflejos). "
                      "0.3-2.0, se aplica en el siguiente fotograma");
REXCVAR_DEFINE_DOUBLE(mp3_densidad_coches, 0.6, "MP3",
                      "Escala de la densidad de coches del juego (aleatorios 0x82AA1888 y aparcados 0x82AA188C): lo que "
                      "el juego o un script ponga se multiplica por esto en el siguiente fotograma. 0-1");
REXCVAR_DEFINE_DOUBLE(mp3_densidad_peatones, 0.6, "MP3",
                      "Escala de la densidad de peatones del juego (0x82AA2940, y los de escenario 0x82AA2944/2948): lo "
                      "que el juego o un script ponga se multiplica por esto en el siguiente fotograma. 0-1");
REXCVAR_DEFINE_INT32(mp3_prioridad_anillo, 0, "MP3",
                     "Prioridad de Horizon (44-59; menor = mas alta) del hilo del anillo de la GPU; 0 = 0x2C. 0x3B lo "
                     "pone con los hilos del juego (no les quita el nucleo al despertar)");
REXCVAR_DEFINE_BOOL(mp3_registros_en_bloque, true, "MP3",
                    "Las escrituras seguidas de constantes de shader y de fetch del anillo en un bucle, sin las "
                    "comprobaciones de cada registro (su generacion sube una vez si algo cambia)");
REXCVAR_DEFINE_BOOL(mp3_sin_prepaso_z, true, "MP3",
                    "Sin el pase previo de solo profundidad de la escena: el pase base escribe su propia profundidad. "
                    "Con el pase previo, la profundidad de dos vertex shaders compilados aparte no coincide bit a bit y "
                    "el pase base (GEQUAL) se descartaba: el mundo salia negro");
REXCVAR_DEFINE_BOOL(mp3_consultas_siempre_visibles, false, "MP3",
                    "Consultas de oclusion: cada segundo ZPD cuenta como el final de una consulta visible, aunque no lleve la marca de D3D (prueba)");
REXCVAR_DEFINE_BOOL(mp3_oclusion_real, true, "MP3",
                    "Consultas de oclusion reales: la GPU cuenta los pixeles de la caja de cada consulta que pasan la "
                    "profundidad, y el juego deja de dibujar lo que queda tapado. false = todo visible, como antes");
REXCVAR_DEFINE_INT32(mp3_oclusion_espera_ms, 40, "MP3",
                     "Consultas de oclusion reales: milisegundos desde el final de una consulta que el juego puede "
                     "esperar su resultado; pasado ese tiempo se le contesta visible");
REXCVAR_DEFINE_INT32(mp3_oclusion_sin_esperar_ms, 8, "MP3",
                     "Consultas de oclusion reales: cuando el juego se pone a esperar un resultado, las consultas "
                     "terminadas hace al menos estos milisegundos que la GPU aun no conto se contestan visibles al "
                     "momento (sin parar el fotograma). -1 = esperar hasta mp3_oclusion_espera_ms");
REXCVAR_DEFINE_STRING(mp3_sonda, "700,650", "MP3",
                      "Sonda del fotograma tras la traza (ZL+ZR+Left): x,y de un cuadrado de 8x8 pixeles; cada dibujo "
                      "del pase base se dibuja solo ahi y la GPU cuenta sus pixeles: el ultimo que pinta es el que se ve "
                      "en ese punto (vacio = sin sonda)");
REXCVAR_DEFINE_BOOL(mp3_saltar_logos, false, "MP3",
                    "Sin los videos de los logotipos del arranque (Epic y Microsoft Game Studios). Apagado: sin el "
                    "archivo el juego lee un puntero nulo y no arranca (pantalla negra)");
REXCVAR_DEFINE_BOOL(mp3_busqueda_ps, true, "MP3",
                    "Modos de busqueda (ZL+ZR+Right): cada pulsacion deja sin dibujar un pixel shader del pase base "
                    "(false: un vertex shader de los que escriben la profundidad de la escena)");
REXCVAR_DEFINE_BOOL(mp3_busqueda_directa, true, "MP3",
                    "ZL+ZR+Right va directo a los modos de busqueda, sin pasar por los modos 1-10");
REXCVAR_DEFINE_BOOL(mp3_saltar_vs_rotos, false, "MP3",
                    "No dibujar los vertex shaders que rompen la geometria (nivel 2: el VS 38234053372F8F2F, con "
                    "matrices elegidas por indice, dibuja triangulos gigantes sobre toda la pantalla)");
REXCVAR_DEFINE_STRING(mp3_omitir_vs, "", "MP3",
                      "Prueba: no dibujar los vertex shaders de estos contenedores (huellas en hex separadas por comas, "
                      "p. ej. 33041FEED58EB114: la chaqueta de Max con la tela)");
REXCVAR_DEFINE_DOUBLE(mp3_exposicion, 1.0, "MP3",
                      "Exposicion del tonemap: multiplica sus Exposures (c137) antes de la curva. 1 = la del juego; menos "
                      "de 1 baja el pase de luz quemado (0.4 a 1.0)");
REXCVAR_DEFINE_BOOL(mp3_fxaa_sin_sesgo, true, "MP3",
                    "El FXAA del juego (ps 8D42760B0383C0D8) escribe en el destino 7e3 de la escena sin su exp bias de "
                    "color (2, x4): la salida ya es 0..1 y el resolve a 8 bits no lo deshace; con el, el cuadro x4 y "
                    "quemado");
REXCVAR_DEFINE_BOOL(mp3_traza_bufers, true, "MP3",
                    "En la traza completa (ZL+ZR+Left): de cada bufer de vertices que el shader lee por indice (la tela "
                    "de Max: otro bufer que el de la malla), su rango de valores, NaN e infinitos y sus primeros float4");
REXCVAR_DEFINE_INT32(mp3_arboles, 1, "MP3",
                     "Los arboles del nivel 2 (VS 38234053372F8F2F): 0 = no se dibujan (mp3_saltar_vs_rotos), 1 = tal cual, "
                     "2 = con las matrices de balanceo (c11-c22) cambiadas por la identidad, 3 = identidad y c10 = 0, "
                     "4 = solo c10 = 0");
REXCVAR_DEFINE_BOOL(mp3_velocidad_sin_z, false, "MP3",
                    "Prueba: los dibujos del buffer de velocidades (PS 1929EAB6BE60FA77) sin test de profundidad contra "
                    "la profundidad reducida (5A0, 640x360)");
REXCVAR_DEFINE_BOOL(mp3_velocidad_prueba, false, "MP3",
                    "ZL+ZR+Right cambia mp3_velocidad_sin_z con una captura, en vez de los modos de busqueda");
REXCVAR_DEFINE_BOOL(mp3_arboles_prueba, false, "MP3",
                    "ZL+ZR+Right cambia el modo de los arboles (0, 1, 2) con una captura, en vez de los modos de busqueda");
REXCVAR_DEFINE_INT32(mp3_auto_volcado_dibujos, 800, "MP3",
                     "Dibujos que debe tener el fotograma anterior para un volcado automatico (la escena 3D, no un menu)");
REXCVAR_DEFINE_INT32(mp3_auto_volcados_max, 4, "MP3", "Volcados automaticos como mucho (0 = ninguno)");
REXCVAR_DEFINE_INT32(mp3_traza_vport_max, 0, "MP3",
                     "La traza de dibujos solo escribe los dibujos cuyo viewport mide como mucho este ancho (0 = todos)");
REXCVAR_DEFINE_BOOL(mp3_traza_solo_quads, false, "MP3",
                    "La traza de dibujos solo escribe los dibujos de 6 vertices o menos (los pases de pantalla completa)");
REXCVAR_DEFINE_BOOL(mp3_volcado_con_dibujos, false, "MP3",
                    "Un volcado de fotograma escribe tambien cada dibujo (texturas y constantes) en el log: lento con "
                    "miles de dibujos (un minuto en Max Payne 3). false = solo la estructura del fotograma");
REXCVAR_DEFINE_INT32(mp3_traza_cada_s, 60, "MP3", "Segundos entre trazas de la estructura de un fotograma pesado");
REXCVAR_DEFINE_INT32(mp3_trazas_max, 20, "MP3", "Trazas de fotograma como mucho");
REXCVAR_DEFINE_BOOL(mp3_ver_texturas, true, "MP3",
                    "Diagnostico: ZL + ZR + derecha muestra en pantalla la siguiente textura de color resuelta del "
                    "fotograma anterior en vez del fotograma (y vuelve al fotograma tras la ultima), con una linea "
                    "[ver] en el log y una captura");
REXCVAR_DEFINE_BOOL(mp3_tile_completo, true, "MP3",
                    "Con mp3_sin_tiling: el scissor 1280x256 del primer tile (pase del G-buffer) y sus resolves se "
                    "amplian a todo el fotograma 1280x720");
REXCVAR_DECLARE(bool, mp3_sin_tiling);  // src/mp3_tiling.cpp
REXCVAR_DECLARE(bool, x360_edram_compartida);  // sdk/src/x360/deko/edram.cpp
REXCVAR_DEFINE_BOOL(mp3_resolve_msaa_alias, false, "MP3",
                    "Un resolve 4x de un destino sin dibujos en el fotograma lee su gemelo 1x de doble pitch (la "
                    "reduccion de la escena a 640x360 de GTA IV: la cadena de luminancia y bloom)");
REXCVAR_DEFINE_BOOL(mp3_tiles_en_pantalla, false, "MP3",
                    "Los tiles del G-buffer (bandas de 256 filas) se dibujan y resuelven en su posicion de pantalla "
                    "(parche 0053 del SDK)");
REXCVAR_DEFINE_BOOL(mp3_bins_visibles, false, "MP3",
                    "Tiling predicado: los paquetes con mascara de bins 80000000 (visibilidad sin resolver) se ejecutan "
                    "en todos los tiles; false = como el hardware sin consultas de visibilidad (solo el primero)");
REXCVAR_DEFINE_BOOL(mp3_z_borrado_del_juego, true, "MP3",
                    "Los borrados de profundidad emulados escriben la profundidad que trae el rectangulo de borrado de "
                    "Direct3D (la del juego) en vez de adivinarla por la direccion de los dibujos");
REXCVAR_DEFINE_BOOL(mp3_perfil_pilas, true, "MP3",
                    "Crea logs/rex/perfil_pilas.flag al arrancar (con o sin telemetria.flag): el perfil de CPU muestrea "
                    "las pilas de cada hilo cada 1 ms (rex_perfil.log), ~8 % mas lento; ZL + ZR + R lo cambia jugando");
#endif

void Mp3AplicarPrioridades();  // mp3_perf_hooks.cpp

namespace mp3::gpu {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;  // X_STATUS_SUCCESS and X_STATUS_UNSUCCESSFUL need it too
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;  // RegisterFile::kRegisterCount

constexpr uint32_t kRegCpRbWptr = rex::graphics::XE_GPU_REG_CP_RB_WPTR;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

constexpr int kMaxIndirectDepth = 4;
// The real GPU waits as long as it takes; without one some conditions are never met, so the wait is cut
// short and the ring goes on.
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint16_t kMaxExtent = 2048 >> 3;  // xenos::kTexture2DCubeMaxWidthHeight >> 3
constexpr int32_t kFakeSampleCount = 1000;  // the SDK's query_occlusion_fake_sample_count
constexpr auto kReportEvery = std::chrono::seconds(10);

// Reads the ring (a power-of-two buffer of words that wraps) or an indirect buffer (linear).
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t end = 0;

  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t value = Peek();
    Skip(1);
    return value;
  }
  void Skip(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0: return false;
    case 0x1: return value < reference;
    case 0x2: return value <= reference;
    case 0x3: return value == reference;
    case 0x4: return value != reference;
    case 0x5: return value >= reference;
    case 0x6: return value > reference;
    default: return true;
  }
}

// Register base of each SET_CONSTANT / LOAD_ALU_CONSTANT type, or UINT32_MAX.
uint32_t ConstantBase(uint32_t type_index) {
  switch ((type_index >> 16) & 0xFF) {
    case 0: return 0x4000;  // ALU
    case 1: return 0x4800;  // fetch
    case 2: return 0x4900;  // bool
    case 3: return 0x4908;  // loop
    case 4: return 0x2000;  // registers
    default: return UINT32_MAX;
  }
}

class RingGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  RingGraphicsSystem() : registers_(kRegisterCount, 0) {}
  ~RingGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
#if MP3_DEKO3D
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
      provider_ = rex::deko3d::ProveedorDeko3d::Crear();  // a deko3d failure ends the process, logged
      if (!provider_) {
        REXLOG_ERROR("[gpu] could not create the deko3d device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[gpu] could not create the deko3d presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[gpu] deko3d presenter up (the game's frames come with the renderer)");
#else
    (void)app_context;
#endif
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ =
          memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this, &ReadMmio, &WriteMmio);
      if (!mmio_registered_) {
        REXLOG_ERROR("[gpu] could not register the GPU's MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
#if MP3_DEKO3D
    REXCVAR_SET(nfsmw_sombras_sin_vegetacion, false);
    // GTA IV tiles its G-buffer itself (bands of 256 rows through the window offset): drawn and resolved at their
    // screen position (SDK patch 0053), the bands below the first were empty otherwise.
    if (REXCVAR_GET(mp3_tiles_en_pantalla)) REXCVAR_SET(nfsmw_dk_tiles_en_pantalla, true);
    REXCVAR_SET(nfsmw_nativo_ps_solo_alfa, false);
    REXCVAR_SET(nfsmw_dk_sombras_sesgo_pitch, 960);
    // Telemetry (automatic captures, frame dumps, per-draw traces of light frames, CPU stack sampling) only with
    // telemetria.flag next to the NRO: it costs frames (the stack sampler stops every thread each millisecond).
    {
      std::error_code ec;
      telemetry_ = std::filesystem::exists(rex::filesystem::GetExecutableFolder() / "telemetria.flag", ec);
      const auto folder = rex::filesystem::GetExecutableFolder() / "logs" / "rex";
      const auto stack_flag = folder / "perfil_pilas.flag";
      // The profiler's stack sampling (rex_perfil.log: where each thread spends its time) is on from the start while
      // the port is brought up, telemetry or not (mp3_perfil_pilas = false turns it off: it costs ~8 %).
      if (REXCVAR_GET(mp3_perfil_pilas)) {
        std::filesystem::create_directories(folder, ec);
        if (FILE* flag = std::fopen(stack_flag.string().c_str(), "a")) std::fclose(flag);
      } else {
        std::filesystem::remove(stack_flag, ec);  // left by an earlier telemetry run
      }
      REXLOG_INFO("[gpu] telemetry {}", telemetry_ ? "on (telemetria.flag)" : "off");
      // The renderer's GPU profile of a frame every 20 s (a barrier and two timestamps around each of ~1300 draws, the
      // first draw of each pass drawn twice) and the frame structure traces (hundreds of log lines to the SD card once
      // a minute) are telemetry too: each was a visible hitch.
      if (!telemetry_) {
        REXCVAR_SET(nfsmw_dk_perfil_gpu_s, 0);
      }
      hide_glow_ = std::filesystem::exists(rex::filesystem::GetExecutableFolder() / "sin_resplandor.flag", ec);
      if (hide_glow_) REXLOG_INFO("[gpu] sin_resplandor.flag: lamp glows and coronas are not drawn");
    }
    // Before the game creates its shaders (the create hooks look the containers up in it).
    mp3::shaders::Global().Cargar(rex::filesystem::GetExecutableFolder() / "mp3_shaders.dslib");
#endif
    last_report_ = Clock::now();
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU vblank");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring");
    ring_thread_->Create();
    REXLOG_INFO("[gpu] ring reader started");
    // The startup logos: the game skips a movie it cannot open (as it does with the _RUS ones it looks for first).
    if (REXCVAR_GET(mp3_saltar_logos) && REXCVAR_GET(rex_archivos_ocultos).empty()) {
      REXCVAR_SET(rex_archivos_ocultos, std::string("movies\\epiclogo,movies\\mgslogo"));
      REXLOG_INFO("[gpu] startup logos skipped (mp3_saltar_logos)");
    }
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[gpu] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As in CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[gpu] ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_pointer_writeback_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    if (!active_.exchange(false)) {
      DestroyPresentation();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(ring_mutex_);
    }
    ring_cv_.notify_all();
    if (ring_thread_) {
      ring_thread_->Wait(0, 0, 0, nullptr);
      ring_thread_.reset();
    }
    if (vblank_thread_) {
      vblank_thread_->Wait(0, 0, 0, nullptr);
      vblank_thread_.reset();
    }
    DestroyPresentation();
  }

  // After the threads: the presenter goes before its provider, on the UI thread.
  void DestroyPresentation() {
    destinos_.reset();  // frees its deko3d memory before the device goes
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO registers ---------------------------------------------------------

  static uint32_t ReadMmio(void* ppc_context, void* context, uint32_t address) {
    (void)ppc_context;
    return static_cast<RingGraphicsSystem*>(context)->ReadMmioRegister(address);
  }

  static void WriteMmio(void* ppc_context, void* context, uint32_t address, uint32_t value) {
    (void)ppc_context;
    static_cast<RingGraphicsSystem*>(context)->WriteMmioRegister(address, value);
  }

  // The same fixed values as the SDK's emulation (graphics/graphics_system.cpp).
  uint32_t ReadMmioRegister(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // in vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width << 16) | height;
      }
      default:
        return Register(r);
    }
  }

  void WriteMmioRegister(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r < kRegisterCount) {
      registers_[r] = value;  // no side effects through MMIO (graphics_system.cpp)
    }
    if (r == kRegCpRbWptr) {
      write_pointer_.store(value, std::memory_order_seq_cst);
      // Only a sleeping ring thread needs the lock and the notify (each KickOff paid both, 3-5 % of the rendering
      // thread). seq_cst pairs with ring_dormido_ in RingLoop: either the ring's check sees this write pointer, or this
      // load sees it asleep and wakes it; taking the lock for an instant keeps the wakeup from being lost.
      if (ring_dormido_.load(std::memory_order_seq_cst)) {
        {
          std::lock_guard<std::mutex> lock(ring_mutex_);
        }
        ring_cv_.notify_one();
      }
    }
  }

  uint32_t Register(uint32_t index) const { return index < kRegisterCount ? registers_[index] : 0; }

  // Register writes from the ring, with the side effects the game relies on.
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) {
      return;
    }
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      value |= UINT32_C(0x80000000);
    }
#if MP3_DEKO3D
    // One tile (mp3_sin_tiling): Direct3D still sets the first tile's scissor, 1280x256, for the G-buffer pass, so
    // its draws and its resolves only covered the top band of the frame (the diagnostic views showed the G-buffer
    // there and black below). The renderer has no EDRAM size to fit: that scissor becomes the whole 1280x720 frame.
    // Any first band, not only 256 rows: the cutscenes came out as their top 256 rows and black below (run31,
    // cap_003); a 1280-wide window scissor shorter than the frame is a tile's (the other targets' are their own size).
    if (index == rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR && (value & 0x7FFF) == AnchoJuego() &&
        ((value >> 16) & 0x7FFF) > 0 && ((value >> 16) & 0x7FFF) < AltoJuego() && REXCVAR_GET(mp3_sin_tiling) &&
        REXCVAR_GET(mp3_tile_completo)) {
      const uint32_t filas = (value >> 16) & 0x7FFF;
      if (bandas_vistas_.insert(filas).second && bandas_vistas_.size() <= 16) {
        REXLOG_INFO("[gpu] one tile: window scissor {}x{} widened to {}x{}", AnchoJuego(), filas, AnchoJuego(),
                    AltoJuego());
      }
      value = (value & ~0x7FFF7FFFu) | (AltoJuego() << 16) | AnchoJuego();
    }
    // What the renderer caches per generation (PeticionDibujo): bumped only when a value really changes.
    if (registers_[index] != value) {
      if (index >= 0x4000 && index < 0x4800) {
        ++(index < 0x4400 ? generacion_constantes_vs_ : generacion_constantes_ps_);
      } else if (index >= 0x4800 && index <= 0x48BF) {
        ++generacion_fetch_;
      } else if (EsRegistroDeEncuadre(index)) {
        ++generacion_encuadre_;
      }
    }
#endif
    registers_[index] = value;
    // Scratch registers are mirrored to memory (SCRATCH_ADDR) for the ones SCRATCH_UMSK selects: the game
    // waits on those values to know where the GPU is.
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 && index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t address = registers_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
        GuestSeesGpuProgress();
      }
    }
  }

  // GTA IV's detail distance scale (mp3_distancia_lod): a float at 0x82A931BC, 1.0 in the image and written by
  // nothing, read by sub_821DFFE8 when it sets up each render phase's scan (scene, shadows, reflections). An entity is
  // drawn while its LOD distance times this scale exceeds its distance to the camera; the product also goes into
  // 0x82A931B4, which picks model detail levels and releases drawables. Written at each Swap when it changes.
  // The ring thread's own priority (mp3_prioridad_anillo) and the game threads' (mp3_perf_hooks.cpp), at each Swap
  // when they change.
  void AplicarPrioridades() {
    Mp3AplicarPrioridades();
#if defined(__SWITCH__)
    const int32_t pedida = REXCVAR_GET(mp3_prioridad_anillo);
    const int prioridad = pedida >= 44 && pedida <= 0x3B ? pedida : 0x2C;
    if (prioridad != prioridad_anillo_) {
      const bool hecho = R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, u32(prioridad)));
      if (prioridad_anillo_ || prioridad != 0x2C) {
        REXLOG_INFO("[gpu] prioridad del anillo 0x{:X}{}", prioridad, hecho ? "" : " (rechazada)");
      }
      prioridad_anillo_ = prioridad;
    }
#endif
  }
  int prioridad_anillo_ = 0;

  // GTA IV's population density multipliers, 1.0 in the image: random and parked cars (set by the
  // SET_*CAR_DENSITY_MULTIPLIER natives, reset in sub_823A5330 / sub_822D7188), peds (sub_823F2AB0) and scenario peds
  // (sub_823F2AC0; all three reset in sub_823F4860). The game and the scripts write them, so they are watched at each
  // Swap: a value that is not the one written here is the game's, and it is stored back times mp3_densidad_coches /
  // mp3_densidad_peatones. A script's 0 stays 0.
  struct Densidad {
    uint32_t direccion;
    bool coches;
    uint32_t escrita = 0xFFFFFFFFu;  // the bits last stored here (none yet)
    float pedida = 1.0f;             // the game's own value
    float escala = -1.0f;
  };
  Densidad densidades_[5] = {{0x82AA1888, true}, {0x82AA188C, true}, {0x82AA2940, false}, {0x82AA2944, false},
                             {0x82AA2948, false}};
  void AplicarDensidades() {
    const float coches = float(std::clamp(double(REXCVAR_GET(mp3_densidad_coches)), 0.0, 1.0));
    const float peatones = float(std::clamp(double(REXCVAR_GET(mp3_densidad_peatones)), 0.0, 1.0));
    for (Densidad& d : densidades_) {
      uint8_t* p = memory_->TranslateVirtual<uint8_t*>(d.direccion);
      const uint32_t actual = rex::memory::load_and_swap<uint32_t>(p);
      const float escala = d.coches ? coches : peatones;
      if (actual != d.escrita) {
        std::memcpy(&d.pedida, &actual, 4);
      } else if (escala == d.escala) {
        continue;
      }
      const float nueva = d.pedida * escala;
      uint32_t bits;
      std::memcpy(&bits, &nueva, 4);
      rex::memory::store_and_swap<uint32_t>(p, bits);
      if (d.escrita == 0xFFFFFFFFu || escala != d.escala) {
        REXLOG_INFO("[mp3] densidad en {:08X}: {} x {} = {}", d.direccion, d.pedida, escala, nueva);
      }
      d.escrita = bits;
      d.escala = escala;
    }
  }

  // GTA IV's addresses (above and below): off until Max Payne 3's are found.
  static constexpr bool kDireccionesDeGta4 = false;
  static constexpr uint32_t kEscalaDistanciaLod = 0x82A931BC;
  void AplicarDistanciaLod() {
    const float escala = float(std::clamp(double(REXCVAR_GET(mp3_distancia_lod)), 0.3, 2.0));
    if (escala == distancia_lod_escrita_) {
      return;
    }
    uint32_t bits;
    std::memcpy(&bits, &escala, 4);
    rex::memory::store_and_swap<uint32_t>(memory_->TranslateVirtual<uint8_t*>(kEscalaDistanciaLod), bits);
    distancia_lod_escrita_ = escala;
    REXLOG_INFO("[gpu] escala de la distancia de detalle: {}", escala);
  }
  float distancia_lod_escrita_ = -1.0f;

  // `n` consecutive registers from the packet (type 0, SET_CONSTANT, SET_SHADER_CONSTANTS). A run wholly inside the
  // vertex shader, pixel shader or fetch constants has no side effect but its generation, which the renderer only
  // compares for equality: one loop without WriteRegister's checks, the generation bumped once if a value changed
  // (mp3_registros_en_bloque). The ring thread's packet loop was ~8 % of a core in run50, most of it in this.
  void WriteRegisters(uint32_t index, Reader& data, uint32_t n) {
#if MP3_DEKO3D
    const uint32_t end = index + n;
    uint64_t* generation = nullptr;
    if (index >= 0x4000 && end <= 0x4400) {
      generation = &generacion_constantes_vs_;
    } else if (index >= 0x4400 && end <= 0x4800) {
      generation = &generacion_constantes_ps_;
    } else if (index >= 0x4800 && end <= 0x48C0) {
      generation = &generacion_fetch_;
    }
    if (generation && n && REXCVAR_GET(mp3_registros_en_bloque)) {
      uint32_t* const regs = registers_.data() + index;
      uint32_t changed = 0;
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t value = data.Read();
        changed |= regs[i] ^ value;
        regs[i] = value;
      }
      if (changed) {
        ++*generation;
      }
      return;
    }
#endif
    for (uint32_t i = 0; i < n; ++i) {
      WriteRegister(index + i, data.Read());
    }
  }

  // The guest can see the GPU move on (a fence value written to memory, a scratch register mirrored, an interrupt):
  // from there it may rewrite vertex and index memory the GPU already drew from. Dead Space 2 refills one dynamic
  // vertex buffer for its translucent draws this way, several times a frame at the same address. The renderer keeps
  // copies of ranges it already uploaded (in the work slot, and per epoch in the geometry store) and forgets them
  // when this counter moves; without it every translucent batch after the first was drawn with the first one's
  // vertices: the huge triangles across the screen.
  static void GuestSeesGpuProgress() {
#if MP3_DEKO3D
    rex::x360::g_sincronizaciones_anillo.fetch_add(1, std::memory_order_relaxed);
#endif
  }

  // --- GPU memory (the two low bits of the address are the byte order) --------

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    GuestSeesGpuProgress();
    const uint32_t swapped = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &swapped, sizeof(swapped));
  }

  // --- Threads ----------------------------------------------------------------

  // Creates the deko renderer backend on first use (on the ring thread, as the device allows). The game plugs in no
  // effects, so it uses the default hooks.
  bool EnsureRenderer() {
    if (destinos_) {
      return true;
    }
    if (!provider_ || !presenter_) {
      return false;
    }
    destinos_ = rex::x360::CrearDestinosDeko3d(
        static_cast<rex::deko3d::ProveedorDeko3d*>(provider_.get()), memory_, GanchosImagen());
    if (destinos_) {
      // The scene's width for the passes' categories and the sky deferral (the frame's size, AnchoJuego).
      rex::x360::g_ancho_escena.store(AnchoJuego(), std::memory_order_relaxed);
      REXLOG_INFO("[gpu] deko3d renderer backend up (minimal renderer: draws, resolves and the Swap's frontbuffer); "
                  "frame {}x{}", AnchoJuego(), AltoJuego());
    }
    return destinos_ != nullptr;
  }

  // Runs the game's interrupt callback on the calling thread (one of ours, an XHostThread).
  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      // After a long pause (loading, a debugger) the missed vblanks are not replayed.
      if (now - next > std::chrono::milliseconds(250)) {
        next = now;
      }
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int RingLoop() {
    uint32_t read = 0;
    uint32_t generation = 0;
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_dormido_.store(true, std::memory_order_seq_cst);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(pending_queries_.empty() ? 4 : 1), [&] {
          return !active_.load(std::memory_order_acquire) ||
                 write_pointer_.load(std::memory_order_seq_cst) != read ||
                 ring_generation_.load(std::memory_order_acquire) != generation;
        });
        ring_dormido_.store(false, std::memory_order_relaxed);
      }
      if (!active_.load(std::memory_order_acquire)) {
        break;
      }
      Report();
      OcclusionPoll();
      ProbePoll();
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) {
        continue;
      }
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) {
        continue;
      }
      ring_activity_ = std::chrono::steady_clock::now();
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      // The read pointer goes back to the game during a batch too, every kRptrPaso words, as the real command
      // processor does: Direct3D waits for ring space on it (sub_8298C2A0), and a whole batch (with its WAIT_REG_MEMs)
      // used to pass before any space came back. What is behind reader.pos has been read already.
      const uint32_t writeback_lote = read_pointer_writeback_.load(std::memory_order_acquire);
      uint32_t publicado = read;
      while (reader.Pending()) {
        if (!Packet(reader, 0)) {
          break;  // partial packet: the rest comes with the next CP_RB_WPTR
        }
        if (writeback_lote && ((reader.pos - publicado) & reader.mask) >= kRptrPaso) {
          rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback_lote), reader.pos);
          publicado = reader.pos;
        }
      }
      read = reader.pos;
#if MP3_DEKO3D
      if (destinos_) {
        destinos_->EsperarSubidas();
      }
#endif
      const uint32_t writeback = read_pointer_writeback_.load(std::memory_order_acquire);
      if (writeback) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
        // Not a GuestSeesGpuProgress: it is written after every batch of the ring, and each one made the renderer's
        // geometry store check every range again (an epoch). Ring memory comes back only after a wrap, frames later.
      }
    }
    return 0;
  }

  // One line every kReportEvery, on the ring thread, so the log shows whether the game is advancing.
  void Report() {
    const auto now = Clock::now();
    if (now - last_report_ < kReportEvery) {
      return;
    }
    const double seconds = std::chrono::duration<double>(now - last_report_).count();
    last_report_ = now;
    REXLOG_INFO("[gpu] {:.1f} s: {} swaps ({:.1f}/s), {} draws, {} packets, {} vblanks, {} waits cut short",
                seconds, swaps_ - reported_swaps_, double(swaps_ - reported_swaps_) / seconds,
                draws_ - reported_draws_, packets_ - reported_packets_,
                vblanks_.load(std::memory_order_relaxed), waits_cut_short_);
    reported_swaps_ = swaps_;
    reported_draws_ = draws_;
    reported_packets_ = packets_;
    REXLOG_INFO("[gpu] ring waits: {} WAIT_REG_MEM waited {:.1f} ms in total", reg_mem_waits_,
                double(reg_mem_wait_ns_) / 1e6);
    reg_mem_waits_ = 0;
    reg_mem_wait_ns_ = 0;
#if MP3_DEKO3D
    const mp3::shaders::Estadisticas s = mp3::shaders::Global().estadisticas();
    REXLOG_INFO("[gpu] shaders: {} loads (address {}, address refused {}, learned {}, microcode {}, similar {}, "
                "unknown {}); objects {} ({} not in the library, {} without a microcode address)",
                s.cargas, s.por_direccion, s.direccion_rechazada, s.por_aprendizaje, s.por_microcodigo,
                s.por_parecido, s.desconocidas,
                s.objetos, s.objetos_sin_entrada, s.objetos_sin_direccion);
    // What the draws sent to the renderer target and draw (ColorRenderTargetFormat of each written color
    // target, DepthRenderTargetFormat when depth is on, primitive type): the formats it does not handle yet show
    // up here with how many draws they cost.
    std::string formats;
    for (uint32_t f = 0; f < 16; ++f) {
      if (color_formats_[f]) formats += fmt::format(" c{}:{}", f, color_formats_[f]);
    }
    for (uint32_t f = 0; f < 2; ++f) {
      if (depth_formats_[f]) formats += fmt::format(" z{}:{}", f, depth_formats_[f]);
    }
    std::string primitives;
    for (uint32_t t = 0; t < 32; ++t) {
      if (primitive_types_[t]) primitives += fmt::format(" {}:{}", t, primitive_types_[t]);
    }
    std::string orders;
    static constexpr const char* kOrder[4] = {"none", "8in16", "8in32", "16in32"};
    for (uint32_t k = 0; k < 8; ++k) {
      if (index_orders_[k]) orders += fmt::format(" {}-bit {}:{}", k < 4 ? 16 : 32, kOrder[k & 3], index_orders_[k]);
    }
    index_orders_ = {};
#if MP3_DEKO3D
    const uint32_t syncs = rex::x360::g_sincronizaciones_anillo.load(std::memory_order_relaxed);
    const uint32_t syncs_interval = syncs - syncs_reported_;
    syncs_reported_ = syncs;
#else
    const uint32_t syncs_interval = 0;
#endif
    REXLOG_INFO("[gpu] draw targets:{}; primitive types:{}; indices:{}; {} emulated clears, {} multisampled draws drawn "
                "at 1x; {} guest-visible GPU progress points (renderer copies forgotten)", formats, primitives, orders,
                clears_, msaa_draws_, syncs_interval);
    color_formats_ = {};
    depth_formats_ = {};
    primitive_types_ = {};
    uint64_t dibujados = 0, rechazados = 0;
    if (destinos_) {
      const rex::x360::EstadisticasDibujos e = destinos_->EstadisticasDeDibujos();
      dibujados = e.dibujados;
      rechazados = e.rechazados;
    }
#if defined(__SWITCH__)
    {
      // The process's memory (title takeover gives ~3.2 GB): Gears of War 2 ran out of it loading its first level.
      u64 used = 0, total = 0;
      svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      REXLOG_INFO("[mem] process memory: {} MB used of {} MB", used >> 20, total >> 20);
    }
#endif
    mp3_log_main_thread();
    REXLOG_INFO("[gpu] draws: {} geometry, {} sent to the renderer: {} drawn, {} rejected by it; skipped here: "
                "{} without VS, {} without PS (the renderer logs each rejection cause once)",
                geometria_, enviados_, dibujados, rechazados, sin_vs_, sin_ps_);
    if (destinos_) {
      uint64_t q[5];
      destinos_->EstadisticasOclusion(q);
      REXLOG_INFO("[gpu] occlusion: {} queries counted on the GPU, {} without room; {} answered with their count ({} "
                  "of them hidden, {:.1f} ms after their end on average, {} samples, max {}), {} answered visible when "
                  "the game waited for them, {} after waiting too long, {} waiting",
                  q[0], q[1], queries_answered_, queries_hidden_,
                  queries_answered_ ? double(queries_wait_us_) / 1000.0 / double(queries_answered_) : 0.0, q[3], q[4],
                  queries_not_waited_, queries_timed_out_, pending_queries_.size());
    }
    // Which unknown vertex shaders the skipped draws had, the most frequent first, with one draw's state each.
    std::vector<std::pair<uint64_t, uint64_t>> skips(unknown_vs_skips_.begin(), unknown_vs_skips_.end());
    std::sort(skips.begin(), skips.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < skips.size() && i < 4; ++i) {
      REXLOG_INFO("[gpu] skipped without VS: {} draws with microcode {:016X}: {}", skips[i].second, skips[i].first,
                  unknown_vs_sample_[skips[i].first]);
    }
#endif
  }

  // --- PM4 packets ------------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not complete yet.
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0: count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0; break;
      case 1: count = 2; break;
      case 2: count = 0; break;
      default: count = ((packet >> 16) & 0x3FFF) + 1; break;
    }
    if (reader.Pending() < count + 1) {
      return false;
    }
    // The game has just written these words from another core: the next lines are asked for ahead of the loads.
    __builtin_prefetch(reader.base + size_t((reader.pos + 64) & (reader.mask ? reader.mask : ~0u)) * 4);
    reader.Skip(1);
    ++packets_;
    Reader data = reader;
    reader.Skip(count);
    if (!packet) {
      return true;
    }
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool one_register = (packet >> 15) & 0x1;
        if (!one_register) {
          WriteRegisters(index, data, count);
          break;
        }
        for (uint32_t i = 0; i < count; ++i) {
          WriteRegister(index, data.Read());
        }
        break;
      }
      case 1: {
        const uint32_t value1 = data.Read();
        const uint32_t value2 = data.Read();
        WriteRegister(packet & 0x7FF, value1);
        WriteRegister((packet >> 11) & 0x7FF, value2);
        break;
      }
      case 2:
        break;
      default:
        PacketType3(data, packet, count, depth);
        break;
    }
    return true;
  }

  void PacketType3(Reader& data, uint32_t packet, uint32_t count, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    // The traced frame (ZL + ZR + Left, automatic dumps): every type 3 packet but the register writes, with its
    // predicate and the bins, to see where a frame's commands go (Gears of War 2's scene never reached a draw).
    if (draw_tracing_ && packet_trace_n_ < 6000 && opcode != xenos::PM4_SET_CONSTANT &&
        opcode != xenos::PM4_SET_SHADER_CONSTANTS) {
      ++packet_trace_n_;
      const uint32_t w0 = count > 0 ? data.Peek() : 0;
      REXLOG_INFO("[pm4] d{} op {:02X} n {} pred {} sel {:X} mask {:X} w0 {:08X}{}", depth, opcode, count,
                  packet & 0x1, bin_select_, bin_mask_, w0,
                  (packet & 0x1) && !(bin_select_ & bin_mask_) && bin_mask_ != 0x80000000ull ? " SKIPPED" : "");
    }
    // Experiment (off): packets with the bin mask 80000000 run in every tile. In GTA IV those are event writes, not
    // draws (the draws carry 8000003F); run in every tile they multiplied the frame's copies and clears until the GPU
    // timed out. The tiling itself is turned off in the game (src/mp3_tiling.cpp).
    const bool any_bin = (bin_select_ & bin_mask_) ||
                         (bin_mask_ == 0x80000000ull && REXCVAR_GET(mp3_bins_visibles));
    if ((packet & 0x1) && (!any_bin || opcode == xenos::PM4_XE_SWAP)) {
#if MP3_DEKO3D
      ++frame_.predicated_skipped;
#endif
      return;  // predicate fails; predicated Swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (count < 1) break;
        const uint32_t cpus = data.Read();
        GuestSeesGpuProgress();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP:
#if MP3_DEKO3D
        FrameTraceSwap();
        ++frame_serial_;
#endif
        // VdSwap writes "SWAP", the frontbuffer's physical address and its size. The renderer presents whatever is
        // resolved (clears and, once the ring decoder feeds draws, the frame). Handheld 1280x720.
        ++swaps_;
        // GTA IV's detail distance and population density live at GTA IV's addresses: not written in Max Payne 3
        // until its own are found (kDireccionesDeGta4).
        if (kDireccionesDeGta4) {
          AplicarDistanciaLod();
          AplicarDensidades();
        }
        AplicarPrioridades();
        counter_.fetch_add(1, std::memory_order_relaxed);
        if (!depth_vs_frame_.empty()) {
          depth_vs_last_.swap(depth_vs_frame_);
          depth_vs_frame_.clear();
        }
        if (EnsureRenderer()) {
          // VdSwap describes the frontbuffer in fetch constant 0; an empty TexturaSwap finds nothing (black).
          rex::x360::TexturaSwap swap_texture = rex::x360::LeerTexturaSwap(registers_.data());
          ViewOverride(swap_texture);
          destinos_->Presentar(presenter_.get(), swap_texture, AnchoJuego(), AltoJuego());
          if (REXCVAR_GET(switch_omitir_pedido) && REXCVAR_GET(mp3_velocidad_prueba)) {
            REXCVAR_SET(switch_omitir_pedido, false);
            REXCVAR_SET(mp3_velocidad_sin_z, !REXCVAR_GET(mp3_velocidad_sin_z));
            REXLOG_INFO("[gpu] velocity draws {} (the next capture shows it)",
                        REXCVAR_GET(mp3_velocidad_sin_z) ? "WITHOUT depth test" : "with their depth test");
            tree_capture_after_ = 3;
          }
          if (REXCVAR_GET(switch_omitir_pedido) && REXCVAR_GET(mp3_arboles_prueba)) {
            REXCVAR_SET(switch_omitir_pedido, false);
            REXCVAR_SET(mp3_arboles, (REXCVAR_GET(mp3_arboles) + 1) % 5);
            static constexpr const char* kModos[5] = {"not drawn", "drawn as they are", "drawn with identity sway matrices",
                                                      "identity sway matrices and c10 = 0", "c10 = 0 only"};
            REXLOG_INFO("[gpu] trees mode {}: {} (the next capture shows it)", REXCVAR_GET(mp3_arboles),
                        kModos[REXCVAR_GET(mp3_arboles)]);
            tree_capture_after_ = 3;
          }
          if (tree_capture_after_ && --tree_capture_after_ == 0) {
            SaveCapture();
            REXLOG_INFO("[gpu] capture above: trees mode {}, velocity depth test {}", REXCVAR_GET(mp3_arboles),
                        REXCVAR_GET(mp3_velocidad_sin_z) ? "off" : "on");
          }
          if (REXCVAR_GET(switch_omitir_pedido)) {
            REXCVAR_SET(switch_omitir_pedido, false);
            skip_mode_ = ((skip_mode_ ? skip_mode_ : skip_resume_) + 1) % kSkipModes;
            skip_resume_ = 0;
            if (REXCVAR_GET(mp3_busqueda_directa) && skip_mode_ > 0 && skip_mode_ < kSkipModesFixed) {
              skip_mode_ = kSkipModesFixed;
            }
            if (skip_mode_ == kSkipModesFixed) {
              // The candidates: last frame's depth-writing vertex shaders, the most frequent first.
              std::vector<std::pair<uint32_t, uint32_t>> list(depth_vs_last_.begin(), depth_vs_last_.end());
              std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
              search_vs_.clear();
              std::string names;
              for (const auto& [vs, draws] : list) {
                if (search_vs_.size() == kSearchModes) break;
                search_vs_.push_back(vs);
                names += fmt::format(" n{} ({})", vs, draws);
              }
              search_ps_ = REXCVAR_GET(mp3_busqueda_ps);
              REXLOG_INFO("[gpu] search: {} {} shaders:{}", search_vs_.size(),
                          search_ps_ ? "base-pass pixel" : "depth-writing vertex", names);
            }
            if (skip_mode_ >= kSkipModesFixed && skip_mode_ - kSkipModesFixed >= search_vs_.size()) {
              skip_mode_ = 0;
            }
            SetNfsShortcuts(skip_mode_ != 1);
            REXCVAR_SET(nfsmw_dk_todas_las_salidas, skip_mode_ == 9);
            REXCVAR_SET(nfsmw_dk_gamma_lineal, skip_mode_ == 10);
            REXLOG_INFO("[gpu] skip mode {}: {} (the next capture shows it)", skip_mode_, SkipModeName());
            capture_after_skip_ = 3;  // a few frames later, when the change is on screen
          }
          if (capture_after_skip_ && --capture_after_skip_ == 0) {
            SaveCapture();
            REXLOG_INFO("[gpu] capture above: skip mode {} ({})", skip_mode_, SkipModeName());
            // Gears of War 2: the mode stays on screen until the next press (one frame was too short to see it).
          }
          if (REXCVAR_GET(switch_volcado_pedido)) {
            REXCVAR_SET(switch_volcado_pedido, false);
            destinos_->PedirVolcado();
            // And every draw of that frame in the log, whatever its size (FrameTraceSwap ends it at the next Swap), when
            // mp3_volcado_con_dibujos asks: a 2400-draw frame of Max Payne 3 took a minute of SD writes.
            draw_tracing_ = REXCVAR_GET(mp3_volcado_con_dibujos);
            draw_trace_n_ = 0;
            draw_trace_cap_ = 8000;
            packet_trace_n_ = 0;
            // And the frame's structure (each pass with its targets, scissor, viewport, window offset; each resolve).
            tracing_ = true;
            trace_lines_ = 0;
            trace_pass_ = 0;
            trace_key_.fill(0);
            trace_pass_draws_ = 0;
            trace_max_lines_ = 3000;
            REXLOG_INFO("[draws] full trace of the next frame (ZL + ZR + Left)");
          }
          AutomaticCollection();
          if (REXCVAR_GET(switch_captura_pedida)) {
            REXCVAR_SET(switch_captura_pedida, false);
            SaveCapture();
          }
        }
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (count < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxIndirectDepth) {
          Reader indirect;
          indirect.base = memory_->TranslatePhysical(address);
          indirect.end = length;
          while (indirect.Pending() && Packet(indirect, depth + 1)) {
          }
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (count < 5) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const auto start = Clock::now();
        const auto limit = start + kWaitRegMemMax;
        const auto yield_until = start + std::chrono::milliseconds(1);
        bool waited = false;
        for (;;) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (Register(poll) & UINT32_C(0x80000000))) {
            registers_[poll] = 0;  // MakeCoherent: there is no GPU cache to flush
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, reference)) {
            break;
          }
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (waits_cut_short_++ == 0) {
              REXLOG_WARN("[gpu] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X}), going on",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          waited = true;
          // The first millisecond yielding instead of sleeping: a sleep on Horizon often lasts far longer than asked,
          // and the game's main thread waits on the GPU fences behind this wait (D3D BlockOnFence in its profile).
          if (Clock::now() < yield_until) {
            rex::thread::MaybeYield();
          } else {
            rex::thread::Sleep(std::chrono::microseconds(100));
          }
        }
        if (waited) {
          ++reg_mem_waits_;
          reg_mem_wait_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (count < 3) break;
        const uint32_t info = data.Read();
        const uint32_t and_value = data.Read();
        const uint32_t or_value = data.Read();
        uint32_t value = Register(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? Register(and_value & 0x1FFF) : and_value;
        value |= ((info >> 30) & 0x1) ? Register(or_value & 0x1FFF) : or_value;
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (count < 2) break;
        const uint32_t reg = data.Read();
        const uint32_t address = data.Read();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (count < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < count; ++i) {
          WriteMemory(address, data.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (count < 6) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const uint32_t target = data.Read();
        const uint32_t value_to_write = data.Read();
        const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, value & mask, reference)) {
          if (info & 0x100) {
            WriteMemory(target, value_to_write);
          } else {
            WriteRegister(target, value_to_write);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (count < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (count < 3) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        const uint32_t value = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed) : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (count < 2) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Screen extent of the previous draw: the maximum, as the SDK does.
        const uint16_t extent[] = {0, kMaxExtent, 0, kMaxExtent, 0, 1};
        uint8_t* target = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extent); ++i) {
          rex::memory::store_and_swap<uint16_t>(target + i * 2, extent[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (count < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        OcclusionQuery(Register(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR));
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (count < 1) break;
        const uint32_t type_index = data.Read();
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          WriteRegisters(base + (type_index & 0x7FF), data, count - 1);
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // First word: the register index; the rest is written consecutively.
        if (count < 1) break;
        WriteRegisters(data.Read() & 0xFFFF, data, count - 1);
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (count < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t n = data.Read() & 0xFFF;
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 0; i < n; ++i) {
            WriteRegister(index + i, rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // When a query ends it is reported as visible, in case the game reads the result (as the SDK does).
        if (count < 1) break;
        const uint32_t word = data.Read();
        const uint32_t id = word & 0x3F;
        if (!(word & 0x100)) {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t reg = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                       : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          registers_[reg] |= UINT32_C(1) << (id < 32 ? id : id - 32);
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (count < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (count < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (count < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (count < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (count < 2) break;
        const uint64_t high = data.Read();
        bin_mask_ = (high << 32) | data.Read();
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (count < 2) break;
        const uint64_t high = data.Read();
        bin_select_ = (high << 32) | data.Read();
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        ++draws_;
#if MP3_DEKO3D
        // DRAW_INDX starts with the visibility query word. Then the initiator and, for indices in memory (DMA),
        // their base and size, kept in their registers as the SDK's command processor does: the renderer reads
        // everything from the register mirror.
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (count < skip + 1) break;
        if (skip) data.Read();
        const uint32_t initiator = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (count < skip + 3) break;
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, data.Read());
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, data.Read());
        }
        if (!EnsureRenderer()) break;
        // A copy-mode draw is a resolve and/or a clear.
        if (rex::x360::EsCopia(registers_.data())) {
          ResolveCatalog();
          FrameTraceCopy();
          if (draw_tracing_ && draw_trace_n_ < draw_trace_cap_) {
            namespace g = rex::graphics;
            const uint32_t control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
            REXLOG_INFO("[draws]   copy: source {} (c0 {:03X} z {:03X}) pitch {} clear color {} depth {} command {} -> "
                        "{:08X} pitch {} height {} format {}",
                        control & 0x7, Register(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF,
                        Register(g::XE_GPU_REG_RB_DEPTH_INFO) & 0xFFF,
                        Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF, (control >> 8) & 1, (control >> 9) & 1,
                        (control >> 20) & 3, Register(g::XE_GPU_REG_RB_COPY_DEST_BASE),
                        Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH) & 0x3FFF,
                        (Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH) >> 16) & 0x3FFF,
                        (Register(g::XE_GPU_REG_RB_COPY_DEST_INFO) >> 7) & 0x3F);
          }
          rex::x360::RegistrosCopia copia = rex::x360::LeerRegistrosCopia(registers_.data());
          ResolveWholeTile(copia);
          ResolveThroughMsaaAlias(copia);
          destinos_->Copiar(copia);
          RecordResolveForView(copia);
          break;
        }
        Draw();
#else
        (void)data;
#endif
        break;
      }
#if MP3_DEKO3D
      case xenos::PM4_IM_LOAD: {
        // Microcode in GPU memory; the type (0 VS, 1 PS) in the two low bits of the address.
        if (count < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t words = data.Read() & 0xFFFF;
        const uint32_t type = address_type & 0x3;
        if (type > 1 || !words) break;
        const uint8_t* source = memory_->TranslatePhysical(address_type & ~uint32_t(0x3));
        microcode_.resize(words);
        for (uint32_t i = 0; i < words; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
        }
        ShaderLoaded(type, address_type & ~uint32_t(0x3));
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // Microcode inside the packet (D3D's copy path, patched there).
        if (count < 2) break;
        const uint32_t type = data.Read() & 0x3;
        const uint32_t words = data.Read() & 0xFFFF;
        if (type > 1 || !words || words > count - 2) break;
        microcode_.resize(words);
        for (uint32_t i = 0; i < words; ++i) {
          microcode_[i] = data.Read();
        }
        ShaderLoaded(type, 0);
        break;
      }
#endif
      default:
        // IM_LOAD, IM_LOAD_IMMEDIATE, INVALIDATE_STATE, NOP...: nothing the game waits for.
        break;
    }
  }

#if MP3_DEKO3D
  // PA_SC_WINDOW_OFFSET..SCISSOR_BR, PA_CL_VPORT_*, PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL and
  // PA_SU_VTX_CNTL: the viewport/scissor state the renderer caches (as the SDK's command processor).
  static bool EsRegistroDeEncuadre(uint32_t index) {
    if (index - 0x2080 > 0x2302 - 0x2080) return false;
    return index <= 0x2082 || (index >= 0x210F && index <= 0x2114) || (index >= 0x2204 && index <= 0x2206) ||
           index == 0x2302;
  }

  // --- Draw-skipping modes (ZL + ZR + Right), to find which kind of draw makes an artifact: each press goes to the next
  // mode, logs it and takes a capture a few frames later. Classified by the blend of target 0 (RB_BLENDCONTROL0) and the
  // primitive, as the frame traces show Dead Space 2's draws.
  static constexpr uint32_t kSkipModesFixed = 11;
  // Then the search modes: kSkipModesFixed + i skips only the i-th vertex shader of the scene's depth-writing draws
  // (the most frequent first, as counted in the frame before the first search mode), to find which one draws the
  // giant triangles of level 2. After the last one, back to normal rendering.
  static constexpr uint32_t kSearchModes = 128;
  static constexpr uint32_t kSkipModes = kSkipModesFixed + kSearchModes;
  static constexpr const char* kSkipModeNames[kSkipModesFixed] = {
      "none (normal rendering)",
      "SAFE MODE: the renderer's NFS shortcuts off, nothing skipped",
      "Gears of War 2: modulated shadows and other multiplying draws (source factor DST_COLOR, dest ZERO)",
      "Gears of War 2: lights (everything drawn into 2D0 as format 12) and the multiplying draws",
      "Gears of War 2: everything after the base pass (draws not into 2D0 format 3 with blend 00010001)",
      "additive effects (SRC_ALPHA+ONE 00010106, ONE+ONE 01010101)",
      "everything blended (blend other than 00010001)",
      "alpha-tested (RB_COLORCONTROL alpha test on)",
      "Gears of War 2: no depth prepass (already the default, mp3_sin_prepaso_z; here regardless of it)",
      "Gears of War 2: vertex shaders with every interpolator (nfsmw_dk_todas_las_salidas)",
      "Gears of War 2: gamma textures read as linear (nfsmw_dk_gamma_lineal; textures loaded from now on)",
  };
  // The renderer's NFS shortcuts (see the declarations at the top): on as the SDK defaults, off in safe mode.
  static void SetNfsShortcuts(bool on) {
    REXCVAR_SET(nfsmw_nativo_saltar_invisibles, on);
    REXCVAR_SET(nfsmw_nativo_sin_ps_sin_color, on);
    REXCVAR_SET(nfsmw_nativo_sin_desenfoque, on);
    REXCVAR_SET(nfsmw_nativo_pcf_barato, on);
    REXCVAR_SET(nfsmw_nativo_sombra_minimo, on);
    REXCVAR_SET(nfsmw_nativo_z_temprana, on);
    REXCVAR_SET(nfsmw_nativo_cielo_aplazado, on);
    REXCVAR_SET(nfsmw_dk_constantes_por_diferencias, on);
    REXCVAR_SET(nfsmw_nativo_saltar_borrados_repetidos, on);
    REXCVAR_SET(nfsmw_nativo_resolver_sin_copia, on);
    REXCVAR_SET(nfsmw_nativo_intercambiar_sin_borrado, on);
    REXCVAR_SET(nfsmw_nativo_frontal_perezoso, on);
    REXCVAR_SET(nfsmw_dk_borrar_area_util, on);
    REXCVAR_SET(nfsmw_dk_borrado_color_aplazado, on);
    REXCVAR_SET(nfsmw_dk_borrado_z_aplazado, on);
  }

  std::string SkipModeName() const {
    if (skip_mode_ < kSkipModesFixed) return kSkipModeNames[skip_mode_];
    return fmt::format("search {} of {}: {} shader n{} not drawn", skip_mode_ - kSkipModesFixed + 1,
                       search_vs_.size(), search_ps_ ? "pixel" : "vertex", search_vs_[skip_mode_ - kSkipModesFixed]);
  }

  bool SkippedByMode() const {
    namespace g = rex::graphics;
    if (skip_mode_ >= kSkipModesFixed) {
      const auto* shader = search_ps_ ? ps_ : vs_;
      return shader && shader->numero == search_vs_[skip_mode_ - kSkipModesFixed];
    }
    const uint32_t blend = Register(g::XE_GPU_REG_RB_BLENDCONTROL0);
    const uint32_t mask = Register(g::XE_GPU_REG_RB_COLOR_MASK);
    const uint32_t prim = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR) & 0x3F;
    const uint32_t colorctl = Register(g::XE_GPU_REG_RB_COLORCONTROL);
    const uint32_t color0_base = Register(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF;
    const uint32_t color0_format = (Register(g::XE_GPU_REG_RB_COLOR_INFO) >> 16) & 0xF;
    // Color = source * destination: Gears of War 2's modulated shadows (01000008) multiply the scene by the shadow.
    const bool multiplying = (mask & 0xF) && (blend & 0x1F) == 8 && ((blend >> 8) & 0x1F) == 0;
    (void)prim;
    switch (skip_mode_ - 1) {  // mode 1 is the safe mode, which skips nothing
      case 1: return multiplying;
      case 2: return multiplying || (color0_base == 0x2D0 && color0_format == 12);
      case 3: return (mask & 0xF) && !(color0_base == 0x2D0 && color0_format == 3 && blend == 0x00010001);
      case 4: return (mask & 0xF) && (blend == 0x00010106 || blend == 0x01010101);
      case 5: return (mask & 0xF) && blend != 0x00010001;
      case 6: return ((colorctl >> 3) & 0x1) && (colorctl & 0x7) != 7;
      case 7:
        return (Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5 &&
               (Register(g::XE_GPU_REG_RB_DEPTH_INFO) & 0xFFF) == 0 && (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x4);
      default: return false;
    }
  }

  // Level 2's giant triangles (found with the search modes): this vertex shader picks its matrices from the float
  // constants by an index it computes from a per-vertex value (fetch r1, 32_32_32_32_FLOAT at offset 32 of stream 31.1:
  // a0 = ((r1.x + c10.x) mod 3) * 4, then c11..c14 + a0). What it is fed, for the first draws.
  static constexpr uint64_t kBrokenSkinVs = UINT64_C(0x38234053372F8F2F);
  void LogBrokenSkinDraw() {
    if (broken_vs_logged_ >= 6) return;
    ++broken_vs_logged_;
    namespace g = rex::graphics;
    const uint32_t v = 0x4800 + 31 * 6;
    std::string text = fmt::format("[gpu] skin VS n{} draw: fetch 31 {:08X} {:08X} | {:08X} {:08X} | {:08X} {:08X}; "
                                   "index offset {} prim {:08X}; constants",
                                   vs_->numero, Register(v), Register(v + 1), Register(v + 2), Register(v + 3),
                                   Register(v + 4), Register(v + 5), Register(g::XE_GPU_REG_VGT_INDX_OFFSET),
                                   Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR));
    for (uint32_t c = 10; c <= 26; ++c) {
      const uint32_t r = 0x4000 + c * 4;
      text += fmt::format(" c{}=({:g} {:g} {:g} {:g})", c, std::bit_cast<float>(Register(r)),
                          std::bit_cast<float>(Register(r + 1)), std::bit_cast<float>(Register(r + 2)),
                          std::bit_cast<float>(Register(r + 3)));
    }
    // Stream 31.1's first vertices, bytes 32-47 (the index and the two weights), as stored (big-endian floats).
    const uint32_t base = Register(v + 2) & ~0x3u;
    if (const uint8_t* m = base ? memory_->TranslatePhysical(base) : nullptr) {
      text += "; stream 31.1 at +32:";
      for (uint32_t i = 0; i < 6; ++i) {
        text += " (";
        for (uint32_t k = 0; k < 4; ++k) {
          text += fmt::format("{}{:g}", k ? " " : "",
                              std::bit_cast<float>(rex::memory::load_and_swap<uint32_t>(m + i * 64 + 32 + k * 4)));
        }
        text += ")";
      }
    }
    REXLOG_INFO("{}", text);
    // The first vertices the index buffer names: position (stream 31.2, float3) and the index/weights (31.1 +32), with
    // c0-c9 and c23-c26, to redo the shader's math on the PC.
    {
      const uint32_t init = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
      const uint32_t dma = Register(g::XE_GPU_REG_VGT_DMA_BASE);
      const bool idx32 = (init >> 11) & 1;
      const uint32_t pos_base = Register(v + 4) & ~0x3u, w_base = Register(v + 2) & ~0x3u;
      const uint8_t* ib = dma ? memory_->TranslatePhysical(dma & ~0x3u) : nullptr;
      const uint8_t* pb = pos_base ? memory_->TranslatePhysical(pos_base) : nullptr;
      const uint8_t* wb = w_base ? memory_->TranslatePhysical(w_base) : nullptr;
      auto f32 = [](const uint8_t* m) { return std::bit_cast<float>(rex::memory::load_and_swap<uint32_t>(m)); };
      std::string verts;
      for (uint32_t k = 0; ib && pb && wb && k < 6; ++k) {
        const uint32_t i = idx32 ? rex::memory::load_and_swap<uint32_t>(ib + k * 4)
                                 : rex::memory::load_and_swap<uint16_t>(ib + k * 2);
        verts += fmt::format(" [i{} p({:g} {:g} {:g}) w({:g} {:g} {:g} {:g})]", i, f32(pb + i * 12), f32(pb + i * 12 + 4),
                             f32(pb + i * 12 + 8), f32(wb + i * 64 + 32), f32(wb + i * 64 + 36), f32(wb + i * 64 + 40),
                             f32(wb + i * 64 + 44));
      }
      std::string cs;
      for (uint32_t c : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 27u, 28u, 29u, 30u}) {
        const uint32_t r = 0x4000 + c * 4;
        cs += fmt::format(" c{}=({:g} {:g} {:g} {:g})", c, std::bit_cast<float>(Register(r)),
                          std::bit_cast<float>(Register(r + 1)), std::bit_cast<float>(Register(r + 2)),
                          std::bit_cast<float>(Register(r + 3)));
      }
      REXLOG_INFO("[gpu] skin VS n{} vertices (index base {:08X}, {}-bit, endian {}):{};{}", vs_->numero, dma,
                  idx32 ? 32 : 16, Register(g::XE_GPU_REG_VGT_DMA_SIZE) >> 30, verts, cs);
    }
  }

  // The lamp glow (gVFGlowInfo) and corona (modulate_by_corona_info_vs) vertex shaders: both read the lamp's
  // visibility from a texture in the vertex shader (tfetch2DVertice) and build their quads from it. Suspects for the
  // streaks next to flickering lamps; sin_resplandor.flag next to the NRO leaves them out to tell.
  static bool IsGlowShader(const rex::x360::EntradaShader* entry) {
    return entry && (entry->huella_contenedor == UINT64_C(0x36929751F8A50138) ||
                     entry->huella_contenedor == UINT64_C(0xC94E83E0DB28EA74));
  }

  // Once per patched variant: the vertex fetches D3D wrote for the declaration (format, stride, offset), which the
  // container does not have.
  void LogGlowFetches(const rex::x360::EntradaShader* entry) {
    const uint64_t key = XXH3_64bits(microcode_.data(), microcode_.size() * sizeof(uint32_t));
    if (glow_logged_.size() >= 16 || !glow_logged_.insert(key).second) return;
    std::string fetches;
    for (size_t i = 0; i + 2 < microcode_.size(); i += 3) {
      const uint32_t w0 = microcode_[i], w1 = microcode_[i + 1], w2 = microcode_[i + 2];
      if ((w0 & 0x1F) != 0 || !((w0 >> 19) & 1)) continue;  // vertex fetch: opcode 0, must-be-one bit
      fetches += fmt::format(" [slot {} r{} const {}.{} format {} signed {} int {} stride {} offset {} swz {:03X}]",
                             i / 3, (w0 >> 12) & 0x3F, (w0 >> 20) & 0x1F, (w0 >> 25) & 3, (w1 >> 16) & 0x3F,
                             (w1 >> 14) & 1, (w1 >> 13) & 1, w2 & 0xFF, (w2 >> 8) & 0x7FFFFF, w1 & 0xFFF);
    }
    REXLOG_INFO("[gpu] glow VS {:016X} (n{}): {} words, fetches:{}", entry->huella_contenedor, entry->numero,
                microcode_.size(), fetches);
  }

  // An IM_LOAD / IM_LOAD_IMMEDIATE of type 0 (VS) or 1 (PS), its microcode in microcode_ (host order).
  void ShaderLoaded(uint32_t type, uint32_t physical) {
    const rex::x360::EntradaShader* entry = mp3::shaders::Global().Identificar(type == 0, physical, microcode_);
    if (type == 1) {
      ps_ = entry;
      return;
    }
    // The same vertex shader loaded again from the same address (D3D reloads it for every draw that sets it): its
    // patched microcode and generation are the ones already here. Checked by length and three words, as the library
    // does, instead of copying and hashing it again.
    const size_t n = microcode_.size();
    if (physical && entry && entry == vs_ && physical == vs_physical_ && n && n == vs_microcode_.size() &&
        microcode_[0] == vs_microcode_[0] && microcode_[n / 2] == vs_microcode_[n / 2] &&
        microcode_[n - 1] == vs_microcode_[n - 1]) {
      return;
    }
    vs_physical_ = physical;
    vs_ = entry;
    vs_microcode_ = microcode_;  // patched by D3D: the vertex input is read from here
    if (IsGlowShader(entry)) LogGlowFetches(entry);
    // The same microcode keeps its generation, so the renderer's per-generation caches stay valid.
    const uint64_t key = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t)) ^
                         vs_microcode_.size();
    auto [it, inserted] = vs_generations_.try_emplace(key, vs_generations_.size() + 1);
    generacion_vs_ = it->second;
  }

  static uint32_t TargetKey(uint32_t color_info, uint32_t pitch) {
    uint32_t format = (color_info >> 16) & 0xF;
    if (format == 12) format = 3;  // EDRAM format aliases, as the renderer's DestinoColor
    if (format == 10) format = 2;
    return ((color_info & 0xFFF) << 18) | (format << 14) | (pitch & 0x3FFF);
  }

  void MarkDrawnTargets() {
    namespace g = rex::graphics;
    if (!vs_ || (Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5) return;
    const uint32_t mask = Register(g::XE_GPU_REG_RB_COLOR_MASK);
    const uint32_t pitch = Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF;
    static constexpr uint32_t kInfo[4] = {g::XE_GPU_REG_RB_COLOR_INFO, g::XE_GPU_REG_RB_COLOR1_INFO,
                                          g::XE_GPU_REG_RB_COLOR2_INFO, g::XE_GPU_REG_RB_COLOR3_INFO};
    for (uint32_t i = 0; i < 4; ++i) {
      if ((mask >> (i * 4)) & 0xF) drawn_targets_[TargetKey(Register(kInfo[i]), pitch)] = frame_serial_;
    }
  }

  // GTA IV halves its scene with a resolve: the 1280x720 target read as a 4x multisampled surface of half the pitch
  // (the same EDRAM bytes; the resolve averages each pixel's four samples, a 2x2 box). Its luminance and bloom chain
  // (640x360, 320x180 ... 8x1) starts there. The renderer keeps EDRAM targets as images keyed by pitch, so that resolve
  // read an empty 640-pitch image: the whole chain was black and so was the tone mapped frame. A 4x resolve from a
  // target nobody drew into this frame, whose 1x twin of twice the pitch was drawn, reads the twin instead (rectangle
  // and scissors doubled); the renderer shrinks it into the 640x360 destination with a linear blit.
  // One tile (mp3_sin_tiling, see WriteRegister): the G-buffer's resolves still carry the first tile's rectangle,
  // (0,0)-(1280,256); with the scissor widened they copy the whole frame.
  void ResolveWholeTile(rex::x360::RegistrosCopia& c) {
    if (!REXCVAR_GET(mp3_sin_tiling) || !REXCVAR_GET(mp3_tile_completo)) return;
    const uint32_t ancho = AnchoJuego(), alto = AltoJuego();
    if ((c.pa_sc_window_scissor_br & 0x7FFF7FFF) != ((alto << 16) | ancho) || (c.rb_surface_info & 0x3FFF) != ancho) {
      return;
    }
    if (((c.rb_copy_dest_pitch >> 16) & 0x3FFF) != alto) return;  // into a whole-frame texture
    if ((c.fetch_vertices[0] & 0x3) != uint32_t(xenos::FetchConstantType::kVertex) ||
        ((c.fetch_vertices[1] >> 2) & 0xFFFFFF) != 6) {
      return;
    }
    const auto endian = static_cast<xenos::Endian>(c.fetch_vertices[1] & 0x3);
    const uint8_t* v = memory_->TranslatePhysical(c.fetch_vertices[0] & ~uint32_t(0x3));
    float rect[6];
    float max_y = 0.0f;
    for (uint32_t i = 0; i < 6; ++i) {
      float f;
      std::memcpy(&f, v + i * 4, 4);
      rect[i] = xenos::GpuSwap(f, endian);
      if (i & 1) max_y = std::max(max_y, rect[i]);
    }
    // The first tile's rectangle (0,0)-(1280,256): the 2x multisampled G-buffer's comes with other values than exactly
    // 256 (the check for 256.0 alone never matched, and its resolves kept the top band only). Anything that stops short
    // of the frame's bottom goes down to row 720.
    if (max_y >= float(alto) || max_y <= 0.0f) return;
    for (uint32_t i = 1; i < 6; i += 2) {
      if (rect[i] == max_y) rect[i] = float(alto);
    }
    if (!tile_rect_) tile_rect_ = memory_->SystemHeapAlloc(32, 32, rex::memory::kSystemHeapPhysical);
    if (!tile_rect_) return;
    std::memcpy(memory_->TranslateVirtual<uint8_t*>(tile_rect_), rect, sizeof(rect));
    c.fetch_vertices[0] = (memory_->GetPhysicalAddress(tile_rect_) & ~uint32_t(0x3)) |
                          uint32_t(xenos::FetchConstantType::kVertex);
    c.fetch_vertices[1] = (6u << 2) | uint32_t(xenos::Endian::kNone);
    if (tile_logged_ < 4) {
      ++tile_logged_;
      REXLOG_INFO("[gpu] one tile: resolve to {:08X} widened from {}x{} to {}x{}", c.rb_copy_dest_base, ancho, max_y,
                  ancho, alto);
    }
  }
  uint32_t tile_rect_ = 0, tile_logged_ = 0;

  bool ResolveThroughMsaaAlias(rex::x360::RegistrosCopia& c) {
    if (!REXCVAR_GET(mp3_resolve_msaa_alias)) return false;
    const uint32_t surface = c.rb_surface_info;
    const uint32_t msaa = (surface >> 16) & 0x3, pitch = surface & 0x3FFF;
    const uint32_t source = c.rb_copy_control & 0x7;
    if (msaa != 2 || source > 3 || !pitch) return false;
    const uint32_t info = c.rb_color_info[source];
    auto drawn = [&](uint32_t p) {
      auto it = drawn_targets_.find(TargetKey(info, p));
      return it != drawn_targets_.end() && it->second == frame_serial_;
    };
    if (drawn(pitch) || !drawn(pitch * 2)) return false;
    if ((c.fetch_vertices[0] & 0x3) != uint32_t(xenos::FetchConstantType::kVertex) ||
        ((c.fetch_vertices[1] >> 2) & 0xFFFFFF) != 6) {
      return false;
    }
    if (!alias_rect_) alias_rect_ = memory_->SystemHeapAlloc(32, 32, rex::memory::kSystemHeapPhysical);
    if (!alias_rect_) return false;
    const auto endian = static_cast<xenos::Endian>(c.fetch_vertices[1] & 0x3);
    const uint8_t* v = memory_->TranslatePhysical(c.fetch_vertices[0] & ~uint32_t(0x3));
    float rect[6];
    for (uint32_t i = 0; i < 6; ++i) {
      float f;
      std::memcpy(&f, v + i * 4, 4);
      rect[i] = xenos::GpuSwap(f, endian) * 2.0f;
    }
    std::memcpy(memory_->TranslateVirtual<uint8_t*>(alias_rect_), rect, sizeof(rect));
    c.fetch_vertices[0] = (memory_->GetPhysicalAddress(alias_rect_) & ~uint32_t(0x3)) |
                          uint32_t(xenos::FetchConstantType::kVertex);
    c.fetch_vertices[1] = (6u << 2) | uint32_t(xenos::Endian::kNone);
    c.rb_surface_info = (surface & ~uint32_t(0x33FFF)) | std::min<uint32_t>(pitch * 2, 0x3FFF);
    auto twice = [](uint32_t v, uint32_t bits) {
      const uint32_t m = (1u << bits) - 1;
      return (v & ~(m | (m << 16))) | std::min((v & m) * 2, m) | (std::min(((v >> 16) & m) * 2, m) << 16);
    };
    c.pa_sc_window_scissor_tl = twice(c.pa_sc_window_scissor_tl, 14);
    c.pa_sc_window_scissor_br = twice(c.pa_sc_window_scissor_br, 14);
    if (alias_logged_ < 4) {
      ++alias_logged_;
      REXLOG_INFO("[gpu] resolve through the 1x twin: c{} base {:03X} pitch {} (4x) read at pitch {}", source,
                  info & 0xFFF, pitch, pitch * 2);
    }
    return true;
  }
  uint32_t alias_logged_ = 0;

  // Diagnostics (mp3_ver_texturas = seconds per view): in gameplay the screen shows, in turn, each color texture the
  // last frame resolved (G-buffer, HDR scene, its smaller copies, bloom ...) instead of the frame, with a capture of
  // each one and a log line naming it, to see at which step of the frame the picture is lost in one run.
  struct ResolveSeen {
    uint32_t base, width, height, format;
  };
  std::vector<ResolveSeen> resolves_frame_, resolves_last_;
  uint64_t draws_this_frame_ = 0, view_gameplay_frames_ = 0;
  int32_t view_index_ = -1;
  uint32_t view_capture_after_ = 0;

  void RecordResolveForView(const rex::x360::RegistrosCopia& c) {
    if (!REXCVAR_GET(mp3_ver_texturas)) return;
    if ((c.rb_copy_control & 0x7) > 3 || ((c.rb_copy_control >> 20) & 0x3) == 3) return;  // depth, clear only
    const uint32_t base = c.rb_copy_dest_base & 0x1FFFF000u;
    const uint32_t width = c.rb_copy_dest_pitch & 0x3FFF, height = (c.rb_copy_dest_pitch >> 16) & 0x3FFF;
    if (width < 160 || height < 90) return;
    for (const auto& r : resolves_frame_) {
      if (r.base == base) return;
    }
    resolves_frame_.push_back({base, width, height, (c.rb_copy_dest_info >> 7) & 0x3F});
  }

  void ViewOverride(rex::x360::TexturaSwap& swap) {
    draws_this_frame_ = 0;
    resolves_last_.swap(resolves_frame_);
    resolves_frame_.clear();
    if (!REXCVAR_GET(mp3_ver_texturas)) return;
    // ZL + ZR + Right: the next view (the game's frame, then each color texture the last frame resolved, in order).
    if (REXCVAR_GET(switch_omitir_pedido)) {
      REXCVAR_SET(switch_omitir_pedido, false);
      view_index_ = view_index_ + 1 >= int32_t(resolves_last_.size()) ? -1 : view_index_ + 1;
      view_capture_after_ = 20;
      if (view_index_ < 0) {
        REXLOG_INFO("[ver] modo 0: the game's frame");
      } else {
        const auto& r = resolves_last_[size_t(view_index_)];
        REXLOG_INFO("[ver] modo {} de {}: resolved texture {:08X} {}x{} format {}", view_index_ + 1,
                    resolves_last_.size(), r.base, r.width, r.height, r.format);
        view_base_ = r.base;
      }
    }
    if (view_index_ >= 0) {
      swap.dword[1] = (swap.dword[1] & 0xFFFu) | view_base_;
    }
    if (view_capture_after_ && --view_capture_after_ == 0) {
      SaveCapture();
      REXLOG_INFO("[ver] capture above: modo {}", view_index_ + 1);
    }
  }
  uint32_t view_base_ = 0;

  void Draw() {
    ++geometria_;
    FrameTraceDraw();
    MarkDrawnTargets();
    ++draws_this_frame_;
    namespace g = rex::graphics;
    // Depth only (RB_MODECONTROL 5): the GPU runs no pixel shader, so the VS is enough.
    const bool depth_only = (Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5;
    const uint32_t msaa = (Register(g::XE_GPU_REG_RB_SURFACE_INFO) >> 16) & 0x3;
    // Which way the scene's depth test goes, for the depth value of the emulated clears.
    if (!msaa && (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x2)) {
      const uint32_t zfunc = (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) >> 4) & 0x7;
      if (zfunc == 4 || zfunc == 6) ++depth_greater_; else if (zfunc == 1 || zfunc == 3) ++depth_less_;
      // And per depth buffer (its EDRAM base): Gears of War 2's scene goes GREATER (far = 0), its shadow maps LESS
      // (far = 1). With one direction for all, the shadow maps were cleared to 0, nothing could be drawn into them,
      // every pixel came out in shadow and the modulated shadows darkened the whole world to black.
      auto& [mayor, menor] =
          depth_direction_[DepthKey(Register(g::XE_GPU_REG_RB_DEPTH_INFO), Register(g::XE_GPU_REG_RB_SURFACE_INFO))];
      if (zfunc == 4 || zfunc == 6) ++mayor; else if (zfunc == 1 || zfunc == 3) ++menor;
    }
    // Direct3D clears a target by drawing over it with its own internal shaders (not created by the game, so not in
    // the library), through the same EDRAM seen as a multisampled surface of half the pitch. Done as the renderer's
    // clears instead.
    // Only full-surface ones (window scissor to 8192): Dead Space 2's light stencil masks are multisampled draws too,
    // and one whose VS was not identified, taken for a clear, wiped the stencil and let a light flood the screen.
    // A single-sampled full-surface draw without an identified VS is usually a real draw (the lamps' 64x8 visibility
    // pass is one, and clearing for it wiped the scene's depth in the middle of the frame). Single-sampled, only
    // Direct3D's own clear shape is taken: one rectangle (rect list, 3 vertices) with all four targets' mask. The
    // decompression scene clears its quarter-size particle buffer that way every frame; left as it was, the smoke and
    // debris of every frame piled up into grey smears. And only onto an 8-bit color target with a color in 0..1 in the
    // clear shader's c0: the same shape also fills the 80-wide float exposure target with game data (c0 like 0.027 0
    // -18.4 1), which is not a clear.
    const uint32_t initiator = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    // Also onto a k_16_16 target (fixed point, -32..32): Gears of War 2 clears its motion blur velocity buffer (EDRAM
    // 658, 640x360) that way before writing the moving objects' velocities with MAX blending. Never cleared, it kept
    // the largest velocity ever written, and the hero riding a vehicle stayed smeared though he does not move.
    const uint32_t clear_format = (Register(g::XE_GPU_REG_RB_COLOR_INFO) >> 16) & 0xF;  // (its color: EmulateClear)
    bool rect_clear = !msaa && (initiator & 0x3F) == 8 && (initiator >> 16) == 3 &&
                      Register(g::XE_GPU_REG_RB_COLOR_MASK) == 0xFFFF && (clear_format <= 1 || clear_format == 4);
    for (uint32_t j = 0; j < 4 && rect_clear; ++j) {
      float v;
      std::memcpy(&v, &registers_[0x4400 + j], 4);
      rect_clear = clear_format == 4 ? (std::isfinite(v) && v >= -32.0f && v <= 32.0f) : (v >= 0.0f && v <= 1.0f);
    }
    // Gears of War 2 fills each shadow map with the far plane before drawing it: one rectangle, depth-only, depth
    // written with ALWAYS (depthcontrol 00000076), through a vertex shader of its own that is not in the library.
    // Skipped, the 800x800 shadow map kept 0 everywhere, every pixel came out shadowed and the modulated shadows
    // blacked out characters and walls. Done as a depth clear (to the far value of that depth buffer).
    const uint32_t depthcontrol = Register(g::XE_GPU_REG_RB_DEPTHCONTROL);
    const bool depth_fill = !msaa && (initiator & 0x3F) == 8 && (initiator >> 16) == 3 &&
                            (Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5 && (depthcontrol & 0x6) == 0x6 &&
                            ((depthcontrol >> 4) & 0x7) == 7;
    if (!vs_ && ((msaa && (Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR) & 0x3FFF) >= 0x2000) || rect_clear ||
                 depth_fill)) {
      if (draw_tracing_) DrawTraceLine(true, "Direct3D clear, emulated");
      EmulateClear(msaa);
      return;
    }
    if (!vs_) {
      ++sin_vs_;
      {
        const uint64_t key = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
        if (unknown_vs_skips_[key]++ == 0) {
          unknown_vs_sample_[key] = fmt::format(
              "{} words, prim {} count {}, source {}, color0 {:08X} mask {:04X}, depth {:08X} control {:08X}, mode {}, "
              "msaa {}, PS n{}",
              vs_microcode_.size(), initiator & 0x3F, initiator >> 16, (initiator >> 6) & 0x3,
              Register(g::XE_GPU_REG_RB_COLOR_INFO), Register(g::XE_GPU_REG_RB_COLOR_MASK),
              Register(g::XE_GPU_REG_RB_DEPTH_INFO), Register(g::XE_GPU_REG_RB_DEPTHCONTROL),
              Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7, msaa, ps_ ? int(ps_->numero) : -1);
        }
      }
      // A real draw whose VS was not identified (not a Direct3D clear): which microcode, once each.
      if (!msaa && unknown_vs_logged_.size() < 16) {
        const uint64_t key = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
        if (unknown_vs_logged_.insert(key).second) {
          REXLOG_WARN("[gpu] draw skipped, VS not identified: microcode {:016X} ({} words, first {:08X} {:08X} "
                      "{:08X}), target c0 {:08X} pitch {}, PS n{}",
                      key, vs_microcode_.size(), vs_microcode_.size() > 0 ? vs_microcode_[0] : 0,
                      vs_microcode_.size() > 1 ? vs_microcode_[1] : 0, vs_microcode_.size() > 2 ? vs_microcode_[2] : 0,
                      Register(g::XE_GPU_REG_RB_COLOR_INFO), Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF,
                      ps_ ? int(ps_->numero) : -1);
        }
      }
      if (draw_tracing_) DrawTraceLine(false, "no VS");
      return;
    }
    if (!ps_ && !depth_only) {
      ++sin_ps_;
      return;
    }
    // mp3_sin_prepaso_z: the scene's depth prepass (depth-only draws writing the depth buffer at EDRAM 000) is not
    // drawn; the base pass that follows tests GEQUAL and writes depth itself, so it ends up the same.
    if (REXCVAR_GET(mp3_sin_prepaso_z) && skip_mode_ != 8 && (Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5 &&
        (Register(g::XE_GPU_REG_RB_DEPTH_INFO) & 0xFFF) == 0 && (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x4)) {
      ++prepass_skipped_;
      return;
    }
    if (skip_mode_ > 1 && SkippedByMode()) {
      ++skipped_by_mode_;
      return;
    }
    // The search modes' candidates: vertex shaders writing depth while drawing color and depth.
    // (pixel search: the base pass, color into 2D0 format 3 with depth on)
    if (REXCVAR_GET(mp3_busqueda_ps)) {
      if (ps_ && (Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 4 &&
          (Register(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF) == 0x2D0 &&
          ((Register(g::XE_GPU_REG_RB_COLOR_INFO) >> 16) & 0xF) == 3 && depth_vs_frame_.size() < 256) {
        // Weighted by vertices: the big meshes (the ground) are one or two draws each.
        depth_vs_frame_[ps_->numero] += std::max<uint32_t>(Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR) >> 16, 1);
      }
    } else if ((Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 4 &&
               (Register(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x4) && depth_vs_frame_.size() < 256) {
      ++depth_vs_frame_[vs_->numero];
    }
    if (ps_) LogExposureConstants();
    if (OmittedVs(vs_->huella_contenedor)) {
      if (draw_tracing_) DrawTraceLine(false, "omitted (mp3_omitir_vs)");
      return;
    }
    bool tree_identity = false, tree_c10_zero = false;
    if (vs_->huella_contenedor == kBrokenSkinVs) {
      LogBrokenSkinDraw();
      const int32_t mode = REXCVAR_GET(mp3_arboles);
      if (mode == 0 && REXCVAR_GET(mp3_saltar_vs_rotos)) {
        ++skipped_broken_vs_;
        return;
      }
      tree_identity = mode == 2 || mode == 3;
      tree_c10_zero = mode == 3 || mode == 4;
    }
    if (IsGlowShader(vs_)) {
      if (glow_draws_logged_ < 12) {
        ++glow_draws_logged_;
        // Fetch constant 17 (vertex texture register 1): the visibility texture the shader reads.
        const uint32_t f = 0x4800 + 17 * 6;
        // Vertex fetch constants 31.0-31.2 (where the containers point their fetches) share fetch slot 31's words.
        const uint32_t v = 0x4800 + 31 * 6;
        REXLOG_INFO("[gpu] glow draw (VS n{}, PS n{}): texture fetch 17 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}; "
                    "vertex fetch 31.x {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                    vs_->numero, ps_ ? ps_->numero : 0, Register(f), Register(f + 1), Register(f + 2),
                    Register(f + 3), Register(f + 4), Register(f + 5), Register(v), Register(v + 1), Register(v + 2),
                    Register(v + 3), Register(v + 4), Register(v + 5));
      }
      if (hide_glow_) return;
    }
    // Diagnostics (Report): formats and primitive types of what is sent.
    const uint32_t mask = Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK);
    static constexpr uint32_t kColorInfo[4] = {
        rex::graphics::XE_GPU_REG_RB_COLOR_INFO, rex::graphics::XE_GPU_REG_RB_COLOR1_INFO,
        rex::graphics::XE_GPU_REG_RB_COLOR2_INFO, rex::graphics::XE_GPU_REG_RB_COLOR3_INFO};
    for (uint32_t i = 0; i < 4 && ps_ && !depth_only; ++i) {
      if (((mask >> (i * 4)) & 0xF) && ((ps_->salidas >> i) & 0x1)) {
        ++color_formats_[(Register(kColorInfo[i]) >> 16) & 0xF];
      }
    }
    if (Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL) & 0x6) {  // Z test or Z write
      ++depth_formats_[(Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO) >> 16) & 0x1];
    }
    ++primitive_types_[Register(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR) & 0x1F];
    {
      // Indexed draws by index size and byte order (VGT_DMA_SIZE bits 30-31): 16-bit indices in k8in32 or k16in32
      // were read unswapped by the SDK.
      const uint32_t init = Register(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR);
      if (((init >> 6) & 0x3) == 0) {  // source select: DMA (an index buffer)
        ++index_orders_[((init >> 11) & 0x1) * 4 + (Register(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE) >> 30)];
      }
    }

    rex::x360::PeticionDibujo request;
    request.registros = registers_.data();
    AliasScope alias(*this);
    // With the EDRAM shared by its views (SDK patch 0057) a multisampled pass is drawn into its own view, in pixels,
    // and the renderer moves its tiles to the 1x views that read them; scaling it here as well drew GTA IV's 2x
    // G-buffer 1440 rows tall into a 1x image its resolves never read.
    if (msaa && !REXCVAR_GET(x360_edram_compartida)) {
      // Direct3D's trick for quarter-rate passes (Dead Space 2's light stencil masks): the 1280x720 depth buffer
      // drawn as a 640x360 4x-multisampled surface over the same EDRAM, each pixel's four samples landing on 2x2
      // pixels. The renderer has no multisampling and keys targets by pitch, so the draw goes to the 1x surface
      // with everything in pixels scaled up instead (same pixels covered).
      request.registros = alias.Apply(msaa);
      ++generacion_encuadre_;  // the scaled viewport and scissor are not the mirror's
    }
    const bool rect_list = ExpandRectList(alias);
    if (rect_list) ++generacion_fetch_;  // the fetch constant points at our quads
    request.vs = vs_;
    request.ps = ps_;
    request.vs_microcodigo = vs_microcode_;
    request.generacion_vs = generacion_vs_;
    request.generacion_constantes_vs = generacion_constantes_vs_;
    request.generacion_constantes_ps = generacion_constantes_ps_;
    request.generacion_fetch = generacion_fetch_;
    request.generacion_encuadre = generacion_encuadre_;
    ++enviados_;
    // The probe frame: a base-pass draw only into the probe square, its samples counted on the GPU.
    const bool probe = probing_ && !msaa && ps_ && (Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 4 &&
                       (Register(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF) == 0x2D0 &&
                       ((Register(g::XE_GPU_REG_RB_COLOR_INFO) >> 16) & 0xF) == 3 && probes_.size() < 3000;
    uint32_t saved_tl = 0, saved_br = 0;
    const uint32_t probe_key = 0xFFF00000u + uint32_t(probes_.size());
    if (probe) {
      saved_tl = registers_[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
      saved_br = registers_[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
      registers_[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 0x80000000u | (probe_y_ << 16) | probe_x_;
      registers_[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 0x80000000u | ((probe_y_ + 8) << 16) | (probe_x_ + 8);
      ++generacion_encuadre_;
      request.generacion_encuadre = generacion_encuadre_;
      destinos_->EmpezarOclusion(probe_key);
    }
    // mp3_velocidad_sin_z: the velocity buffer's draws without their depth test (z enable off for this draw).
    const bool velocity_no_z = REXCVAR_GET(mp3_velocidad_sin_z) && ps_ &&
                               ps_->huella_contenedor == UINT64_C(0x1929EAB6BE60FA77);
    const uint32_t saved_depthcontrol = registers_[g::XE_GPU_REG_RB_DEPTHCONTROL];
    if (velocity_no_z) {
      registers_[g::XE_GPU_REG_RB_DEPTHCONTROL] = saved_depthcontrol & ~0x2u;
      ++generacion_encuadre_;
      request.generacion_encuadre = generacion_encuadre_;
    }
    // mp3_arboles = 2: the trees' three sway matrices (c11-c22) replaced by the identity for this draw.
    std::array<uint32_t, 48> saved_sway{};
    std::array<uint32_t, 4> saved_c10{};
    if (tree_c10_zero) {
      // The index offset c10.x: with it 0 the matrix index is r1.x mod 3 of a 1 (no truncation of a 2/3 involved).
      for (uint32_t i = 0; i < 4; ++i) {
        saved_c10[i] = registers_[0x4000 + 10 * 4 + i];
        registers_[0x4000 + 10 * 4 + i] = 0;
      }
      ++generacion_constantes_vs_;
      request.generacion_constantes_vs = generacion_constantes_vs_;
    }
    if (tree_identity) {
      for (uint32_t i = 0; i < 48; ++i) {
        saved_sway[i] = registers_[0x4000 + 11 * 4 + i];
        registers_[0x4000 + 11 * 4 + i] = (i % 16) % 5 == 0 ? 0x3F800000u : 0u;  // rows 0-3 of each 4x4: 1 on the diagonal
      }
      ++generacion_constantes_vs_;
      request.generacion_constantes_vs = generacion_constantes_vs_;
    }
    // mp3_exposicion: the tonemap's Exposures (c137.xy) scaled for this draw, before its curve (not a ramp on the
    // output: the highlights keep their roll-off).
    const float exposure = float(REXCVAR_GET(mp3_exposicion));
    const bool exposure_scaled = exposure != 1.0f && ps_ && IsTonemap(ps_->huella_contenedor);
    std::array<uint32_t, 2> saved_exposures{};
    if (exposure_scaled) {
      for (uint32_t i = 0; i < 2; ++i) {
        uint32_t& word = registers_[0x4000 + (256 + 137) * 4 + i];
        saved_exposures[i] = word;
        word = std::bit_cast<uint32_t>(std::bit_cast<float>(word) * exposure);
      }
      ++generacion_constantes_ps_;
      request.generacion_constantes_ps = generacion_constantes_ps_;
    }
    // mp3_fxaa_sin_sesgo: the game's FXAA writes into the scene's 7e3 target, whose RB_COLOR_INFO keeps color_exp_bias
    // 2 (x4) from the lighting; its output is already the 0..1 picture and the resolve to the 8-bit front buffer has no
    // bias of its own, so with it the frame came out x4, burnt white. The bias bits (20-25) cleared for this draw.
    static constexpr uint64_t kFxaa = UINT64_C(0x8D42760B0383C0D8);
    const bool fxaa = ps_ && ps_->huella_contenedor == kFxaa;
    const uint32_t color_info = registers_[rex::graphics::XE_GPU_REG_RB_COLOR_INFO];
    const int32_t fxaa_bias = fxaa ? int32_t(color_info << 6) >> 26 : 0;
    if (fxaa && Clock::now() >= next_fxaa_log_) {
      next_fxaa_log_ = Clock::now() + std::chrono::seconds(10);
      REXLOG_INFO("[exposicion] fxaa: RB_COLOR_INFO {:08X} (formato {}, exp bias {}){}", color_info,
                  (color_info >> 16) & 0xF, fxaa_bias,
                  fxaa_bias && REXCVAR_GET(mp3_fxaa_sin_sesgo) ? ": se dibuja sin el" : "");
    }
    const bool fxaa_unbiased = fxaa_bias && REXCVAR_GET(mp3_fxaa_sin_sesgo);
    if (fxaa_unbiased) registers_[rex::graphics::XE_GPU_REG_RB_COLOR_INFO] = color_info & ~(0x3Fu << 20);
    const bool drawn = destinos_->Dibujar(request);
    if (fxaa_unbiased) registers_[rex::graphics::XE_GPU_REG_RB_COLOR_INFO] = color_info;
    if (exposure_scaled) {
      for (uint32_t i = 0; i < 2; ++i) registers_[0x4000 + (256 + 137) * 4 + i] = saved_exposures[i];
      ++generacion_constantes_ps_;
    }
    if (tree_identity) {
      for (uint32_t i = 0; i < 48; ++i) registers_[0x4000 + 11 * 4 + i] = saved_sway[i];
      ++generacion_constantes_vs_;
    }
    if (tree_c10_zero) {
      for (uint32_t i = 0; i < 4; ++i) registers_[0x4000 + 10 * 4 + i] = saved_c10[i];
      ++generacion_constantes_vs_;
    }
    if (velocity_no_z) {
      registers_[g::XE_GPU_REG_RB_DEPTHCONTROL] = saved_depthcontrol;
      ++generacion_encuadre_;
    }
    if (probe) {
      const uint64_t ticket = destinos_->TerminarOclusionAsincrona(probe_key);
      registers_[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = saved_tl;
      registers_[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = saved_br;
      ++generacion_encuadre_;
      probes_.push_back({uint32_t(probes_.size()), vs_ ? vs_->numero : 0, ps_->numero,
                         Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR) >> 16, ticket, UINT64_MAX});
    }
    if (draw_tracing_) {
      DrawTraceLine(drawn);
    } else if (vs_ && (vs_->numero == 49 || vs_->numero == 199) && volume_traces_ < 6) {
      // The light volumes that take a second per draw (VS n49 / n199, 4456 indices): their whole draw state, once
      // each for a few, to see what makes their triangles cover the screen thousands of times.
      ++volume_traces_;
      const uint32_t n = draw_trace_n_, cap = draw_trace_cap_;
      draw_trace_n_ = 0;
      draw_trace_cap_ = 1;
      DrawTraceLine(drawn, "light volume");
      draw_trace_n_ = n;
      draw_trace_cap_ = cap;
      std::string vc;
      for (uint32_t c = 0; c < 32; ++c) {
        const uint32_t r = 0x4000 + c * 4;
        vc += fmt::format(" c{}=({:g} {:g} {:g} {:g})", c, std::bit_cast<float>(registers_[r]),
                          std::bit_cast<float>(registers_[r + 1]), std::bit_cast<float>(registers_[r + 2]),
                          std::bit_cast<float>(registers_[r + 3]));
      }
      REXLOG_INFO("[draws]       light volume: initiator {:08X} dma base {:08X} size {:08X} vte {:08X} clip {:08X} "
                  "sc mode {:08X}; vs constants{}",
                  Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR), Register(g::XE_GPU_REG_VGT_DMA_BASE),
                  Register(g::XE_GPU_REG_VGT_DMA_SIZE), Register(g::XE_GPU_REG_PA_CL_VTE_CNTL),
                  Register(g::XE_GPU_REG_PA_CL_CLIP_CNTL), Register(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL), vc);
      // The vertex shader's textures (sampler sN = fetch constant 16 + N): the shadow map it shapes the volume with.
      for (size_t i = 0; i < vs_->samplers.size() && i < 4; ++i) {
        const uint32_t reg = 16u + vs_->samplers[i].registro;
        if (reg >= 32) continue;
        const uint32_t* f = &registers_[0x4800 + reg * 6];
        REXLOG_INFO("[draws]       vs s{} (fetch {}) tipo {}: base {:08X} fmt {} {}x{} dim {} swz {:03X} signs {:02X} "
                    "type {} words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                    vs_->samplers[i].registro, reg, vs_->samplers[i].tipo, f[1] & 0xFFFFF000u, f[1] & 0x3F,
                    (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1, (f[5] >> 9) & 0x3, (f[3] >> 1) & 0xFFF,
                    (f[0] >> 2) & 0xFF, f[0] & 0x3, f[0], f[1], f[2], f[3], f[4], f[5]);
      }
    }
    if (msaa) {
      ++generacion_encuadre_;  // back to the mirror's for the next draw
      ++msaa_draws_;
    }
    if (rect_list) ++generacion_fetch_;  // back to the game's fetch constant
  }


  // The register mirror with a multisampled surface turned into the 1x surface it aliases: 4X doubles width and
  // height (pitch x2), 2X doubles the height (samples stacked vertically). Viewport and scissors scale the same way.
  // Done in place on the ten registers involved and undone when the scope ends (copying the whole 80 KB mirror for
  // each of the ~30 multisampled draws per frame cost ~2.4 MB of copies per frame on the ring thread).
  class AliasScope {
   public:
    explicit AliasScope(RingGraphicsSystem& system) : system_(system) {}
    ~AliasScope() {
      for (uint32_t i = 0; i < count_; ++i) system_.registers_[saved_[i].first] = saved_[i].second;
    }
    const uint32_t* Apply(uint32_t msaa) { return system_.AliasedRegisters(msaa, *this); }
    void Save(uint32_t index) { saved_[count_++] = {index, system_.registers_[index]}; }

   private:
    RingGraphicsSystem& system_;
    std::pair<uint32_t, uint32_t> saved_[16];
    uint32_t count_ = 0;
  };

  const uint32_t* AliasedRegisters(uint32_t msaa, AliasScope& scope) {
    namespace g = rex::graphics;
    for (uint32_t index : {uint32_t(g::XE_GPU_REG_RB_SURFACE_INFO), uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE),
                           uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 1), uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 2),
                           uint32_t(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 3), uint32_t(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL),
                           uint32_t(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR), uint32_t(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL),
                           uint32_t(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR)}) {
      scope.Save(index);
    }
    uint32_t* r = registers_.data();
    const uint32_t sx = msaa == 2 ? 2 : 1, sy = 2;
    const uint32_t surface = r[g::XE_GPU_REG_RB_SURFACE_INFO];
    r[g::XE_GPU_REG_RB_SURFACE_INFO] = (surface & ~uint32_t(0x33FFF)) | std::min<uint32_t>((surface & 0x3FFF) * sx, 0x3FFF);
    auto scale_float = [&](uint32_t index, uint32_t factor) {
      float v;
      std::memcpy(&v, &r[index], 4);
      v *= float(factor);
      std::memcpy(&r[index], &v, 4);
    };
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE, sx);
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 1, sx);  // XOFFSET
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 2, sy);  // YSCALE
    scale_float(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 3, sy);  // YOFFSET
    auto scale_xy = [&](uint32_t index, uint32_t bits) {
      const uint32_t v = r[index];
      const uint32_t mask = (1u << bits) - 1;
      const uint32_t x = std::min((v & mask) * sx, mask), y = std::min(((v >> 16) & mask) * sy, mask);
      r[index] = (v & ~(mask | (mask << 16))) | x | (y << 16);
    };
    scale_xy(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, 14);
    scale_xy(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, 14);
    scale_xy(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, 15);
    scale_xy(g::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, 15);
    return r;
  }

  // Rectangle lists (primitive type 8): Unreal Engine 3 draws its full-screen passes (post-processing, the final copy
  // to the back buffer, UI quads) as rectangles of three vertices, the fourth implied, which deko3d has no primitive
  // for. Each rectangle becomes a quad (deko3d draws quad lists natively): the corner is the vertex opposite the
  // longest edge (by POSITION0's x and y), the missing vertex is a + b - corner for every component (a rectangle's
  // attributes are affine across it), written to a guest buffer of ours that the fetch constant points at for this
  // draw. Only what such quads use: auto-indexed draws, one vertex stream, in k8in32 or no swap. Elements in 32-bit
  // float are computed as floats; Max Payne 3's UI rectangles also carry packed elements (bytes, 16-bit, 2_10_10_10,
  // half floats): a + b - corner per lane, in modular integer arithmetic (exact when the result is in range, as an
  // affine value inside the rectangle is) or through float for half floats. The position must be float or 16_16. The
  // registers are restored with the alias scope.
  bool ExpandRectList(AliasScope& scope) {
    namespace g = rex::graphics;
    const uint32_t init = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    if ((init & 0x3F) != 8 || ((init >> 6) & 0x3) != 2 || !vs_) return false;  // rect list, auto index
    const uint32_t count = init >> 16;
    if (!count || count % 3 || count > 3 * 4096) return Unexpanded("vertex count");
    int32_t slot = -1;
    uint32_t stride = 0, position = UINT32_MAX;
    bool position_float = true;
    // How each word of a vertex is extrapolated (Lane): words no element reads are copied from a neighbour.
    enum Lane : uint8_t { kCopy, kFloat, kBytes, kHalves, kHalfFloats, k10_10_10_2, kWord };
    Lane lanes[256] = {};
    for (const rex::x360::ElementoVertice& el : vs_->elementos) {
      const size_t i = size_t(el.instruccion) * 3;
      if (i + 2 >= vs_microcode_.size()) return Unexpanded("fetch outside the microcode");
      const uint32_t w0 = vs_microcode_[i], w1 = vs_microcode_[i + 1], w2 = vs_microcode_[i + 2];
      const int32_t s = int32_t(((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 0x3));
      if (slot >= 0 && s != slot) return Unexpanded("more than one stream");
      slot = s;
      const uint32_t format = (w1 >> 16) & 0x3F;
      Lane lane;
      uint32_t words;
      switch (format) {
        case 36: lane = kFloat; words = 1; break;                // 32_FLOAT
        case 37: lane = kFloat; words = 2; break;                // 32_32_FLOAT
        case 57: lane = kFloat; words = 3; break;                // 32_32_32_FLOAT
        case 38: lane = kFloat; words = 4; break;                // 32_32_32_32_FLOAT
        case 6: lane = kBytes; words = 1; break;                 // 8_8_8_8
        case 7: lane = k10_10_10_2; words = 1; break;            // 2_10_10_10
        case 25: lane = kHalves; words = 1; break;               // 16_16
        case 26: lane = kHalves; words = 2; break;               // 16_16_16_16
        case 31: lane = kHalfFloats; words = 1; break;           // 16_16_FLOAT
        case 32: lane = kHalfFloats; words = 2; break;           // 16_16_16_16_FLOAT
        case 33: lane = kWord; words = 1; break;                 // 32
        case 34: lane = kWord; words = 2; break;                 // 32_32
        case 35: lane = kWord; words = 4; break;                 // 32_32_32_32
        default: return Unexpanded("vertex format");
      }
      if (stride && (w2 & 0xFF) != stride) return Unexpanded("strides differ");
      stride = w2 & 0xFF;
      const uint32_t offset = (w2 >> 8) & 0x7FFFFF;
      if (offset + words > stride || stride > 256) return Unexpanded("element outside the vertex");
      for (uint32_t w = 0; w < words; ++w) lanes[offset + w] = lane;
      if (el.uso == 0 && el.indice_uso == 0) {
        if (lane != kFloat && format != 25) return Unexpanded("position format");
        position = offset;
        position_float = lane == kFloat;
      }
    }
    if (slot < 0 || !stride || position == UINT32_MAX || position + (position_float ? 2 : 1) > stride) {
      return Unexpanded("no position");
    }
    const uint32_t reg = 0x4800 + uint32_t(slot) * 2;
    const uint32_t f0 = registers_[reg], f1 = registers_[reg + 1];
    const uint32_t endian = f1 & 0x3;
    if ((f0 & 0x3) != 3 || (endian != 0 && endian != 2)) return Unexpanded("fetch constant");
    const uint32_t first = Register(g::XE_GPU_REG_VGT_INDX_OFFSET) & 0xFFFFFF;
    const uint32_t available = (f1 >> 2) & 0xFFFFFF;  // words
    if (uint64_t(first + count) * stride > available) return Unexpanded("past the buffer");
    const uint32_t* src = memory_->TranslatePhysical<const uint32_t*>((f0 & 0x1FFFFFFC) + first * stride * 4);
    const uint32_t rects = count / 3;
    const uint32_t words = rects * 4 * stride;
    if (words * 4 > rect_buffer_size_) {
      if (rect_buffer_) memory_->SystemHeapFree(rect_buffer_);
      rect_buffer_size_ = std::max<uint32_t>(words * 4, 4096);
      rect_buffer_ = memory_->SystemHeapAlloc(rect_buffer_size_, 256, rex::memory::kSystemHeapPhysical);
      if (!rect_buffer_) {
        rect_buffer_size_ = 0;
        return Unexpanded("no guest memory");
      }
    }
    uint32_t* dst = memory_->TranslateVirtual<uint32_t*>(rect_buffer_);
    auto load = [&](uint32_t w) {
      float f;
      const uint32_t v = endian ? __builtin_bswap32(w) : w;
      std::memcpy(&f, &v, 4);
      return f;
    };
    auto store = [&](float f) {
      uint32_t v;
      std::memcpy(&v, &f, 4);
      return endian ? __builtin_bswap32(v) : v;
    };
    auto value = [&](uint32_t w) { return endian ? __builtin_bswap32(w) : w; };  // the word as the GPU reads it
    // a + b - c in lanes of `bits` bits (modular), for one word.
    auto lanes_sum = [](uint32_t a, uint32_t b, uint32_t c, std::initializer_list<uint32_t> bits) {
      uint32_t out = 0, shift = 0;
      for (uint32_t n : bits) {
        const uint32_t mask = n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1);
        out |= ((((a >> shift) & mask) + ((b >> shift) & mask) - ((c >> shift) & mask)) & mask) << shift;
        shift += n;
      }
      return out;
    };
    auto half_sum = [](uint32_t a, uint32_t b, uint32_t c) {
      uint32_t out = 0;
      for (uint32_t shift : {0u, 16u}) {
        auto h = [&](uint32_t v) { return float(std::bit_cast<_Float16>(uint16_t(v >> shift))); };
        const _Float16 r = _Float16(h(a) + h(b) - h(c));
        out |= uint32_t(std::bit_cast<uint16_t>(r)) << shift;
      }
      return out;
    };
    auto extrapolate = [&](uint32_t w, uint32_t a, uint32_t b, uint32_t c) -> uint32_t {
      switch (lanes[w]) {
        case kCopy: return a;
        case kFloat: return store(load(a) + load(b) - load(c));
        case kBytes: return value(lanes_sum(value(a), value(b), value(c), {8, 8, 8, 8}));
        case kHalves: return value(lanes_sum(value(a), value(b), value(c), {16, 16}));
        case kHalfFloats: return value(half_sum(value(a), value(b), value(c)));
        case k10_10_10_2: return value(lanes_sum(value(a), value(b), value(c), {10, 10, 10, 2}));
        case kWord: return value(lanes_sum(value(a), value(b), value(c), {32}));
      }
      return a;
    };
    // The position's x and y (16_16: two signed lanes; which one is x does not matter for the edge lengths).
    auto xy = [&](const uint32_t* v) -> std::pair<float, float> {
      if (position_float) return {load(v[position]), load(v[position + 1])};
      const uint32_t p = value(v[position]);
      return {float(int16_t(p >> 16)), float(int16_t(p & 0xFFFF))};
    };
    for (uint32_t r = 0; r < rects; ++r) {
      const uint32_t* v[3] = {src + (r * 3) * stride, src + (r * 3 + 1) * stride, src + (r * 3 + 2) * stride};
      auto edge = [&](int a, int b) {
        const auto [ax, ay] = xy(v[a]);
        const auto [bx, by] = xy(v[b]);
        const float dx = ax - bx, dy = ay - by;
        return dx * dx + dy * dy;
      };
      const float e12 = edge(1, 2), e20 = edge(2, 0), e01 = edge(0, 1);
      const int c = e12 >= e20 && e12 >= e01 ? 0 : (e20 >= e01 ? 1 : 2);  // opposite the longest edge
      const uint32_t* corner = v[c];
      const uint32_t* a = v[(c + 1) % 3];
      const uint32_t* b = v[(c + 2) % 3];
      uint32_t* q = dst + r * 4 * stride;
      std::memcpy(q, corner, stride * 4);
      std::memcpy(q + stride, a, stride * 4);
      for (uint32_t w = 0; w < stride; ++w) q[2 * stride + w] = extrapolate(w, a[w], b[w], corner[w]);
      std::memcpy(q + 3 * stride, b, stride * 4);
    }
    scope.Save(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    scope.Save(g::XE_GPU_REG_VGT_INDX_OFFSET);
    scope.Save(reg);
    scope.Save(reg + 1);
    registers_[g::XE_GPU_REG_VGT_DRAW_INITIATOR] = (init & 0xFFC0) | 13 | ((rects * 4) << 16);  // quad list
    registers_[g::XE_GPU_REG_VGT_INDX_OFFSET] = 0;
    registers_[reg] = (memory_->GetPhysicalAddress(rect_buffer_) & 0x1FFFFFFC) | (f0 & 0x3);
    registers_[reg + 1] = (f1 & ~uint32_t(0x03FFFFFC)) | ((words & 0xFFFFFF) << 2);
    if (rect_lists_++ < 4) {
      REXLOG_INFO("[gpu] rect list as quads: {} rectangle(s), stride {} words, position at {} (VS n{})", rects, stride,
                  position, vs_->numero);
    }
    return true;
  }

  bool Unexpanded(const char* why) {
    if (rect_unexpanded_logged_ < 8) {
      ++rect_unexpanded_logged_;
      REXLOG_WARN("[gpu] rect list not expanded ({}): VS n{}", why, vs_ ? int(vs_->numero) : -1);
    }
    return false;
  }

  // A Direct3D clear draw, as the renderer's clears. Direct3D's fast clear binds c0 (the color to clear) and, for the
  // depth buffer, c1-c3 over the depth buffer's EDRAM written as color, with a placeholder depth target that may even
  // sit on c0's base (Dead Space 2's light buffer clear: c0 2D0, c1-c3 000, depth 2D0). So: c0 is cleared as color
  // (to 0); c1-c3 on another base than c0 clear the depth buffer at their base (far plane, stencil 0); the bound
  // depth target is cleared only when depth or stencil is written and it is not on c0's base; depth-only mode
  // clears no color. The first few are logged with what the game had set, to check.
  void EmulateClear(uint32_t msaa) {
    namespace g = rex::graphics;
    AliasScope alias(*this);
    const uint32_t* r = msaa ? alias.Apply(msaa) : registers_.data();
    rex::x360::RegistrosCopia base = rex::x360::LeerRegistrosCopia(r);
    // Direct3D's clear rectangle (VS 6D38CFA5): 7 dwords per vertex, POSITION float3 then COLOR float4, in fetch
    // constant 0. A color clear done through the depth path carries the color's EDRAM bits as depth24 << 8 | stencil,
    // so the depth it writes (z of the first vertex, through the viewport) is the color the target gets. Read before
    // fetch constant 0 is replaced with the renderer's own rectangle below.
    float game_z = 0.0f;
    float game_color[4] = {};
    bool game_vertices = false;
    if ((base.fetch_vertices[0] & 0x3) == uint32_t(xenos::FetchConstantType::kVertex) &&
        ((base.fetch_vertices[1] >> 2) & 0xFFFFFF) >= 7) {
      const auto endian = static_cast<xenos::Endian>(base.fetch_vertices[1] & 0x3);
      const uint8_t* v = memory_->TranslatePhysical(base.fetch_vertices[0] & ~uint32_t(0x3));
      float z;
      std::memcpy(&z, v + 2 * 4, 4);
      z = xenos::GpuSwap(z, endian);
      for (uint32_t j = 0; j < 4; ++j) {  // COLOR, after POSITION
        std::memcpy(&game_color[j], v + (3 + j) * 4, 4);
        game_color[j] = xenos::GpuSwap(game_color[j], endian);
      }
      float zs, zo;
      std::memcpy(&zs, &r[g::XE_GPU_REG_PA_CL_VPORT_ZSCALE], 4);
      std::memcpy(&zo, &r[g::XE_GPU_REG_PA_CL_VPORT_ZOFFSET], 4);
      const uint32_t vte = r[g::XE_GPU_REG_PA_CL_VTE_CNTL];
      if (vte & (1u << 4)) z *= zs;
      if (vte & (1u << 5)) z += zo;
      game_z = z;
      game_vertices = std::isfinite(z);
    }
    // The whole surface: the renderer takes a copy's rectangle from three vertices in fetch constant 0 (where
    // Direct3D puts a resolve's), clipped by the window scissor. Ours, in a small guest buffer made once.
    if (!clear_rect_) {
      clear_rect_ = memory_->SystemHeapAlloc(32, 32, rex::memory::kSystemHeapPhysical);
    }
    if (!clear_rect_) return;
    // As tall as the clear rectangle's viewport reaches (Direct3D's is the surface's): the renderer's views of the
    // EDRAM (SDK patch 0057) claim the cleared rows, and 8192 rows were every tile, wiping all other targets.
    const float w = float(r[g::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF);
    float h = 8192.0f;
    if ((r[g::XE_GPU_REG_PA_CL_VTE_CNTL] & 0xC) == 0xC) {
      float ys, yo;
      std::memcpy(&ys, &r[g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 2], 4);
      std::memcpy(&yo, &r[g::XE_GPU_REG_PA_CL_VPORT_XSCALE + 3], 4);
      const float abajo = std::ceil(yo + std::abs(ys));
      if (std::isfinite(abajo) && abajo >= 8.0f && abajo < h) h = abajo;
    }
    const float rect[6] = {0.0f, 0.0f, w, 0.0f, 0.0f, h};  // host order, read with Endian::kNone
    std::memcpy(memory_->TranslateVirtual<uint8_t*>(clear_rect_), rect, sizeof(rect));
    const uint32_t physical = memory_->GetPhysicalAddress(clear_rect_);
    base.fetch_vertices[0] = (physical & ~uint32_t(0x3)) | uint32_t(xenos::FetchConstantType::kVertex);
    base.fetch_vertices[1] = (6u << 2) | uint32_t(xenos::Endian::kNone);
    base.pa_sc_window_offset = 0;
    base.pa_sc_window_scissor_tl = 0x80000000u;  // window_offset_disable, from 0,0
    base.pa_sc_window_scissor_br = 0x3FFF3FFFu;
    base.pa_su_sc_mode_cntl &= ~(1u << 16);       // no vtx_window_offset
    const uint32_t mask = r[g::XE_GPU_REG_RB_COLOR_MASK];
    const uint32_t depth_info = r[g::XE_GPU_REG_RB_DEPTH_INFO];
    const uint32_t depth_control = r[g::XE_GPU_REG_RB_DEPTHCONTROL];
    const bool depth_only = (r[g::XE_GPU_REG_RB_MODECONTROL] & 0x7) == 5;
    const bool reversed = depth_greater_ > depth_less_;
    // The stencil: Direct3D's clear quad writes the stencil reference with a REPLACE (depthcontrol 00008777: stencil on,
    // ALWAYS, z-pass REPLACE), not 0. Dead Space 2 clears it to its own value before the menu's Scaleform masks, whose
    // frames test LESS against it; written as 0, every frame and backing panel of the menus failed and only the text
    // (drawn without stencil) was left.
    uint32_t stencil = 0;
    if ((depth_control & 0x1) && ((depth_control >> 14) & 0x7) == 2) {
      const uint32_t ref_mask = r[g::XE_GPU_REG_RB_STENCILREFMASK];
      stencil = ref_mask & (ref_mask >> 16) & 0xFF;
    }
    constexpr uint32_t kNullCommand = 3u << 20;  // CopyCommand::kNull: clear only, copy nothing
    // Direct3D's clear quad with depth writes off and the stencil on (depthcontrol 00008701): a stencil-only clear.
    // Gears of War 2 does one after each light (through the scene's own depth buffer); as a full clear it wiped the
    // scene's depth in the middle of the frame. Bit 30 of RB_COPY_CONTROL (unused by the Xenos) tells the renderer
    // (SDK patch 0030).
    const bool stencil_only = !(depth_control & 0x4) && (depth_control & 0x1);
    auto clear_depth_at = [&](uint32_t edram_base, std::string& what, bool only_stencil = false) {
      // The direction of the draws into this depth buffer when it has any, the frame's otherwise.
      bool reversed_here = reversed;
      if (auto it = depth_direction_.find(
              DepthKey((depth_info & ~uint32_t(0xFFF)) | (edram_base & 0xFFF), r[g::XE_GPU_REG_RB_SURFACE_INFO]));
          it != depth_direction_.end() && it->second.first != it->second.second) {
        reversed_here = it->second.first > it->second.second;
      }
      // GTA IV: the depth Direct3D's clear rectangle carries (z of its first vertex through the viewport) is the value
      // the game asked for; guessing it from the draws' direction cleared the scene's depth buffer to 1 (its clear goes
      // through the EDRAM as a 4x surface of half the pitch, so the per-buffer direction was never found, and the
      // frame's shadow maps test LESS): every G-buffer draw, testing GEQUAL, failed and the world came out black.
      uint32_t depth_value = (reversed_here ? 0u : 0xFFFFFF00u) | stencil;
      if (REXCVAR_GET(mp3_z_borrado_del_juego) && game_vertices && game_z >= 0.0f && game_z <= 1.0f) {
        depth_value = (uint32_t(game_z * float(0xFFFFFF) + 0.5f) << 8) | stencil;
      }
      rex::x360::RegistrosCopia c = base;
      c.rb_depth_info = (depth_info & ~uint32_t(0xFFF)) | (edram_base & 0xFFF);
      c.rb_copy_control = 4u | (1u << 9) | kNullCommand | (only_stencil ? 1u << 30 : 0u);
      c.rb_depth_clear = depth_value;
      destinos_->Copiar(c);
      what += fmt::format(" z={:03X}->{:08X}{}", edram_base & 0xFFF, depth_value, only_stencil ? " (stencil only)" : "");
    };
    std::string what;
    const uint32_t c0_base = base.rb_color_info[0] & 0xFFF;
    const bool c0_written = !depth_only && (mask & 0xF);
    // The color: RB_COLOR_CLEAR, the D3DCOLOR (A, R, G, B from the top byte) Direct3D sets for the clear. Not the clear
    // shader's c0: Max Payne 3 has (1 0 0 0) there for every clear, with RB_COLOR_CLEAR FF000000 for the G-buffer and 0
    // for the rest. Taken from c0, every clear was red (as R, G, B, A: dead enemies red all over) or opaque black (as
    // A, R, G, B: the per-ped blood targets fully opaque, enemies black, the effects mask always on).
    uint32_t color = 0;
    {
      const uint32_t argb = r[g::XE_GPU_REG_RB_COLOR_CLEAR];
      color = ((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16) | (argb & 0xFF000000u);
    }
    // A k_16_16 target: the clear rectangle's vertex color (its pixel shader's c0 holds 8, not the color: as the color,
    // the velocity buffer was filled with "a moving object everywhere" and the whole screen smeared) as the target's
    // EDRAM bits (16-bit fixed point, value / 32 * 32767), passed as such (bit 31 of RB_COPY_CONTROL, SDK patch 0040).
    bool color_as_bits = false;
    if (!msaa && ((base.rb_color_info[0] >> 16) & 0xF) == 4) {
      color = 0;
      for (uint32_t j = 0; j < 2; ++j) {
        const float v = game_vertices && std::isfinite(game_color[j]) ? game_color[j] : 0.0f;
        const int32_t q = int32_t(std::lround(std::clamp(v / 32.0f, -1.0f, 1.0f) * 32767.0f));
        color |= uint32_t(uint16_t(int16_t(q))) << (j * 16);
      }
      color_as_bits = true;
    }
    if (c0_written) {
      rex::x360::RegistrosCopia c = base;
      c.rb_copy_control = 0u | (1u << 8) | kNullCommand | (color_as_bits ? 1u << 31 : 0u);
      c.rb_color_clear = color;
      c.rb_color_clear_lo = 0;
      destinos_->Copiar(c);
      what += fmt::format(" c0={:03X}", c0_base);
      if (color) what += fmt::format("={:08X}", color);
    }
    uint32_t aliased_depth[3] = {};
    uint32_t n_aliased = 0;
    for (uint32_t i = 1; i < 4 && !depth_only; ++i) {
      const uint32_t b = base.rb_color_info[i] & 0xFFF;
      if (!((mask >> (i * 4)) & 0xF) || (c0_written && b == c0_base)) continue;
      if (std::find(aliased_depth, aliased_depth + n_aliased, b) != aliased_depth + n_aliased) continue;
      aliased_depth[n_aliased++] = b;
      clear_depth_at(b, what);
    }
    const uint32_t z_base = depth_info & 0xFFF;
    const bool z_written = depth_only || (depth_control & 0x4) || (depth_control & 0x1);
    if (z_written && !(c0_written && z_base == c0_base) &&
        std::find(aliased_depth, aliased_depth + n_aliased, z_base) == aliased_depth + n_aliased) {
      clear_depth_at(z_base, what, stencil_only);
      // Depth written over c0's EDRAM (Dead Space 2's light buffer clear: depth-only mode, depth target on 2D0 where
      // the light buffer is): on the console the same EDRAM bytes are the color target's, so the color is cleared by
      // it (depth 0 and stencil 0 there: black). The renderer keeps depth and color in separate images, so the color
      // image is cleared too; without it the lights were added on top of the normals left there by the G-buffer.
      if (z_base == c0_base && (mask & 0xF) && !stencil_only) {
        // The color those EDRAM bits are (Gears of War 2 clears its ambient occlusion buffer, 16_16_FLOAT, this way:
        // with 0 the pixels it masks out, the player's character among them, were multiplied to black, and the
        // occlusion's temporal filter trusted that black history and dragged it behind moving things).
        uint32_t bits = 0;
        if (game_vertices) {
          const bool d24fs = (depth_info >> 16) & 0x1;
          const float z = std::clamp(game_z, 0.0f, d24fs ? 1.99999988f : 1.0f);
          // Truncated, in double: GTA IV's splash screens clear their 7e3 target to black through the depth path with a z
          // that, rounded as float, gave C00001 for C00000. That last bit is the red channel's lowest exponent bit in
          // 7e3 (0.5): the logo and legal screens came out on dark red.
          const uint32_t d24 = d24fs ? xenos::Float32To20e4(z, true)
                                     : uint32_t(std::min(double(z) * 16777215.0, 16777215.0));
          bits = (d24 << 8) | (stencil & 0xFF);
        }
        rex::x360::RegistrosCopia c = base;
        // Bit 31 (unused by the Xenos, like bit 30 in SDK patch 0030): RB_COLOR_CLEAR holds the target's EDRAM bits.
        c.rb_copy_control = 0u | (1u << 8) | kNullCommand | (1u << 31);
        c.rb_color_clear = bits;
        c.rb_color_clear_lo = bits;
        destinos_->Copiar(c);
        what += fmt::format(" c0={:03X}={:08X} (by the depth over it)", c0_base, bits);
      }
    }
    ++clears_;
    if (clear_shapes_.insert(what).second && clear_shapes_.size() <= 32) {
      REXLOG_INFO("[gpu] clear shape: msaa {}, mode {}:{}", msaa, r[g::XE_GPU_REG_RB_MODECONTROL] & 0x7, what);
    }
    // Single-sampled ones counted apart: they are new, and their game clear color (PS c0) tells whether 0 is right.
    if (msaa ? clears_logged_ < 12 : clears_logged_1x_ < 8) {
      ++(msaa ? clears_logged_ : clears_logged_1x_);
      auto f = [&](uint32_t index) {
        float v;
        std::memcpy(&v, &registers_[index], 4);
        return v;
      };
      REXLOG_INFO("[gpu] clear draw (msaa {}, pitch {}):{}; game had: PS c0 ({:.3f} {:.3f} {:.3f} {:.3f}) VS c0 "
                  "({:.3f} {:.3f} {:.3f} {:.3f}) RB_COLOR_CLEAR {:08X} RB_DEPTH_CLEAR {:08X} depthcontrol {:08X} "
                  "mask {:04X}",
                  msaa, Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF, what, f(0x4400), f(0x4401), f(0x4402),
                  f(0x4403), f(0x4000), f(0x4001), f(0x4002), f(0x4003), Register(g::XE_GPU_REG_RB_COLOR_CLEAR),
                  Register(g::XE_GPU_REG_RB_DEPTH_CLEAR), depth_control, mask);
    }
  }

  // The frame just presented as a 24-bit BMP in capturas/ next to the NRO (ZL + ZR + Down): what is on screen, to look
  // at rendering problems without describing them.
  // Captures every mp3_auto_captura_s and frame dumps on their schedule (seconds since the first Swap).
  void AutomaticCollection() {
    if (!telemetry_) return;
    const auto now = Clock::now();
    if (first_swap_ == Clock::time_point{}) return;
    const double t = std::chrono::duration<double>(now - first_swap_).count();
    const int32_t every = REXCVAR_GET(mp3_auto_captura_s);
    if (every > 0 && t >= next_auto_capture_s_) {
      next_auto_capture_s_ = t + every;
      SaveCapture(true);
    }
    const int32_t max_dumps = REXCVAR_GET(mp3_auto_volcados_max);
    if (auto_dumps_ < uint32_t(std::max(max_dumps, 0))) {
      if (next_auto_dump_s_ < 0) next_auto_dump_s_ = REXCVAR_GET(mp3_auto_volcado_primero_s);
      // Only a frame of the game itself (the 3D scene: hundreds of draws), not a menu or loading screen: dumps by
      // time alone caught Gears of War 2's menus (270 draws) while the problem was in the level.
      if (t >= next_auto_dump_s_ && last_frame_draws_ >= uint64_t(std::max(REXCVAR_GET(mp3_auto_volcado_dibujos), 0))) {
        ++auto_dumps_;
        const int32_t gap = REXCVAR_GET(mp3_auto_volcado_cada_s);
        next_auto_dump_s_ = gap > 0 ? t + gap : 1e30;
        REXLOG_INFO("[gpu] automatic frame dump {} at {:.0f} s ({} draws in the previous frame)", auto_dumps_, t,
                    last_frame_draws_);
        destinos_->PedirVolcado();
        // And every draw of that frame in the log, as the manual dump (ZL + ZR + Left) does (mp3_volcado_con_dibujos),
        // and the frame's structure (passes and resolves, a few lines each).
        draw_tracing_ = REXCVAR_GET(mp3_volcado_con_dibujos);
        draw_trace_n_ = 0;
        draw_trace_cap_ = 8000;
        packet_trace_n_ = 0;
        tracing_ = true;
        trace_lines_ = 0;
        trace_pass_ = 0;
      }
    }
  }

  void SaveCapture(bool small = false) {
    rex::ui::RawImage image;
    if (!presenter_ || !presenter_->CaptureGuestOutput(image) || !image.width || !image.height) {
      REXLOG_WARN("[gpu] capture: no guest output to capture");
      return;
    }
    const auto folder = rex::filesystem::GetExecutableFolder() / "capturas";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    char name[32];
    std::filesystem::path path;
    if (small) {
      std::snprintf(name, sizeof(name), "auto_%04u.bmp", auto_captures_++);
      path = folder / name;
      // Half size: each 2x2 block averaged into the top-left texel, then compacted (0.7 MB instead of 2.7 MB).
      const uint32_t w = image.width / 2, h = image.height / 2;
      std::vector<uint8_t> half(size_t(w) * h * 4);
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t* a = image.data.data() + size_t(2 * y) * image.stride + size_t(2 * x) * 4;
          const uint8_t* b = a + image.stride;
          for (int k = 0; k < 4; ++k) half[(size_t(y) * w + x) * 4 + k] = uint8_t((a[k] + a[k + 4] + b[k] + b[k + 4] + 2) / 4);
        }
      }
      image.data = std::move(half);
      image.width = w;
      image.height = h;
      image.stride = size_t(w) * 4;
    } else {
      // The first free number once, then counted: the file is written later, so looking for it again found the same
      // free name and captures taken close together overwrote each other.
      if (next_capture_ == UINT32_MAX) {
        for (next_capture_ = 0; next_capture_ < 1000; ++next_capture_) {
          std::snprintf(name, sizeof(name), "cap_%03u.bmp", next_capture_);
          if (!std::filesystem::exists(folder / name, ec)) break;
        }
      }
      std::snprintf(name, sizeof(name), "cap_%03u.bmp", next_capture_++);
      path = folder / name;
    }
    const uint32_t row = image.width * 3;  // 1280 * 3 is a multiple of 4; others are padded below
    const uint32_t padded = (row + 3) & ~3u;
    const uint32_t pixels = padded * image.height;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(2, 54 + pixels);
    put32(10, 54);
    put32(14, 40);
    put32(18, image.width);
    put32(22, image.height);
    header[26] = 1;
    header[28] = 24;
    put32(34, pixels);
    std::vector<uint8_t> data(pixels, 0);
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* s = image.data.data() + size_t(image.height - 1 - y) * image.stride;  // BMP rows go bottom-up
      uint8_t* d = data.data() + size_t(y) * padded;
      for (uint32_t x = 0; x < image.width; ++x, s += 4, d += 3) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
      }
    }
    // One write, on a thread of its own: small writes to the SD card take 10-50 ms each, and the ring thread must not
    // wait for them (the game stalls with it).
    std::vector<uint8_t> file(header, header + sizeof(header));
    file.insert(file.end(), data.begin(), data.end());
    const bool log = !small || auto_captures_ % 12 == 1;  // the automatic ones once a minute in the log
    rex::x360::EncolarEscrituraSd([path, file = std::move(file), log, w = image.width, h = image.height]() {
      if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(file.data(), 1, file.size(), f);
        std::fclose(f);
        if (log) REXLOG_INFO("[gpu] capture: {} ({}x{})", path.string(), w, h);
      } else {
        REXLOG_WARN("[gpu] capture: could not write {}", path.string());
      }
    });
  }

  // One line per draw of a traced light frame: what it draws with and what became of it.
  void DrawTraceLine(bool drawn, const char* why = nullptr) {
    namespace g = rex::graphics;
    if (draw_trace_n_ >= draw_trace_cap_) return;
    // Only draws whose viewport is at most this wide (mp3_traza_vport_max; 0 = all): the luminance chain of a frame
    // without its thousands of scene draws.
    // Only full-screen passes (mp3_traza_solo_quads: at most 6 vertices): the post-processing of a frame, any size.
    if (REXCVAR_GET(mp3_traza_solo_quads) && (Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR) >> 16) > 6) return;
    if (const int32_t maximo = REXCVAR_GET(mp3_traza_vport_max); maximo > 0) {
      float escala;
      const uint32_t bits = Register(g::XE_GPU_REG_PA_CL_VPORT_XSCALE);
      std::memcpy(&escala, &bits, 4);
      if (std::fabs(escala) * 2.0f > float(maximo)) return;
    }
    const uint32_t init = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    REXLOG_INFO("[draws] {:3} prim {} n {} vs {} ps {} c0 {:03X}/f{} z {:03X} mask {:04X} blend0 {:08X} colorctl {:08X} "
                "depthctl {:08X} stencil {:08X} mode {} pitch {} msaa {} -> {}",
                draw_trace_n_++, init & 0x3F, init >> 16, vs_ ? int(vs_->numero) : -1, ps_ ? int(ps_->numero) : -1,
                Register(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF, (Register(g::XE_GPU_REG_RB_COLOR_INFO) >> 16) & 0xF,
                Register(g::XE_GPU_REG_RB_DEPTH_INFO) & 0xFFF,
                Register(g::XE_GPU_REG_RB_COLOR_MASK), Register(g::XE_GPU_REG_RB_BLENDCONTROL0),
                Register(g::XE_GPU_REG_RB_COLORCONTROL), Register(g::XE_GPU_REG_RB_DEPTHCONTROL),
                Register(g::XE_GPU_REG_RB_STENCILREFMASK), Register(g::XE_GPU_REG_RB_MODECONTROL) & 0x7,
                Register(g::XE_GPU_REG_RB_SURFACE_INFO) & 0x3FFF, (Register(g::XE_GPU_REG_RB_SURFACE_INFO) >> 16) & 0x3,
                why ? why : drawn ? "drawn" : "REJECTED by the renderer");
    // The vertex input as D3D patched it: each fetch's format, swizzle, stride and offset. No vertex bytes: guest
    // memory is committed lazily, and even inside a vertex buffer the pages before the draw's first index may be
    // unmapped (reading vertex 0 crashed in game).
    for (size_t i = 0; i + 2 < vs_microcode_.size() && vs_; i += 3) {
      const uint32_t w0 = vs_microcode_[i], w1 = vs_microcode_[i + 1], w2 = vs_microcode_[i + 2];
      if ((w0 & 0x1F) != 0 || !((w0 >> 19) & 1)) continue;
      REXLOG_INFO("[draws]       vfetch r{} const {}.{} format {} swz {:03X} signed {} int {} stride {} offset {}",
                  (w0 >> 12) & 0x3F, (w0 >> 20) & 0x1F, (w0 >> 25) & 3, (w1 >> 16) & 0x3F, w1 & 0xFFF, (w1 >> 12) & 1,
                  (w1 >> 13) & 1, (w2 & 0xFF) * 4, ((w2 >> 8) & 0x7FFFFF) * 4);
    }
    if (vs_ && REXCVAR_GET(mp3_traza_bufers)) TraceIndexedBuffers();
  }

  // The exposure as the game sets it, every 10 s of play, without a dump: the tonemap's (ps_cd9469857a0c532a) and the
  // luminance adaptation's (ps_7067b48d822df127) pixel constants, by name.
  // The tonemaps: the cutscenes' (ps_cd9469857a0c532a) and gameplay's (ps_6671591252a5bce2, ps_fe226603f40e2533,
  // which blend Exposures.x towards .y and the gamma towards deSatContrastGammaIFX by the 128x128 effects mask).
  static bool IsTonemap(uint64_t huella) {
    return huella == UINT64_C(0xCD9469857A0C532A) || huella == UINT64_C(0x6671591252A5BCE2) ||
           huella == UINT64_C(0xFE226603F40E2533);
  }

  void LogExposureConstants() {
    static constexpr uint64_t kAdaptation = UINT64_C(0x7067B48D822DF127);
    const bool tonemap = IsTonemap(ps_->huella_contenedor);
    if (!tonemap && ps_->huella_contenedor != kAdaptation) return;
    Clock::time_point& next = tonemap ? next_tonemap_log_ : next_adaptation_log_;
    const auto now = Clock::now();
    // The gameplay tonemaps' painkiller / kill effect (InterlaceMask c147.w over 0): logged as soon as it starts, not
    // only on the 10 s tick (the screen went all red under it).
    const bool effect = tonemap && std::bit_cast<float>(registers_[0x4000 + (256 + 147) * 4 + 3]) > 0.0f;
    const bool effect_started = effect && !tonemap_effect_logged_;
    if (tonemap) tonemap_effect_logged_ = effect;
    if (now < next && !effect_started) return;
    next = now + std::chrono::seconds(10);
    auto c = [&](uint32_t n) {
      const uint32_t r = 0x4000 + (256 + n) * 4;
      return fmt::format("({:g} {:g} {:g} {:g})", std::bit_cast<float>(registers_[r]),
                         std::bit_cast<float>(registers_[r + 1]), std::bit_cast<float>(registers_[r + 2]),
                         std::bit_cast<float>(registers_[r + 3]));
    };
    if (tonemap) {
      REXLOG_INFO("[exposicion] tonemap {:016X}: gAdaptedLum c88 {} Exposures c137 {} ToneMapParams c139 {} BurnoutLimit "
                  "c142 {} deSatContrastGamma c46 {} deSatContrastGammaIFX c75 {} ColorCorrectTop c43 {} "
                  "ColorCorrectBottom c44 {} BrightnessSetting c130 {} KillFlashCol c146 {} InterlaceMask c147 {}",
                  ps_->huella_contenedor, c(88), c(137), c(139), c(142), c(46), c(75), c(43), c(44), c(130), c(146),
                  c(147));
    } else {
      REXLOG_INFO("[exposicion] adaptacion: AdaptedLumMin c140 {} AdaptedLumMax c141 {} ElapsedTime c159 {} AdaptTime "
                  "c160 {}",
                  c(140), c(141), c(159), c(160));
    }
  }

  // mp3_omitir_vs: the container fingerprints, parsed again only when the setting changes.
  bool OmittedVs(uint64_t huella) {
    const std::string& lista = REXCVAR_GET(mp3_omitir_vs);
    if (lista.empty()) return false;
    if (lista != omitted_vs_text_) {
      omitted_vs_text_ = lista;
      omitted_vs_.clear();
      for (size_t i = 0; i < lista.size();) {
        size_t fin = lista.find(',', i);
        if (fin == std::string::npos) fin = lista.size();
        const std::string h = lista.substr(i, fin - i);
        if (!h.empty()) omitted_vs_.push_back(std::strtoull(h.c_str(), nullptr, 16));
        i = fin + 1;
      }
      REXLOG_INFO("[gpu] mp3_omitir_vs: {} vertex shaders not drawn", omitted_vs_.size());
    }
    return std::find(omitted_vs_.begin(), omitted_vs_.end(), huella) != omitted_vs_.end();
  }

  // The vertex buffers a traced draw's shader fetches from other than its mesh's (fetch constants other than the first
  // fetch's): Max's jacket reads its cloth particles (float4, stride 16) by an index from the mesh. The renderer copies
  // each such buffer whole (sdk deko/dibujos.cpp, the shader's own fetches), so reading it here is as safe. Per buffer:
  // the fetch constant, how many floats are NaN, infinite or huge, the range of each component and the first float4s.
  void TraceIndexedBuffers() {
    uint32_t first = UINT32_MAX;
    std::vector<uint32_t> slots;
    for (size_t i = 0; i + 2 < vs_microcode_.size(); i += 3) {
      const uint32_t w0 = vs_microcode_[i];
      if ((w0 & 0x1F) != 0 || !((w0 >> 19) & 1)) continue;
      const uint32_t slot = ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3);
      if (first == UINT32_MAX) first = slot;
      if (slot != first && std::find(slots.begin(), slots.end(), slot) == slots.end()) slots.push_back(slot);
    }
    for (const uint32_t slot : slots) {
      const uint32_t d0 = registers_[0x4800 + slot * 2], d1 = registers_[0x4800 + slot * 2 + 1];
      const uint32_t address = d0 & 0x1FFFFFFC, bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
      if ((d0 & 0x3) != 3 || !bytes || bytes > (1u << 20) || address + bytes > 0x20000000u) {
        REXLOG_INFO("[draws]       buffer {}.{}: words {:08X} {:08X} (not read)", slot / 3, slot % 3, d0, d1);
        continue;
      }
      const uint8_t* p = memory_->TranslatePhysical(address);
      const uint32_t endian = d1 & 0x3;
      auto word = [&](uint32_t i) {
        uint32_t w;
        std::memcpy(&w, p + i * 4, 4);
        if (endian == 2) return __builtin_bswap32(w);
        if (endian == 1) return ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
        if (endian == 3) return (w << 16) | (w >> 16);
        return w;
      };
      const uint32_t n = bytes / 4;
      uint32_t nan = 0, inf = 0, huge = 0;
      float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
      for (uint32_t i = 0; i < n; ++i) {
        const float f = std::bit_cast<float>(word(i));
        if (std::isnan(f)) { ++nan; continue; }
        if (std::isinf(f)) { ++inf; continue; }
        if (std::fabs(f) > 1.0e4f) ++huge;
        lo[i & 3] = std::min(lo[i & 3], f);
        hi[i & 3] = std::max(hi[i & 3], f);
      }
      std::string first_values;
      for (uint32_t i = 0; i < n && i < 32; i += 4) {
        first_values += fmt::format(" ({:g} {:g} {:g} {:g})", std::bit_cast<float>(word(i)),
                                    i + 1 < n ? std::bit_cast<float>(word(i + 1)) : 0.0f,
                                    i + 2 < n ? std::bit_cast<float>(word(i + 2)) : 0.0f,
                                    i + 3 < n ? std::bit_cast<float>(word(i + 3)) : 0.0f);
      }
      REXLOG_INFO("[draws]       buffer {}.{}: base {:08X} {} bytes endian {}; {} NaN, {} inf, {} over 1e4; x {:g}..{:g} "
                  "y {:g}..{:g} z {:g}..{:g} w {:g}..{:g}; first{}",
                  slot / 3, slot % 3, address, bytes, endian, nan, inf, huge, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                  lo[3], hi[3], first_values);
    }
    // The pixel shader's textures: fetch constant of each sampler (base, format, size, dimension, swizzle, signs).
    // Every sampler (blended materials use up to 8: S4-S7 were not shown), with the raw words for the mip range.
    for (size_t i = 0; ps_ && i < ps_->samplers.size() && i < 16; ++i) {
      const uint32_t reg = ps_->samplers[i].registro;
      if (reg >= 32) continue;
      const uint32_t* f = &registers_[0x4800 + reg * 6];
      REXLOG_INFO("[draws]       s{} tipo {}: base {:08X} fmt {} {}x{} dim {} swz {:03X} signs {:02X} tiled {} type {} "
                  "words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  reg, ps_->samplers[i].tipo, f[1] & 0xFFFFF000u, f[1] & 0x3F, (f[2] & 0x1FFF) + 1,
                  ((f[2] >> 13) & 0x1FFF) + 1, (f[5] >> 9) & 0x3, (f[3] >> 1) & 0xFFF, (f[0] >> 2) & 0xFF,
                  (f[0] >> 31) & 1, f[0] & 0x3, f[0], f[1], f[2], f[3], f[4], f[5]);
    }
    // The pixel shader's first float constants (a material without textures shows its colour here).
    if (ps_) {
      std::string pc;
      // With mp3_traza_solo_quads (few draws: the post-processing) every non-zero one up to c255: Max Payne 3's tonemap
      // reads its exposure from c88 and c137.
      // In the whole frame's trace too, for its full-screen passes (at most 6 vertices): the tonemap's exposure.
      const uint32_t hasta =
          REXCVAR_GET(mp3_traza_solo_quads) || (Register(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR) >> 16) <= 6
              ? 256 : 16;
      for (uint32_t c = 0; c < hasta; ++c) {
        const uint32_t r = 0x4000 + (256 + c) * 4;
        if (c >= 16 && !(registers_[r] | registers_[r + 1] | registers_[r + 2] | registers_[r + 3])) continue;
        pc += fmt::format(" c{}=({:g} {:g} {:g} {:g})", c, std::bit_cast<float>(registers_[r]),
                          std::bit_cast<float>(registers_[r + 1]), std::bit_cast<float>(registers_[r + 2]),
                          std::bit_cast<float>(registers_[r + 3]));
      }
      REXLOG_INFO("[draws]       ps constants{}", pc);
    }
  }

  // --- Frame trace -----------------------------------------------------------
  //
  // How the game builds a heavy frame, once a minute up to three times: each pass (a run of draws into the same
  // targets with the same window offset), each resolve and the Swap, one line each. This is what is needed to
  // compare the frame with what the renderer does (multisampling, predicated tiling, EDRAM bases).

  struct FrameState {
    uint64_t draws = 0;              // geometry draws in the frame being counted
    uint64_t predicated_skipped = 0;
  };

  static constexpr uint32_t kTraceMaxLines = 400;
  uint32_t trace_max_lines_ = kTraceMaxLines;  // 3000 for the trace of ZL + ZR + Left

  void FrameTraceSwap() {
    const auto now = Clock::now();
    if (tracing_) {
      FrameTraceClosePass();
      REXLOG_INFO("[frame] end: {} geometry draws, {} predicated packets skipped, {} passes, swap frontbuffer "
                  "fetch0 {:08X} {:08X}",
                  frame_.draws, frame_.predicated_skipped, trace_pass_, Register(0x4800), Register(0x4801));
      tracing_ = false;
      ++traces_done_;
      next_trace_ = now + std::chrono::seconds(std::max(REXCVAR_GET(mp3_traza_cada_s), 1));
    } else if (telemetry_ && traces_done_ < uint32_t(std::max(REXCVAR_GET(mp3_trazas_max), 0)) && frame_.draws >= 800 &&
               now >= next_trace_ && now - first_swap_ >= std::chrono::seconds(60)) {
      tracing_ = true;
      trace_lines_ = 0;
      trace_pass_ = 0;
      trace_key_.fill(0);
      trace_pass_draws_ = 0;
      trace_max_lines_ = kTraceMaxLines;
      REXLOG_INFO("[frame] trace {} of a frame ({} draws in the previous one)", traces_done_ + 1, frame_.draws);
    }
    // Per-draw trace of a light frame (loading screens, menus: the interface), with telemetry.
    if (probing_) {
      probing_ = false;  // its draws' counts come back in ProbePoll
      REXLOG_INFO("[probe] end of the probed frame: {} base-pass draws", probes_.size());
    }
    if (draw_tracing_) {
      draw_tracing_ = false;
      REXLOG_INFO("[draws] end of the traced frame");
      // The next frame: which base-pass draw paints the probe square (mp3_sonda).
      unsigned px = 0, py = 0;
      if (destinos_ && std::sscanf(REXCVAR_GET(mp3_sonda).c_str(), "%u,%u", &px, &py) == 2) {
        probing_ = true;
        probe_x_ = px;
        probe_y_ = py;
        probes_.clear();
        probe_started_ = now;
      }
    } else if (telemetry_ && draw_traces_ < 8 && frame_.draws >= 10 && frame_.draws <= 400 && now >= next_draw_trace_) {
      draw_tracing_ = true;
      ++draw_traces_;
      draw_trace_n_ = 0;
      draw_trace_cap_ = 400;
      next_draw_trace_ = now + std::chrono::seconds(20);
      REXLOG_INFO("[draws] trace {} of a light frame ({} draws in the previous one)", draw_traces_, frame_.draws);
    }
    if (first_swap_ == Clock::time_point{}) first_swap_ = now;
    last_frame_draws_ = frame_.draws;
    frame_ = {};
  }

  void FrameTraceLine(const std::string& line) {
    if (trace_lines_++ < trace_max_lines_) REXLOG_INFO("[frame] {}", line);
  }

  std::array<uint32_t, 9> FrameTraceKey() const {
    namespace g = rex::graphics;
    return {Register(g::XE_GPU_REG_RB_SURFACE_INFO), Register(g::XE_GPU_REG_RB_COLOR_INFO),
            Register(g::XE_GPU_REG_RB_COLOR1_INFO),  Register(g::XE_GPU_REG_RB_COLOR2_INFO),
            Register(g::XE_GPU_REG_RB_COLOR3_INFO),  Register(g::XE_GPU_REG_RB_DEPTH_INFO),
            Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET), Register(g::XE_GPU_REG_RB_MODECONTROL),
            Register(g::XE_GPU_REG_RB_COLOR_MASK)};
  }

  void FrameTraceClosePass() {
    if (!trace_pass_draws_) return;
    const auto& k = trace_key_;
    std::string colors;
    for (uint32_t i = 0; i < 4; ++i) {
      if ((k[8] >> (i * 4)) & 0xF) {
        colors += fmt::format(" c{}={:03X}/f{}", i, k[1 + i] & 0xFFF, (k[1 + i] >> 16) & 0xF);
      }
    }
    FrameTraceLine(fmt::format(
        "pass {}: {} draws ({} unknown VS), pitch {} msaa {}{} z={:03X}/f{} window ({},{}) mode {} "
        "scissor {:08X}-{:08X} vport x {:.1f}+{:.1f} y {:.1f}+{:.1f} bins {:X}/{:X}",
        trace_pass_, trace_pass_draws_, trace_pass_no_vs_, k[0] & 0x3FFF, (k[0] >> 16) & 0x3, colors, k[5] & 0xFFF,
        (k[5] >> 16) & 0x1, int32_t(k[6] << 17) >> 17, int32_t((k[6] >> 16) << 17) >> 17, k[7] & 0x7,
        trace_scissor_tl_, trace_scissor_br_, trace_vport_[0], trace_vport_[1], trace_vport_[2], trace_vport_[3],
        bin_select_, bin_mask_));
    ++trace_pass_;
    trace_pass_draws_ = 0;
    trace_pass_no_vs_ = 0;
  }

  void FrameTraceDraw() {
    ++frame_.draws;
    if (!tracing_) return;
    const auto key = FrameTraceKey();
    if (key != trace_key_ || !trace_pass_draws_) {
      FrameTraceClosePass();
      trace_key_ = key;
      namespace g = rex::graphics;
      trace_scissor_tl_ = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
      trace_scissor_br_ = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
      for (uint32_t i = 0; i < 4; ++i) {
        // PA_CL_VPORT_XSCALE, XOFFSET, YSCALE, YOFFSET
        const uint32_t v = Register(g::XE_GPU_REG_PA_CL_VPORT_XSCALE + i);
        std::memcpy(&trace_vport_[i], &v, 4);
      }
    }
    ++trace_pass_draws_;
    trace_pass_no_vs_ += vs_ == nullptr;
  }

  // Every distinct resolve, once, when it is first seen (up to 200): the effects (kinesis, hits, screen blood) show up
  // with the time they start, to compare with what is on screen.
  void ResolveCatalog() {
    namespace g = rex::graphics;
    if (resolve_seen_.size() >= 200) return;
    const uint32_t control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    const uint32_t source = control & 0x7;
    static constexpr uint32_t kColorInfo[4] = {g::XE_GPU_REG_RB_COLOR_INFO, g::XE_GPU_REG_RB_COLOR1_INFO,
                                               g::XE_GPU_REG_RB_COLOR2_INFO, g::XE_GPU_REG_RB_COLOR3_INFO};
    const uint32_t src_info = source < 4 ? Register(kColorInfo[source]) : Register(g::XE_GPU_REG_RB_DEPTH_INFO);
    const uint32_t surface = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL), br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    const uint32_t dest = Register(g::XE_GPU_REG_RB_COPY_DEST_BASE), pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    const uint32_t info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO), offset = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    // The rectangle D3D puts in fetch constant 0 (three vertices), as the renderer reads it.
    const uint32_t f0 = Register(0x4800), f1 = Register(0x4801);
    float rect[6] = {};
    if ((f0 & 0x3) == 3 && ((f1 >> 2) & 0xFFFFFF) == 6) {
      const uint8_t* v = memory_->TranslatePhysical(f0 & ~uint32_t(0x3));
      for (int i = 0; i < 6; ++i) {
        uint32_t w;
        std::memcpy(&w, v + i * 4, 4);
        w = xenos::GpuSwap(w, static_cast<xenos::Endian>(f1 & 0x3));
        std::memcpy(&rect[i], &w, 4);
      }
    }
    const uint64_t key = XXH3_64bits(std::array<uint32_t, 10>{control, src_info, surface, tl, br, dest, pitch, info,
                                                              offset, uint32_t(rect[0]) ^ uint32_t(rect[1]) << 16}
                                         .data(),
                                     40);
    if (!resolve_seen_.insert(key).second) return;
    REXLOG_INFO("[resolve] new: {} base {:03X} f{} pitch {} msaa {} scissor {:08X}-{:08X} window ({},{}) rect "
                "({:.0f},{:.0f})-({:.0f},{:.0f})-({:.0f},{:.0f}) -> {:08X} pitch {} height {} fmt {} swap {} control "
                "{:08X}",
                source < 4 ? fmt::format("c{}", source) : std::string("depth"), src_info & 0xFFF,
                (src_info >> 16) & 0xF, surface & 0x3FFF, (surface >> 16) & 0x3, tl, br, int32_t(offset << 17) >> 17,
                int32_t((offset >> 16) << 17) >> 17, rect[0], rect[1], rect[2], rect[3], rect[4], rect[5], dest,
                pitch & 0x3FFF, (pitch >> 16) & 0x3FFF, (info >> 7) & 0x3F, (info >> 24) & 0x1, control);
  }

  void FrameTraceCopy() {
    if (!tracing_) return;
    FrameTraceClosePass();
    namespace g = rex::graphics;
    const uint32_t control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    const uint32_t source = control & 0x7;
    const uint32_t surface = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    static constexpr uint32_t kColorInfo[4] = {g::XE_GPU_REG_RB_COLOR_INFO, g::XE_GPU_REG_RB_COLOR1_INFO,
                                               g::XE_GPU_REG_RB_COLOR2_INFO, g::XE_GPU_REG_RB_COLOR3_INFO};
    const uint32_t src_info = source < 4 ? Register(kColorInfo[source]) : Register(g::XE_GPU_REG_RB_DEPTH_INFO);
    const uint32_t dest_info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    const uint32_t dest_pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    const uint32_t offset = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    FrameTraceLine(fmt::format(
        "resolve from {} base {:03X} f{} pitch {} msaa {} rect {:08X}-{:08X} window ({},{}) -> {:08X} pitch {} "
        "height {} fmt {} control {:08X} (copy {}, clear color {} depth {})",
        source < 4 ? fmt::format("c{}", source) : std::string("depth"), src_info & 0xFFF, (src_info >> 16) & 0xF,
        surface & 0x3FFF, (surface >> 16) & 0x3, Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL),
        Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR), int32_t(offset << 17) >> 17,
        int32_t((offset >> 16) << 17) >> 17, Register(g::XE_GPU_REG_RB_COPY_DEST_BASE), dest_pitch & 0x3FFF,
        (dest_pitch >> 16) & 0x3FFF, (dest_info >> 7) & 0x3F, control, (control >> 20) & 0x3, (control >> 8) & 0x1,
        (control >> 9) & 0x1));
  }
#endif

  // EVENT_WRITE_ZPD, as the SDK's CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD: D3D marks the
  // structure with 0xFFFFFEED on Issue(END) and GetData answers "not yet" while the mark is there. D3D's query is 64
  // bytes, the begin structure at +0x20 and the end one at +0 (the count is end minus begin, the begin one stays
  // zeroed). With mp3_oclusion_real the renderer counts on the GPU the samples drawn between the two; the end keeps
  // its mark until that count is back (OcclusionPoll), or mp3_oclusion_espera_ms passed (then it is faked visible).
  void OcclusionQuery(uint32_t address) {
    if (!address) {
      return;
    }
    const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEEDu);  // written big-endian, the struct is LE
    auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
    const bool end_via_z_pass = counts->ZPass_A == kQueryFinished || counts->ZPass_B == kQueryFinished;
    const bool end_via_z_fail = counts->ZFail_A == kQueryFinished || counts->ZFail_B == kQueryFinished;
    const bool end = end_via_z_pass || end_via_z_fail;
    // What the game left there, for the first queries and every traced frame (Gears of War 2 culled its whole world:
    // whether its occlusion queries come back visible is the first thing to know).
    if (queries_logged_ < 48 || draw_tracing_) {
      ++queries_logged_;
      const uint32_t* w = reinterpret_cast<const uint32_t*>(counts);
      REXLOG_INFO("[query] ZPD at {:08X} ctl {:08X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> {}",
                  address, Register(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_CTL), w[0], w[1], w[2], w[3], w[4], w[5],
                  w[6], w[7], end ? "end" : "begin: zeroed");
    }
    const uint32_t query = address & ~0x3Fu;
    // A query issued again: whatever was still waited for at that structure is not written over it any more.
    std::erase_if(pending_queries_, [&](const PendingQuery& q) { return (q.address & ~0x3Fu) == query; });
    const bool real = REXCVAR_GET(mp3_oclusion_real) && destinos_;
    if (real && !end) {
      std::memset(counts, 0, sizeof(*counts));
      destinos_->EmpezarOclusion(query);
      return;
    }
    if (real) {
      if (const uint64_t ticket = destinos_->TerminarOclusionAsincrona(query)) {
        pending_queries_.push_back({address, ticket, std::chrono::steady_clock::now()});
        return;  // the mark stays: GetData waits
      }
    }
    std::memset(counts, 0, sizeof(*counts));
    // The test option: without D3D's end marker, every second ZPD is taken as a query's end (begin and end come in
    // pairs), so a query is visible even if the marker never shows up where we look.
    const bool end_by_pair = REXCVAR_GET(mp3_consultas_siempre_visibles) && (zpd_count_++ & 1);
    if (end || end_by_pair) {
      counts->ZPass_A = kFakeSampleCount;
      counts->Total_A = kFakeSampleCount;
    }
  }

  // The probe frame's counts: once all are back (or 3 s passed), the draws that painted the probe square, in order;
  // the last one is what shows there.
  void ProbePoll() {
    if (probes_.empty() || probing_) return;
    bool all = true;
    for (Probe& p : probes_) {
      if (p.samples == UINT64_MAX && p.ticket) {
        uint64_t n = 0;
        if (destinos_->ResultadoOclusion(p.ticket, n)) p.samples = n; else all = false;
      }
    }
    if (!all && std::chrono::steady_clock::now() - probe_started_ < std::chrono::seconds(3)) return;
    std::string text;
    const Probe* last = nullptr;
    for (const Probe& p : probes_) {
      if (p.samples != UINT64_MAX && p.samples > 0) {
        text += fmt::format(" #{} vs{} ps{} n{} ({})", p.index, p.vs, p.ps, p.count, p.samples);
        last = &p;
      }
    }
    REXLOG_INFO("[probe] square {},{}: {} base-pass draws painted it:{}", probe_x_, probe_y_,
                last ? "these" : "no", text);
    if (last) {
      REXLOG_INFO("[probe] the one that shows: #{} vs{} ps{} ({} vertices)", last->index, last->vs, last->ps,
                  last->count);
    }
    probes_.clear();
  }

  // The real queries' counts back to the game, in order; one waited for too long is answered visible.
  void OcclusionPoll() {
    if (pending_queries_.empty()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto limit = std::chrono::milliseconds(std::max(REXCVAR_GET(mp3_oclusion_espera_ms), 0));
    const int32_t no_wait_ms = REXCVAR_GET(mp3_oclusion_sin_esperar_ms);
    const bool game_waits = g_mp3_query_waited.exchange(false, std::memory_order_acq_rel) && no_wait_ms >= 0;
    // The ring has been quiet for a while with a query waited for: the game may be waiting for one whose work is still
    // being recorded (it would never be submitted).
    if (destinos_ && now - pending_queries_.front().ended >= std::chrono::milliseconds(1) &&
        now - ring_activity_ >= std::chrono::milliseconds(1)) {
      destinos_->EnviarOclusiones();
    }
    size_t done = 0;
    for (; done < pending_queries_.size(); ++done) {
      const PendingQuery& q = pending_queries_[done];
      uint64_t samples = 0;
      const bool counted = destinos_ && destinos_->ResultadoOclusion(q.ticket, samples);
      const bool answer_now = game_waits && now - q.ended >= std::chrono::milliseconds(no_wait_ms);
      if (!counted && now - q.ended < limit && !answer_now) {
        break;  // the later ones are in later work
      }
      auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(q.address);
      const uint32_t n = counted ? uint32_t(std::min<uint64_t>(samples, INT32_MAX)) : uint32_t(kFakeSampleCount);
      counts->Total_A = n;
      counts->Total_B = 0;
      counts->ZFail_A = 0;
      counts->ZFail_B = 0;
      counts->StencilFail_A = 0;
      counts->StencilFail_B = 0;
      counts->ZPass_B = 0;
      std::atomic_thread_fence(std::memory_order_release);
      counts->ZPass_A = n;  // the mark goes last
      if (!counted) {
        ++(now - q.ended < limit ? queries_not_waited_ : queries_timed_out_);
      } else {
        ++queries_answered_;
        if (n == 0) ++queries_hidden_;
        queries_wait_us_ += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - q.ended).count());
      }
    }
    pending_queries_.erase(pending_queries_.begin(), pending_queries_.begin() + done);
  }

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  std::unique_ptr<rex::x360::DestinosNativos> destinos_;  // the universal deko renderer backend
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;
  std::atomic<bool> active_{false};

  std::vector<uint32_t> registers_;
  uint64_t bin_mask_ = ~uint64_t(0);
  uint64_t bin_select_ = ~uint64_t(0);

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0};
  std::atomic<uint32_t> read_pointer_writeback_{0};
  std::atomic<uint32_t> counter_{0};  // vblanks + swaps, for EVENT_WRITE_SHD
  std::atomic<uint64_t> vblanks_{0};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;
  std::atomic<bool> ring_dormido_{false};  // the ring thread is (about to be) waiting on ring_cv_
  static constexpr uint32_t kRptrPaso = 256;  // words between the read pointer writebacks inside a batch
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;

  // Only the ring thread touches these.
#if MP3_DEKO3D
  std::vector<uint32_t> microcode_;     // the last IM_LOAD, host byte order
  std::vector<uint32_t> vs_microcode_;  // the last VS load, as D3D patched it
  const rex::x360::EntradaShader* vs_ = nullptr;
  const rex::x360::EntradaShader* ps_ = nullptr;
  std::unordered_map<uint64_t, uint64_t> vs_generations_;
  uint64_t generacion_vs_ = 0, generacion_constantes_vs_ = 0, generacion_constantes_ps_ = 0;
  uint64_t generacion_fetch_ = 0, generacion_encuadre_ = 0;
  uint64_t geometria_ = 0, enviados_ = 0, sin_vs_ = 0, sin_ps_ = 0;
  std::unordered_map<uint64_t, uint64_t> unknown_vs_skips_;          // microcode hash -> draws skipped
  std::unordered_map<uint64_t, std::string> unknown_vs_sample_;      // one draw's state each
  std::array<uint64_t, 16> color_formats_{};
  std::array<uint64_t, 2> depth_formats_{};
  std::array<uint64_t, 32> primitive_types_{};
  std::unordered_set<uint64_t> resolve_seen_;  // ResolveCatalog
  std::unordered_set<std::string> clear_shapes_;  // EmulateClear
  std::unordered_set<uint32_t> bandas_vistas_;     // WriteRegister: the tile heights widened, logged once each
  uint32_t clear_rect_ = 0;          // guest buffer with the emulated clears' rectangle
  uint32_t alias_rect_ = 0;          // guest buffer with a downsampling resolve's rectangle (ResolveThroughMsaaAlias)
  uint64_t frame_serial_ = 1;        // Swaps seen, for drawn_targets_
  // Color targets (EDRAM base, format, pitch) drawn into, with the frame they were last drawn in.
  std::unordered_map<uint32_t, uint64_t> drawn_targets_;
  uint32_t rect_buffer_ = 0;         // guest buffer with the rectangle lists made quads (ExpandRectList)
  uint32_t rect_buffer_size_ = 0;
  uint64_t last_frame_draws_ = 0;   // geometry draws of the last finished frame
  uint32_t packet_trace_n_ = 0;     // [pm4] lines of the traced frame
  uint32_t queries_logged_ = 0;     // [query] lines
  uint64_t zpd_count_ = 0;          // mp3_consultas_siempre_visibles
  struct PendingQuery {
    uint32_t address;  // the end structure, still marked
    uint64_t ticket;
    std::chrono::steady_clock::time_point ended;
  };
  std::deque<PendingQuery> pending_queries_;
  std::chrono::steady_clock::time_point ring_activity_;  // the ring thread's last batch of packets  // real occlusion queries the GPU has not counted yet, oldest first
  uint64_t queries_answered_ = 0, queries_hidden_ = 0, queries_timed_out_ = 0, queries_wait_us_ = 0;
  uint64_t queries_not_waited_ = 0;  // answered visible at once because the game was waiting
  uint64_t rect_lists_ = 0;
  uint32_t rect_unexpanded_logged_ = 0;
  double next_auto_capture_s_ = 10, next_auto_dump_s_ = -1;  // AutomaticCollection
  uint32_t auto_captures_ = 0, auto_dumps_ = 0;
  uint64_t depth_greater_ = 0, depth_less_ = 0, clears_ = 0, clears_logged_ = 0, msaa_draws_ = 0;
  // Per depth buffer: draws testing GREATER/GEQUAL and LESS/LEQUAL into it (EmulateClear's far value).
  std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> depth_direction_;
  // A depth buffer for depth_direction_: its EDRAM base, its depth format and its pitch. Gears of War 2 keeps its
  // 800-wide D24S8 shadow maps (LEQUAL) and a 640-wide float depth buffer (GEQUAL) at the same base 5A0; keyed by the
  // base alone the GEQUAL draws won, the shadow maps were filled with 0 and characters in shadow came out black.
  static uint32_t DepthKey(uint32_t depth_info, uint32_t surface_info) {
    return (depth_info & 0xFFF) | (((depth_info >> 16) & 0x1) << 12) | ((surface_info & 0x3FFF) << 13);
  }
  FrameState frame_;
  bool telemetry_ = false;
  bool hide_glow_ = false;  // sin_resplandor.flag
  std::array<uint64_t, 8> index_orders_{};
  uint32_t vs_physical_ = 0;
  uint32_t syncs_reported_ = 0;
  uint32_t skip_mode_ = 0, capture_after_skip_ = 0, skip_resume_ = 0;
  std::unordered_map<uint32_t, uint32_t> depth_vs_frame_, depth_vs_last_;  // search modes: VS number -> draws
  std::vector<uint32_t> search_vs_;  // the search modes' shader numbers (vertex or pixel, search_ps_)
  struct Probe {
    uint32_t index, vs, ps, count;
    uint64_t ticket, samples;
  };
  bool probing_ = false;
  uint32_t probe_x_ = 0, probe_y_ = 0;
  std::vector<Probe> probes_;
  std::chrono::steady_clock::time_point probe_started_;
  bool search_ps_ = false;
  uint32_t broken_vs_logged_ = 0;
  uint32_t tree_capture_after_ = 0;
  uint64_t skipped_broken_vs_ = 0;
  uint64_t prepass_skipped_ = 0;
  uint64_t skipped_by_mode_ = 0;
  uint64_t clears_logged_1x_ = 0;
  std::unordered_set<uint64_t> glow_logged_;
  std::unordered_set<uint64_t> unknown_vs_logged_;
  uint32_t glow_draws_logged_ = 0;
  bool draw_tracing_ = false;
  uint32_t draw_traces_ = 0, draw_trace_n_ = 0, draw_trace_cap_ = 400;
  Clock::time_point next_tonemap_log_{}, next_adaptation_log_{}, next_fxaa_log_{};
  bool tonemap_effect_logged_ = false;
  std::string omitted_vs_text_;
  std::vector<uint64_t> omitted_vs_;
  uint32_t volume_traces_ = 0;  // the light volumes' draws logged (Draw)
  uint32_t next_capture_ = UINT32_MAX;  // SaveCapture
  Clock::time_point next_draw_trace_{};
  bool tracing_ = false;
  uint32_t traces_done_ = 0, trace_lines_ = 0, trace_pass_ = 0;
  uint64_t trace_pass_draws_ = 0, trace_pass_no_vs_ = 0;
  std::array<uint32_t, 9> trace_key_{};
  uint32_t trace_scissor_tl_ = 0, trace_scissor_br_ = 0;
  float trace_vport_[4] = {};
  Clock::time_point next_trace_{}, first_swap_{};
#endif
  uint64_t swaps_ = 0, draws_ = 0, packets_ = 0, waits_cut_short_ = 0;
  uint64_t reg_mem_waits_ = 0, reg_mem_wait_ns_ = 0;  // WAIT_REG_MEM that had to wait, and how long in total
  uint64_t reported_swaps_ = 0, reported_draws_ = 0, reported_packets_ = 0;
  Clock::time_point last_report_;
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<RingGraphicsSystem>();
}

}  // namespace mp3::gpu

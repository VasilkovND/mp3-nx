// mp3-nx - CPU time the console has no use for.
//
// The Xbox 360 has six hardware threads; the Switch gives the game three cores. GTA IV's job threads take a spin lock
// (sub_821D5F70, named mp3_SpinLockAcquire in codegen/names.toml) by spinning: a compare-and-swap, then an empty loop
// counting up to 8, 16 ... 1024 between tries. The first console run had four of them at 30-54 % of a core each in that
// loop and the CPU at 280-305 % of 300 %, while the thread holding the lock waited for a core: the frame rate fell and
// the audio stuttered. Here the lock is taken the same way (the word goes 0 -> 1, big-endian, the release is the game's
// own store of 0), and a thread that does not get it yields its core, then sleeps briefly.

#include "mp3_pch.h"

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(mp3_spinlock_nativo, true, "MP3",
                    "El spin lock de los hilos de trabajo del juego (sub_821D5F70) cede el nucleo y duerme en vez de "
                    "girar en vacio (tres nucleos, no seis hilos)");
REXCVAR_DEFINE_INT32(mp3_spinlock_sueno_us, 20, "MP3",
                     "Microsegundos que duerme un hilo que no consigue el spin lock tras ceder el nucleo varias veces");

REXCVAR_DEFINE_INT32(mp3_espera_gpu_us, 100, "MP3",
                     "Microsegundos que duerme el hilo de render del juego mientras espera a que la GPU termine "
                     "fotogramas (en vez de preguntar sin parar); 0 = como en la Xbox 360");

REXCVAR_DEFINE_INT32(mp3_espera_d3d_us, 50, "MP3",
                     "Microsegundos que duerme un hilo entre pasos de las esperas de Direct3D a la GPU (sub_82A41320) "
                     "tras ceder el nucleo unas veces; -1 = girar como en la Xbox 360");

#ifdef MP3_TIENE_mp3_SpinLockAcquire  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_SpinLockAcquire);
REX_EXTERN(mp3_SpinLockAcquire) {
  if (!REXCVAR_GET(mp3_spinlock_nativo)) {
    __imp__mp3_SpinLockAcquire(ctx, base);
    return;
  }
  auto* word = reinterpret_cast<uint32_t*>(base + ctx.r3.u32);
  const uint32_t taken = __builtin_bswap32(1u);
  const int64_t sleep_ns = int64_t(REXCVAR_GET(mp3_spinlock_sueno_us)) * 1000;
  for (uint32_t attempt = 0;; ++attempt) {
    uint32_t expected = 0;
    if (__atomic_load_n(word, __ATOMIC_RELAXED) == 0 &&
        __atomic_compare_exchange_n(word, &expected, taken, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      break;
    }
#if defined(__SWITCH__)
    if (attempt < 8) {
      for (int i = 0; i < 32; ++i) __asm__ __volatile__("yield");
    } else if (attempt < 32) {
      svcSleepThread(0);  // yield the core to a thread of the same priority (the holder, if it is ready)
    } else {
      svcSleepThread(sleep_ns);
    }
#else
    std::this_thread::yield();
#endif
  }
  ctx.r3.u64 = 0;  // what the guest's last compare-and-swap leaves there: the old value
}
#endif  // MP3_TIENE_mp3_SpinLockAcquire

// Direct3D's frames-in-flight query (sub_82A46D70). The game's render thread calls it in a loop (sub_828BF420) until
// fewer than two frames are waiting for the GPU: the first console runs had that thread at 16 % of a core in the
// loop. While the answer is "two or more" the thread sleeps a little before asking again.
#ifdef MP3_TIENE_mp3_D3D_QueryFramesInFlight  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_D3D_QueryFramesInFlight);
REX_EXTERN(mp3_D3D_QueryFramesInFlight) {
  const uint32_t result = ctx.r4.u32;
  __imp__mp3_D3D_QueryFramesInFlight(ctx, base);
  const int32_t sleep_us = REXCVAR_GET(mp3_espera_gpu_us);
  if (sleep_us > 0 && __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + result + 8)) >= 2) {
#if defined(__SWITCH__)
    svcSleepThread(int64_t(sleep_us) * 1000);
#else
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
#endif
  }
}
#endif  // MP3_TIENE_mp3_D3D_QueryFramesInFlight

// One step of Direct3D's waits for the GPU (sub_82A41320): r3 = 1 while the caller keeps waiting. On the 360 the step
// is a short spin; here the GPU is our ring thread on another core, so a waiting thread gives its core away (the
// first steps of a wait only yield, the later ones sleep). A wait is told from the next by its state's address and
// the time since the last step.
#ifdef MP3_TIENE_mp3_D3D_WaitStep  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_D3D_WaitStep);
REX_EXTERN(mp3_D3D_WaitStep) {
  const uint32_t state = ctx.r3.u32;
  __imp__mp3_D3D_WaitStep(ctx, base);
  const int32_t sleep_us = REXCVAR_GET(mp3_espera_d3d_us);
  if (sleep_us < 0 || ctx.r3.u32 == 0) {
    return;
  }
  using Clock = std::chrono::steady_clock;
  thread_local uint32_t last_state = 0;
  thread_local Clock::time_point last_step{};
  thread_local uint32_t steps = 0;
  const Clock::time_point now = Clock::now();
  if (state != last_state || now - last_step > std::chrono::milliseconds(2)) {
    steps = 0;
  }
  last_state = state;
  ++steps;
#if defined(__SWITCH__)
  if (steps <= 16 || sleep_us == 0) {
    svcSleepThread(0);
  } else {
    svcSleepThread(int64_t(sleep_us) * 1000);
  }
#else
  if (steps <= 16 || sleep_us == 0) {
    std::this_thread::yield();
  } else {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  }
#endif
  last_step = Clock::now();
}
#endif  // MP3_TIENE_mp3_D3D_WaitStep

// The XAudio2 processing thread (sub_821909D0, its entry; it never returns while the game runs): the mixer and the
// voices' DSP, ~1.4 of the three cores the process gets in run37. When the process mask has core 3 (Horizon gives it
// to a homebrew only with an exheader override for hbloader: atmosphere/contents/0100B00B51230000/exefs/main.npdm with
// core 3 allowed), the thread prefers core 3; its mask stays the whole process mask, so it can still move (a thread
// locked to core 3 alone can be starved there by system threads). Without core 3 nothing changes.
REXCVAR_DEFINE_BOOL(mp3_audio_nucleo3, true, "MP3",
                    "El hilo de audio del juego (XAudio2) prefiere el nucleo 3 si el proceso lo tiene (override de "
                    "exheader de hbloader)");

REXCVAR_DEFINE_BOOL(mp3_audio_motor_nucleo3, true, "MP3",
                    "El hilo del motor de audio del juego (el que llama a sub_8291A580) prefiere el nucleo 3 si el "
                    "proceso lo tiene");

// The audio engine's update: the first call on a thread (not the game's main thread) makes that thread prefer core 3,
// next to the mixer. In run49 the frame's threads (main 54 %, render 42 %, ring 59 %) shared cores 0-2 with the XMA
// decoder and this thread, at ~280 % of 300 %.
#ifdef MP3_TIENE_mp3_AudioEngineUpdate  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_AudioEngineUpdate);
REX_EXTERN(mp3_AudioEngineUpdate) {
#if defined(__SWITCH__)
  static thread_local bool visto = false;
  if (!visto) {
    visto = true;
    auto* hilo = rex::system::XThread::GetCurrentThread();
    const std::string nombre = hilo ? hilo->name() : std::string();
    if (REXCVAR_GET(mp3_audio_motor_nucleo3) && nombre.rfind("Main XThread", 0) != 0) {
      u64 proceso = 0;
      bool movido = false;
      if (R_SUCCEEDED(svcGetInfo(&proceso, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && (proceso & 0x8)) {
        Thread* yo = threadGetSelf();
        const Handle h = yo != nullptr ? yo->handle : CUR_THREAD_HANDLE;
        movido = R_SUCCEEDED(svcSetThreadCoreMask(h, 3, proceso)) ||
                 R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, proceso));
      }
      REXLOG_INFO("[mp3] motor de audio en el hilo '{}': {}", nombre, movido ? "prefiere el nucleo 3" : "sin cambio");
    }
  }
#endif
  __imp__mp3_AudioEngineUpdate(ctx, base);
}
#endif  // MP3_TIENE_mp3_AudioEngineUpdate

#if defined(__SWITCH__)
extern "C" void RexSwitchRegistrarHiloAudio(void);   // sdk/src/core/threading_switch.cpp (SDK patch 0204)
extern "C" void RexSwitchAudioSoloNucleo3(int solo);
#endif

// Run 27 at stock clocks: the XAudio2 thread (~82 % of a core) and the XMA decoder (~33 %) need more than core 3, and
// what does not fit runs on the cores of the game's main, render and ring threads. With mp3_audio_solo_nucleo3 the
// audio threads (these two, the audio worker) may only run on core 3: the game keeps cores 0-2, and the audio may run
// late if core 3 is not enough (to measure with x360_ab_estados). Applied at the next Swap when it changes.
REXCVAR_DEFINE_BOOL(mp3_audio_solo_nucleo3, false, "MP3",
                    "Los hilos de audio (XAudio2, descodificador XMA, trabajador de audio) solo en el nucleo 3, sin "
                    "pasar a los nucleos del juego");

#ifdef MP3_TIENE_mp3_AudioThreadEntry  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_AudioThreadEntry);
REX_EXTERN(mp3_AudioThreadEntry) {
#if defined(__SWITCH__)
  if (REXCVAR_GET(mp3_audio_nucleo3)) {
    u64 proceso = 0;
    const bool leido = R_SUCCEEDED(svcGetInfo(&proceso, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0));
    bool movido = false;
    if (leido && (proceso & 0x8)) {
      Thread* yo = threadGetSelf();
      const Handle h = yo != nullptr ? yo->handle : CUR_THREAD_HANDLE;
      movido = R_SUCCEEDED(svcSetThreadCoreMask(h, 3, proceso)) ||
               R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, proceso));
    }
    RexSwitchRegistrarHiloAudio();
    REXLOG_INFO("[mp3] hilo de audio: mascara del proceso {:X}, {}", proceso,
                movido ? "prefiere el nucleo 3" : (proceso & 0x8) ? "el nucleo 3 rechazado" : "sin nucleo 3 (3 nucleos)");
  }
#endif
  __imp__mp3_AudioThreadEntry(ctx, base);
}
#endif  // MP3_TIENE_mp3_AudioThreadEntry

// --- Horizon priorities for the threads the main thread waits on ---------------------------------------------------
//
// Every guest thread runs at 0x3B (ignore_thread_priorities), the band Horizon only time-slices about every 10 ms, and
// the ring (0x2C) and the audio threads (0x2B) run above it. In run54 the main thread spent ~45 % of its time waiting
// for the job workers (sub_8285D368: three threads at ~15 % of a core each, 80 % of it waiting for work) and ~6 % for the
// render thread: a job that wakes on a busy core waits for that core. These settings put the job workers and the
// render thread (sub_821B6F18) above the other guest threads; both block on events or sleep while they wait (the spin
// lock sleeps too, mp3_spinlock_nativo), so they cannot hold a core. Applied at the next Swap when they change, so an
// A/B run (x360_ab_estados) can switch them.
REXCVAR_DEFINE_INT32(mp3_prioridad_trabajos, 0, "MP3",
                     "Prioridad de Horizon (44-58; menor = mas alta) de los 3 hilos de trabajos del juego "
                     "(sub_8285D368); 0 = 0x3B como los demas hilos del juego");
REXCVAR_DEFINE_INT32(mp3_prioridad_render, 0, "MP3",
                     "Prioridad de Horizon (44-58; menor = mas alta) del hilo de render del juego (sub_821B6F18); "
                     "0 = 0x3B como los demas hilos del juego");

namespace {

constexpr int kPrioridadInvitado = 0x3B;

struct HiloConPrioridad {
#if defined(__SWITCH__)
  Handle handle;
#endif
  bool trabajos;  // a job worker, else the render thread
};

std::mutex g_hilos_mutex;
std::vector<HiloConPrioridad> g_hilos;
int g_aplicada_trabajos = 0, g_aplicada_render = 0;

int PrioridadPedida(bool trabajos) {
  const int pedida = trabajos ? REXCVAR_GET(mp3_prioridad_trabajos) : REXCVAR_GET(mp3_prioridad_render);
  return pedida >= 44 && pedida < kPrioridadInvitado ? pedida : kPrioridadInvitado;
}

// The current thread joins the list and gets its priority now.
void Registrar(bool trabajos) {
#if defined(__SWITCH__)
  Thread* yo = threadGetSelf();
  const Handle h = yo != nullptr ? yo->handle : CUR_THREAD_HANDLE;
  const int prioridad = PrioridadPedida(trabajos);
  const bool hecho = R_SUCCEEDED(svcSetThreadPriority(h, u32(prioridad)));
  {
    std::lock_guard<std::mutex> lock(g_hilos_mutex);
    g_hilos.push_back({h, trabajos});
  }
  auto* hilo = rex::system::XThread::GetCurrentThread();
  REXLOG_INFO("[mp3] hilo de {} '{}': prioridad 0x{:X}{}", trabajos ? "trabajos" : "render",
              hilo ? hilo->name() : std::string("?"), prioridad, hecho ? "" : " (rechazada)");
#else
  (void)trabajos;
#endif
}

}  // namespace

// Called by the ring thread at each Swap (mp3_gpu.cpp): the registered threads get the settings' priorities if they
// changed.
void Mp3AplicarPrioridades() {
#if defined(__SWITCH__)
  static int audio_solo = 0;
  static uint32_t swaps_audio = 0;
  const int pedido_audio = REXCVAR_GET(mp3_audio_solo_nucleo3) ? 1 : 0;
  if (audio_solo && (++swaps_audio % 600) == 0) {
    RexSwitchAudioSoloNucleo3(1);  // audio threads started since (the mixer starts with the game's audio)
  }
  if (pedido_audio != audio_solo) {
    audio_solo = pedido_audio;
    RexSwitchAudioSoloNucleo3(audio_solo);
    REXLOG_INFO("[mp3] hilos de audio: {}", audio_solo ? "solo el nucleo 3" : "todo el proceso, prefieren el 3");
  }
#endif
  const int trabajos = PrioridadPedida(true), render = PrioridadPedida(false);
  if (trabajos == g_aplicada_trabajos && render == g_aplicada_render) {
    return;
  }
  const bool primera = g_aplicada_trabajos == 0;
  g_aplicada_trabajos = trabajos;
  g_aplicada_render = render;
#if defined(__SWITCH__)
  std::lock_guard<std::mutex> lock(g_hilos_mutex);
  for (const HiloConPrioridad& h : g_hilos) {
    svcSetThreadPriority(h.handle, u32(h.trabajos ? trabajos : render));
  }
  if (!primera) {
    REXLOG_INFO("[mp3] prioridades: trabajos 0x{:X}, render 0x{:X} ({} hilos)", trabajos, render, g_hilos.size());
  }
#else
  (void)primera;
#endif
}

// --- The main thread runs queued jobs while it waits for them ----------------------------------------------------------
//
// GTA IV's job queue (seen from its workers, sub_8285D368, r3 = the worker): the queue is at [worker + 188] + 8, a ring
// of 256 job pointers, then a critical section at +1024, the write index at +1056, the read index at +1060, the count
// at +1064 and a semaphore at +1068 that is released once per job queued (sub_8285D2C8). A worker waits on the
// semaphore, takes the next pointer under the critical section, copies the job's 156 bytes, puts its own scratch
// buffer ([worker + 28], [worker + 24] bytes) at copy + 64, calls [copy + 4] with r3 = copy + 8 and signals the job's
// event at [job + 156] (sub_82A13160). A null pointer stops the worker. sub_8285D170 (r3 = count, r4 = jobs, r5 = all)
// waits for those events.
//
// In run54-56 the main thread spent ~45 % of its time in that wait while the three workers were ~80 % idle: queued
// jobs waited for a worker to get a core. With mp3_trabajos_en_espera the waiting thread first takes the queued jobs
// itself, exactly as a worker does (the semaphore taken with a zero timeout, so the workers never see a job twice), with
// its own scratch buffer, and only then waits.
REXCVAR_DEFINE_BOOL(mp3_trabajos_en_espera, true, "MP3",
                    "El hilo que espera trabajos del juego (sub_8285D170, sobre todo el principal) ejecuta antes los "
                    "trabajos que siguen en la cola, como un hilo de trabajo mas");

namespace {

struct ColaTrabajos {
  uint32_t cola;     // guest address of the ring
  uint32_t memoria;  // the workers' scratch size
};
std::mutex g_colas_mutex;
std::vector<ColaTrabajos> g_colas;
std::atomic<size_t> g_num_colas{0};
std::atomic<uint64_t> g_robados{0}, g_esperas{0};

void RegistrarCola(PPCContext& ctx, uint8_t* base) {
  const uint32_t trabajador = ctx.r3.u32;
  const uint32_t cola = REX_LOAD_U32(trabajador + 188) + 8;
  const uint32_t memoria = REX_LOAD_U32(trabajador + 24);
  std::lock_guard<std::mutex> lock(g_colas_mutex);
  for (ColaTrabajos& c : g_colas) {
    if (c.cola == cola) {
      c.memoria = std::max(c.memoria, memoria);
      return;
    }
  }
  if (g_colas.size() >= 8) return;
  g_colas.push_back({cola, memoria});
  g_num_colas.store(g_colas.size(), std::memory_order_release);
  REXLOG_INFO("[mp3] cola de trabajos en {:08X} ({} bytes de memoria por trabajador)", cola, memoria);
}

// A waiting thread's own job copy and scratch buffer, in guest memory.
struct Ladron {
  uint32_t copia = 0;
  uint32_t memoria = 0, tam_memoria = 0;
  bool dentro = false;  // a stolen job waits for others: no stealing inside it
};
thread_local Ladron t_ladron;

// One queued job of `c`, run on this thread. False when the queue had none.
bool RobarUno(PPCContext& ctx, uint8_t* base, const ColaTrabajos& c) {
  auto* kernel = rex::system::kernel_state();
  const uint32_t q = c.cola;
  // Nothing queued (most waits): the semaphore is not touched. A failed zero-timeout wait yields the core
  // (XObject::Wait, MaybeYield): run58 had ~11 % of the main thread's samples in sched_yield from here.
  if (REX_LOAD_U32(q + 1064) == 0) return false;
  auto semaforo = kernel->object_table()->LookupObject<rex::system::XSemaphore>(REX_LOAD_U32(q + 1068));
  if (!semaforo) return false;
  uint64_t cero = 0;
  if (semaforo->Wait(0, 0, 0, &cero) != 0) return false;  // X_STATUS_SUCCESS
  Ladron& l = t_ladron;
  if (!l.copia) l.copia = kernel->memory()->SystemHeapAlloc(192, 16);
  if (l.tam_memoria < c.memoria) {
    l.memoria = kernel->memory()->SystemHeapAlloc(c.memoria, 16);
    l.tam_memoria = c.memoria;
  }
  // The worker's pop (sub_8285D368), under the queue's critical section; a null job stays for its worker.
  ctx.r3.u64 = q + 1024;
  REX_CALL_INDIRECT_FUNC(0x8285FE78);
  uint32_t lectura = REX_LOAD_U32(q + 1060) + 1;
  if (lectura == 256) lectura = 0;
  const uint32_t trabajo = REX_LOAD_U32(q + lectura * 4);
  if (trabajo) {
    REX_STORE_U32(q + 1060, lectura);
    REX_STORE_U32(q + 1064, REX_LOAD_U32(q + 1064) - 1);
  }
  ctx.r3.u64 = q + 1024;
  REX_CALL_INDIRECT_FUNC(0x8285FEE0);
  if (!trabajo || !l.copia || !l.memoria) {
    semaforo->ReleaseSemaphore(1, nullptr);
    return false;
  }
  const uint32_t evento = REX_LOAD_U32(trabajo + 156);
  std::memcpy(base + l.copia, base + trabajo, 156);
  REX_STORE_U32(l.copia + 64, l.memoria);
  REX_STORE_U32(l.copia + 156, q - 8);  // what follows a worker's copy: its [worker + 188], the queue's owner
  ctx.r3.u64 = l.copia + 8;
  REX_CALL_INDIRECT_FUNC(REX_LOAD_U32(l.copia + 4));
  ctx.r3.u64 = evento;
  REX_CALL_INDIRECT_FUNC(0x82A13160);
  g_robados.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace

#ifdef MP3_TIENE_mp3_JobWorker  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_JobWorker);
REX_EXTERN(mp3_JobWorker) {
  RegistrarCola(ctx, base);
  Registrar(true);
  __imp__mp3_JobWorker(ctx, base);
}
#endif  // MP3_TIENE_mp3_JobWorker

// Max Payne 3's job workers (sub_82DADC08): only their Horizon priority (mp3_prioridad_trabajos), none of GTA IV's
// queue stealing, whose layout is GTA IV's.
#ifdef MP3_TIENE_mp3_JobWorkerMp3  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_JobWorkerMp3);
REX_EXTERN(mp3_JobWorkerMp3) {
  Registrar(true);
  __imp__mp3_JobWorkerMp3(ctx, base);
}
#endif  // MP3_TIENE_mp3_JobWorkerMp3

#ifdef MP3_TIENE_mp3_JobWait  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_JobWait);
REX_EXTERN(mp3_JobWait) {
  const size_t colas = g_num_colas.load(std::memory_order_acquire);
  if (colas && !t_ladron.dentro && REXCVAR_GET(mp3_trabajos_en_espera)) {
    t_ladron.dentro = true;
    // The wait's arguments and frame back afterwards; the jobs run on a frame below this one.
    const PPCRegister r1 = ctx.r1, r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5;
    const uint64_t lr = ctx.lr;
    ctx.r1.s64 = r1.s64 - 1184;
    ColaTrabajos lista[8];
    {
      std::lock_guard<std::mutex> lock(g_colas_mutex);
      std::copy(g_colas.begin(), g_colas.end(), lista);
    }
    for (int vuelta = 0; vuelta < 64; ++vuelta) {
      bool alguno = false;
      for (size_t i = 0; i < colas; ++i) {
        ctx.lr = 0x8285D1B8;
        alguno |= RobarUno(ctx, base, lista[i]);
      }
      if (!alguno) break;
    }
    ctx.r1 = r1;
    ctx.r3 = r3;
    ctx.r4 = r4;
    ctx.r5 = r5;
    ctx.lr = lr;
    t_ladron.dentro = false;
  }
  if ((g_esperas.fetch_add(1, std::memory_order_relaxed) & 0x3FFF) == 0x3FFF) {
    REXLOG_INFO("[mp3] esperas de trabajos: {}, trabajos ejecutados por quien esperaba: {}",
                g_esperas.load(std::memory_order_relaxed), g_robados.load(std::memory_order_relaxed));
  }
  __imp__mp3_JobWait(ctx, base);
}
#endif  // MP3_TIENE_mp3_JobWait

#ifdef MP3_TIENE_mp3_RenderThreadLoop  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_RenderThreadLoop);
REX_EXTERN(mp3_RenderThreadLoop) {
  Registrar(false);
  __imp__mp3_RenderThreadLoop(ctx, base);
}
#endif  // MP3_TIENE_mp3_RenderThreadLoop

// --- The main thread's wait for its asynchronous requests -------------------------------------------------------------
//
// sub_826799E0 (r3 = the owner) drains an 8-entry ring of pending requests at owner + 12564 (the read index at +36,
// the count at +40, i.e. owner + 12604): while the count is not 0 it polls the next request's handle ([request + 12])
// with a zero-timeout wait (sub_82849790); a finished one is taken off the ring and handled (sub_82679908), otherwise
// the thread sleeps 10 ms (Sleep, sub_82849918) and polls again. In run58 the main thread spent ~10 % of its time in that
// sleep: a request that finished 1 ms later still cost 10. mp3_espera_peticiones waits on the handle itself for up to
// the same 10 ms instead: the poll and the sleep become one wait, which consumes the signal exactly as the poll did.
REXCVAR_DEFINE_BOOL(mp3_espera_peticiones, true, "MP3",
                    "El hilo principal espera el evento de su peticion pendiente (hasta 10 ms) en vez de dormir 10 ms "
                    "entre comprobaciones (sub_826799E0)");

#ifdef MP3_TIENE_mp3_AsyncDrain  // codegen/names.toml names it (CMakeLists.txt)
REX_EXTERN(__imp__mp3_AsyncDrain);
REX_EXTERN(mp3_AsyncDrain) {
  if (!REXCVAR_GET(mp3_espera_peticiones)) {
    __imp__mp3_AsyncDrain(ctx, base);
    return;
  }
  auto* kernel = rex::system::kernel_state();
  const uint32_t dueno = ctx.r3.u32;
  const uint32_t anillo = dueno + 12564;
  const PPCRegister r1 = ctx.r1;
  const uint64_t lr = ctx.lr;
  ctx.r1.s64 = r1.s64 - 112;
  while (REX_LOAD_U32(dueno + 12604) != 0) {
    uint32_t siguiente = REX_LOAD_U32(anillo + 36) + 1;
    if (siguiente == 8) siguiente = 0;
    const uint32_t peticion = REX_LOAD_U32(anillo + siguiente * 4);
    const uint32_t handle = REX_LOAD_U32(peticion + 12);
    bool lista = false;
    auto objeto = handle ? kernel->object_table()->LookupObject<rex::system::XObject>(handle)
                         : rex::system::object_ref<rex::system::XObject>();
    if (objeto) {
      uint64_t diez_ms = uint64_t(-int64_t(10 * 10000));  // relative, in 100 ns ticks
      lista = objeto->Wait(0, 0, 0, &diez_ms) == 0;
    } else {
      // As the game does: poll, then sleep 10 ms.
      ctx.r3.u64 = handle;
      ctx.lr = 0x82679A28;
      REX_CALL_INDIRECT_FUNC(0x82849790);
      lista = (ctx.r3.u32 & 0xFF) != 0;
      if (!lista) {
        ctx.r3.u64 = 10;
        ctx.lr = 0x82679A78;
        REX_CALL_INDIRECT_FUNC(0x82849918);
      }
    }
    if (!lista) continue;
    // Taken off the ring as the guest does (index wraps after it is stored), then handled.
    REX_STORE_U32(anillo + 36, REX_LOAD_U32(anillo + 36) + 1);
    REX_STORE_U32(anillo + 40, REX_LOAD_U32(anillo + 40) - 1);
    if (REX_LOAD_U32(anillo + 36) == 8) REX_STORE_U32(anillo + 36, 0);
    ctx.r3.u64 = dueno;
    ctx.r4.u64 = REX_LOAD_U32(anillo + REX_LOAD_U32(anillo + 36) * 4);
    ctx.lr = 0x82679A6C;
    REX_CALL_INDIRECT_FUNC(0x82679908);
  }
  ctx.r1 = r1;
  ctx.lr = lr;
}
#endif  // MP3_TIENE_mp3_AsyncDrain

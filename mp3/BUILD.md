# Building the rexglue-sdk-nx / deko3d build of GTA IV

Not playable yet: this build is waiting for its first run with the game (see [PORTING.md](PORTING.md)). Nothing derived
from the game is distributed: the code, the shaders and the NRO are made on your computer from your own copy.

## What you need

| | |
| --- | --- |
| Switch | Atmosphère and the Homebrew Menu (title takeover: hold R while starting any game) |
| Game | Your GTA IV for Xbox 360: a **disc image** (ISO) or a **Games on Demand package** (tested: the Russian one, Media ID 4A53F9F6), and its **Title Update 8** package (`TU_1A581VI_...`, or `tu00000006_00000000` from the console's `545407F2/000B0000/`) |
| Computer | macOS (tested) or Linux (untested), ~15 GB free, internet for the first build |

## Tools (once)

1. `xcode-select --install`, then Homebrew (https://brew.sh) and `brew install cmake ninja python git ccache`.
2. devkitPro (https://github.com/devkitPro/pacman/releases), then `sudo dkp-pacman -S switch-dev deko3d`.
3. The Vulkan SDK (https://vulkan.lunarg.com: `dxc` and `spirv-val`, for the shaders).

## Build

```
git submodule update --init sdk
python3 mp3/tools/stage_sd.py --disc "<GTA IV.iso>" --tu "<TU8 package>" -o sd/mp3-nx
mp3/tools/build_mac.sh sd/mp3-nx/game
```

`stage_sd.py` extracts the disc to `sd/mp3-nx/game/` (TU8's `default.xexp` beside `default.xex`) and every file of
TU8 to `sd/mp3-nx/update/` (the game's `update:`: `update.img`, shaders, text ...).

With a Games on Demand package: leave `--disc` out (only `update/` is made), put the package as it is in `god/`
(below), and give `build_mac.sh` a folder with the package's `default.xex` and TU8's `default.xexp` (the xex comes
out of the package with dantes-inferno-nx's `god_xex.py`, or the console copies it to `god_xex/` at the first start).
`build_mac.sh` applies the SDK patches, builds the host code generator (ReXGlue v0.10.0), generates the game's code
from your executable (`mp3/tools/codegen.sh`, ~2 minutes), builds the shader library when there is one
(`mp3/tools/shaders.sh`) and the NRO (`mp3/out/sw/mp3.nro`). `SD=sd/mp3-nx` copies the results there.

Variants (environment variables):

| | |
| --- | --- |
| `LOCALREGS=0` | without the guest-registers-in-locals pass (to compare speed) |
| `PLR_CHECK=1` | the check build of that pass (slower; logs `[regcheck]`, see [CPU_OPTIMIZATION.md](CPU_OPTIMIZATION.md)) |
| `BARRIERS=0` | without the ARM memory barriers (to compare; not for playing) |

Use a separate build folder per variant to keep both: copy `mp3/out/sw/mp3.nro` away before switching.

## On the card

```
sdmc:/switch/mp3-nx/
    mp3-nx.nro              mp3/out/sw/mp3.nro, renamed
    mp3_shaders.dslib       mp3/out/shaders/ (when built)
    shadercache_dksh.bin     mp3/out/shaders/ (when built)
    update/                  sd/mp3-nx/update/ (Title Update 8's files)
    game/                    sd/mp3-nx/game/ (a disc), or
    god/                     a Games on Demand package as it is: <header file> and <header file>.data/Data0000 ...
```

Logs go to `sdmc:/switch/mp3-nx/logs/`; shaders the library lacks are dumped to `shaders_dump/`.

`mp3/tools/make_package.sh` puts a finished build into a zip laid out for the card, with the settings in
`mp3/dist/mp3.toml`, `stage_sd.py` and a Russian install guide for a Games on Demand package
(`mp3/dist/INSTRUKCIYA_RU.txt`). It holds the NRO and the shaders, made from the game: keep it to yourself.

## On the console (while the port is brought up)

- The FPS counter is on screen from the start (top left; ZL + ZR + Up hides it), and the debug shortcuts are on.
- The profiler runs from the start: every 10 s `logs/rex/rex_perfil.log` gets the clocks, the game's frame rate and
  each thread's CPU, and its stack sampling (1 ms) says where each thread spends its time (~8 % slower; ZL + ZR + R
  switches it during play). The ELF that names the addresses is kept in `mp3/out/elf/<NRO hash>.elf`.
- To turn them off, in `sdmc:/switch/mp3-nx/mp3.toml`: `mp3_fps_al_arrancar = false`, `mp3_perfil_pilas = false`.

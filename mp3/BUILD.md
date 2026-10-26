# Building the rexglue-sdk-nx / deko3d build of Max Payne 3

Not playable yet (see [PORTING.md](PORTING.md)). Nothing derived from the game is distributed: the code, the shaders
and the NRO are made on your computer from your own copy.

## What you need

| | |
| --- | --- |
| Switch | Atmosphère and the Homebrew Menu (title takeover: hold R while starting any game) |
| Game | Your Max Payne 3 for Xbox 360: disc 1 as a **Games on Demand package** (tested: Media ID 1DFE6863, the header file `3054723F8A3477D01A12` and its `.data/` folder from `5454086B/00007000/`) or a disc image, and **Title Update 6** (`tu00000006_00000000` from `5454086B/000B0000/`) |
| Computer | macOS (tested) or Linux (untested), ~15 GB free, internet for the first build |

## Tools (once)

1. `xcode-select --install`, then Homebrew (https://brew.sh) and `brew install cmake ninja python git ccache`.
2. devkitPro (https://github.com/devkitPro/pacman/releases), then `sudo dkp-pacman -S switch-dev deko3d`.
3. The Vulkan SDK (https://vulkan.lunarg.com: `dxc` and `spirv-val`, for the shaders).

## Build

```
git submodule update --init sdk
python3 mp3/tools/stage_sd.py --tu <tu00000006_00000000> -o sd/mp3-nx
python3 <dantes-inferno-nx>/dante/tools/god_xex.py <disc 1 package's .data folder> default.xex <game folder>/default.xex
cp sd/mp3-nx/update/default.xexp <game folder>/
mp3/tools/build_mac.sh <game folder>
```

`stage_sd.py` writes the files of Title Update 6's `disc001/` folder to `sd/mp3-nx/update/` (the game's `update:`:
`default.xexp`, `update.rpf`, `commonup.rpf`, `platformup.rpf`, the levels' script patches ...). `god_xex.py` reads only
the pieces `default.xex` lies in (Data0007 of disc 1, with Data0000 for the directory). `build_mac.sh` applies the
SDK patches, builds the host code generator (ReXGlue v0.10.0) unless `mp3/out/rexglue-host/` already has one,
generates the game's code from your executable (`mp3/tools/codegen.sh`), builds the shader library when there is one
(`mp3/tools/shaders.sh`) and the NRO (`mp3/out/sw/mp3.nro`). `SD=sd/mp3-nx` copies the results there.

Variants (environment variables): `LOCALREGS=0`, `PLR_CHECK=1`, `BARRIERS=0`, as in gta4-nx (CPU_OPTIMIZATION.md).

## On the card

```
sdmc:/switch/mp3-nx/
    mp3-nx.nro               mp3/out/sw/mp3.nro, renamed
    mp3.toml                 settings (mp3/dist/mp3.toml)
    mp3_shaders.dslib        mp3/out/shaders/ (when built)
    shadercache_dksh.bin     mp3/out/shaders/ (when built)
    update/                  sd/mp3-nx/update/ (Title Update 6's disc001/ files)
    god/                     disc 1's Games on Demand package as it is: <header file> and <header file>.data/Data0000 ...
                             (only disc 1: the package is the first one found under god/)
    disc2/                   disc 2's package (not used yet)
    packages/                the TU6 package and the DLC as they came (not used by the port)
```

Instead of copying the package into `god/`, `mp3_god` in `mp3.toml` can name its header file where it already is on the
card (for example where a download manager put it). Logs go to `sdmc:/switch/mp3-nx/logs/`.

# Remedy texture fix

ReShade add-ons that stop textures in Remedy's Northlight games from going blurry a few minutes into play on 6–8 GB graphics cards, with ray tracing or path tracing left on. They change nothing on disk.

| Game | Add-on | Download | Played on |
|---|---|---|---|
| Control Resonant | `CRStreamingFix.addon64` | [v1.1.0](https://github.com/Gabarsolon/remedy-texture-fix/releases/tag/v1.1.0) | game 0.563.737.9, see [Tested with](#tested-with) |
| Alan Wake 2 | `AW2StreamingFix.addon64` | [v1.0.0](https://github.com/Gabarsolon/remedy-texture-fix/releases/tag/aw2-v1.0.0) | game 0.559.302.8, see [Alan Wake 2](#alan-wake-2) |

Both are built from one source and work the same way. The rest of this page describes the Control Resonant add-on. For Alan Wake 2, read `AW2StreamingFix` for `CRStreamingFix` and `AlanWake2.exe` for `CONTROLResonant.exe`.

## The problem

Textures look fine when you load in, then turn to mush and stay that way. Changing Texture Resolution doesn't help. Turning off ray tracing or dropping the resolution does.

The cause, from disassembling `CONTROLResonant.exe` 0.563.737.9:

- **Pool size.** The texture streamer sizes its pool as the DXGI budget minus everything else the game process has in VRAM, clamped between 100 MiB and a ceiling. Path tracing, ray reconstruction, frame generation and anything injected into the process all come out of the pool.
- **Texture Resolution.** It only sets the ceiling: 1664 MiB on Low, 3 GiB on Medium, 4 GiB above that. When your leftover VRAM is below the ceiling, the setting does nothing.
- **Blur.** While textures need more than 95% of the pool, the engine adds 0.1 mips of blur per update, up to 10. It removes blur only once demand falls under 90%.

So on a small card the pool follows your free VRAM down, and the blur follows the pool.

In one test session (8 GB card, 1440p, path tracing, ray reconstruction and frame generation on) the game left a median of 1.1 GB for textures. Within three minutes that was down to 228 MB.

## What the add-on does

It sets a floor under the pool: 2048 MB by default instead of the game's 100 MB. In the same test session the pool held at 2048 MB and the blur stayed flat at about 2.2 mips.

It can also raise the ceiling and cap the blur. The settings and the live numbers are in a panel in the ReShade menu, and in `CRStreamingFix.ini` and `CRStreamingFix.log`.

It finds what it needs by code signature. If the signatures don't match your game version, it logs an error and does nothing.

## Install

1. Install [ReShade](https://reshade.me) **with full add-on support** for the game. RenoDX needs the same build.
2. Put `CRStreamingFix.addon64` next to `CONTROLResonant.exe`.
3. Start the game. `CRStreamingFix.ini` and `CRStreamingFix.log` appear in the same folder.

To remove it, delete the file.

It also runs without ReShade: rename it to `CRStreamingFix.asi` and load it with an ASI loader. The offline test covers that path, but it has not been tried in the game.

## In the ReShade menu

Open the ReShade menu and look for the **CRStreamingFix** window. It shows the pool, how full it is, the current blur and what the game would have left for textures by itself. Below that are the settings.

- Changes apply within a quarter of a second. No restart.
- A slider is saved to `CRStreamingFix.ini` when you let go of it.
- If the window isn't already a tab, it starts as a floating window on the right. Drag its title onto the ReShade tab bar to dock it.

The panel needs a ReShade build that carries ImGui 1.92.5, such as 6.8.0. On other builds the add-on still works; it just has no panel, and the log says so.

## Settings

The panel and `CRStreamingFix.ini` hold the same four settings. The ini is re-read while the game runs, so editing it by hand works too.

| Key | Default | Meaning |
|---|---|---|
| `MinPoolMB` | 2048 | Pool floor. 0 leaves the game's 100 MB. |
| `MaxPoolMB` | 0 | Pool ceiling. 0 leaves the game's value. |
| `BiasLimit` | -1 | Largest mip bias the streamer may add. -1 leaves the game's 10. |
| `LogIntervalSec` | 5 | Seconds between stats lines. 0 turns them off. |

## Tuning

A stats line looks like this:

```
pool 2048 MB [min 2048, max 3072] | used 1933 MB | demand 1922 MB | bias 2.30 mips | VRAM left for textures 1118-1142 MB (pool is 930 MB above it)
```

- **VRAM left for textures** is the pool the game would have picked by itself.
- **bias** is how many mip levels of blur the streamer is applying. 0 is full resolution.
- **pool is N MB above it** means the game is using more VRAM than its budget, and Windows pages the difference to system RAM.

In the test session the pool sat about 950 MB above free VRAM with no stutter reported, but that was a short session. If you get stutter, lower `MinPoolMB`, or free VRAM another way: frame generation, path tracing quality, render resolution.

2048 suits an 8 GB card. For 6 GB, 1536 is a reasonable first guess; it has not been tested.

Lowering `BiasLimit` without a bigger pool doesn't sharpen anything, because the pool is also a hard limit on what gets loaded.

## Tested with

Game 0.563.737.9, ReShade 6.8.0, RTX 5060 Laptop 8 GB, alongside RenoDX and OptiScaler. That is the only setup it has been played on, panel included.

## Alan Wake 2

Alan Wake 2 runs the same texture streamer with the same numbers: a 100 MiB floor, ceilings of 1664 MiB, 3 GiB and 4 GiB from Texture Resolution, and the same fit-to-pool controller. `AW2StreamingFix.addon64` is this add-on built for `AlanWake2.exe`. Install and settings are as above, with `AW2StreamingFix` in the file names: put `AW2StreamingFix.addon64` next to `AlanWake2.exe`.

One test session showed the pool driving the blur directly. It ran on an 8 GB card at 1440p from a 720p render, with path tracing, ray reconstruction and frame generation on and Texture Resolution on High. The pool was moved with the add-on's sliders while standing in one spot:

| Pool | Textures loaded | Blur |
|---|---|---|
| 128 MB, forced, as when VRAM runs out | 0.15 GB | 10 mips, the game's limit |
| 2048 MB, the add-on's default floor | 1.9 GB | 1.2 to 1.5 mips |
| 8192 MB, forced | 2.9 GB | 0 |

Five minutes in, the game by itself had 1.4 GB left for textures, and that number was still falling.

Tested with game 0.559.302.8 and ReShade 6.8.0 on an RTX 5060 Laptop 8 GB, with no other mods. That is the only setup it has been played on, and the session was short. If it misbehaves for you, please open an issue with `AW2StreamingFix.log` attached.

## If you used the old patcher from this repo

`patch.py` and `patch.ps1` are gone. They patched the exe based on a misreading of the engine's tweakables. Put back the `CONTROLResonant.exe.bak` they made.

## How it works

Addresses are for 0.563.737.9. The add-on finds them by signature.

| What | Where |
|---|---|
| `StreamedTextureHeap` constructor: pool 1 GiB, min 100 MiB, max 3 GiB | `exe+0x1D03BD0` |
| Pool update from `QueryVideoMemoryInfo` | `exe+0x1D05670` |
| Ceiling from Texture Resolution | `exe+0x2F17DDB` |
| Fit-to-pool controller | `exe+0x2E8C3E0` |
| Mip selection per texture | `exe+0x2E8C9B0` |
| `StreamedTextureHeap*` | `exe+0x5C36B48`, then `+0x00` pool, `+0x08` min, `+0x10` max |
| Texture streaming manager | `exe+0x5D2B470`, then `+0x00` demand, `+0x08` bias |

Four times a second the add-on writes the floor (and the ceiling, if set) into the heap object, and the bias limit into the tweakable's value. When it is unloaded it leaves memory alone.

The same places in Alan Wake 2 0.559.302.8:

| What | Where |
|---|---|
| Pool update from `QueryVideoMemoryInfo` | `exe+0x2087D80` |
| Ceiling from Texture Resolution | `exe+0x2267E6B` |
| Fit-to-pool controller | `exe+0x22075A0` |
| `StreamedTextureHeap*` | `exe+0x3A34698`, then `+0x00` pool, `+0x08` min, `+0x10` max |
| Texture streaming manager | `exe+0x397FEA8`, then `+0x08` demand, `+0x10` bias |

## Build

`build.bat` needs Visual Studio 2022 or its Build Tools (x64). Both games come from one source; `src/game.h` holds what differs.

- `build.bat` builds `build\CRStreamingFix.addon64` and `build\AW2StreamingFix.addon64`.
- `build.bat test` also runs the offline test for both. It fakes the game and ReShade, menu included, to exercise loading, unloading and every setting.
- `build.bat test "path\to\CONTROLResonant.exe" "path\to\AlanWake2.exe"` also checks that the signatures resolve in those executables. Either one is enough.

The menu panel is drawn through the ImGui function table that ReShade gives add-ons, so no ImGui code is compiled in. The two headers that takes are in `third_party`.

## License

MIT. The headers in `third_party` keep their own licenses: Dear ImGui is MIT, and ReShade's `reshade_overlay.hpp` is BSD-3-Clause OR MIT. See `third_party/README.md`.

# CRStreamingFix

A ReShade add-on that stops textures in **Control Resonant** from going blurry a few minutes into play on 6–8 GB graphics cards, with ray tracing or path tracing left on. It changes nothing on disk.

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

It can also raise the ceiling and cap the blur. Every few seconds it writes the live numbers to `CRStreamingFix.log`.

It finds what it needs by code signature. If the signatures don't match your game version, it logs an error and does nothing.

## Install

1. Install [ReShade](https://reshade.me) **with full add-on support** for the game. RenoDX needs the same build.
2. Put `CRStreamingFix.addon64` next to `CONTROLResonant.exe`.
3. Start the game. `CRStreamingFix.ini` and `CRStreamingFix.log` appear in the same folder.

To remove it, delete the file.

It also runs without ReShade: rename it to `CRStreamingFix.asi` and load it with an ASI loader. The offline test covers that path, but it has not been tried in the game.

## Settings

`CRStreamingFix.ini` is re-read while the game runs, so you can alt-tab, edit it and watch the result.

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

Game 0.563.737.9, ReShade 6.8.0, RTX 5060 Laptop 8 GB, alongside RenoDX and OptiScaler. That is the only setup it has been played on.

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

## Build

`build.bat` needs Visual Studio 2022 or its Build Tools (x64).

- `build.bat` builds `build\CRStreamingFix.addon64`.
- `build.bat test` also runs the offline test. It fakes the game and ReShade to exercise loading, unloading and every setting.
- `build.bat test "path\to\CONTROLResonant.exe"` also checks that the signatures resolve in that exe.

## License

MIT

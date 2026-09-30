# Control Resonant Texture Streaming Fix

Complete reverse-engineered fix for blurry, muddy, and degrading textures in **Control Resonant** (Remedy's Northlight Engine with Path Tracing & Neural Rendering).

---

## Root Causes of the Blurry Textures

In *Control Resonant*, running **Path Tracing**, **DLSS Ray Reconstruction**, and **DLSS Frame Generation** requires high VRAM bandwidth and capacity. On **6 GB to 8 GB GPUs** (e.g. RTX 2060, 3060, 4050, 4060, 5060 Laptop):

1. **Aggressive Tile Heap Purging:**
   * `Tile Defrag: Trigger On Over Budget` drops all active texture tiles when memory is pressured.
   * `Tile Defrag: Trigger On Failed Allocation` triggers an emergency defragmentation whenever an allocation spike occurs.
2. **Dynamic Downscaling Bias (`Fit to pool`):**
   * The Northlight streaming supervisor features an internal controller called `Fit to pool`.
   * When memory is under pressure, it ramps up an artificial **Mip Target Bias** up to **`+20.0f`** (at rate `10.0f/s`). A mip bias of +20 forces the engine to display the absolute lowest 16x16 or 32x32 mipmaps for all world assets.
3. **Constrained Sparse Tile Heap:**
   * The engine's sparse tile heap reserve default is hardcoded to only **512 MB**, which exhausts almost immediately in modern 1440p/4K scenes with Path Tracing enabled.
4. **Sub-optimal In-Game Settings:**
   * Often `renderer.ini` defaults to internal 720p render resolution upscaled to 1440p with `0x` Anisotropic Filtering and Low texture resolution.

---

## What the Patch Fixes

| Patch | Vanilla Value | Patched Value | Technical Offset | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **Tile Defrag: Over Budget** | `01 01` (Enabled) | `00 00` (Disabled) | `0x6BFFD` | Prevents dropping texture tiles when over memory budget |
| **Tile Defrag: Failed Alloc** | `01 01` (Enabled) | `00 00` (Disabled) | `0x6BF7D` | Prevents emergency defrag on allocation spikes |
| **Force Max Res Textures** | `00 00` (Disabled) | `01 01` (Enabled) | `0x2D01FD` | Instructs the streaming manager to hold maximum mipmaps |
| **Fit to pool: Bias Limit** | `20.0f` (`00 00 A0 41`) | `0.0f` (`00 00 00 00`) | `0x4DD6AD8` | **Stops the engine from adding +20.0 blur bias to textures** |
| **Tile Heap: Reserve** | `512 MB` (`00 02 00 00`) | `2048 MB` (`00 08 00 00`) | `0x4923A0F` | **Expands the sparse tile memory budget to 2 GB** |

---

## Recommended In-Game Settings (`renderer.ini`)

Located in `%LOCALAPPDATA%\Remedy\CONTROLResonant\renderer.ini`:

* `"m_eTextureResolution": 2` (High)
* `"m_eTextureFilteringQuality": 2` (Ultra / 16x Anisotropic Filtering)
* `"m_iRenderResolutionX": 1707` and `"m_iRenderResolutionY": 960` (DLSS Quality at 1440p output)
* `"m_bFilmGrain": false`
* `"m_bDepthOfField": false`
* `"m_eMotionBlur": 0`

### OptiScaler Texture Enhancements (`OptiScaler.ini`)
* `AnisotropyOverride = 16`
* `MipmapBiasOverride = -0.5`
* `MipmapBiasOverrideAll = true`

---

## How to Install

### Option 1: PowerShell (Windows)

```powershell
.\patch.ps1 -ExePath "E:\path\to\CONTROLResonant.exe"
```

### Option 2: Python (Cross-platform)

```bash
python patch.py "E:\path\to\CONTROLResonant.exe"
```

---

## How to Revert

```powershell
.\patch.ps1 -ExePath "E:\path\to\CONTROLResonant.exe" -Revert
```

Or restore the automatic backup created at `CONTROLResonant.exe.bak`.

---

## License

MIT License. See [LICENSE](LICENSE) for details.

# Control Resonant Texture Streaming Fix

Fix for blurry, muddy, and degrading textures in **Control Resonant** (Remedy's Northlight Engine with Path Tracing & Neural Rendering).

---

## The Problem

In *Control Resonant*, running **Path Tracing**, **DLSS Ray Reconstruction**, and **DLSS Frame Generation** together requires substantial VRAM (~6.5 GB to 7.0 GB before game textures are loaded).

On graphics cards with **6 GB to 8 GB of VRAM** (e.g. RTX 2060, 3060, 4050, 4060, 5060 Laptop):
1. VRAM headroom quickly exhausts in dense areas (such as the Central Executive or Central Field Office).
2. Northlight Engine's built-in sparse texture streaming supervisor triggers an emergency defragmentation:
   ```
   Texture Streaming: Tile Defrag: Trigger On Over Budget
   ```
3. The engine aggressively purges high-resolution texture tiles and permanently locks posters, walls, signs, and character outfits to the lowest level-of-detail (LOD) mipmaps to prevent an out-of-memory crash.

---

## Why Nexus Mod 135 Crashes Control Resonant

Many players attempt to use [Nexus Mod 135 (Blurry Textures Fix & RTX Overhaul)](https://www.nexusmods.com/control/mods/135) from the original 2019 *Control*. However, Mod 135 crashes Control Resonant on startup:

* **Missing Module:** Mod 135 uses `iphlpapi.dll` (reg2k's Loose Files Loader v1.0), which looks for `rl_rmdwin10_f.dll` (from the 2019 split architecture) and fails with `FATAL: Could not get rl_rmdwinX_f.dll`. In Resonant, the engine is statically linked directly into `CONTROLResonant.exe`.
* **Pack2FileSystem:** Resonant upgraded the engine's archive system from `PackFileSystem` (`.epack`) to `Pack2FileSystem` (`data_pack2`), while natively reading loose files from `data/` without third-party loaders.
* **Shader Pipeline Collision:** Mod 135 injects legacy 2019 DXIL `.obj` shaders. Control Resonant uses modern **Slang** shaders with RenoDX pipeline hooks for Path Tracing. Injecting legacy bytecode results in an immediate `0xc0000005` access violation inside `OnInitPipelineLayout`.
* **Deprecated XML:** Resonant discontinued reading `data/globaldb/tweakables.xml`.

---

## The Solution: Engine Binary Patch

Through reverse engineering of `CONTROLResonant.exe`, the internal Northlight streaming flags were located directly inside the executable's `.text` section:

| Setting | Vanilla Default | Patched | File Offset | Description |
| :--- | :--- | :--- | :--- | :--- |
| `Tile Defrag:Trigger On Over Budget` | `01 01` (Enabled) | `00 00` (Disabled) | `0x6BFFD` | Stops the engine from dropping mipmaps when VRAM headroom is tight |
| `Force max res textures` | `00 00` (Disabled) | `01 01` (Enabled) | `0x2D01FD` | Forces the streaming supervisor to hold maximum resolution mipmaps |

---

## How to Install

### Option 1: PowerShell (Windows)

1. Open PowerShell.
2. Run the patch script, pointing to your game directory:
   ```powershell
   .\patch.ps1 -ExePath "E:\path\to\CONTROLResonant.exe"
   ```
   *(A `.bak` backup of your original executable is automatically created before any modification.)*

### Option 2: Python (Cross-platform)

```bash
python patch.py "E:\path\to\CONTROLResonant.exe"
```

---

## How to Revert

To restore your executable to original vanilla defaults:

* **PowerShell:**
  ```powershell
  .\patch.ps1 -ExePath "E:\path\to\CONTROLResonant.exe" -Revert
  ```
* **Python:**
  ```bash
  python patch.py "E:\path\to\CONTROLResonant.exe" --revert
  ```
* Or simply restore `CONTROLResonant.exe.bak`.

---

## License

MIT License. See [LICENSE](LICENSE) for details.

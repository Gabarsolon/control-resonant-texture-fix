# Third-party headers

Only headers; nothing here is compiled as a library.

| Folder | What | Version | License |
|---|---|---|---|
| `imgui/` | Dear ImGui `imgui.h`, `imconfig.h` | 1.92.5 docking, commit `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c` | MIT, see `imgui/LICENSE.txt` |
| `reshade/` | ReShade add-on API `reshade_overlay.hpp` | v6.8.0 | BSD-3-Clause OR MIT, see `reshade/LICENSE.md` |

`reshade_overlay.hpp` maps ImGui calls onto the function table ReShade hands to add-ons. It must match the ImGui version above (19250).

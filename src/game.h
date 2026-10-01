// Per-game build settings. The fix is the same in both games (same engine, same texture streamer);
// what differs is the executable, a few structure offsets and how the code is found.
//
//   default           Control Resonant   -> CRStreamingFix.addon64
//   /DCRSF_GAME_AW2   Alan Wake 2        -> AW2StreamingFix.addon64
//
// Also included by the resource script, which only understands the #defines.
#ifndef CRSF_GAME_H
#define CRSF_GAME_H

#if defined(CRSF_GAME_AW2)
#define CRSF_NAME "AW2StreamingFix"
#define CRSF_GAME "Alan Wake 2"
#define CRSF_EXE "AlanWake2.exe"
#define CRSF_VERSION "0.1.0"
#define CRSF_VERSION_NUM 0, 1, 0, 0
#else
#define CRSF_NAME "CRStreamingFix"
#define CRSF_GAME "Control Resonant"
#define CRSF_EXE "CONTROLResonant.exe"
#define CRSF_VERSION "1.1.0"
#define CRSF_VERSION_NUM 1, 1, 0, 0
#endif

#define CRSF_WIDE_(s) L##s
#define CRSF_WIDE(s) CRSF_WIDE_(s)
#define CRSF_NAME_W CRSF_WIDE(CRSF_NAME)
#define CRSF_EXE_W CRSF_WIDE(CRSF_EXE)

#ifndef RC_INVOKED
#include <cstdint>

// Offsets inside the game's objects. The heap object itself is the same in both games:
// +0x00 pool, +0x08 minimum, +0x10 maximum, +0x18 pointer to its stats.
struct GameLayout
{
    uint32_t stats_heap_bytes; // bytes in whole heaps
    uint32_t stats_tiles;      // 64 KiB tiles in use
    uint32_t stats_left_lo;    // lowest "VRAM left for textures" in the current sampling window
    uint32_t stats_left_hi;    // highest
    uint32_t mgr_demand;       // texture streaming manager: bytes wanted at the current bias
    uint32_t mgr_bias;         // texture streaming manager: current mip bias (float)
};

#if defined(CRSF_GAME_AW2)
constexpr GameLayout kLayout = {0x210, 0x170, 0x1D8, 0x1E0, 0x08, 0x10}; // AlanWake2.exe 0.559.302.8
#else
constexpr GameLayout kLayout = {0x1E0, 0x140, 0x1A8, 0x1B0, 0x00, 0x08}; // CONTROLResonant.exe 0.563.737.9
#endif

#endif // RC_INVOKED
#endif // CRSF_GAME_H

// Offline test for the add-on's logic and load/unload lifecycle. No game needed.
//
// The add-on only wakes up inside "CONTROLResonant.exe", so this builds to an exe with that name. It fakes
// what the add-on looks for (the signature bytes in .text, the heap and manager objects in .data) and plays
// ReShade by exporting ReShadeRegisterAddon / ReShadeUnregisterAddon. build.bat test runs it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

constexpr uint64_t MiB = 1024ull * 1024ull;

// ---- fake game state (.data). The add-on writes these from outside, hence volatile. -----
uint8_t g_stats[0x200] = {1};                                                // heap->stats
volatile uint64_t g_heap_obj[5] = {1024 * MiB, 100 * MiB, 3072 * MiB, 0, 0}; // pool, min, max, stats*
volatile uint64_t g_heap_ptr = 1;  // StreamedTextureHeap* (0 until the "renderer" exists)
uint8_t g_mgr_obj[0x40] = {1};     // +0 demand (u64), +8 bias (float)
volatile uint64_t g_mgr_ptr = 1;
volatile float g_bias_limit = 10.0f;
float g_high = 0.1f, g_low = 0.05f, g_rate = 0.1f;

namespace
{
int g_register_calls = 0, g_unregister_calls = 0;
uint32_t g_last_api = 0, g_max_api = 18;

// ---- fake game code (.text): never executed, only scanned -------------------------------
#pragma warning(suppress : 4325) // .text already has these attributes
#pragma section(".text", read, execute)
__declspec(allocate(".text")) unsigned char g_code[0x400] = {0xCC};

void put(size_t at, std::initializer_list<int> bytes)
{
    for (int b : bytes)
        g_code[at++] = static_cast<unsigned char>(b);
}
void rel32(size_t at, const volatile void *target)
{
    const int32_t disp = static_cast<int32_t>(reinterpret_cast<intptr_t>(target) -
                                              reinterpret_cast<intptr_t>(g_code + at + 4));
    std::memcpy(g_code + at, &disp, 4);
}

void build_fake_game()
{
    DWORD old;
    VirtualProtect(g_code, sizeof(g_code), PAGE_EXECUTE_READWRITE, &old);
    std::memset(g_code, 0xCC, sizeof(g_code));

    const size_t fit = 0x000, heap_getter = 0x200, pool_getter = 0x210, get_mgr = 0x220, get_bias = 0x230,
                 overlay = 0x240;
    put(fit, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC,
              0xC0, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x29, 0x70, 0xC8, 0xC5, 0xF8, 0x29, 0x78, 0xB8, 0x4C, 0x8B, 0xF1,
              0xE8, 0, 0, 0, 0, 0x48, 0x8B, 0xC8, 0xE8, 0, 0, 0, 0, 0x48, 0x8B, 0xC8, 0xC5, 0xF0, 0x57, 0xC9});
    rel32(fit + 35, g_code + heap_getter);
    rel32(fit + 43, g_code + pool_getter);
    put(fit + 0x5C, {0xC5, 0xDA, 0x5C, 0x05});
    rel32(fit + 0x5C + 4, &g_high);
    put(fit + 0x97, {0xC5, 0xDA, 0x5C, 0x05});
    rel32(fit + 0x97 + 4, &g_low);
    put(fit + 0xD2, {0xC5, 0xC2, 0x5C, 0x05});
    rel32(fit + 0xD2 + 4, &g_rate);
    put(fit + 0xF9, {0xC5, 0xCA, 0x5D, 0x35});
    rel32(fit + 0xF9 + 4, &g_bias_limit);

    put(heap_getter, {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3});
    rel32(heap_getter + 3, &g_heap_ptr);
    put(pool_getter, {0x48, 0x8B, 0x01, 0xC3});
    put(get_mgr, {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3});
    rel32(get_mgr + 3, &g_mgr_ptr);
    put(get_bias, {0xC5, 0xFA, 0x10, 0x41, 0x08, 0xC3});
    put(overlay, {0xE8, 0, 0, 0, 0, 0x48, 0x8B, 0xC8, 0xE8, 0, 0, 0, 0, 0xC5, 0xF8, 0x28, 0xC8});
    rel32(overlay + 1, g_code + get_mgr);
    rel32(overlay + 9, g_code + get_bias);

    auto u64_at = [](uint8_t *p, size_t off) -> uint64_t & { return *reinterpret_cast<uint64_t *>(p + off); };
    std::memset(g_stats, 0, sizeof(g_stats));
    u64_at(g_stats, 0x140) = 4096;        // 64 KiB tiles
    u64_at(g_stats, 0x1A8) = 0;           // "left" low: the game's not-sampled-yet state is 0 and ~0
    u64_at(g_stats, 0x1B0) = ~0ull;
    u64_at(g_stats, 0x1E0) = 1500 * MiB;  // heap bytes
    g_heap_obj[3] = reinterpret_cast<uint64_t>(g_stats);
    std::memset(g_mgr_obj, 0, sizeof(g_mgr_obj));
    u64_at(g_mgr_obj, 0) = 1900 * MiB;
    const float bias = 2.2f;
    std::memcpy(g_mgr_obj + 8, &bias, 4);
    g_heap_ptr = 0; // renderer not created yet
    g_mgr_ptr = reinterpret_cast<uint64_t>(g_mgr_obj);
}

// ---- helpers ----------------------------------------------------------------------------
std::wstring g_dir, g_addon, g_ini, g_log;
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

void write_ini(const char *text)
{
    HANDLE h = CreateFileW(g_ini.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n;
    WriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &n, nullptr);
    CloseHandle(h);
}

void set_ini(unsigned min_mb, unsigned max_mb, const char *bias)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[CRStreamingFix]\r\nMinPoolMB=%u\r\nMaxPoolMB=%u\r\nBiasLimit=%s\r\nLogIntervalSec=1\r\n",
                  min_mb, max_mb, bias);
    Sleep(20); // make sure the timestamp moves
    write_ini(buf);
    Sleep(700); // a few ticks
}

bool log_contains(const char *text)
{
    HANDLE h = CreateFileW(g_log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::string content(GetFileSize(h, nullptr), '\0');
    DWORD n = 0;
    ReadFile(h, content.data(), static_cast<DWORD>(content.size()), &n, nullptr);
    CloseHandle(h);
    return content.find(text) != std::string::npos;
}

bool loaded()
{
    return GetModuleHandleW(L"CRStreamingFix.addon64") != nullptr;
}

uint32_t g_seed = 12345;
uint32_t rnd(uint32_t n)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (g_seed >> 8) % n;
}
} // namespace

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(HMODULE, uint32_t api_version)
{
    if (api_version > g_max_api)
        return false;
    ++g_register_calls;
    g_last_api = api_version;
    return true;
}
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(HMODULE)
{
    ++g_unregister_calls;
}

int wmain()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir.resize(g_dir.find_last_of(L'\\') + 1);
    g_addon = g_dir + L"CRStreamingFix.addon64";
    g_ini = g_dir + L"CRStreamingFix.ini";
    g_log = g_dir + L"CRStreamingFix.log";
    DeleteFileW(g_ini.c_str());
    DeleteFileW(g_log.c_str());
    build_fake_game();

    std::printf("1. ReShade-style load/unload storm (unloaded before the first tick)\n");
    bool storm_ok = true;
    for (int i = 0; i < 30; ++i)
    {
        HMODULE m = LoadLibraryW(g_addon.c_str());
        storm_ok = storm_ok && m != nullptr;
        Sleep(rnd(300));
        if (m)
            FreeLibrary(m);
        storm_ok = storm_ok && !loaded();
    }
    check(storm_ok, "30 load/unload cycles, module gone after each");
    check(g_register_calls == 30 && g_unregister_calls == 30, "registered and unregistered 30 times");
    check(g_last_api == 18, "registered with API version 18");
    check(GetFileAttributesW(g_log.c_str()) == INVALID_FILE_ATTRIBUTES, "no log written by instances that never ticked");

    std::printf("2. Unload while the first tick may be scanning\n");
    bool scan_ok = true;
    ULONGLONG worst = 0;
    for (int i = 0; i < 12; ++i)
    {
        HMODULE m = LoadLibraryW(g_addon.c_str());
        Sleep(985 + rnd(60));
        const ULONGLONG t0 = GetTickCount64();
        FreeLibrary(m);
        worst = max(worst, GetTickCount64() - t0);
        scan_ok = scan_ok && !loaded();
    }
    check(scan_ok, "12 unloads around the first tick, module gone after each");
    std::printf("        slowest FreeLibrary: %llu ms\n", worst);
    check(worst < 3000, "unload never stalls");

    std::printf("3. Normal run\n");
    HMODULE m = LoadLibraryW(g_addon.c_str());
    Sleep(1500); // first tick happened; renderer still missing
    check(g_heap_obj[1] == 100 * MiB, "nothing written before the renderer exists");
    check(GetFileAttributesW(g_ini.c_str()) != INVALID_FILE_ATTRIBUTES, "default ini created");
    g_heap_ptr = reinterpret_cast<uint64_t>(&g_heap_obj[0]);
    Sleep(700);
    check(g_heap_obj[1] == 2048 * MiB, "min pool raised to 2048 MB");
    check(g_heap_obj[0] == 2048 * MiB, "pool lifted from 1024 MB to the floor");
    check(g_heap_obj[2] == 3072 * MiB, "max pool untouched");
    check(g_bias_limit == 10.0f, "bias limit untouched");
    g_heap_obj[0] = 500 * MiB;
    Sleep(600);
    check(g_heap_obj[0] == 2048 * MiB, "pool lifted again after the game shrank it");
    set_ini(2048, 0, "-1"); // one stats line per second from here on
    Sleep(1200);
    check(log_contains("VRAM left for textures n/a"), "stats say n/a until the game has sampled its VRAM budget");
    *reinterpret_cast<uint64_t *>(g_stats + 0x1A8) = 1100 * MiB; // the game sampled: 1100-1300 MB left
    *reinterpret_cast<uint64_t *>(g_stats + 0x1B0) = 1300 * MiB;
    Sleep(1200);
    check(log_contains("VRAM left for textures 1100-1300 MB (pool is 948 MB above it)"),
          "stats show what the game would have left for textures");

    std::printf("4. Live ini changes\n");
    set_ini(1536, 0, "2");
    check(g_heap_obj[1] == 1536 * MiB, "MinPoolMB=1536 applied");
    check(g_bias_limit == 2.0f, "BiasLimit=2 applied");
    set_ini(0, 0, "-1");
    check(g_heap_obj[1] == 100 * MiB, "MinPoolMB=0 restores the game's 100 MB");
    check(g_bias_limit == 10.0f, "BiasLimit=-1 restores the game's 10");
    set_ini(4000, 0, "-1");
    check(g_heap_obj[1] == 4000 * MiB && g_heap_obj[2] == 4000 * MiB, "floor above the game's ceiling raises the ceiling");
    g_heap_obj[2] = 4096 * MiB; // game: Texture Resolution -> High
    Sleep(600);
    check(g_heap_obj[2] == 4096 * MiB, "a higher ceiling set by the game is kept");
    set_ini(2048, 0, "-1");
    check(g_heap_obj[1] == 2048 * MiB && g_heap_obj[2] == 4096 * MiB, "back to 2048 keeps the game's ceiling");
    set_ini(2048, 6000, "-1");
    check(g_heap_obj[2] == 6000 * MiB, "MaxPoolMB=6000 applied");
    g_heap_obj[2] = 3072 * MiB; // game: Texture Resolution -> Medium
    Sleep(600);
    check(g_heap_obj[2] == 6000 * MiB, "MaxPoolMB re-applied after the game changed its ceiling");
    set_ini(2048, 0, "-1");
    check(g_heap_obj[2] == 3072 * MiB, "MaxPoolMB=0 returns to the game's current ceiling");

    std::printf("5. Unload while running, reload in the same process\n");
    const int unreg_before = g_unregister_calls;
    const ULONGLONG t0 = GetTickCount64();
    FreeLibrary(m);
    const ULONGLONG unload_ms = GetTickCount64() - t0;
    check(!loaded(), "module gone");
    check(g_unregister_calls == unreg_before + 1, "unregistered from ReShade");
    check(unload_ms < 3000, "unload did not stall");
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1600);
    set_ini(0, 0, "-1");
    check(g_heap_obj[1] == 100 * MiB, "after reload, MinPoolMB=0 still restores the game's 100 MB (not our 2048)");
    set_ini(2048, 0, "-1");
    FreeLibrary(m);

    std::printf("6. Older and unwilling ReShade\n");
    g_max_api = 12;
    m = LoadLibraryW(g_addon.c_str());
    check(m != nullptr && g_last_api == 12, "falls back to the API version ReShade accepts (12)");
    if (m)
        FreeLibrary(m);
    g_max_api = 0;
    m = LoadLibraryW(g_addon.c_str());
    check(m == nullptr && !loaded(), "refused by ReShade: add-on does not stay loaded");
    g_max_api = 18;

    std::printf("7. Loaded as .asi (no ReShade involved)\n");
    const std::wstring asi = g_dir + L"CRStreamingFix.asi";
    CopyFileW(g_addon.c_str(), asi.c_str(), FALSE);
    const int reg_before = g_register_calls;
    m = LoadLibraryW(asi.c_str());
    Sleep(1600);
    set_ini(1800, 0, "-1");
    check(m != nullptr && g_heap_obj[1] == 1800 * MiB, "works without registering: MinPoolMB=1800 applied");
    check(g_register_calls == reg_before, "did not register with ReShade");
    if (m)
        FreeLibrary(m);
    check(GetModuleHandleW(L"CRStreamingFix.asi") == nullptr, "module gone after unload");
    set_ini(2048, 0, "-1");

    std::printf("8. Exit with the add-on loaded\n");
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1300);
    check(m != nullptr, "loaded; the process now exits without unloading it");

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
}

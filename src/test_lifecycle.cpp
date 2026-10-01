// Offline test for the add-on's logic and load/unload lifecycle. No game needed.
//
// The add-on only wakes up inside the game's executable, so this builds to an exe with that name
// (CONTROLResonant.exe, or AlanWake2.exe with /DCRSF_GAME_AW2). It fakes what the add-on looks for (the
// signature bytes in .text, the heap and manager objects in .data) and plays ReShade: it exports the add-on
// registration functions and hands out an ImGui function table whose widgets are stubs driven by a small
// script. build.bat test runs it for both games.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade_overlay.hpp>
#pragma warning(pop)

#include "game.h"

constexpr uint64_t MiB = 1024ull * 1024ull;

// ---- fake game state (.data). The add-on writes these from outside, hence volatile. -----
uint8_t g_stats[0x400] = {1};                                                // heap->stats
volatile uint64_t g_heap_obj[5] = {1024 * MiB, 100 * MiB, 3072 * MiB, 0, 0}; // pool, min, max, stats*
volatile uint64_t g_heap_ptr = 1;  // StreamedTextureHeap* (0 until the "renderer" exists)
uint8_t g_mgr_obj[0x40] = {1};     // demand (u64) and bias (float), at the game's offsets
volatile uint64_t g_mgr_ptr = 1;
volatile uint64_t g_jobs = 1;      // Alan Wake 2: the job system the streaming update is handed to
volatile uint64_t g_other_ptr = 1; // Alan Wake 2: some other job's object
volatile float g_bias_limit = 10.0f;
float g_high = 0.1f, g_low = 0.05f, g_rate = 0.1f;

namespace
{
int g_register_calls = 0, g_unregister_calls = 0;
uint32_t g_last_api = 0, g_max_api = 18;

// ---- fake game code (.text): never executed, only scanned -------------------------------
#pragma warning(suppress : 4325) // .text already has these attributes
#pragma section(".text", read, execute)
__declspec(allocate(".text")) unsigned char g_code[0x8000] = {0xCC};
size_t g_manager_site = 0; // where in g_code the instructions are that give away the manager pointer

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

uint64_t &stat(uint32_t offset)
{
    return *reinterpret_cast<uint64_t *>(g_stats + offset);
}

void build_fake_game()
{
    DWORD old;
    VirtualProtect(g_code, sizeof(g_code), PAGE_EXECUTE_READWRITE, &old);
    std::memset(g_code, 0xCC, sizeof(g_code));

    const size_t fit = 0x000;
#if defined(CRSF_GAME_AW2)
    // Fit-to-pool loads the heap pointer itself: ... ; mov rsi,rcx ; mov rax,[rip+heap] ; mov rcx,[rax] ; ...
    const size_t high = 0x58, low = 0x93, rate = 0xCE, limit = 0xF5;
    put(fit, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83,
              0xEC, 0x60, 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x50, 0xC5, 0xF8, 0x29, 0x7C, 0x24, 0x40, 0x48, 0x8B,
              0xF1, 0x48, 0x8B, 0x05, 0,    0,    0,    0,    0x48, 0x8B, 0x08, 0xC5, 0xF0, 0x57, 0xC9});
    rel32(fit + 0x24, &g_heap_ptr);

    // Two job start stubs. The streaming one calls a worker next to fit-to-pool; the other one's worker is
    // far away, so the object its call site passes must not be taken for the streaming manager.
    const size_t worker = 0x180, start = 0x200, other_start = 0x240, site = 0x280, other_site = 0x2A0,
                 far_worker = 0x6000;
    for (size_t at : {start, other_start})
        put(at, {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xE9, 0xE8, 0,    0,    0,
                 0,    0x8B, 0xC0, 0x48, 0x8D, 0x15, 0,    0,    0,    0,    0x48, 0x8B, 0x14, 0xC2, 0x48, 0x85, 0xD2,
                 0x74, 0x15, 0x83, 0x3A, 0x02, 0x75, 0x10, 0x48, 0x8D, 0x4C, 0x24, 0x38, 0xE8, 0,    0,    0,    0,
                 0x48, 0x83, 0xC4, 0x20, 0x5D, 0xC3});
    rel32(start + 47, g_code + worker);
    rel32(other_start + 47, g_code + far_worker);
    //   mov rdx,[rip+object] ; mov rcx,[rip+jobs] ; call start
    for (size_t at : {site, other_site})
        put(at, {0x48, 0x8B, 0x15, 0, 0, 0, 0, 0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0});
    rel32(site + 3, &g_mgr_ptr);
    rel32(site + 10, &g_jobs);
    rel32(site + 15, g_code + start);
    rel32(other_site + 3, &g_other_ptr);
    rel32(other_site + 10, &g_jobs);
    rel32(other_site + 15, g_code + other_start);
    g_manager_site = site;
#else
    // Fit-to-pool goes through two accessors: ... ; call getHeap ; mov rcx,rax ; call getPool ; ...
    const size_t high = 0x5C, low = 0x97, rate = 0xD2, limit = 0xF9;
    const size_t heap_getter = 0x200, pool_getter = 0x210, get_mgr = 0x220, get_bias = 0x230, overlay = 0x240;
    put(fit, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC,
              0xC0, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x29, 0x70, 0xC8, 0xC5, 0xF8, 0x29, 0x78, 0xB8, 0x4C, 0x8B, 0xF1,
              0xE8, 0, 0, 0, 0, 0x48, 0x8B, 0xC8, 0xE8, 0, 0, 0, 0, 0x48, 0x8B, 0xC8, 0xC5, 0xF0, 0x57, 0xC9});
    rel32(fit + 35, g_code + heap_getter);
    rel32(fit + 43, g_code + pool_getter);
    put(heap_getter, {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3});
    rel32(heap_getter + 3, &g_heap_ptr);
    put(pool_getter, {0x48, 0x8B, 0x01, 0xC3});

    // The stats overlay reads the bias: call getManager ; mov rcx,rax ; call getBias ; vmovaps xmm1,xmm0
    put(get_mgr, {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3});
    rel32(get_mgr + 3, &g_mgr_ptr);
    put(get_bias, {0xC5, 0xFA, 0x10, 0x41, 0x08, 0xC3});
    put(overlay, {0xE8, 0, 0, 0, 0, 0x48, 0x8B, 0xC8, 0xE8, 0, 0, 0, 0, 0xC5, 0xF8, 0x28, 0xC8});
    rel32(overlay + 1, g_code + get_mgr);
    rel32(overlay + 9, g_code + get_bias);
    g_manager_site = overlay;
#endif

    // Both games read the four tweakables with the same instructions.
    put(fit + high, {0xC5, 0xDA, 0x5C, 0x05});
    rel32(fit + high + 4, &g_high);
    put(fit + low, {0xC5, 0xDA, 0x5C, 0x05});
    rel32(fit + low + 4, &g_low);
    put(fit + rate, {0xC5, 0xC2, 0x5C, 0x05});
    rel32(fit + rate + 4, &g_rate);
    put(fit + limit, {0xC5, 0xCA, 0x5D, 0x35});
    rel32(fit + limit + 4, &g_bias_limit);

    // Objects: junk everywhere except the fields the add-on is supposed to read.
    std::memset(g_stats, 0xA5, sizeof(g_stats));
    stat(kLayout.stats_tiles) = 4096;            // 64 KiB tiles
    stat(kLayout.stats_left_lo) = 0;             // "left": the game's not-sampled-yet state is 0 and ~0
    stat(kLayout.stats_left_hi) = ~0ull;
    stat(kLayout.stats_heap_bytes) = 1500 * MiB; // heap bytes
    g_heap_obj[3] = reinterpret_cast<uint64_t>(g_stats);
    std::memset(g_mgr_obj, 0xA5, sizeof(g_mgr_obj));
    const uint64_t demand = 1900 * MiB;
    std::memcpy(g_mgr_obj + kLayout.mgr_demand, &demand, 8);
    const float bias = 2.2f;
    std::memcpy(g_mgr_obj + kLayout.mgr_bias, &bias, 4);
    g_heap_ptr = 0; // renderer not created yet
    g_mgr_ptr = reinterpret_cast<uint64_t>(g_mgr_obj);
    g_jobs = 0x1111;
    g_other_ptr = reinterpret_cast<uint64_t>(g_stats); // readable, and nothing like a streaming manager
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
    std::snprintf(buf, sizeof(buf),
                  "[" CRSF_NAME "]\r\nMinPoolMB=%u\r\nMaxPoolMB=%u\r\nBiasLimit=%s\r\nLogIntervalSec=1\r\n", min_mb, max_mb,
                  bias);
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
    return GetModuleHandleW(CRSF_NAME_W L".addon64") != nullptr;
}

uint32_t g_seed = 12345;
uint32_t rnd(uint32_t n)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (g_seed >> 8) % n;
}

// ---- fake ReShade menu ------------------------------------------------------------------
// Only the widgets the add-on uses are stubbed; calling anything else jumps to null and fails the test.
imgui_function_table g_table = {};
bool g_table_available = true;
void (*g_overlay)(void *) = nullptr;
std::string g_overlay_title;

std::vector<std::string> g_texts;           // everything drawn in the last frame
std::map<std::string, double> g_shown;      // value each slider showed in the last frame
std::string g_last_item;
struct
{
    std::string edit;       // slider to change this frame...
    double value = 0;       // ...to this
    std::string toggle;     // checkbox to click this frame
    std::string click;      // button to click this frame
    std::string release;    // item that reports "released after edit" this frame
} g_script;

bool g_docked = false;
int g_wrap_depth = 0; // PushTextWrapPos / PopTextWrapPos must balance
ImVec2 g_window_pos, g_window_size;
char g_io_storage[sizeof(ImGuiIO)]; // ImGuiIO's constructor lives in imgui.cpp, which is not linked

void build_fake_menu()
{
    reinterpret_cast<ImGuiIO *>(g_io_storage)->DisplaySize = ImVec2(2560.0f, 1440.0f);
    g_table.GetIO = []() -> ImGuiIO & { return *reinterpret_cast<ImGuiIO *>(g_io_storage); };
    g_table.GetFontSize = []() { return 20.0f; };
    g_table.IsWindowDocked = []() { return g_docked; };
    g_table.SetWindowPos = [](const ImVec2 &pos, ImGuiCond) { g_window_pos = pos; };
    g_table.SetWindowSize = [](const ImVec2 &size, ImGuiCond) { g_window_size = size; };
    g_table.PushTextWrapPos = [](float) { ++g_wrap_depth; };
    g_table.PopTextWrapPos = []() { --g_wrap_depth; };
    g_table.TextUnformatted = [](const char *text, const char *) { g_texts.emplace_back(text); };
    g_table.SeparatorText = [](const char *label) { g_texts.emplace_back(label); };
    g_table.ProgressBar = [](float, const ImVec2 &, const char *overlay) { g_texts.emplace_back(overlay ? overlay : ""); };
    g_table.SetItemTooltipV = [](const char *, va_list) {};
    g_table.PushStyleColor2 = [](ImGuiCol, const ImVec4 &) {};
    g_table.PopStyleColor = [](int) {};
    g_table.SliderInt = [](const char *label, int *v, int, int, const char *, ImGuiSliderFlags) {
        g_last_item = label;
        g_shown[label] = *v;
        if (g_script.edit != label)
            return false;
        *v = static_cast<int>(g_script.value);
        g_script.edit.clear();
        return true;
    };
    g_table.SliderFloat = [](const char *label, float *v, float, float, const char *, ImGuiSliderFlags) {
        g_last_item = label;
        g_shown[label] = *v;
        if (g_script.edit != label)
            return false;
        *v = static_cast<float>(g_script.value);
        g_script.edit.clear();
        return true;
    };
    g_table.Checkbox = [](const char *label, bool *v) {
        g_last_item = label;
        if (g_script.toggle != label)
            return false;
        *v = !*v;
        g_script.toggle.clear();
        return true;
    };
    g_table.Button = [](const char *label, const ImVec2 &) {
        g_last_item = label;
        if (g_script.click != label)
            return false;
        g_script.click.clear();
        return true;
    };
    g_table.IsItemDeactivatedAfterEdit = []() {
        if (g_script.release.empty() || g_script.release != g_last_item)
            return false;
        g_script.release.clear();
        return true;
    };
}

// The first frame takes the scripted input; the following ones hand it to the add-on's tick thread
// (the add-on only try-locks, so one frame can miss).
void frames(int count = 4)
{
    for (int i = 0; i < count && g_overlay; ++i)
    {
        if (i)
            Sleep(15);
        g_texts.clear();
        g_shown.clear();
        g_overlay(nullptr);
    }
}

bool drew(const char *text)
{
    for (const std::string &t : g_texts)
        if (t.find(text) != std::string::npos)
            return true;
    return false;
}

int ini_int(const wchar_t *key)
{
    return static_cast<int>(GetPrivateProfileIntW(CRSF_NAME_W, key, -12345, g_ini.c_str()));
}

std::wstring ini_str(const wchar_t *key)
{
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(CRSF_NAME_W, key, L"?", buf, 64, g_ini.c_str());
    return buf;
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
extern "C" __declspec(dllexport) const imgui_function_table *ReShadeGetImGuiFunctionTable(uint32_t version)
{
    return g_table_available && version == IMGUI_VERSION_NUM ? &g_table : nullptr;
}
extern "C" __declspec(dllexport) void ReShadeRegisterOverlay(const char *title, void (*callback)(void *))
{
    g_overlay_title = title ? title : "";
    g_overlay = callback;
}
extern "C" __declspec(dllexport) void ReShadeUnregisterOverlay(const char *, void (*callback)(void *))
{
    if (g_overlay == callback)
        g_overlay = nullptr;
}

int wmain()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir.resize(g_dir.find_last_of(L'\\') + 1);
    g_addon = g_dir + CRSF_NAME_W L".addon64";
    g_ini = g_dir + CRSF_NAME_W L".ini";
    g_log = g_dir + CRSF_NAME_W L".log";
    DeleteFileW(g_ini.c_str());
    DeleteFileW(g_log.c_str());
    build_fake_game();
    build_fake_menu();
    std::printf("Testing " CRSF_NAME " " CRSF_VERSION " inside a fake " CRSF_EXE "\n");

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
    check(!log_contains("no settings tab"), "a tick caught by the unload does not log that the tab is missing");

    std::printf("3. Normal run\n");
    HMODULE m = LoadLibraryW(g_addon.c_str());
    Sleep(1500); // first tick happened; renderer still missing
    check(g_heap_obj[1] == 100 * MiB, "nothing written before the renderer exists");
    check(GetFileAttributesW(g_ini.c_str()) != INVALID_FILE_ATTRIBUTES, "default ini created");
    check(log_contains("manager ptr exe+0x"), "streaming manager found");
    g_heap_ptr = reinterpret_cast<uint64_t>(&g_heap_obj[0]);
    Sleep(700);
    check(g_heap_obj[1] == 2048 * MiB, "min pool raised to 2048 MB");
    check(g_heap_obj[0] == 2048 * MiB, "pool lifted from 1024 MB to the floor");
    check(g_heap_obj[2] == 3072 * MiB, "max pool untouched");
    check(g_bias_limit == 10.0f, "bias limit untouched");
    check(log_contains("bias limit 10.00, +/-0.10 per update, raise above 95% of pool, lower below 90%"),
          "the game's fit-to-pool settings are read and logged");
    g_heap_obj[0] = 500 * MiB;
    Sleep(600);
    check(g_heap_obj[0] == 2048 * MiB, "pool lifted again after the game shrank it");
    set_ini(2048, 0, "-1"); // one stats line per second from here on
    Sleep(1200);
    check(log_contains("used 1756 MB | demand 1900 MB | bias 2.20 mips | VRAM left for textures n/a"),
          "stats show use, demand and bias, and n/a until the game has sampled its VRAM budget");
    stat(kLayout.stats_left_lo) = 1100 * MiB; // the game sampled: 1100-1300 MB left
    stat(kLayout.stats_left_hi) = 1300 * MiB;
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
    const std::wstring asi = g_dir + CRSF_NAME_W L".asi";
    CopyFileW(g_addon.c_str(), asi.c_str(), FALSE);
    const int reg_before = g_register_calls;
    m = LoadLibraryW(asi.c_str());
    Sleep(1600);
    set_ini(1800, 0, "-1");
    check(m != nullptr && g_heap_obj[1] == 1800 * MiB, "works without registering: MinPoolMB=1800 applied");
    check(g_register_calls == reg_before, "did not register with ReShade");
    if (m)
        FreeLibrary(m);
    check(GetModuleHandleW(CRSF_NAME_W L".asi") == nullptr, "module gone after unload");
    set_ini(2048, 0, "-1");

    std::printf("8. Settings tab in the ReShade menu\n");
    g_heap_obj[0] = 2048 * MiB; // the game's own pool update, after the floor was at 4000 MB for a while
    m = LoadLibraryW(g_addon.c_str());
    check(g_overlay != nullptr && g_overlay_title == CRSF_NAME, "tab registered with ReShade");
    frames(1);
    check(drew("Starting..."), "says it is starting before the first tick");
    Sleep(1600);
    frames();
    check(drew("1756 of 2048 MB used") && drew("Blur: 2.20 mips") && drew("Textures want 1900 MB") &&
              drew("The game alone would give textures 1100-1300 MB"),
          "shows pool, blur, demand and what the game would have left");
    check(g_shown["Minimum pool"] == 2048, "slider shows the current minimum");
    check(g_window_size.x == 680.0f && g_window_pos.x == 2560.0f - 680.0f - 40.0f && drew("Tip: drag"),
          "a floating window is placed on the right and explains docking");
    g_docked = true;
    g_window_pos = ImVec2();
    frames();
    check(g_window_pos.x == 0.0f && !drew("Tip: drag"), "a docked tab is left alone");
    g_docked = false;
    stat(kLayout.stats_left_hi) = 1100 * MiB; // low and high equal
    frames();
    check(drew("The game alone would give textures 1100 MB") && !drew("1100-1100"), "an equal range is shown as one number");
    stat(kLayout.stats_left_hi) = 1300 * MiB;
    check(g_wrap_depth == 0, "text wrap pushes and pops balance");
    g_script.edit = "Minimum pool";
    g_script.value = 1536;
    frames();
    Sleep(500);
    check(g_heap_obj[1] == 1536 * MiB, "dragging the slider applies within a tick");
    check(ini_int(L"MinPoolMB") == 2048, "ini not written while dragging");
    g_script.release = "Minimum pool";
    frames();
    Sleep(500);
    check(ini_int(L"MinPoolMB") == 1536, "ini written when the slider is released");
    g_script.toggle = "Limit blur";
    frames();
    Sleep(500);
    check(g_bias_limit == 2.0f && ini_str(L"BiasLimit") == L"2.00", "Limit blur switches the cap on at 2 mips and saves it");
    g_script.edit = "Blur limit";
    g_script.value = 3.5;
    g_script.release = "Blur limit";
    frames();
    Sleep(500);
    check(g_bias_limit == 3.5f && ini_str(L"BiasLimit") == L"3.50", "blur limit slider applies and saves");
    g_script.click = "Defaults";
    frames();
    Sleep(500);
    check(g_heap_obj[1] == 2048 * MiB && g_bias_limit == 10.0f, "Defaults restores 2048 MB and the game's blur limit");
    check(ini_int(L"MinPoolMB") == 2048 && ini_str(L"BiasLimit") == L"-1", "Defaults is saved");
    set_ini(1792, 0, "-1");
    frames();
    check(g_shown["Minimum pool"] == 1792, "an edit of the ini file shows up in the tab");
    check(log_contains("Config saved from the ReShade menu"), "saves are logged");
    FreeLibrary(m);
    check(g_overlay == nullptr && !loaded(), "tab unregistered on unload");
    set_ini(2048, 0, "-1");

    std::printf("9. ReShade without the ImGui 1.92.5 table\n");
    g_table_available = false;
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1600);
    set_ini(1600, 0, "-1");
    check(m != nullptr && g_overlay == nullptr, "loads without a settings tab");
    check(g_heap_obj[1] == 1600 * MiB, "the fix still works through the ini");
    check(log_contains("no settings tab"), "log says why there is no tab");
    if (m)
        FreeLibrary(m);
    g_table_available = true;
    set_ini(2048, 0, "-1");

    std::printf("10. Game build in which the streaming manager cannot be found\n");
    const unsigned char site_byte = g_code[g_manager_site];
    g_code[g_manager_site] = 0xCC;
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1600);
    frames();
    check(g_heap_obj[1] == 2048 * MiB, "the fix still applies (minimum back to 2048 MB)");
    check(drew("MB used") && !drew("Blur:") && !drew("Textures want"), "tab shows the pool, without blur and demand");
    check(log_contains("manager ptr not found") && log_contains("| demand n/a | bias n/a |"),
          "log says so and the stats lines say n/a");
    if (m)
        FreeLibrary(m);
    g_code[g_manager_site] = site_byte;
    m = LoadLibraryW(g_addon.c_str());
    std::memset(g_mgr_obj, 0xA5, sizeof(g_mgr_obj)); // found, but the object holds nothing like a bias
    Sleep(1600);
    frames();
    check(drew("MB used") && !drew("Blur:") && !drew("Textures want"), "values that cannot be a bias are not shown either");
    if (m)
        FreeLibrary(m);

    std::printf("11. Unsupported game version (fit-to-pool not found)\n");
    g_code[0] ^= 0xFF;
    g_heap_obj[1] = 100 * MiB;
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1600);
    frames(1);
    check(drew("This game version is not supported"), "tab says the game version is not supported");
    check(g_heap_obj[1] == 100 * MiB, "nothing is written");
    check(log_contains("Unsupported game version"), "log says why");
    if (m)
        FreeLibrary(m);
    check(!loaded(), "unloads cleanly");
    g_code[0] ^= 0xFF;

    std::printf("12. Exit with the add-on loaded\n");
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1300);
    check(m != nullptr, "loaded; the process now exits without unloading it");

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
}

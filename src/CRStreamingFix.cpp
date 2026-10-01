// CRStreamingFix / AW2StreamingFix - texture streaming pool fix for Control Resonant and Alan Wake 2
// (Northlight, DX12). One source; game.h picks the game at build time.
//
// Why textures go blurry (from disassembly of CONTROLResonant.exe 0.563.737.9; AlanWake2.exe
// 0.559.302.8 has the same code with a few offsets moved):
//   * The texture streamer (StreamedTextureHeap) sizes its pool as
//       pool = clamp(DXGI Budget - (process VRAM usage - streamer's own memory), min, max)
//     with min = 100 MiB (hard-coded) and max = 1664 / 3072 / 4096 MiB from the
//     Texture Resolution setting. Every MB used by anything else in the process
//     (path tracing, RR, frame generation, injected mods) comes out of the pool.
//   * "Texture Streaming:Fit to pool" then raises a global mip bias by 0.1 per update
//     while texture demand is above 95% of the pool (up to 10 mips), and only lowers it
//     again once demand drops under 90%. On 6-8 GB cards the pool stays small, so the
//     bias climbs over a few minutes and never recovers.
//
// This add-on raises the pool floor at runtime (and optionally the ceiling and the bias
// limit). Nothing on disk is patched. Addresses are found by signature, and the add-on
// does nothing if a signature does not match.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

// Dear ImGui through the function table ReShade hands to add-ons (no ImGui code is linked).
#pragma warning(push, 0)
#include <imgui.h>
#include <reshade_overlay.hpp>
#pragma warning(pop)

#include "game.h"

extern "C" __declspec(dllexport) const char *NAME = CRSF_NAME;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Keeps " CRSF_GAME "'s texture streaming pool from shrinking to mush on low-VRAM GPUs. "
    "Settings: the " CRSF_NAME " tab, or " CRSF_NAME ".ini";

namespace
{
constexpr uint32_t kReShadeApiVersion = 18; // accepted by ReShade 6.8; older ReShade is offered lower versions
constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr DWORD kFirstTickMs = 1000; // ReShade loads and drops add-ons a few times at startup; skip those
constexpr DWORD kTickMs = 250;
constexpr char kOverlayTitle[] = CRSF_NAME;

HMODULE g_module = nullptr;
uintptr_t g_exe_base = 0;
std::wstring g_dir; // folder of this DLL, with trailing backslash
HANDLE g_log = INVALID_HANDLE_VALUE;
HANDLE g_timer = nullptr;
volatile LONG g_busy = 0;
volatile LONG g_located = 0;       // signatures resolved, g_state.targets is final
volatile LONG g_locate_failed = 0; // unsupported game version
bool g_registered_with_reshade = false;
bool g_overlay_registered = false;
bool g_has_tab = false; // the settings tab was registered at load; unlike the flag above, never cleared
SRWLOCK g_lock = SRWLOCK_INIT; // guards g_state.cfg / base / announce / save_pending

struct Config
{
    uint64_t min_pool_mb = 2048;
    uint64_t max_pool_mb = 0;  // 0 = leave the game's value
    float bias_limit = -1.0f;  // < 0 = leave the game's value
    uint32_t log_interval_s = 5;
};

struct Targets
{
    uintptr_t heap_ptr = 0;       // global: StreamedTextureHeap*
    uintptr_t mgr_ptr = 0;        // global: TextureStreamingManager*
    uintptr_t bias_limit = 0;     // float tweakable value "Fit to pool:Bias limit"
    uintptr_t high_threshold = 0; // float "Fit to pool:High memory threshold"
    uintptr_t low_threshold = 0;  // float "Fit to pool:Low memory threshold"
    uintptr_t bias_rate = 0;      // float "Fit to pool:Rate of bias change"
};

// The game's own values, read before the first write. Kept in a process environment variable
// too, so they survive ReShade unloading and reloading the add-on within one run.
struct Baseline
{
    bool valid = false;
    uint64_t game_min = 0;
    uint64_t game_max = 0; // follows the Texture Resolution setting
    uint64_t our_max = 0;  // last max we wrote (0 = none), to tell our writes from the game's
    float game_bias_limit = 10.0f;
};

struct State
{
    bool started = false;
    bool announce = true;      // log the limits at the next apply
    bool save_pending = false; // cfg was changed in the ReShade menu and must be written to the ini
    Targets targets;
    Config cfg;
    FILETIME cfg_time = {}; // tick thread only
    Baseline base;
    ULONGLONG last_stats = 0;
};
State g_state;

// ---------------------------------------------------------------------------------------
// logging

void log_line(const char *fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    n += std::vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, args);
    va_end(args);
    if (n > static_cast<int>(sizeof(buf)) - 3)
        n = static_cast<int>(sizeof(buf)) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written;
    WriteFile(g_log, buf, n, &written, nullptr);
}

void open_log()
{
    // Start a fresh log per game run, but keep appending if the add-on is reloaded within the run.
    wchar_t marker[2];
    const bool first_load = GetEnvironmentVariableW(CRSF_NAME_W L"_LOG", marker, 2) == 0;
    const std::wstring path = g_dir + CRSF_NAME_W L".log";
    g_log = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        first_load ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(g_log, 0, nullptr, FILE_END);
    SetEnvironmentVariableW(CRSF_NAME_W L"_LOG", L"1");
}

// ---------------------------------------------------------------------------------------
// guarded memory access (game objects can be freed during shutdown)

bool read_u64(uintptr_t addr, uint64_t &out)
{
    __try
    {
        out = *reinterpret_cast<volatile uint64_t *>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool read_f32(uintptr_t addr, float &out)
{
    __try
    {
        out = *reinterpret_cast<volatile float *>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool write_u64(uintptr_t addr, uint64_t value)
{
    __try
    {
        *reinterpret_cast<volatile uint64_t *>(addr) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool write_f32(uintptr_t addr, float value)
{
    __try
    {
        *reinterpret_cast<volatile float *>(addr) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// ---------------------------------------------------------------------------------------
// signature scanning

struct Section
{
    uintptr_t begin = 0, end = 0;
};

bool find_section(uintptr_t base, const char *name, Section &out)
{
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (std::strncmp(reinterpret_cast<const char *>(sec->Name), name, 8) == 0)
        {
            out.begin = base + sec->VirtualAddress;
            out.end = out.begin + std::max(sec->Misc.VirtualSize, sec->SizeOfRawData);
            return true;
        }
    }
    return false;
}

// "48 8B ?? C3" style pattern
std::vector<int> parse_pattern(const char *pattern)
{
    std::vector<int> bytes;
    for (const char *p = pattern; *p;)
    {
        if (*p == ' ')
        {
            ++p;
            continue;
        }
        if (*p == '?')
        {
            bytes.push_back(-1);
            while (*p == '?')
                ++p;
            continue;
        }
        bytes.push_back(static_cast<int>(std::strtoul(p, const_cast<char **>(&p), 16)));
    }
    return bytes;
}

std::vector<uintptr_t> scan(const Section &s, const char *pattern, size_t max_hits = 16)
{
    const std::vector<int> pat = parse_pattern(pattern);
    std::vector<uintptr_t> hits;
    const auto *mem = reinterpret_cast<const uint8_t *>(s.begin);
    const size_t size = s.end - s.begin;
    if (pat.empty() || size < pat.size())
        return hits;
    for (size_t i = 0; i + pat.size() <= size; ++i)
    {
        if (pat[0] >= 0 && mem[i] != pat[0])
            continue;
        size_t j = 1;
        for (; j < pat.size(); ++j)
            if (pat[j] >= 0 && mem[i + j] != pat[j])
                break;
        if (j == pat.size())
        {
            hits.push_back(s.begin + i);
            if (hits.size() >= max_hits)
                break;
        }
    }
    return hits;
}

uintptr_t rel32_target(uintptr_t disp_addr)
{
    const int32_t disp = *reinterpret_cast<const int32_t *>(disp_addr);
    return disp_addr + 4 + disp;
}

bool bytes_match(uintptr_t addr, const char *pattern)
{
    const std::vector<int> pat = parse_pattern(pattern);
    const auto *mem = reinterpret_cast<const uint8_t *>(addr);
    for (size_t i = 0; i < pat.size(); ++i)
        if (pat[i] >= 0 && mem[i] != pat[i])
            return false;
    return true;
}

bool in_section(uintptr_t addr, const Section &s)
{
    return addr >= s.begin && addr < s.end;
}

// The four fit-to-pool tweakables are read with the same instructions in both games, at different offsets:
//   vsubss xmm0,xmm4,[High] ; vsubss xmm0,xmm4,[Low] ; vsubss xmm0,xmm7,[Rate] ; vminss xmm6,xmm6,[Bias limit]
bool locate_tweakables(uintptr_t f, uintptr_t high, uintptr_t low, uintptr_t rate, uintptr_t limit, Targets &t)
{
    if (!bytes_match(f + high, "C5 DA 5C 05") || !bytes_match(f + low, "C5 DA 5C 05") ||
        !bytes_match(f + rate, "C5 C2 5C 05") || !bytes_match(f + limit, "C5 CA 5D 35"))
    {
        log_line("ERROR: fit-to-pool body looks different than expected, doing nothing.");
        return false;
    }
    t.high_threshold = rel32_target(f + high + 4);
    t.low_threshold = rel32_target(f + low + 4);
    t.bias_rate = rel32_target(f + rate + 4);
    t.bias_limit = rel32_target(f + limit + 4);
    return true;
}

#if defined(CRSF_GAME_AW2)

// Alan Wake 2 (AlanWake2.exe 0.559.302.8). Tweakable names are stripped from this build and the
// accessors are inlined, so the controller loads the heap pointer directly.
bool locate_game(const Section &text, Targets &t, uintptr_t &f)
{
    //   ... ; mov rsi,rcx ; mov rax,[rip+heap] ; mov rcx,[rax] ; vxorps xmm1,xmm1,xmm1
    const auto fit = scan(text, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 60 C5 F8 29 74 24 50 "
                                "C5 F8 29 7C 24 40 48 8B F1 48 8B 05 ?? ?? ?? ?? 48 8B 08 C5 F0 57 C9");
    if (fit.size() != 1)
    {
        log_line("ERROR: fit-to-pool signature matched %zu times (expected 1). Unsupported game version, doing nothing.",
                 fit.size());
        return false;
    }
    f = fit[0];
    t.heap_ptr = rel32_target(f + 0x24);
    if (!locate_tweakables(f, 0x58, 0x93, 0xCE, 0xF5, t))
        return false;

    // Texture streaming manager: the frame code starts the streaming update with
    //   mov rdx,[rip+manager] ; mov rcx,[rip+jobs] ; call start
    // "start" is a small stub the game has one of per kind of job. The streaming one is the stub whose
    // worker sits next to the fit-to-pool function (same class, so the linker keeps them together).
    std::vector<uintptr_t> starts;
    for (uintptr_t hit : scan(text, "48 89 54 24 10 55 48 83 EC 20 48 8B E9 E8 ?? ?? ?? ?? 8B C0 48 8D 15 ?? ?? ?? ?? "
                                    "48 8B 14 C2 48 85 D2 74 ?? 83 3A 02 75 ?? 48 8D 4C 24 38 E8 ?? ?? ?? ??", SIZE_MAX))
    {
        const uintptr_t worker = rel32_target(hit + 47);
        if ((worker > f ? worker - f : f - worker) < 0x4000)
            starts.push_back(hit);
    }
    // Every call site of that shape has to name the same object, or none is trusted.
    uintptr_t manager = 0;
    bool agree = true;
    if (starts.empty())
        return true;
    for (uintptr_t hit : scan(text, "48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??", SIZE_MAX))
    {
        if (std::find(starts.begin(), starts.end(), rel32_target(hit + 15)) == starts.end())
            continue;
        const uintptr_t candidate = rel32_target(hit + 3);
        agree = agree && (manager == 0 || manager == candidate);
        manager = candidate;
    }
    t.mgr_ptr = agree ? manager : 0;
    return true;
}

#else

// Control Resonant (CONTROLResonant.exe 0.563.737.9).
bool locate_game(const Section &text, Targets &t, uintptr_t &f)
{
    //   ... ; call StreamedTextureHeap::get ; mov rcx,rax ; call StreamedTextureHeap::poolSize
    const auto fit = scan(text,
                          "48 8B C4 48 89 58 20 55 56 57 41 56 41 57 48 81 EC C0 00 00 00 C5 F8 29 70 C8 "
                          "C5 F8 29 78 B8 4C 8B F1 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B C8 C5 F0 57 C9");
    if (fit.size() != 1)
    {
        log_line("ERROR: fit-to-pool signature matched %zu times (expected 1). Unsupported game version, doing nothing.",
                 fit.size());
        return false;
    }
    f = fit[0];
    const uintptr_t heap_getter = rel32_target(f + 35);
    const uintptr_t pool_getter = rel32_target(f + 43);
    if (!in_section(heap_getter, text) || !in_section(pool_getter, text) ||
        !bytes_match(heap_getter, "48 8B 05 ?? ?? ?? ?? C3") || !bytes_match(pool_getter, "48 8B 01 C3"))
    {
        log_line("ERROR: StreamedTextureHeap accessors look different than expected, doing nothing.");
        return false;
    }
    t.heap_ptr = rel32_target(heap_getter + 3);
    if (!locate_tweakables(f, 0x5C, 0x97, 0xD2, 0xF9, t))
        return false;

    // Texture streaming manager: stats code does
    //   call getManager (mov rax,[rip+x]; ret) ; mov rcx,rax ; call getBias (vmovss xmm0,[rcx+8]; ret)
    for (uintptr_t hit : scan(text, "E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? C5 F8 28 C8"))
    {
        const uintptr_t get_mgr = rel32_target(hit + 1);
        const uintptr_t get_bias = rel32_target(hit + 9);
        if (in_section(get_mgr, text) && in_section(get_bias, text) && bytes_match(get_mgr, "48 8B 05 ?? ?? ?? ?? C3") &&
            bytes_match(get_bias, "C5 FA 10 41 08 C3"))
        {
            t.mgr_ptr = rel32_target(get_mgr + 3);
            break;
        }
    }
    return true;
}

#endif

// FileVersion of the game executable, for the log. Read from the mapped image instead of through
// version.dll: the fixed part of a version resource starts with a signature and sits on a 4-byte boundary.
bool exe_version(uintptr_t base, unsigned version[4])
{
    __try
    {
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        const auto *words = reinterpret_cast<const uint32_t *>(base + dir.VirtualAddress);
        for (size_t i = 0; i + 4 <= dir.Size / 4; ++i)
        {
            if (words[i] != 0xFEEF04BD) // VS_FIXEDFILEINFO: signature, struct version, file version high, low
                continue;
            version[0] = words[i + 2] >> 16;
            version[1] = words[i + 2] & 0xFFFF;
            version[2] = words[i + 3] >> 16;
            version[3] = words[i + 3] & 0xFFFF;
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

bool locate(uintptr_t base, Targets &t)
{
    unsigned version[4];
    if (exe_version(base, version))
        log_line("Game executable: " CRSF_EXE " %u.%u.%u.%u", version[0], version[1], version[2], version[3]);

    Section text, data;
    if (!find_section(base, ".text", text) || !find_section(base, ".data", data))
    {
        log_line("ERROR: could not read the section table of the game executable");
        return false;
    }

    uintptr_t f = 0;
    if (!locate_game(text, t, f))
        return false;

    const uintptr_t all[] = {t.heap_ptr, t.high_threshold, t.low_threshold, t.bias_rate, t.bias_limit};
    for (uintptr_t a : all)
    {
        if (!in_section(a, data))
        {
            log_line("ERROR: resolved address %p is outside .data, doing nothing.", reinterpret_cast<void *>(a));
            return false;
        }
    }
    if (t.mgr_ptr && !in_section(t.mgr_ptr, data))
        t.mgr_ptr = 0;

    char manager[64] = "not found (stats will lack bias/demand)";
    if (t.mgr_ptr)
        std::snprintf(manager, sizeof(manager), "exe+0x%llX", static_cast<unsigned long long>(t.mgr_ptr - base));
    log_line("Found fit-to-pool at exe+0x%llX, heap ptr exe+0x%llX, manager ptr %s",
             static_cast<unsigned long long>(f - base), static_cast<unsigned long long>(t.heap_ptr - base), manager);
    return true;
}

// ---------------------------------------------------------------------------------------
// config

std::wstring ini_path()
{
    return g_dir + CRSF_NAME_W L".ini";
}

void write_default_ini()
{
    const std::wstring path = ini_path();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    static const char text[] =
        "; " CRSF_NAME " settings. Edits are picked up while the game runs.\r\n"
        "; The same settings are in the " CRSF_NAME " tab of the ReShade menu.\r\n"
        "[" CRSF_NAME "]\r\n"
        "; Smallest texture streaming pool in MB. The game allows 100 MB and shrinks the pool to whatever\r\n"
        "; VRAM is left after everything else, which is what makes textures blurry on 6-8 GB cards.\r\n"
        "; Higher = sharper, but past your free VRAM Windows starts paging to system RAM (stutter).\r\n"
        "; 0 = leave the game's value.\r\n"
        "MinPoolMB=2048\r\n"
        "; Largest pool in MB. The game sets 1664 / 3072 / 4096 from Texture Resolution Low / Medium / High.\r\n"
        "; 0 = leave the game's value.\r\n"
        "MaxPoolMB=0\r\n"
        "; Largest mip bias the streamer may add when textures don't fit the pool (game default 10).\r\n"
        "; -1 = leave the game's value.\r\n"
        "BiasLimit=-1\r\n"
        "; Seconds between stats lines in " CRSF_NAME ".log. 0 = off.\r\n"
        "LogIntervalSec=5\r\n";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written;
    WriteFile(h, text, sizeof(text) - 1, &written, nullptr);
    CloseHandle(h);
}

Config read_config()
{
    const std::wstring path = ini_path();
    Config c;
    c.min_pool_mb = GetPrivateProfileIntW(CRSF_NAME_W, L"MinPoolMB", 2048, path.c_str());
    c.max_pool_mb = GetPrivateProfileIntW(CRSF_NAME_W, L"MaxPoolMB", 0, path.c_str());
    c.log_interval_s = GetPrivateProfileIntW(CRSF_NAME_W, L"LogIntervalSec", 5, path.c_str());
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(CRSF_NAME_W, L"BiasLimit", L"-1", buf, 64, path.c_str());
    c.bias_limit = static_cast<float>(std::wcstod(buf, nullptr));
    if (c.min_pool_mb > 16384)
        c.min_pool_mb = 16384;
    if (c.max_pool_mb > 16384)
        c.max_pool_mb = 16384;
    if (c.bias_limit > 20.0f)
        c.bias_limit = 20.0f;
    return c;
}

// Writes the four values in place; comments and anything else in the file stay.
void write_config(const Config &c)
{
    const std::wstring path = ini_path();
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%llu", static_cast<unsigned long long>(c.min_pool_mb));
    WritePrivateProfileStringW(CRSF_NAME_W, L"MinPoolMB", buf, path.c_str());
    std::swprintf(buf, 32, L"%llu", static_cast<unsigned long long>(c.max_pool_mb));
    WritePrivateProfileStringW(CRSF_NAME_W, L"MaxPoolMB", buf, path.c_str());
    if (c.bias_limit < 0.0f)
        std::swprintf(buf, 32, L"-1");
    else
        std::swprintf(buf, 32, L"%.2f", static_cast<double>(c.bias_limit));
    WritePrivateProfileStringW(CRSF_NAME_W, L"BiasLimit", buf, path.c_str());
    std::swprintf(buf, 32, L"%u", c.log_interval_s);
    WritePrivateProfileStringW(CRSF_NAME_W, L"LogIntervalSec", buf, path.c_str());
}

FILETIME ini_time()
{
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    GetFileAttributesExW(ini_path().c_str(), GetFileExInfoStandard, &d);
    return d.ftLastWriteTime;
}

void log_config(const char *what, const Config &c)
{
    log_line("%s: MinPoolMB=%llu MaxPoolMB=%llu BiasLimit=%.2f LogIntervalSec=%u", what,
             static_cast<unsigned long long>(c.min_pool_mb), static_cast<unsigned long long>(c.max_pool_mb),
             c.bias_limit, c.log_interval_s);
}

// ---------------------------------------------------------------------------------------
// baseline (the game's own values)

void save_baseline(const Baseline &b)
{
    wchar_t buf[128];
    std::swprintf(buf, 128, L"%llu %llu %llu %.9g", static_cast<unsigned long long>(b.game_min),
                  static_cast<unsigned long long>(b.game_max), static_cast<unsigned long long>(b.our_max),
                  static_cast<double>(b.game_bias_limit));
    SetEnvironmentVariableW(CRSF_NAME_W L"_BASELINE", buf);
}

bool load_baseline(Baseline &b)
{
    wchar_t buf[128] = {};
    if (GetEnvironmentVariableW(CRSF_NAME_W L"_BASELINE", buf, 128) == 0)
        return false;
    unsigned long long mn = 0, mx = 0, our = 0;
    double bias = 0;
    if (swscanf_s(buf, L"%llu %llu %llu %lf", &mn, &mx, &our, &bias) != 4)
        return false;
    b.game_min = mn;
    b.game_max = mx;
    b.our_max = our;
    b.game_bias_limit = static_cast<float>(bias);
    b.valid = true;
    return true;
}

// ---------------------------------------------------------------------------------------
// the fix

// Called with g_lock held.
void apply(State &s)
{
    const Targets &t = s.targets;
    const Config &c = s.cfg;
    Baseline &b = s.base;

    uint64_t heap = 0;
    if (!read_u64(t.heap_ptr, heap) || heap == 0)
        return; // renderer not created yet

    uint64_t pool = 0, min_pool = 0, max_pool = 0;
    float bias_limit = 0;
    if (!read_u64(heap + 0x00, pool) || !read_u64(heap + 0x08, min_pool) || !read_u64(heap + 0x10, max_pool) ||
        !read_f32(t.bias_limit, bias_limit))
        return;

    if (!b.valid && !load_baseline(b))
    {
        // The renderer exists, so the tweakables were initialised long ago and nothing here is ours yet.
        b.game_min = min_pool;
        b.game_max = max_pool;
        b.our_max = 0;
        b.game_bias_limit = bias_limit;
        b.valid = true;
        save_baseline(b);
        float hi = 0, lo = 0, rate = 0;
        read_f32(t.high_threshold, hi);
        read_f32(t.low_threshold, lo);
        read_f32(t.bias_rate, rate);
        log_line("Game values: pool min %llu MB, max %llu MB; fit-to-pool bias limit %.2f, +/-%.2f per update, "
                 "raise above %.0f%% of pool, lower below %.0f%%",
                 static_cast<unsigned long long>(b.game_min / kMiB), static_cast<unsigned long long>(b.game_max / kMiB),
                 b.game_bias_limit, rate, (1.0f - lo) * 100.0f, (1.0f - hi) * 100.0f);
    }
    else if (max_pool != b.our_max && max_pool != b.game_max)
    {
        b.game_max = max_pool; // the game set a new ceiling (Texture Resolution changed)
        save_baseline(b);
        s.announce = true;
    }

    const uint64_t want_min = c.min_pool_mb ? c.min_pool_mb * kMiB : b.game_min;
    uint64_t want_max = c.max_pool_mb ? c.max_pool_mb * kMiB : b.game_max;
    if (want_min > want_max)
        want_max = want_min; // e.g. Texture Resolution Low caps the pool at 1664 MB

    if (max_pool != want_max && write_u64(heap + 0x10, want_max))
    {
        b.our_max = want_max != b.game_max ? want_max : 0;
        save_baseline(b);
        s.announce = true;
    }
    if (min_pool != want_min && write_u64(heap + 0x08, want_min))
        s.announce = true;
    if (pool < want_min)
        write_u64(heap + 0x00, want_min); // take effect now instead of at the game's next pool update

    const float want_bias = c.bias_limit >= 0.0f ? c.bias_limit : b.game_bias_limit;
    if (bias_limit != want_bias && write_f32(t.bias_limit, want_bias))
        s.announce = true;

    if (s.announce)
    {
        s.announce = false;
        log_line("Applied: pool min %llu MB (game %llu), max %llu MB (game %llu), bias limit %.2f (game %.2f)",
                 static_cast<unsigned long long>(want_min / kMiB), static_cast<unsigned long long>(b.game_min / kMiB),
                 static_cast<unsigned long long>(want_max / kMiB), static_cast<unsigned long long>(b.game_max / kMiB),
                 want_bias, b.game_bias_limit);
    }
}

// What the streamer is doing right now, read straight from the game.
struct Live
{
    bool renderer = false; // heap object exists
    uint64_t pool = 0, min_pool = 0, max_pool = 0, used = 0;
    bool manager = false; // demand and bias could be read
    uint64_t demand = 0;
    float bias = 0.0f;
    // What the game computes as free for textures (DXGI budget minus the rest of the process), as a
    // low-high range over its sampling window: the pool it would pick by itself.
    bool left_valid = false;
    uint64_t left_lo = 0, left_hi = 0;
};

Live read_live(const Targets &t)
{
    Live v;
    uint64_t heap = 0, st = 0, heap_bytes = 0, tiles = 0, mgr = 0;
    if (!read_u64(t.heap_ptr, heap) || heap == 0)
        return v;
    v.renderer = true;
    read_u64(heap + 0x00, v.pool);
    read_u64(heap + 0x08, v.min_pool);
    read_u64(heap + 0x10, v.max_pool);
    read_u64(heap + 0x18, st);
    if (st)
    {
        read_u64(st + kLayout.stats_heap_bytes, heap_bytes);
        read_u64(st + kLayout.stats_tiles, tiles);
        read_u64(st + kLayout.stats_left_lo, v.left_lo);
        read_u64(st + kLayout.stats_left_hi, v.left_hi);
        // Until the game has sampled once, the two fields hold 0 and ~0.
        v.left_valid = v.left_lo <= v.left_hi && v.left_hi <= (1ull << 50);
    }
    v.used = heap_bytes + (tiles << 16);
    if (t.mgr_ptr && read_u64(t.mgr_ptr, mgr) && mgr)
    {
        // Shown only if it looks like a mip bias and a byte count, in case a game update moved the fields.
        v.manager = read_u64(mgr + kLayout.mgr_demand, v.demand) && read_f32(mgr + kLayout.mgr_bias, v.bias) &&
                    v.bias >= 0.0f && v.bias <= 32.0f && v.demand < (1ull << 40);
    }
    return v;
}

// "1118-1142 MB", or "4118 MB" when both ends are the same.
void format_left(char *out, size_t size, const Live &v)
{
    const unsigned long long lo = v.left_lo / kMiB, hi = v.left_hi / kMiB;
    if (lo == hi)
        std::snprintf(out, size, "%llu MB", lo);
    else
        std::snprintf(out, size, "%llu-%llu MB", lo, hi);
}

void log_stats(const Targets &t)
{
    const Live v = read_live(t);
    if (!v.renderer)
    {
        log_line("waiting for the renderer...");
        return;
    }

    char left[96];
    if (!v.left_valid)
    {
        std::snprintf(left, sizeof(left), "n/a");
    }
    else
    {
        format_left(left, sizeof(left), v);
        if (v.pool > v.left_lo)
        {
            const size_t n = std::strlen(left);
            std::snprintf(left + n, sizeof(left) - n, " (pool is %llu MB above it)",
                          static_cast<unsigned long long>((v.pool - v.left_lo) / kMiB));
        }
    }

    char streamer[64];
    if (v.manager)
        std::snprintf(streamer, sizeof(streamer), "demand %4llu MB | bias %.2f mips",
                      static_cast<unsigned long long>(v.demand / kMiB), v.bias);
    else
        std::snprintf(streamer, sizeof(streamer), "demand n/a | bias n/a");

    log_line("pool %4llu MB [min %llu, max %llu] | used %4llu MB | %s | VRAM left for textures %s",
             static_cast<unsigned long long>(v.pool / kMiB), static_cast<unsigned long long>(v.min_pool / kMiB),
             static_cast<unsigned long long>(v.max_pool / kMiB), static_cast<unsigned long long>(v.used / kMiB),
             streamer, left);
}

// Runs on a thread pool thread every kTickMs. Must not take the loader lock: DllMain waits for it on unload.
VOID CALLBACK tick(PVOID, BOOLEAN)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return; // previous tick still running (first one scans the executable)

    State &s = g_state;
    if (!s.started)
    {
        s.started = true;
        open_log();
        log_line(CRSF_NAME " " CRSF_VERSION " (%s)",
                 !g_registered_with_reshade ? "loaded without ReShade"
                 : g_has_tab                ? "ReShade add-on, settings tab available"
                                            : "ReShade add-on, no settings tab: this ReShade lacks the ImGui 1.92.5 table");
        if (locate(g_exe_base, s.targets))
        {
            write_default_ini();
            s.cfg = read_config();
            s.cfg_time = ini_time();
            log_config("Config", s.cfg);
            InterlockedExchange(&g_located, 1); // from here on the settings tab may touch g_state (under g_lock)
        }
        else
        {
            InterlockedExchange(&g_locate_failed, 1);
        }
    }

    if (g_located)
    {
        // File I/O happens outside the lock; the settings tab only ever try-locks, so it never waits on us.
        const FILETIME now_time = ini_time();
        const bool file_changed = CompareFileTime(&now_time, &s.cfg_time) != 0;
        Config from_file;
        if (file_changed)
            from_file = read_config();

        bool save = false;
        AcquireSRWLockExclusive(&g_lock);
        if (s.save_pending)
        {
            s.save_pending = false; // an edit in the menu wins over a simultaneous edit of the file
            save = true;
        }
        else if (file_changed)
        {
            s.cfg = from_file;
            s.announce = true;
        }
        const Config cfg = s.cfg;
        apply(s);
        ReleaseSRWLockExclusive(&g_lock);

        if (save)
        {
            write_config(cfg);
            s.cfg_time = ini_time();
            log_config("Config saved from the ReShade menu", cfg);
        }
        else if (file_changed)
        {
            s.cfg_time = now_time;
            log_config("Config reloaded", cfg);
        }

        const ULONGLONG now = GetTickCount64();
        if (cfg.log_interval_s && now - s.last_stats >= cfg.log_interval_s * 1000ull)
        {
            s.last_stats = now;
            log_stats(s.targets);
        }
    }

    InterlockedExchange(&g_busy, 0);
}

// ---------------------------------------------------------------------------------------
// settings tab in the ReShade menu

void tooltip(const char *text)
{
    ImGui::SetItemTooltip("%s", text);
}

// Called by ReShade on its render thread while the tab is visible. It never blocks on the tick thread and
// does no file I/O: edits are handed over under a try-lock and applied and saved by the next tick.
void draw_overlay(void * /*reshade::api::effect_runtime*/)
{
    // ReShade only docks add-on windows as tabs when it builds its layout for the first time. On an existing
    // layout a new window floats at ImGui's default spot, small and under the main menu, so place it once.
    const bool floating = !ImGui::IsWindowDocked();
    if (floating)
    {
        const float em = ImGui::GetFontSize();
        const float width = 34.0f * em;
        ImGui::SetWindowSize(ImVec2(width, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetWindowPos(ImVec2(std::max(0.0f, ImGui::GetIO().DisplaySize.x - width - 2.0f * em), 3.0f * em),
                            ImGuiCond_FirstUseEver);
    }

    if (!g_located)
    {
        ImGui::TextUnformatted(g_locate_failed ? "This game version is not supported. See " CRSF_NAME ".log."
                                               : "Starting...");
        return;
    }

    State &s = g_state;
    static Config ui_cfg;
    static Baseline ui_base;
    static bool ui_synced = false, ui_dirty = false, ui_save = false;
    if (TryAcquireSRWLockExclusive(&g_lock))
    {
        if (ui_dirty)
        {
            s.cfg = ui_cfg;
            s.announce = true;
            ui_dirty = false;
        }
        else
        {
            ui_cfg = s.cfg; // picks up edits made in the ini file
        }
        if (ui_save)
        {
            s.save_pending = true;
            ui_save = false;
        }
        ui_base = s.base;
        ui_synced = true;
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (!ui_synced)
    {
        ImGui::TextUnformatted("Starting...");
        return;
    }

    char text[192];
    const Live v = read_live(s.targets);

    ImGui::PushTextWrapPos(0.0f); // wrap text lines at the window edge instead of clipping them
    ImGui::SeparatorText("Right now");
    if (!v.renderer)
    {
        ImGui::TextUnformatted("Waiting for the renderer...");
    }
    else
    {
        std::snprintf(text, sizeof(text), "%llu of %llu MB used", static_cast<unsigned long long>(v.used / kMiB),
                      static_cast<unsigned long long>(v.pool / kMiB));
        ImGui::ProgressBar(v.pool ? static_cast<float>(static_cast<double>(v.used) / static_cast<double>(v.pool)) : 0.0f,
                           ImVec2(-FLT_MIN, 0.0f), text);
        tooltip("The texture streaming pool and how much of it is filled.");

        if (v.manager)
        {
            std::snprintf(text, sizeof(text), "Blur: %.2f mips%s", v.bias, v.bias < 0.05f ? " (full resolution)" : "");
            ImGui::TextUnformatted(text);
            tooltip("Mip levels the streamer is dropping to make textures fit the pool. 0 is full resolution.");

            std::snprintf(text, sizeof(text), "Textures want %llu MB at this blur",
                          static_cast<unsigned long long>(v.demand / kMiB));
            ImGui::TextUnformatted(text);
        }

        if (v.left_valid)
        {
            char left[48];
            format_left(left, sizeof(left), v);
            std::snprintf(text, sizeof(text), "The game alone would give textures %s", left);
            ImGui::TextUnformatted(text);
            tooltip("VRAM budget minus everything else the game has in VRAM. Without this add-on, that is the pool.");
            if (v.pool > v.left_lo)
            {
                std::snprintf(text, sizeof(text), "Pool is %llu MB above that. Windows pages the difference to system RAM.",
                              static_cast<unsigned long long>((v.pool - v.left_lo) / kMiB));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.25f, 1.0f));
                ImGui::TextUnformatted(text);
                ImGui::PopStyleColor();
                tooltip("Fine in small amounts. If the game stutters, lower the minimum pool or free VRAM another way "
                        "(frame generation, path tracing quality, render resolution).");
            }
        }
    }

    ImGui::SeparatorText("Settings");
    bool changed = false, released = false;
    char format[64];

    int min_mb = static_cast<int>(ui_cfg.min_pool_mb);
    if (min_mb)
        std::snprintf(format, sizeof(format), "%%d MB");
    else
        std::snprintf(format, sizeof(format), "game value (%llu MB)", static_cast<unsigned long long>(ui_base.game_min / kMiB));
    if (ImGui::SliderInt("Minimum pool", &min_mb, 0, 8192, format))
    {
        ui_cfg.min_pool_mb = static_cast<uint64_t>(std::clamp((min_mb + 32) / 64 * 64, 0, 16384));
        changed = true;
    }
    released |= ImGui::IsItemDeactivatedAfterEdit();
    tooltip("Smallest texture pool. Higher is sharper. Above your free VRAM, Windows pages textures to system RAM, "
            "which can stutter. 0 leaves the game's value.");

    int max_mb = static_cast<int>(ui_cfg.max_pool_mb);
    if (max_mb)
        std::snprintf(format, sizeof(format), "%%d MB");
    else
        std::snprintf(format, sizeof(format), "game value (%llu MB)", static_cast<unsigned long long>(ui_base.game_max / kMiB));
    if (ImGui::SliderInt("Maximum pool", &max_mb, 0, 8192, format))
    {
        ui_cfg.max_pool_mb = static_cast<uint64_t>(std::clamp((max_mb + 32) / 64 * 64, 0, 16384));
        changed = true;
    }
    released |= ImGui::IsItemDeactivatedAfterEdit();
    tooltip("Largest texture pool. 0 leaves the game's value, which follows Texture Resolution. "
            "It is raised to the minimum if the minimum is higher.");

    bool limit_blur = ui_cfg.bias_limit >= 0.0f;
    if (ImGui::Checkbox("Limit blur", &limit_blur))
    {
        ui_cfg.bias_limit = limit_blur ? 2.0f : -1.0f;
        changed = released = true;
    }
    tooltip("Cap the mip levels the streamer may drop. Without a bigger pool this does not sharpen anything: "
            "textures that no longer fit just fail to load.");
    if (limit_blur)
    {
        float limit = ui_cfg.bias_limit;
        if (ImGui::SliderFloat("Blur limit", &limit, 0.0f, 10.0f, "%.1f mips"))
        {
            ui_cfg.bias_limit = std::clamp(limit, 0.0f, 20.0f);
            changed = true;
        }
        released |= ImGui::IsItemDeactivatedAfterEdit();
        std::snprintf(text, sizeof(text), "Game value: %.0f mips.", ui_base.game_bias_limit);
        tooltip(text);
    }

    int log_s = static_cast<int>(ui_cfg.log_interval_s);
    if (ImGui::SliderInt("Log interval", &log_s, 0, 60, log_s ? "%d s" : "off"))
    {
        ui_cfg.log_interval_s = static_cast<uint32_t>(std::clamp(log_s, 0, 3600));
        changed = true;
    }
    released |= ImGui::IsItemDeactivatedAfterEdit();
    tooltip("Seconds between stats lines in " CRSF_NAME ".log.");

    if (ImGui::Button("Defaults"))
    {
        ui_cfg = Config();
        changed = released = true;
    }
    tooltip("Minimum 2048 MB, everything else left to the game.");

    if (floating)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextUnformatted("Tip: drag this window's title onto the ReShade tab bar to dock it.");
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();

    if (changed)
        ui_dirty = true; // handed to the tick thread at the next frame's try-lock
    if (released)
        ui_save = true; // written to the ini by the tick thread
}

// ---------------------------------------------------------------------------------------
// loading

// ReShade add-on registration without the SDK: find the module that exports ReShadeRegisterAddon.
HMODULE find_reshade()
{
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return nullptr;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < count; ++i)
        if (GetProcAddress(modules[i], "ReShadeRegisterAddon") && GetProcAddress(modules[i], "ReShadeUnregisterAddon"))
            return modules[i];
    return nullptr;
}

bool has_addon_extension(const wchar_t *path)
{
    const wchar_t *ext = std::wcsrchr(path, L'.');
    return ext && (_wcsicmp(ext, L".addon64") == 0 || _wcsicmp(ext, L".addon") == 0);
}

using overlay_callback = void (*)(void *);
using overlay_fn = void(__cdecl *)(const char *, overlay_callback);

// The settings tab needs ReShade's ImGui function table for exactly the ImGui version this was built with.
// Without it the fix still runs; it is just configured through the ini only.
void register_overlay(HMODULE reshade)
{
    using table_fn = const imgui_function_table *(__cdecl *)(uint32_t);
    const auto get_table = reinterpret_cast<table_fn>(GetProcAddress(reshade, "ReShadeGetImGuiFunctionTable"));
    const auto reg = reinterpret_cast<overlay_fn>(GetProcAddress(reshade, "ReShadeRegisterOverlay"));
    if (!get_table || !reg)
        return;
    const imgui_function_table *table = get_table(IMGUI_VERSION_NUM);
    if (!table)
        return;
    imgui_function_table_instance() = table;
    reg(kOverlayTitle, &draw_overlay);
    g_overlay_registered = g_has_tab = true;
}

void unregister_overlay(HMODULE reshade)
{
    if (!g_overlay_registered)
        return;
    if (const auto unreg = reinterpret_cast<overlay_fn>(GetProcAddress(reshade, "ReShadeUnregisterOverlay")))
        unreg(kOverlayTitle, &draw_overlay);
    g_overlay_registered = false;
}

bool process_is_exiting()
{
    using fn = BOOLEAN(NTAPI *)();
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (const auto in_progress = reinterpret_cast<fn>(GetProcAddress(ntdll, "RtlDllShutdownInProgress")))
            return in_progress() != FALSE;
    return false;
}

void stop_timer()
{
    if (!g_timer)
        return;
    // Wait for a running tick to finish before the DLL goes away. Ticks never need the loader lock.
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (done)
    {
        if (DeleteTimerQueueTimer(nullptr, g_timer, done) || GetLastError() == ERROR_IO_PENDING)
            WaitForSingleObject(done, 5000);
        CloseHandle(done);
    }
    else
    {
        DeleteTimerQueueTimer(nullptr, g_timer, INVALID_HANDLE_VALUE);
    }
    g_timer = nullptr;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;

        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const wchar_t *exe_name = std::wcsrchr(path, L'\\');
        exe_name = exe_name ? exe_name + 1 : path;
        if (_wcsicmp(exe_name, CRSF_EXE_W) != 0)
            return TRUE; // some other process (another game, a setup tool): stay inert
        g_exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

        GetModuleFileNameW(module, path, MAX_PATH);
        g_dir = path;
        g_dir.resize(g_dir.find_last_of(L'\\') + 1);

        if (has_addon_extension(path))
        {
            if (HMODULE reshade = find_reshade())
            {
                using register_fn = bool(__cdecl *)(HMODULE, uint32_t);
                const auto reg = reinterpret_cast<register_fn>(GetProcAddress(reshade, "ReShadeRegisterAddon"));
                for (uint32_t version = kReShadeApiVersion; version >= 1 && !g_registered_with_reshade; --version)
                    g_registered_with_reshade = reg(module, version);
                if (!g_registered_with_reshade)
                    return FALSE; // ReShade refused the add-on and would unload it anyway
                register_overlay(reshade);
            }
        }

        if (!CreateTimerQueueTimer(&g_timer, nullptr, tick, nullptr, kFirstTickMs, kTickMs, WT_EXECUTEDEFAULT))
            g_timer = nullptr;
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // At process exit the other threads are already gone; there is nothing to wait for or hand back.
        if (reserved != nullptr || process_is_exiting())
            return TRUE;

        HMODULE reshade = g_registered_with_reshade ? find_reshade() : nullptr;
        if (reshade)
            unregister_overlay(reshade); // no more draw_overlay calls after this
        stop_timer();
        if (reshade)
        {
            using unregister_fn = void(__cdecl *)(HMODULE);
            if (const auto unreg = reinterpret_cast<unregister_fn>(GetProcAddress(reshade, "ReShadeUnregisterAddon")))
                unreg(module);
        }
        g_registered_with_reshade = false;
        if (g_log != INVALID_HANDLE_VALUE)
        {
            log_line("Unloaded.");
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}

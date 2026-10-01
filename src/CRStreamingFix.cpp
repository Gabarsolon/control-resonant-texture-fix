// CRStreamingFix / AW2StreamingFix / ControlStreamingFix - texture streaming pool fix for Remedy's
// Northlight games. One source; game.h picks the game at build time.
//
// These games keep their streamed textures inside a pool, and blur them (a global mip bias) until what
// is in use fits. The pool is sized from the VRAM that is left over, so on 6-8 GB cards it ends up near
// its 100 MB minimum and the blur climbs to the limit. The add-on raises that minimum at runtime (and
// optionally caps the blur). Nothing on disk is patched.
//
// This file is what the games share: loading, the timer, settings and the tab in the ReShade menu.
// How a game's pool is found and changed is in its backend:
//   backend_heap.h         Control Resonant, Alan Wake 2   (addresses found by signature)
//   backend_tweakables.h   Control                         (settings the game looks up by name)

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
char g_exe_name[64] = "";
std::wstring g_dir; // folder of this DLL, with trailing backslash
HANDLE g_log = INVALID_HANDLE_VALUE;
HANDLE g_timer = nullptr;
volatile LONG g_busy = 0;
volatile LONG g_located = 0;       // the backend found the game's pool, g_state.targets is final
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
// what the backends build on

// Guarded memory access: game objects can be freed during shutdown.
template <typename T> bool read_mem(uintptr_t addr, T &out)
{
    __try
    {
        out = *reinterpret_cast<volatile T *>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T> bool write_mem(uintptr_t addr, T value)
{
    __try
    {
        *reinterpret_cast<volatile T *>(addr) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool read_f32(uintptr_t addr, float &out)
{
    return read_mem(addr, out);
}

bool write_f32(uintptr_t addr, float value)
{
    return write_mem(addr, value);
}

// The first loaded module that exports `name` (and `also`, if given). Asks the loader, so only for DllMain.
HMODULE find_module_exporting(const char *name, const char *also = nullptr)
{
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return nullptr;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < count; ++i)
        if (GetProcAddress(modules[i], name) && (!also || GetProcAddress(modules[i], also)))
            return modules[i];
    return nullptr;
}

void tooltip(const char *text)
{
    ImGui::SetItemTooltip("%s", text);
}

// Each backend provides: kHasMaxPool, the CRSF_INI_* texts, Targets, Baseline, Live, on_attach(), locate(),
// apply(), read_live(), log_stats() and draw_status().
#if defined(CRSF_BACKEND_TWEAKABLES)
#include "backend_tweakables.h"
#else
#include "backend_heap.h"
#endif

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

void log_game_version(uintptr_t base, const char *exe_name)
{
    unsigned version[4];
    if (exe_version(base, version))
        log_line("Game executable: %s %u.%u.%u.%u", exe_name, version[0], version[1], version[2], version[3]);
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
        CRSF_INI_MIN_POOL
        "MinPoolMB=2048\r\n"
        CRSF_INI_MAX_POOL_BLOCK
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
    if constexpr (kHasMaxPool)
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

// Writes the values in place; comments and anything else in the file stay.
void write_config(const Config &c)
{
    const std::wstring path = ini_path();
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%llu", static_cast<unsigned long long>(c.min_pool_mb));
    WritePrivateProfileStringW(CRSF_NAME_W, L"MinPoolMB", buf, path.c_str());
    if constexpr (kHasMaxPool)
    {
        std::swprintf(buf, 32, L"%llu", static_cast<unsigned long long>(c.max_pool_mb));
        WritePrivateProfileStringW(CRSF_NAME_W, L"MaxPoolMB", buf, path.c_str());
    }
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
    char max_pool[40] = "";
    if constexpr (kHasMaxPool)
        std::snprintf(max_pool, sizeof(max_pool), " MaxPoolMB=%llu", static_cast<unsigned long long>(c.max_pool_mb));
    log_line("%s: MinPoolMB=%llu%s BiasLimit=%.2f LogIntervalSec=%u", what,
             static_cast<unsigned long long>(c.min_pool_mb), max_pool, c.bias_limit, c.log_interval_s);
}

// ---------------------------------------------------------------------------------------
// the timer

// Runs on a thread pool thread every kTickMs. Must not take the loader lock: DllMain waits for it on unload.
VOID CALLBACK tick(PVOID, BOOLEAN)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return; // previous tick still running (the first one may scan the executable)

    State &s = g_state;
    if (!s.started)
    {
        s.started = true;
        open_log();
        log_line(CRSF_NAME " " CRSF_VERSION " (%s)",
                 !g_registered_with_reshade ? "loaded without ReShade"
                 : g_has_tab                ? "ReShade add-on, settings tab available"
                                            : "ReShade add-on, no settings tab: this ReShade lacks the ImGui 1.92.5 table");
        log_game_version(g_exe_base, g_exe_name);
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
        apply(s.targets, s.cfg, s.base, s.announce);
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

    ImGui::PushTextWrapPos(0.0f); // wrap text lines at the window edge instead of clipping them
    ImGui::SeparatorText("Right now");
    draw_status(read_live(s.targets));

    ImGui::SeparatorText("Settings");
    bool changed = false, released = false;
    char text[192];
    char format[64];

    int min_mb = static_cast<int>(ui_cfg.min_pool_mb);
    if (min_mb)
        std::snprintf(format, sizeof(format), "%%d MB");
    else
        std::snprintf(format, sizeof(format), "game value (%llu MB)",
                      static_cast<unsigned long long>(ui_base.game_min_mb()));
    if (ImGui::SliderInt("Minimum pool", &min_mb, 0, 8192, format))
    {
        ui_cfg.min_pool_mb = static_cast<uint64_t>(std::clamp((min_mb + 32) / 64 * 64, 0, 16384));
        changed = true;
    }
    released |= ImGui::IsItemDeactivatedAfterEdit();
    tooltip("Smallest texture pool. Higher is sharper. Above your free VRAM, Windows pages textures to system RAM, "
            "which can stutter. 0 leaves the game's value.");

    if constexpr (kHasMaxPool)
    {
        int max_mb = static_cast<int>(ui_cfg.max_pool_mb);
        if (max_mb)
            std::snprintf(format, sizeof(format), "%%d MB");
        else
            std::snprintf(format, sizeof(format), "game value (%llu MB)",
                          static_cast<unsigned long long>(ui_base.game_max_mb()));
        if (ImGui::SliderInt("Maximum pool", &max_mb, 0, 8192, format))
        {
            ui_cfg.max_pool_mb = static_cast<uint64_t>(std::clamp((max_mb + 32) / 64 * 64, 0, 16384));
            changed = true;
        }
        released |= ImGui::IsItemDeactivatedAfterEdit();
        tooltip("Largest texture pool. 0 leaves the game's value, which follows Texture Resolution. "
                "It is raised to the minimum if the minimum is higher.");
    }

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
    return find_module_exporting("ReShadeRegisterAddon", "ReShadeUnregisterAddon");
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
        bool our_game = _wcsicmp(exe_name, CRSF_EXE_W) == 0;
#if defined(CRSF_EXE_ALT)
        our_game = our_game || _wcsicmp(exe_name, CRSF_EXE_ALT_W) == 0;
#endif
        if (!our_game)
            return TRUE; // some other process (another game, a setup tool): stay inert
        g_exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::snprintf(g_exe_name, sizeof(g_exe_name), "%ls", exe_name);

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

        on_attach();
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

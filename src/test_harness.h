// What the offline tests share: a pass/fail counter, the add-on's files, and a stand-in for ReShade.
// The test executable exports the add-on registration functions and hands out an ImGui function table
// whose widgets are stubs driven by a small script.

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

namespace
{
int g_register_calls = 0, g_unregister_calls = 0;
uint32_t g_last_api = 0, g_max_api = 18;

std::wstring g_dir, g_addon, g_ini, g_log;
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// The add-on's files live next to the test executable. Starts from a clean slate.
void init_files()
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
}

void write_ini(const char *text)
{
    HANDLE h = CreateFileW(g_ini.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n;
    WriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &n, nullptr);
    CloseHandle(h);
}

// Replaces the ini and gives the add-on a few ticks to pick it up.
void replace_ini(const char *text)
{
    Sleep(20); // make sure the timestamp moves
    write_ini(text);
    Sleep(700);
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

int finish()
{
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
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

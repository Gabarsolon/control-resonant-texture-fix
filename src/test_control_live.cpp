// Check of the Control add-on against the game's real DLLs, without starting the game.
//
// It loads the game's rl and renderer DLLs for real, so their initialisers run and register the real
// tweakables. Then it loads the add-on next to this executable and asks the game's own getter
// (rend::TextureResource::getPoolSizeMB) whether the pool is what the add-on's floor says. Nothing else of
// the game runs: no device, no window, no streamer.
//
// The add-on only wakes up inside the game's executable, so this is built as Control_DX12.exe or
// Control_DX11.exe; the name also picks which of the game's DLLs are loaded.
//   usage: Control_DX12.exe "<game folder>"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#include "game.h"

namespace
{
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

std::string read_file(const std::wstring &path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    std::string content(GetFileSize(h, nullptr), '\0');
    DWORD n = 0;
    ReadFile(h, content.data(), static_cast<DWORD>(content.size()), &n, nullptr);
    CloseHandle(h);
    return content;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        std::printf("usage: %ls \"<game folder>\"\n", argv[0]);
        return 2;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring here = path;
    const std::wstring exe = here.substr(here.find_last_of(L'\\') + 1);
    here.resize(here.find_last_of(L'\\') + 1);
    const bool dx11 = _wcsicmp(exe.c_str(), CRSF_EXE_ALT_W) == 0;
    const wchar_t *flavour = dx11 ? L"_rmdwin7_f.dll" : L"_rmdwin10_f.dll";

    std::wstring game = argv[1];
    if (!game.empty() && game.back() != L'\\')
        game += L'\\';
    HMODULE renderer = nullptr;
    for (const wchar_t *part : {L"rl", L"renderer"})
    {
        const std::wstring dll = game + part + flavour;
        renderer = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!renderer)
        {
            std::printf("FAIL could not load %ls (error %lu)\n", dll.c_str(), GetLastError());
            return 1;
        }
    }
    using get_pool_fn = unsigned __int64(__cdecl *)();
    const auto get_pool = reinterpret_cast<get_pool_fn>(GetProcAddress(renderer, "?getPoolSizeMB@TextureResource@rend@@SA_KXZ"));
    if (!get_pool)
    {
        std::printf("FAIL the renderer has no TextureResource::getPoolSizeMB\n");
        return 1;
    }
    const unsigned long long before = get_pool();
    std::printf("     the game's %s renderer is loaded; its pool is %llu MB\n", dx11 ? "DX11" : "DX12", before);

    const std::wstring ini = here + CRSF_NAME_W L".ini", log = here + CRSF_NAME_W L".log";
    DeleteFileW(ini.c_str());
    DeleteFileW(log.c_str());
    HMODULE addon = LoadLibraryW((here + CRSF_NAME_W L".addon64").c_str());
    check(addon != nullptr, "the add-on loads");
    Sleep(1700); // its first tick

    std::string text = read_file(log);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::printf("---- " CRSF_NAME ".log\n%s----\n", text.c_str());
    check(text.find("Found the game's texture pool tweakables through rl_rmdwin") != std::string::npos,
          "it finds the tweakables through the game's own lookup");
    char expected[160];
    std::snprintf(expected, sizeof(expected),
                  dx11 ? "Game values: pool %llu MB, which the DX11 renderer never shrinks; blur limit 10.00 mips"
                       : "Game values: pool %llu MB, shrinking towards 100 MB once free VRAM is under 64 MB; blur limit 10.00 mips",
                  before);
    check(text.find(expected) != std::string::npos, "it reads the game's values where the disassembly says they are");
    const unsigned long long after = get_pool();
    std::printf("     the game now says its pool is %llu MB\n", after);
    check(after == 2048, "the game's own getter returns the add-on's 2048 MB floor");

    if (addon)
        FreeLibrary(addon);
    check(GetModuleHandleW(CRSF_NAME_W L".addon64") == nullptr, "the add-on unloads");
    std::printf("%s\n", g_failures ? "FAILED" : "ALL PASSED");
    std::fflush(stdout);
    // The game's DLLs are not meant to be unloaded or to see a normal process exit without the game.
    TerminateProcess(GetCurrentProcess(), g_failures ? 1 : 0);
    return 1;
}

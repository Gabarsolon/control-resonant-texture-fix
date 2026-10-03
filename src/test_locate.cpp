// Offline check against the real game files: map them as images and run the add-on's own lookup on them.
// Nothing is executed from the game.
#include "CRStreamingFix.cpp"

#if defined(CRSF_BACKEND_TWEAKABLES)

// Control: the add-on needs four exports of the game's DLLs and four tweakables by name. With the DLLs
// only mapped, their initialisers have not run, so the tweakables cannot be looked up; the names are
// searched for in the renderer image instead.
namespace
{
bool image_contains(HMODULE image, const char *text)
{
    const auto base = reinterpret_cast<const uint8_t *>(image);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage, len = std::strlen(text) + 1;
    MEMORY_BASIC_INFORMATION info;
    for (size_t at = 0; at < size && VirtualQuery(base + at, &info, sizeof(info)); at += info.RegionSize)
    {
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            continue;
        const auto *p = static_cast<const uint8_t *>(info.BaseAddress);
        for (size_t i = 0; i + len <= info.RegionSize; ++i)
            if (p[i] == static_cast<uint8_t>(text[0]) && std::memcmp(p + i, text, len) == 0)
                return true;
    }
    return false;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        std::printf("usage: test_locate <path to " CRSF_EXE " or " CRSF_EXE_ALT ">\n");
        return 2;
    }
    g_log = GetStdHandle(STD_OUTPUT_HANDLE);
    std::wstring dir = argv[1];
    const size_t slash = dir.find_last_of(L"\\/");
    const std::wstring exe = dir.substr(slash == std::wstring::npos ? 0 : slash + 1);
    dir.resize(slash == std::wstring::npos ? 0 : slash + 1);
    const bool dx11 = _wcsicmp(exe.c_str(), CRSF_EXE_ALT_W) == 0;
    const wchar_t *flavour = dx11 ? L"_rmdwin7_f.dll" : L"_rmdwin10_f.dll";

    if (HMODULE image = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES))
    {
        char name[64];
        std::snprintf(name, sizeof(name), "%ls", exe.c_str());
        log_game_version(reinterpret_cast<uintptr_t>(image), name);
    }
    HMODULE renderer = nullptr;
    for (const wchar_t *part : {L"rl", L"renderer", L"d3d"})
    {
        const std::wstring path = dir + part + flavour;
        HMODULE image = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!image)
        {
            std::printf("FAIL could not map %ls (error %lu)\n", path.c_str(), GetLastError());
            return 1;
        }
        if (part[1] == L'e')
            renderer = image;
    }

    on_attach();
    bool ok = g_get_tweakable && g_get_video_memory && g_missing_mips && g_heap_below_limit;
    std::printf("%s tweakable lookup %s (in %s), VRAM query %s, missing mips %s, pool room flag %s\n", ok ? "OK  " : "FAIL",
                g_get_tweakable ? "found" : "MISSING", g_tweakable_module, g_get_video_memory ? "found" : "MISSING",
                g_missing_mips ? "found" : "MISSING", g_heap_below_limit ? "found" : "MISSING");
    for (const char *name : {"Texture Streaming:Target texture pool size MB", "Texture Streaming:Min Pool Size MB",
                             "Texture Streaming:Reduce Pool Size After Free VRAM < MB", "Texture Streaming:Mip adjust [Display]"})
    {
        const bool present = image_contains(renderer, name);
        std::printf("%s the renderer registers \"%s\"\n", present ? "OK  " : "FAIL", name);
        ok = ok && present;
    }
    return ok ? 0 : 1;
}

#else

namespace
{
// Addresses and stats offsets found by hand (disassembly) in the builds the add-on was developed against.
// If the executable is one of them, the result has to match exactly.
struct KnownBuild
{
    unsigned version[4];
    uint32_t heap_ptr, mgr_ptr, bias_limit, high, low, rate;
    uint32_t heap_bytes, tiles, left_lo, left_hi;
};
#if defined(CRSF_GAME_AW2)
constexpr KnownBuild kKnown[] = {
    {{0, 559, 302, 8}, 0x3A34698, 0x397FEA8, 0x3867D50, 0x3867C78, 0x3867C98, 0x3867CD8, 0x210, 0x170, 0x1D8, 0x1E0}};
#else
constexpr KnownBuild kKnown[] = {
    {{0, 563, 737, 9}, 0x5C36B48, 0x5D2B470, 0x5D2B688, 0x5D2B508, 0x5D2B530, 0x5D2B5A0, 0x1E0, 0x140, 0x1A8, 0x1B0},
    {{0, 564, 208, 5}, 0x5D07328, 0x5E00C70, 0x5E00E18, 0x5E00DC8, 0x5E00DF0, 0x5E00E88, 0x218, 0x140, 0x1E0, 0x1E8}};
#endif
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        std::printf("usage: test_locate <path to " CRSF_EXE ">\n");
        return 2;
    }
    g_log = GetStdHandle(STD_OUTPUT_HANDLE);
    HMODULE image = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image)
    {
        std::printf("could not map the executable (error %lu)\n", GetLastError());
        return 1;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    log_game_version(base, CRSF_EXE);
    Targets t;
    bool ok = locate(base, t);
    const auto rva = [base](uintptr_t address) { return static_cast<uint32_t>(address ? address - base : 0); };
    std::printf("%s heap_ptr=+%X mgr_ptr=+%X bias_limit=+%X high=+%X low=+%X rate=+%X\n", ok ? "OK  " : "FAIL",
                rva(t.heap_ptr), rva(t.mgr_ptr), rva(t.bias_limit), rva(t.high_threshold), rva(t.low_threshold),
                rva(t.bias_rate));
    const StatsLayout &st = t.stats;
    const bool stats_ok = st.used_known && st.left_known;
    std::printf("%s stats: heap_bytes=+%X tiles=+%X (%s) left=+%X/+%X (%s)\n", stats_ok ? "OK  " : "FAIL", st.heap_bytes,
                st.tiles, st.used_known ? "found" : "NOT FOUND", st.left_lo, st.left_hi,
                st.left_known ? "found" : "NOT FOUND");
    ok = ok && stats_ok;

    unsigned version[4] = {};
    exe_version(base, version);
    bool known = false;
    for (const KnownBuild &k : kKnown)
    {
        if (std::memcmp(k.version, version, sizeof(version)) != 0)
            continue;
        known = true;
        const bool same = rva(t.heap_ptr) == k.heap_ptr && rva(t.mgr_ptr) == k.mgr_ptr &&
                          rva(t.bias_limit) == k.bias_limit && rva(t.high_threshold) == k.high &&
                          rva(t.low_threshold) == k.low && rva(t.bias_rate) == k.rate && st.heap_bytes == k.heap_bytes &&
                          st.tiles == k.tiles && st.left_lo == k.left_lo && st.left_hi == k.left_hi;
        std::printf("%s the addresses and offsets found by hand in this build\n", same ? "OK   matches" : "FAIL differs from");
        ok = ok && same;
    }
    if (!known)
        std::printf("note: not a build with known addresses, nothing to compare against\n");
    return ok ? 0 : 1;
}

#endif

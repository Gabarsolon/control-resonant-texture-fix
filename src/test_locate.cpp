// Offline check: map the game executable as an image and run the add-on's signature logic on it.
// Nothing is executed from the game; the tweakable values read as 0 because its initialisers never run.
#include "CRStreamingFix.cpp"

namespace
{
// Addresses found by hand in the builds the add-on was developed against. If the executable is one of
// them, the result has to match exactly.
struct KnownBuild
{
    unsigned version[4];
    uint32_t heap_ptr, mgr_ptr, bias_limit, high, low, rate;
};
#if defined(CRSF_GAME_AW2)
constexpr KnownBuild kKnown[] = {{{0, 559, 302, 8}, 0x3A34698, 0x397FEA8, 0x3867D50, 0x3867C78, 0x3867C98, 0x3867CD8}};
#else
constexpr KnownBuild kKnown[] = {{{0, 563, 737, 9}, 0x5C36B48, 0x5D2B470, 0x5D2B688, 0x5D2B508, 0x5D2B530, 0x5D2B5A0}};
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
    Targets t;
    bool ok = locate(base, t);
    const auto rva = [base](uintptr_t address) { return static_cast<uint32_t>(address ? address - base : 0); };
    std::printf("%s heap_ptr=+%X mgr_ptr=+%X bias_limit=+%X high=+%X low=+%X rate=+%X\n", ok ? "OK  " : "FAIL",
                rva(t.heap_ptr), rva(t.mgr_ptr), rva(t.bias_limit), rva(t.high_threshold), rva(t.low_threshold),
                rva(t.bias_rate));

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
                          rva(t.low_threshold) == k.low && rva(t.bias_rate) == k.rate;
        std::printf("%s the addresses found by hand in this build\n", same ? "OK   matches" : "FAIL differs from");
        ok = ok && same;
    }
    if (!known)
        std::printf("note: not a build with known addresses, nothing to compare against\n");
    return ok ? 0 : 1;
}

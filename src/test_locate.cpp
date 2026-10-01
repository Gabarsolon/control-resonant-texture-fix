// Offline check: map a CONTROLResonant.exe as an image and run the add-on's signature logic on it.
// Nothing is executed from the game; the tweakable values read as 0 because its initialisers never run.
#include "CRStreamingFix.cpp"

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        std::printf("usage: test_locate <path to CONTROLResonant.exe>\n");
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
    const bool ok = locate(base, t);
    std::printf("%s heap_ptr=+%llX mgr_ptr=+%llX bias_limit=+%llX high=+%llX low=+%llX rate=+%llX\n",
                ok ? "OK  " : "FAIL", static_cast<unsigned long long>(t.heap_ptr ? t.heap_ptr - base : 0),
                static_cast<unsigned long long>(t.mgr_ptr ? t.mgr_ptr - base : 0),
                static_cast<unsigned long long>(t.bias_limit ? t.bias_limit - base : 0),
                static_cast<unsigned long long>(t.high_threshold ? t.high_threshold - base : 0),
                static_cast<unsigned long long>(t.low_threshold ? t.low_threshold - base : 0),
                static_cast<unsigned long long>(t.bias_rate ? t.bias_rate - base : 0));
    return ok ? 0 : 1;
}

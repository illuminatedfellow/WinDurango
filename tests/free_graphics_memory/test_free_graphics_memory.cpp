// Fork-only verification for D3DFreeGraphicsMemory (not part of the upstream PR).
// Usage: test_free_graphics_memory.exe <dir containing d3d11_x.dll>
// Exit code 0 = all checks passed.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

using PFN_Alloc = HRESULT(__stdcall *)(SIZE_T, UINT64, UINT64, UINT, LPVOID *);
using PFN_Free = HRESULT(__stdcall *)(void *);
using PFN_VirtualQuery = SIZE_T(WINAPI *)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);

static int g_Failures = 0;
#define CHECK(cond, ...)                                                                                       \
    do                                                                                                         \
    {                                                                                                          \
        if (cond)                                                                                              \
            std::printf("PASS  " __VA_ARGS__);                                                                 \
        else                                                                                                   \
        {                                                                                                      \
            std::printf("FAIL  " __VA_ARGS__);                                                                 \
            ++g_Failures;                                                                                      \
        }                                                                                                      \
        std::printf("\n");                                                                                     \
    } while (0)

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        std::printf("usage: test_free_graphics_memory.exe <bin dir>\n");
        return 2;
    }

    std::wstring Dir = argv[1];
    SetDllDirectoryW(Dir.c_str());
    HMODULE D3D = LoadLibraryExW((Dir + L"\\d3d11_x.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!D3D)
    {
        std::printf("FAIL  LoadLibrary(d3d11_x.dll) error %lu\n", GetLastError());
        return 1;
    }

    auto Alloc = reinterpret_cast<PFN_Alloc>(GetProcAddress(D3D, "D3DAllocateGraphicsMemory"));
    auto Free = reinterpret_cast<PFN_Free>(GetProcAddress(D3D, "D3DFreeGraphicsMemory"));
    // Use the real OS VirtualQuery so the check does not depend on KernelX's view of memory.
    auto OsVirtualQuery = reinterpret_cast<PFN_VirtualQuery>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VirtualQuery"));
    if (!Alloc || !Free || !OsVirtualQuery)
    {
        std::printf("FAIL  missing export (alloc=%p free=%p)\n", (void *)Alloc, (void *)Free);
        return 1;
    }

    CHECK(Free(nullptr) == E_INVALIDARG, "Free(nullptr) returns E_INVALIDARG");

    const SIZE_T Size = 4 * 0x10000;
    for (UINT Flags = 0; Flags <= 2; ++Flags)
    {
        for (UINT64 Alignment : {UINT64(0x10000), UINT64(0x20000)})
        {
            void *p = nullptr;
            HRESULT hr = Alloc(Size, Alignment, 0, Flags, &p);
            CHECK(SUCCEEDED(hr) && p, "Alloc flags=%u align=0x%llx -> hr=0x%08lX p=%p", Flags,
                  (unsigned long long)Alignment, (unsigned long)hr, p);
            if (FAILED(hr) || !p)
                continue;

            std::memset(p, 0xAB, Size); // memory must be writable before the free

            HRESULT hrInterior = Free(static_cast<BYTE *>(p) + 0x10000);
            CHECK(FAILED(hrInterior), "Free(interior pointer) fails (hr=0x%08lX)", (unsigned long)hrInterior);
            CHECK(static_cast<volatile BYTE *>(p)[Size - 1] == 0xAB, "allocation intact after rejected free");

            hr = Free(p);
            CHECK(hr == S_OK, "Free(base) flags=%u -> hr=0x%08lX", Flags, (unsigned long)hr);

            MEMORY_BASIC_INFORMATION mbi{};
            OsVirtualQuery(p, &mbi, sizeof(mbi));
            CHECK(mbi.State == MEM_FREE, "range is MEM_FREE after free (state=0x%lX)", mbi.State);

            HRESULT hrDouble = Free(p);
            CHECK(FAILED(hrDouble), "double free fails (hr=0x%08lX)", (unsigned long)hrDouble);
        }
    }

    // Repeated alloc/free must not leak address space or start failing.
    bool LoopOk = true;
    for (int i = 0; i < 2000 && LoopOk; ++i)
    {
        void *p = nullptr;
        LoopOk = SUCCEEDED(Alloc(0x400000, 0x10000, 0, 0, &p)) && p && Free(p) == S_OK;
    }
    CHECK(LoopOk, "2000 x 4 MiB alloc/free cycles");

    std::printf("%s: %d failure(s)\n", g_Failures ? "FAILED" : "ALL PASSED", g_Failures);
    return g_Failures ? 1 : 0;
}

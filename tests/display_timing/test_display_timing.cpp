// Fork-only verification for the frame-statistics / VLine helpers (not part of the upstream PR).
// Part 1 tests DisplayTiming.h directly. Part 2 (optional, argv[1] = dir with d3d11_x.dll) calls the exports.
// Exit code 0 = all checks passed.
#include "DisplayTiming.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace wd::display;

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

static_assert(VSyncCountAt(100, 100, 10) == 0);
static_assert(VSyncCountAt(129, 100, 10) == 2);
static_assert(VSyncCountAt(50, 100, 10) == 0); // before start
static_assert(VSyncCountAt(500, 100, 0) == 0); // unknown period

static void TestFrameRing()
{
    FrameRing<4> Ring;
    CHECK(Ring.Count() == 0, "empty ring has no frames");

    for (uint64_t i = 1; i <= 3; i++)
        Ring.Push({i, i + 100, reinterpret_cast<void *>(i)});
    CHECK(Ring.Count() == 3, "ring counts pushed frames (%u)", Ring.Count());
    CHECK(Ring.Recent(0).CpuPresentCalled == 3 && Ring.Recent(2).CpuPresentCalled == 1, "Recent(0) is newest");

    for (uint64_t i = 4; i <= 10; i++)
        Ring.Push({i, i + 100, reinterpret_cast<void *>(i)});
    bool Ok = Ring.Count() == 4;
    for (uint32_t i = 0; i < 4; i++)
        Ok = Ok && Ring.Recent(i).CpuPresentCalled == 10 - i && Ring.Recent(i).Cookie == reinterpret_cast<void *>(10 - i);
    CHECK(Ok, "ring keeps only the newest 4 frames after wrapping");
}

static void TestRefreshPeriod()
{
    LARGE_INTEGER Freq;
    QueryPerformanceFrequency(&Freq);
    double Hz = static_cast<double>(Freq.QuadPart) / static_cast<double>(RefreshPeriodTicks());
    CHECK(Hz >= 20.0 && Hz <= 500.0, "refresh period is plausible (%.1f Hz)", Hz);
}

static bool WaitSignaled(HANDLE Event, DWORD Ms)
{
    return WaitForSingleObject(Event, Ms) == WAIT_OBJECT_0;
}

static void TestNotifier()
{
    VLineNotifier Notifier([] { Sleep(5); });

    CHECK(Notifier.Set(VLineNotifier::MaxCounters, nullptr) == E_INVALIDARG, "counter out of range rejected");
    CHECK(FAILED(Notifier.Set(0, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xBAD0)))), "invalid handle rejected");

    HANDLE Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(Notifier.Set(0, Event) == S_OK, "register event on counter 0");
    CHECK(WaitSignaled(Event, 1000), "event signalled after registration");
    CHECK(WaitSignaled(Event, 1000), "event signalled repeatedly");

    // The notifier owns a duplicate, so the title closing its handle must not stop or break signalling.
    HANDLE Second = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE Watch = nullptr;
    DuplicateHandle(GetCurrentProcess(), Second, GetCurrentProcess(), &Watch, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CHECK(Notifier.Set(1, Second) == S_OK, "register event on counter 1");
    CloseHandle(Second);
    CHECK(WaitSignaled(Watch, 1000), "signalled after caller closed its handle");

    CHECK(Notifier.Set(0, nullptr) == S_OK && Notifier.Set(1, nullptr) == S_OK, "unregister both counters");
    Sleep(50);
    ResetEvent(Event);
    ResetEvent(Watch);
    uint64_t Before = Notifier.SignalCount();
    Sleep(100);
    CHECK(!WaitSignaled(Event, 0) && !WaitSignaled(Watch, 0) && Notifier.SignalCount() == Before,
          "no signals after unregistering (idle thread)");

    CHECK(Notifier.Set(0, Event) == S_OK && WaitSignaled(Event, 1000), "re-register after idle resumes signalling");

    CloseHandle(Event);
    CloseHandle(Watch);
    // ~VLineNotifier must join the worker without hanging; a hang shows up as the CI step timing out.
}

static void TestVBlankWaiter()
{
    VBlankWaiter Waiter;
    LARGE_INTEGER Freq;
    QueryPerformanceFrequency(&Freq);
    uint64_t Period = RefreshPeriodTicks();

    Waiter(); // align to a v-blank
    uint64_t Start = Now();
    for (int i = 0; i < 10; i++)
        Waiter();
    double Elapsed = static_cast<double>(Now() - Start);
    double Periods = Elapsed / static_cast<double>(Period);
    CHECK(Periods >= 8.5 && Periods <= 40.0, "10 waits take about 10 refreshes (%.1f, never faster)", Periods);
}

// ---- Part 2: exported functions --------------------------------------------------------------------------------

struct DXGIX_FRAME_STATISTICS
{
    UINT64 CPUTimePresentCalled;
    UINT64 CPUTimeAddedToQueue;
    UINT QueueLengthAddedToQueue;
    UINT64 CPUTimeFrameComplete;
    UINT64 GPUTimeFrameComplete;
    UINT64 GPUCountTitleUsed;
    UINT64 GPUCountSystemUsed;
    UINT64 CPUTimeVSync;
    UINT64 GPUTimeVSync;
    UINT64 CPUTimeFlip;
    UINT64 GPUTimeFlip;
    UINT64 VSyncCount;
    float PercentScanned;
    void *Cookie[2];
};

using PFN_GetFrameStatistics = HRESULT(__stdcall *)(UINT, DXGIX_FRAME_STATISTICS *);
using PFN_SetVLineNotification = HRESULT(__stdcall *)(UINT, UINT, HANDLE);

static void TestExports(std::wstring const &Dir)
{
    SetDllDirectoryW(Dir.c_str());
    HMODULE D3D = LoadLibraryExW((Dir + L"\\d3d11_x.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!D3D)
    {
        CHECK(false, "LoadLibrary(d3d11_x.dll) error %lu", GetLastError());
        return;
    }

    auto GetStats = reinterpret_cast<PFN_GetFrameStatistics>(GetProcAddress(D3D, "DXGIXGetFrameStatistics"));
    auto SetVLine = reinterpret_cast<PFN_SetVLineNotification>(GetProcAddress(D3D, "DXGIXSetVLineNotification"));
    CHECK(GetStats && SetVLine, "exports found");
    if (!GetStats || !SetVLine)
        return;

    DXGIX_FRAME_STATISTICS Stats[2];
    memset(Stats, 0xCD, sizeof(Stats));
    HRESULT hr = GetStats(2, Stats);
    bool Zeroed = Stats[0].CPUTimePresentCalled == 0 && Stats[1].VSyncCount == 0 && Stats[1].Cookie[1] == nullptr;
    CHECK(hr == S_OK && Zeroed, "GetFrameStatistics with no presents returns S_OK and zeroed output (hr=0x%08lX)",
          static_cast<unsigned long>(hr));
    CHECK(GetStats(1, nullptr) == E_INVALIDARG, "GetFrameStatistics(1, nullptr) -> E_INVALIDARG");
    CHECK(GetStats(0, nullptr) == S_OK, "GetFrameStatistics(0, nullptr) -> S_OK");

    HANDLE Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(SetVLine(0, 100, Event) == S_OK, "SetVLineNotification registers");
    CHECK(WaitSignaled(Event, 2000), "SetVLineNotification event is signalled");
    CHECK(SetVLine(0, 100, nullptr) == S_OK, "SetVLineNotification(nullptr) unregisters");
    CHECK(SetVLine(99, 0, Event) == E_INVALIDARG, "SetVLineNotification bad counter -> E_INVALIDARG");
    CloseHandle(Event);
}

int wmain(int argc, wchar_t **argv)
{
    TestFrameRing();
    TestRefreshPeriod();
    TestNotifier();
    TestVBlankWaiter();
    if (argc >= 2)
        TestExports(argv[1]);

    std::printf("%s (%d failure(s))\n", g_Failures ? "FAILED" : "ALL PASSED", g_Failures);
    return g_Failures ? 1 : 0;
}

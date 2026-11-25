// Fork-only verification for the GPU memory-sync helpers (not part of the upstream PR).
// Tests MemorySync.h directly; no d3d11_x.dll needed. Exit code 0 = all checks passed.
#include "MemorySync.h"

#include <atomic>
#include <cstdio>
#include <thread>

using namespace wd::memsync;

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

// Compare must stay usable at compile time.
static_assert(Compare<uint32_t>(D3D11_COMPARISON_LESS, 1u, 2u));
static_assert(!Compare<uint64_t>(D3D11_COMPARISON_GREATER, 1ull, 2ull));

struct Row
{
    D3D11_COMPARISON_FUNC Func;
    char const *Name;
    bool Less, Equal, Greater; // expected result for value <, ==, > reference
};

static Row const g_Rows[] = {
    {D3D11_COMPARISON_NEVER, "NEVER", false, false, false},
    {D3D11_COMPARISON_LESS, "LESS", true, false, false},
    {D3D11_COMPARISON_EQUAL, "EQUAL", false, true, false},
    {D3D11_COMPARISON_LESS_EQUAL, "LESS_EQUAL", true, true, false},
    {D3D11_COMPARISON_GREATER, "GREATER", false, false, true},
    {D3D11_COMPARISON_NOT_EQUAL, "NOT_EQUAL", true, false, true},
    {D3D11_COMPARISON_GREATER_EQUAL, "GREATER_EQUAL", false, true, true},
    {D3D11_COMPARISON_ALWAYS, "ALWAYS", true, true, true},
};

template <typename T> static void TestCompareTable(char const *Width, T Small, T Big)
{
    for (auto const &R : g_Rows)
    {
        bool Ok = Compare<T>(R.Func, Small, Big) == R.Less && Compare<T>(R.Func, Big, Big) == R.Equal &&
                  Compare<T>(R.Func, Big, Small) == R.Greater;
        CHECK(Ok, "Compare<%s> %s", Width, R.Name);
    }
}

static void TestCompare()
{
    TestCompareTable<uint32_t>("u32", 5u, 0xFFFFFFF0u);
    TestCompareTable<uint64_t>("u64", 0xFFFFFFFFull, 0x100000000ull); // differs only above bit 31

    CHECK(Compare<uint32_t>(static_cast<D3D11_COMPARISON_FUNC>(0), 1u, 2u), "unknown func 0 treated as satisfied");
    CHECK(Compare<uint32_t>(static_cast<D3D11_COMPARISON_FUNC>(99), 1u, 2u), "unknown func 99 treated as satisfied");

    // The context masks the loaded value before comparing: (*p & Mask) <func> Ref.
    uint32_t Word = 0xABCD0012u;
    CHECK(Compare(D3D11_COMPARISON_EQUAL, Load32(&Word) & 0xFFu, 0x12u), "masked EQUAL ignores upper bits");
    CHECK(!Compare(D3D11_COMPARISON_EQUAL, Load32(&Word) & 0x00FF00FFu, 0x12u), "mask keeps bits it covers");
}

static void TestLoads()
{
    uint32_t V32 = 0xDEADBEEFu;
    uint64_t V64 = 0x0123456789ABCDEFull;
    CHECK(Load32(&V32) == 0xDEADBEEFu, "Load32 reads value");
    CHECK(Load64(&V64) == 0x0123456789ABCDEFull, "Load64 reads value");
}

static void TestWaitUntil()
{
    int Pumps = 0;
    bool Ok = WaitUntil([] { return true; }, [&] { ++Pumps; }, 1000);
    CHECK(Ok && Pumps == 0, "WaitUntil returns at once when already satisfied (pumps=%d)", Pumps);

    // Value produced by the pump, as when a background context writes it.
    Pumps = 0;
    uint32_t Value = 0;
    Ok = WaitUntil([&] { return Compare(D3D11_COMPARISON_GREATER_EQUAL, Load32(&Value), 3u); },
                   [&] { ++Pumps; ++Value; }, 1000);
    CHECK(Ok && Pumps == 3, "WaitUntil satisfied by pump after 3 calls (pumps=%d)", Pumps);

    // Value produced by another CPU thread, as when the title signals from a worker.
    std::atomic<uint64_t> Shared{0};
    std::thread Writer([&] {
        Sleep(50);
        Shared.store(42);
    });
    ULONGLONG Start = GetTickCount64();
    Ok = WaitUntil([&] { return Compare<uint64_t>(D3D11_COMPARISON_EQUAL, Shared.load(), 42ull); }, [] {}, 2000);
    ULONGLONG Elapsed = GetTickCount64() - Start;
    Writer.join();
    CHECK(Ok && Elapsed < 2000, "WaitUntil sees a write from another thread (%llu ms)", Elapsed);

    // Never satisfied: must give up instead of hanging.
    Start = GetTickCount64();
    Ok = WaitUntil([] { return false; }, [] {}, 100);
    Elapsed = GetTickCount64() - Start;
    CHECK(!Ok && Elapsed >= 90 && Elapsed < 1000, "WaitUntil times out after ~100 ms (%llu ms)", Elapsed);

    CHECK(WaitTimeoutMs > 0 && WaitTimeoutMs <= 10000, "WaitTimeoutMs is bounded (%llu ms)", WaitTimeoutMs);
}

static void TestTimestamps()
{
    uint64_t Prev = ReadTimestamp();
    bool Monotonic = true;
    for (int i = 0; i < 100000; ++i)
    {
        uint64_t Now = ReadTimestamp();
        if (Now < Prev)
            Monotonic = false;
        Prev = Now;
    }
    CHECK(Monotonic, "ReadTimestamp never goes backwards over 100k reads");

    LARGE_INTEGER Freq;
    QueryPerformanceFrequency(&Freq);
    uint64_t Before = ReadTimestamp();
    Sleep(50);
    uint64_t Ticks = ReadTimestamp() - Before;
    double Ms = 1000.0 * static_cast<double>(Ticks) / static_cast<double>(Freq.QuadPart);
    CHECK(Ms >= 40.0 && Ms < 1000.0, "ReadTimestamp is in QPC ticks (Sleep(50) measured %.1f ms)", Ms);
}

int main()
{
    TestCompare();
    TestLoads();
    TestWaitUntil();
    TestTimestamps();

    uint32_t Dummy = 0;
    LogWaitTimeout("test_memory_sync", &Dummy); // must not crash
    std::printf("PASS  LogWaitTimeout runs\n");

    std::printf("%s (%d failure(s))\n", g_Failures ? "FAILED" : "ALL PASSED", g_Failures);
    return g_Failures ? 1 : 0;
}

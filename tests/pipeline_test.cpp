// DecodePipeline: order, throughput with two threads, never blocking the capture thread.
#include "../native/pipeline.hpp"
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using clk = std::chrono::steady_clock;
using std::chrono::milliseconds;

// Each "frame" is 4 bytes holding its capture number; the fake decoder returns it after a delay.
struct Fake {
    std::vector<std::vector<uint32_t>> out;  // one result buffer per thread
    std::vector<uint32_t> published;
    explicit Fake(int threads) : out(threads, std::vector<uint32_t>(1)) {}
};
static void submitNumber(multi::DecodePipeline& p, uint32_t n) { p.submit(reinterpret_cast<const uint8_t*>(&n), sizeof(n)); }
static uint32_t numberOf(const uint8_t* data) { uint32_t n; std::memcpy(&n, data, 4); return n; }
static bool increasing(const std::vector<uint32_t>& v) {
    for (size_t i = 1; i < v.size(); ++i) if (v[i] <= v[i - 1]) return false;
    return true;
}

// Variable decode time: frames finish out of order, but are published in capture order.
static void keeps_order() {
    Fake f(2);
    {
        multi::DecodePipeline p(2,
            [&](int t, const uint8_t* d, size_t) -> const uint8_t* {
                const uint32_t n = numberOf(d);
                std::this_thread::sleep_for(milliseconds(n % 3 == 0 ? 7 : 1));
                f.out[t][0] = n;
                return reinterpret_cast<const uint8_t*>(f.out[t].data());
            },
            [&](const uint8_t* d) { f.published.push_back(numberOf(d)); });
        for (uint32_t n = 1; n <= 100; ++n) { submitNumber(p, n); std::this_thread::sleep_for(milliseconds(5)); }
        p.finish();
    }
    CHECK(f.published.size() == 100);
    CHECK(increasing(f.published));
}

// 60 frames 8 ms apart, 12 ms to decode each: one thread cannot keep up, two can.
static size_t runThroughput(int threads) {
    Fake f(threads);
    {
        multi::DecodePipeline p(threads,
            [&](int t, const uint8_t* d, size_t) -> const uint8_t* {
                std::this_thread::sleep_for(milliseconds(12));
                f.out[t][0] = numberOf(d);
                return reinterpret_cast<const uint8_t*>(f.out[t].data());
            },
            [&](const uint8_t* d) { f.published.push_back(numberOf(d)); });
        auto next = clk::now();
        for (uint32_t n = 1; n <= 60; ++n) { submitNumber(p, n); next += milliseconds(8); std::this_thread::sleep_until(next); }
        p.finish();
    }
    CHECK(increasing(f.published));
    return f.published.size();
}
static void two_threads_keep_up() {
    const size_t one = runThroughput(1), two = runThroughput(2);
    std::printf("throughput: 1 thread %zu/60, 2 threads %zu/60\n", one, two);
    CHECK(one <= 50);   // the test really overloads a single thread
    CHECK(two >= 58);
}

// Stuck decoders never block submit(): older waiting frames are replaced and counted.
static void never_blocks() {
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Fake f(2);
    uint64_t dropped = 0;
    double ms = 0;
    {
        multi::DecodePipeline p(2,
            [&](int t, const uint8_t* d, size_t) -> const uint8_t* {
                WaitForSingleObject(release, INFINITE);
                f.out[t][0] = numberOf(d);
                return reinterpret_cast<const uint8_t*>(f.out[t].data());
            },
            [&](const uint8_t* d) { f.published.push_back(numberOf(d)); });
        // Frames 1 and 2 occupy both threads; then 98 more arrive at once.
        submitNumber(p, 1); std::this_thread::sleep_for(milliseconds(20));
        submitNumber(p, 2); std::this_thread::sleep_for(milliseconds(20));
        const auto start = clk::now();
        for (uint32_t n = 3; n <= 100; ++n) submitNumber(p, n);
        ms = std::chrono::duration<double, std::milli>(clk::now() - start).count();
        dropped = p.dropped();
        SetEvent(release);
        p.finish();
    }
    CloseHandle(release);
    CHECK(ms < 50);
    CHECK(dropped == 97);                 // 2 in the decoders, the newest one waiting
    CHECK(f.published.size() == 3);
    CHECK(increasing(f.published));
    CHECK(!f.published.empty() && f.published.back() == 100);
}

// A frame that fails to decode is skipped without stalling the frames after it.
static void failure_does_not_stall() {
    Fake f(2);
    {
        multi::DecodePipeline p(2,
            [&](int t, const uint8_t* d, size_t) -> const uint8_t* {
                const uint32_t n = numberOf(d);
                if (n % 2) return nullptr;
                f.out[t][0] = n;
                return reinterpret_cast<const uint8_t*>(f.out[t].data());
            },
            [&](const uint8_t* d) { f.published.push_back(numberOf(d)); });
        for (uint32_t n = 1; n <= 40; ++n) { submitNumber(p, n); std::this_thread::sleep_for(milliseconds(2)); }
        p.finish();
    }
    CHECK(f.published.size() == 20);
    CHECK(increasing(f.published));
}

int main() {
    timeBeginPeriod(1);
    keeps_order(); two_threads_keep_up(); never_blocks(); failure_does_not_stall();
    timeEndPeriod(1);
    if (failures == 0) std::printf("PASS\n");
    return failures;
}

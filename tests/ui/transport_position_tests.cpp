#include "transport_position.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int gFailures = 0;

void Check(bool condition, const char* testName, const char* desc,
           int line) {
    if (!condition) {
        ++gFailures;
        fprintf(stderr, "FAIL: %s (%s) at line %d\n", testName, desc, line);
    } else {
        printf("PASS: %s (%s)\n", testName, desc);
    }
}

#define CHECK(cond, desc) Check((cond), __func__, (desc), __LINE__)

using kessoku::ui::BarToFrame;
using kessoku::ui::FormatTrackTime;
using kessoku::ui::PositionToBar;

// 10 min @ 44.1 kHz, 10 min @ 192 kHz, and the uint32_t ceiling.
constexpr uint32_t kTotal441 = 26460000;
constexpr uint32_t kTotal192 = 115200000;
constexpr uint32_t kTotalMax = 0xFFFFFFFFu;

void TestEndpoints() {
    CHECK(PositionToBar(0, kTotal441) == 0, "pos 0 -> bar 0 (44.1k)");
    CHECK(PositionToBar(kTotal441 / 2, kTotal441) == 500,
          "pos half -> bar 500 (44.1k)");
    CHECK(PositionToBar(kTotal441, kTotal441) == 1000,
          "pos total -> bar 1000 (44.1k)");

    CHECK(PositionToBar(0, kTotal192) == 0, "pos 0 -> bar 0 (192k)");
    CHECK(PositionToBar(kTotal192 / 2, kTotal192) == 500,
          "pos half -> bar 500 (192k)");
    CHECK(PositionToBar(kTotal192, kTotal192) == 1000,
          "pos total -> bar 1000 (192k)");

    CHECK(PositionToBar(0, kTotalMax) == 0, "pos 0 -> bar 0 (max)");
    // Integer floor, not a bug: (2^31-1)*1000/2^32-1 = 499.999... -> 499.
    // BarToFrame(500) still lands past the midpoint (see round-trip below).
    CHECK(PositionToBar(kTotalMax / 2, kTotalMax) == 499,
          "pos half -> bar 499 by floor (max)");
    CHECK(PositionToBar(kTotalMax, kTotalMax) == 1000,
          "pos total -> bar 1000 (max)");

    CHECK(BarToFrame(0, kTotal441) == 0, "bar 0 -> frame 0 (44.1k)");
    CHECK(BarToFrame(1000, kTotal441) == kTotal441,
          "bar 1000 -> frame == total exactly (44.1k)");
    CHECK(BarToFrame(0, kTotal192) == 0, "bar 0 -> frame 0 (192k)");
    CHECK(BarToFrame(1000, kTotal192) == kTotal192,
          "bar 1000 -> frame == total exactly (192k)");
    CHECK(BarToFrame(0, kTotalMax) == 0, "bar 0 -> frame 0 (max)");
    CHECK(BarToFrame(1000, kTotalMax) == kTotalMax,
          "bar 1000 -> frame == total exactly, never above (max)");
}

void TestZeroTotal() {
    // Zero-length file: no division may execute; both maps return 0.
    CHECK(PositionToBar(0, 0) == 0, "pos 0, total 0 -> bar 0");
    CHECK(PositionToBar(12345, 0) == 0, "pos 12345, total 0 -> bar 0");
    CHECK(PositionToBar(0xFFFFFFFFu, 0) == 0, "pos max, total 0 -> bar 0");
    CHECK(BarToFrame(0, 0) == 0, "bar 0, total 0 -> frame 0");
    CHECK(BarToFrame(500, 0) == 0, "bar 500, total 0 -> frame 0");
    CHECK(BarToFrame(1000, 0) == 0, "bar 1000, total 0 -> frame 0");
    CHECK(BarToFrame(-50, 0) == 0, "bar -50, total 0 -> frame 0");
}

void TestClamp() {
    CHECK(BarToFrame(-1, kTotal441) == 0, "negative bar clamps to frame 0");
    CHECK(BarToFrame(-1000000, kTotal441) == 0,
          "large negative bar clamps to frame 0");
    CHECK(BarToFrame(1001, kTotal441) == kTotal441,
          "bar past scale clamps to total");
    CHECK(BarToFrame(1000000, kTotalMax) == kTotalMax,
          "large bar clamps to total, never above");
}

void TestMonotonic() {
    const uint32_t totals[] = {kTotal441, kTotal192, kTotalMax};
    for (uint32_t total : totals) {
        uint32_t prevFrame = 0;
        for (int bar = 0; bar <= 1000; ++bar) {
            uint32_t frame = BarToFrame(bar, total);
            if (frame < prevFrame) {
                CHECK(false, "bar->frame monotonic");
                return;
            }
            if (frame > total) {
                CHECK(false, "bar->frame never exceeds total");
                return;
            }
            prevFrame = frame;
        }
        CHECK(true, "bar->frame monotonic and bounded");

        int prevBar = 0;
        constexpr uint32_t kSteps = 2000;
        for (uint32_t s = 0; s <= kSteps; ++s) {
            uint32_t pos = static_cast<uint32_t>(
                (static_cast<uint64_t>(s) * total) / kSteps);
            int bar = PositionToBar(pos, total);
            if (bar < prevBar || bar > 1000) {
                CHECK(false, "pos->bar monotonic and bounded");
                return;
            }
            prevBar = bar;
        }
        CHECK(true, "pos->bar monotonic and bounded");
    }
}

void TestRoundTrip() {
    // bar -> frame -> bar stays within one step of the starting bar:
    // the mappings are inverse up to integer flooring.
    const uint32_t totals[] = {kTotal441, kTotal192, kTotalMax};
    for (uint32_t total : totals) {
        for (int bar = 0; bar <= 1000; ++bar) {
            int back = PositionToBar(BarToFrame(bar, total), total);
            if (back < bar - 1 || back > bar) {
                CHECK(false, "bar->frame->bar round-trips within one");
                return;
            }
        }
        CHECK(true, "bar->frame->bar round-trips within one");
    }
}

void TestFormatTime() {
    CHECK(FormatTrackTime(0, 44100) == L"0:00", "0 frames -> 0:00");
    CHECK(FormatTrackTime(44100, 44100) == L"0:01", "1 second -> 0:01");
    CHECK(FormatTrackTime(59 * 44100, 44100) == L"0:59", "59 seconds -> 0:59");
    CHECK(FormatTrackTime(61 * 44100, 44100) == L"1:01", "61 seconds -> 1:01");
    CHECK(FormatTrackTime(26460000, 44100) == L"10:00",
          "10 min @ 44.1k -> 10:00");
    CHECK(FormatTrackTime(115200000, 192000) == L"10:00",
          "10 min @ 192k -> 10:00");
    CHECK(FormatTrackTime(158715900u, 44100) == L"59:59",
          "3599 seconds -> 59:59");
    CHECK(FormatTrackTime(12345, 0) == L"0:00", "rate 0 -> 0:00, no divide");
}

} // namespace

int main() {
    std::wprintf(L"=== Transport Position Tests ===\n\n");

    TestEndpoints();
    TestZeroTotal();
    TestClamp();
    TestMonotonic();
    TestRoundTrip();
    TestFormatTime();

    std::wprintf(L"\n=== Results: %d failures ===\n", gFailures);
    return gFailures;
}

#pragma once

#include <cstdint>
#include <string>

// Pure position/seek/time helpers for the playback transport UI.
//
// These expressions live here (instead of inline in the window class) so
// they are unit-testable without a live window. The arithmetic intentionally
// uses 64-bit intermediates throughout: pos * kSeekScale overflows 32 bits
// past 4,294,967 frames (~97 s at 44.1 kHz), and totalFrames is uint32_t.
namespace kessoku::ui {

// Seek bar granularity: the bar spans 0..kSeekScale and maps linearly onto
// 0..totalFrames. Keeps the control in int range no matter how long the
// track is (totalFrames is uint32_t and can exceed INT_MAX in theory).
inline constexpr int kSeekScale = 1000;

// Maps a playback position (frames) in [0, total] onto the seek bar range
// [0, kSeekScale]. Returns 0 when total == 0 (no division performed).
inline int PositionToBar(uint32_t position, uint32_t total)
{
    if (total == 0) {
        return 0;
    }
    return static_cast<int>(
        (static_cast<uint64_t>(position) * kSeekScale) / total);
}

// Maps a seek bar position onto a frame in [0, total]. Out-of-range bar
// positions are clamped. bar == kSeekScale yields exactly total, never above.
// Returns 0 when total == 0 (no division performed).
inline uint32_t BarToFrame(int barPos, uint32_t total)
{
    if (total == 0) {
        return 0;
    }
    if (barPos < 0) {
        barPos = 0;
    }
    if (barPos > kSeekScale) {
        barPos = kSeekScale;
    }
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(barPos) * total) /
        static_cast<uint64_t>(kSeekScale));
}

// Formats frames at sampleRate as m:ss. sampleRate == 0 yields 0:00
// (no division performed).
inline std::wstring FormatTrackTime(uint32_t frames, uint32_t sampleRate)
{
    uint64_t totalSeconds = (sampleRate != 0) ? (frames / sampleRate) : 0;
    uint64_t minutes = totalSeconds / 60;
    uint64_t seconds = totalSeconds % 60;
    std::wstring text = std::to_wstring(minutes) + L":";
    if (seconds < 10) {
        text += L"0";
    }
    text += std::to_wstring(seconds);
    return text;
}

} // namespace kessoku::ui

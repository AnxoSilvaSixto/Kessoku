#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kessoku::audio {

struct WavFormat {
    uint32_t sampleRate;
    uint16_t bitsPerSample;
    uint16_t channelCount;
    uint32_t byteRate;
    uint16_t blockAlign;
};

// Parse the 'fmt ' chunk from a WAV file and return its PCM format.
// Returns std::string with error message on failure (empty on success).
std::string ParseWavFormat(const std::wstring& path, WavFormat& out);

} // namespace kessoku::audio

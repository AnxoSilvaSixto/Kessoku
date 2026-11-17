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

struct WavFileInfo {
    WavFormat format;
    uint64_t dataOffset; // absolute offset of the first data byte
    uint64_t dataSize;   // declared size of the data chunk
};

// Result of the full structural validation used before a WAV file is listed
// or played. The split matters for error reporting: Malformed means the file
// is not a usable PCM WAV container, Unsupported means it is a valid PCM WAV
// whose layout has no exact exclusive-mode container (bit depth, channel
// count or block alignment).
enum class WavValidation {
    Ok,
    Malformed,
    Unsupported
};

// Parse the 'fmt ' chunk from a WAV file and return its PCM format.
// Returns std::string with error message on failure (empty on success).
// Stops at the first valid fmt chunk; use ParseWavFile when the data chunk
// and its range must also be validated.
std::string ParseWavFormat(const std::wstring& path, WavFormat& out);

// Validate a complete WAV file: RIFF/WAVE signature, a PCM fmt chunk with
// non-zero fields, consistent block alignment, a data chunk, and a declared
// data range that fits inside the file. Also rejects valid PCM layouts the
// player cannot map to an exact exclusive-mode container. This is the single
// validator shared by the library metadata reader (so unplayable files never
// reach the list) and Player::Create (so the two cannot disagree).
// On failure `error` explains the rejection; both failure kinds set it.
WavValidation ParseWavFile(const std::wstring& path, WavFileInfo& out,
                           std::string& error);

} // namespace kessoku::audio

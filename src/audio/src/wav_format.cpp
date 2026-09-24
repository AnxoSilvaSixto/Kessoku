#include "kessoku/audio/wav_format.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace kessoku::audio {

namespace {

#pragma pack(push, 1)
struct RiffHeader {
    char riff[4];
    uint32_t fileSize;
    char wave[4];
};

struct ChunkHeader {
    char id[4];
    uint32_t size;
};

struct FmtChunkHeader {
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

} // namespace

std::string ParseWavFormat(const std::wstring& path, WavFormat& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        int required = WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                           static_cast<int>(path.size()),
                                           nullptr, 0, nullptr, nullptr);
        std::string utf8Path(static_cast<size_t>(required), '\0');
        if (required > 0) {
            WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                static_cast<int>(path.size()),
                                utf8Path.data(), required, nullptr, nullptr);
        }
        return "Could not open WAV file: " + utf8Path;
    }

    RiffHeader riffHeader;
    if (!file.read(reinterpret_cast<char*>(&riffHeader), sizeof(riffHeader))) {
        return "Invalid WAV file: cannot read RIFF header";
    }

    if (std::memcmp(riffHeader.riff, "RIFF", 4) != 0 ||
        std::memcmp(riffHeader.wave, "WAVE", 4) != 0) {
        return "Not a valid WAV file: missing RIFF/WAVE signature";
    }

    // Read chunks looking for 'fmt '
    while (file.good()) {
        ChunkHeader chunk;
        if (!file.read(reinterpret_cast<char*>(&chunk), sizeof(chunk))) {
            return "Invalid WAV file: cannot read chunk header";
        }

        if (chunk.size > 1024 * 1024) {
            return "Invalid WAV file: chunk size exceeds 1MB";
        }

        if (std::memcmp(chunk.id, "fmt ", 4) == 0) {
            std::vector<uint8_t> fmtData(chunk.size);
            if (!file.read(reinterpret_cast<char*>(fmtData.data()), chunk.size)) {
                return "Invalid WAV file: cannot read fmt chunk";
            }

            if (chunk.size < sizeof(FmtChunkHeader)) {
                return "Invalid WAV file: fmt chunk too small";
            }

            const FmtChunkHeader* fmt = reinterpret_cast<const FmtChunkHeader*>(fmtData.data());

            // Only support PCM (audioFormat == 1)
            if (fmt->audioFormat != 1) {
                char buf[32];
                int len = snprintf(buf, sizeof(buf),
                    "WAV file uses non-PCM format (0x%x); only PCM is supported",
                    fmt->audioFormat);
                return std::string(buf, len > 0 ? len : 0);
            }

            out.sampleRate = fmt->sampleRate;
            out.bitsPerSample = fmt->bitsPerSample;
            out.channelCount = fmt->numChannels;
            out.byteRate = fmt->byteRate;
            out.blockAlign = fmt->blockAlign;
            return {}; // success
        }

        // Skip this chunk
        if (!file.seekg(chunk.size, std::ios::cur)) {
            return "Invalid WAV file: cannot skip chunk";
        }
    }

    return "Invalid WAV file: fmt chunk not found";
}

} // namespace kessoku::audio

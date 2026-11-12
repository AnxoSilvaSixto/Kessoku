#include "kessoku/audio/wav_format.h"

#include "kessoku/core/utf8.h"

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
        return "Could not open WAV file: " + core::WideToUtf8(path);
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

        if (std::memcmp(chunk.id, "fmt ", 4) == 0) {
            // Only the fmt payload is read into memory, so the allocation
            // sanity cap applies here and nowhere else. Non-fmt chunks are
            // skipped by seek and may legitimately exceed 1MB (LIST, id3,
            // JUNK) before fmt.
            if (chunk.size > 1024 * 1024) {
                return "Invalid WAV file: fmt chunk exceeds 1MB";
            }

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
                // snprintf returns the length it *would* have written. Cap it
                // at what actually fit in the buffer before constructing the
                // string, otherwise the message reads past buf.
                char buf[128];
                int len = snprintf(buf, sizeof(buf),
                    "WAV file uses non-PCM format (0x%x); only PCM is supported",
                    fmt->audioFormat);
                if (len < 0) {
                    return "WAV file uses non-PCM format; only PCM is supported";
                }
                const size_t written =
                    static_cast<size_t>(len) < sizeof(buf)
                        ? static_cast<size_t>(len)
                        : sizeof(buf) - 1;
                return std::string(buf, written);
            }

            out.sampleRate = fmt->sampleRate;
            out.bitsPerSample = fmt->bitsPerSample;
            out.channelCount = fmt->numChannels;
            out.byteRate = fmt->byteRate;
            out.blockAlign = fmt->blockAlign;

            if (out.sampleRate == 0 || out.channelCount == 0 ||
                out.bitsPerSample == 0) {
                return "Invalid WAV file: zero PCM format field";
            }
            return {}; // success
        }

        // Skip this chunk. RIFF chunks are word-aligned: an odd payload is
        // followed by one pad byte that the chunk size does not count.
        const std::streamoff skip =
            static_cast<std::streamoff>(chunk.size) +
            static_cast<std::streamoff>(chunk.size % 2);
        if (!file.seekg(skip, std::ios::cur)) {
            return "Invalid WAV file: cannot skip chunk";
        }
    }

    return "Invalid WAV file: fmt chunk not found";
}

} // namespace kessoku::audio

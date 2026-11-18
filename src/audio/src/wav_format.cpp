#include "kessoku/audio/wav_format.h"

#include "kessoku/core/utf8.h"

#include <cstdio>
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

std::string NonPcmMessage(uint16_t audioFormat) {
    // snprintf returns the length it *would* have written. Cap it at what
    // actually fit in the buffer before constructing the string, otherwise
    // the message reads past buf.
    char buf[128];
    int len = snprintf(buf, sizeof(buf),
        "WAV file uses non-PCM format (0x%x); only PCM is supported",
        audioFormat);
    if (len < 0) {
        return "WAV file uses non-PCM format; only PCM is supported";
    }
    const size_t written = static_cast<size_t>(len) < sizeof(buf)
                               ? static_cast<size_t>(len)
                               : sizeof(buf) - 1;
    return std::string(buf, written);
}

struct ParsedWav {
    WavFormat format{};
    uint64_t dataOffset = 0;
    uint64_t dataSize = 0;
    bool hasFormat = false;
    bool hasData = false;
};

// Walk the RIFF chunk list starting at byte 12. With requireData=false the
// walk stops at the first valid fmt chunk, which is ParseWavFormat's
// historical contract. With requireData=true it continues until the data
// chunk is found so both headers and the declared payload range can be
// checked against the actual file size in one pass.
std::string ParseWavChunks(const std::wstring& path, bool requireData,
                           ParsedWav& out) {
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

    // The RIFF size field is routinely wrong in the wild, so it is not
    // trusted. The actual stream length is what every chunk range is checked
    // against instead.
    file.seekg(0, std::ios::end);
    const std::streamoff fileEnd = static_cast<std::streamoff>(file.tellg());
    if (fileEnd < static_cast<std::streamoff>(sizeof(RiffHeader))) {
        return "Invalid WAV file: cannot determine file size";
    }
    file.seekg(static_cast<std::streamoff>(sizeof(RiffHeader)),
               std::ios::beg);

    while (true) {
        const std::streamoff chunkStart =
            static_cast<std::streamoff>(file.tellg());
        if (chunkStart < 0) {
            return "Invalid WAV file: cannot locate chunk";
        }
        if (chunkStart == fileEnd) {
            break; // clean end of file between chunks
        }

        ChunkHeader chunk;
        if (!file.read(reinterpret_cast<char*>(&chunk), sizeof(chunk))) {
            return "Invalid WAV file: cannot read chunk header";
        }

        const uint64_t payloadStart =
            static_cast<uint64_t>(chunkStart) + sizeof(ChunkHeader);
        const uint64_t payloadEnd =
            payloadStart + static_cast<uint64_t>(chunk.size);
        if (payloadEnd > static_cast<uint64_t>(fileEnd)) {
            return "Invalid WAV file: chunk extends beyond end of file";
        }

        if (std::memcmp(chunk.id, "fmt ", 4) == 0) {
            // Only the fmt payload is read into memory, so the allocation
            // sanity cap applies here and nowhere else. Non-fmt chunks are
            // skipped by seek and may legitimately exceed 1MB (LIST, id3,
            // JUNK) before fmt.
            if (chunk.size > 1024 * 1024) {
                return "Invalid WAV file: fmt chunk exceeds 1MB";
            }
            if (chunk.size < sizeof(FmtChunkHeader)) {
                return "Invalid WAV file: fmt chunk too small";
            }

            FmtChunkHeader fmt{};
            if (!file.read(reinterpret_cast<char*>(&fmt), sizeof(fmt))) {
                return "Invalid WAV file: cannot read fmt chunk";
            }

            // Only support PCM (audioFormat == 1). An exact-container player
            // has no conversion path for IEEE float, WAVE_FORMAT_EXTENSIBLE
            // or any other coding.
            if (fmt.audioFormat != 1) {
                return NonPcmMessage(fmt.audioFormat);
            }
            if (fmt.sampleRate == 0 || fmt.numChannels == 0 ||
                fmt.bitsPerSample == 0) {
                return "Invalid WAV file: zero PCM format field";
            }

            out.format.sampleRate = fmt.sampleRate;
            out.format.bitsPerSample = fmt.bitsPerSample;
            out.format.channelCount = fmt.numChannels;
            out.format.byteRate = fmt.byteRate;
            out.format.blockAlign = fmt.blockAlign;
            out.hasFormat = true;

            if (!requireData || out.hasData) {
                return {};
            }
        } else if (std::memcmp(chunk.id, "data", 4) == 0) {
            out.dataOffset = payloadStart;
            out.dataSize = chunk.size;
            out.hasData = true;
            if (out.hasFormat) {
                return {};
            }
        }

        // Advance to the next chunk: payload plus the pad byte RIFF requires
        // after an odd-sized payload. Both the payload and the pad must lie
        // inside the actual file.
        uint64_t next = payloadEnd;
        if ((chunk.size & 1u) != 0) {
            ++next;
        }
        if (next > static_cast<uint64_t>(fileEnd)) {
            return "Invalid WAV file: chunk extends beyond end of file";
        }
        file.clear();
        file.seekg(static_cast<std::streamoff>(next), std::ios::beg);
        if (!file) {
            return "Invalid WAV file: cannot skip chunk";
        }
    }

    if (!out.hasFormat) {
        return "Invalid WAV file: fmt chunk not found";
    }
    if (requireData && !out.hasData) {
        return "Invalid WAV file: data chunk not found";
    }
    return {};
}

} // namespace

std::string ParseWavFormat(const std::wstring& path, WavFormat& out) {
    ParsedWav parsed;
    std::string error = ParseWavChunks(path, false, parsed);
    if (!error.empty()) {
        return error;
    }
    out = parsed.format;
    return {};
}

uint8_t PcmSilenceByte(uint16_t bitsPerSample) {
    return bitsPerSample == 8 ? 0x80 : 0x00;
}

WavValidation ParseWavFile(const std::wstring& path, WavFileInfo& out,
                           std::string& error) {
    ParsedWav parsed;
    error = ParseWavChunks(path, true, parsed);
    if (!error.empty()) {
        return WavValidation::Malformed;
    }

    out.format = parsed.format;
    out.dataOffset = parsed.dataOffset;
    out.dataSize = parsed.dataSize;

    // Exact PCM wire containers only. Anything else would need bit-depth
    // conversion, which the bit-perfect requirement forbids.
    const uint16_t bits = parsed.format.bitsPerSample;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        error = "WAV bit depth has no exact exclusive-mode PCM container";
        return WavValidation::Unsupported;
    }
    if (parsed.format.channelCount == 0 || parsed.format.channelCount > 8) {
        error = "WAV channel count not supported in exclusive mode";
        return WavValidation::Unsupported;
    }

    // PCM block alignment must match the channel/bit-depth layout used for
    // playback. A mismatch means the declared sample stride is not
    // channels * bytes-per-sample; reading at the derived stride would play
    // misaligned samples.
    const uint16_t expectedBlockAlign = static_cast<uint16_t>(
        parsed.format.channelCount * (parsed.format.bitsPerSample / 8));
    if (parsed.format.blockAlign != expectedBlockAlign) {
        error = "WAV block alignment does not match its channel and bit "
                "depth";
        return WavValidation::Unsupported;
    }

    return WavValidation::Ok;
}

} // namespace kessoku::audio

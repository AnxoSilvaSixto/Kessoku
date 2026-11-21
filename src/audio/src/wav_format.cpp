#include "kessoku/audio/wav_format.h"

#include "kessoku/core/utf8.h"

#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

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

bool ReadAt(HANDLE file, uint64_t offset, void* buffer, DWORD bytes) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        return false;
    }

    DWORD read = 0;
    return ReadFile(file, buffer, bytes, &read, nullptr) != FALSE &&
           read == bytes;
}

bool FileSize(HANDLE file, uint64_t& out) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        return false;
    }
    out = static_cast<uint64_t>(size.QuadPart);
    return true;
}

// Walk the RIFF chunk list starting at byte 12. With requireData=false the
// walk stops at the first valid fmt chunk, which ParseWavFormat's
// historical contract. With requireData=true it continues until the data
// chunk is found so both headers and the declared payload range can be
// checked against the actual file size in one pass.
//
// The caller owns `file`; this function only seeks and reads. Reading from
// the already-validated handle is what binds the containment decision to the
// bytes that are parsed.
std::string ParseWavChunks(HANDLE file, bool requireData, ParsedWav& out) {
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        return "Could not open WAV file";
    }

    uint64_t fileEnd = 0;
    if (!FileSize(file, fileEnd)) {
        return "Invalid WAV file: cannot determine file size";
    }

    RiffHeader riffHeader;
    if (!ReadAt(file, 0, &riffHeader, sizeof(riffHeader))) {
        return "Invalid WAV file: cannot read RIFF header";
    }

    if (std::memcmp(riffHeader.riff, "RIFF", 4) != 0 ||
        std::memcmp(riffHeader.wave, "WAVE", 4) != 0) {
        return "Not a valid WAV file: missing RIFF/WAVE signature";
    }

    // The RIFF size field is routinely wrong in the wild, so it is not
    // trusted. The actual stream length is what every chunk range is checked
    // against instead.
    uint64_t cursor = sizeof(RiffHeader);
    while (true) {
        if (cursor == fileEnd) {
            break; // clean end of file between chunks
        }
        if (cursor > fileEnd) {
            return "Invalid WAV file: cannot locate chunk";
        }

        ChunkHeader chunk;
        if (!ReadAt(file, cursor, &chunk, sizeof(chunk))) {
            return "Invalid WAV file: cannot read chunk header";
        }

        const uint64_t payloadStart = cursor + sizeof(ChunkHeader);
        const uint64_t payloadEnd =
            payloadStart + static_cast<uint64_t>(chunk.size);
        if (payloadEnd > fileEnd) {
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
            if (!ReadAt(file, payloadStart, &fmt, sizeof(fmt))) {
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
        if (next > fileEnd) {
            return "Invalid WAV file: chunk extends beyond end of file";
        }
        cursor = next;
    }

    if (!out.hasFormat) {
        return "Invalid WAV file: fmt chunk not found";
    }
    if (requireData && !out.hasData) {
        return "Invalid WAV file: data chunk not found";
    }
    return {};
}

HANDLE OpenWavPath(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

std::string ParseWavFormat(void* handle, WavFormat& out) {
    ParsedWav parsed;
    std::string error =
        ParseWavChunks(static_cast<HANDLE>(handle), false, parsed);
    if (!error.empty()) {
        return error;
    }
    out = parsed.format;
    return {};
}

std::string ParseWavFormat(const std::wstring& path, WavFormat& out) {
    HANDLE file = OpenWavPath(path);
    if (file == INVALID_HANDLE_VALUE) {
        return "Could not open WAV file: " + core::WideToUtf8(path);
    }

    const std::string error = ParseWavFormat(static_cast<void*>(file), out);
    CloseHandle(file);
    return error;
}

uint8_t PcmSilenceByte(uint16_t bitsPerSample) {
    return bitsPerSample == 8 ? 0x80 : 0x00;
}

WavValidation ParseWavFile(void* handle, WavFileInfo& out,
                           std::string& error) {
    ParsedWav parsed;
    error = ParseWavChunks(static_cast<HANDLE>(handle), true, parsed);
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

WavValidation ParseWavFile(const std::wstring& path, WavFileInfo& out,
                           std::string& error) {
    HANDLE file = OpenWavPath(path);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Could not open WAV file: " + core::WideToUtf8(path);
        return WavValidation::Malformed;
    }

    const WavValidation result =
        ParseWavFile(static_cast<void*>(file), out, error);
    CloseHandle(file);
    return result;
}

} // namespace kessoku::audio

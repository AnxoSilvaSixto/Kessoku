#include "kessoku/audio/flac_format.h"

#include "kessoku/core/utf8.h"

#define NOMINMAX
#include <windows.h>

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cstring>

namespace kessoku::audio {

namespace {

constexpr size_t kId3v2HeaderSize = 10;

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

uint32_t ReadU24BE(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 16) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           static_cast<uint32_t>(bytes[2]);
}

uint64_t ReadU64BE(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

// Read STREAMINFO from the open handle instead of asking libFLAC to open the
// path. The file marker may sit after an ID3v2 tag; that offset is returned in
// FlacFormat::streamOffset so the decoder can start on the same bytes.
std::string ParseFlacStreamInfo(HANDLE file, FlacFormat& out) {
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        return "Could not open FLAC file";
    }

    uint64_t fileSize = 0;
    if (!FileSize(file, fileSize)) {
        return "Could not determine FLAC file size";
    }

    uint64_t streamOffset = 0;
    uint8_t marker[4]{};
    if (!ReadAt(file, 0, marker, sizeof(marker))) {
        return "Could not read FLAC stream marker";
    }

    if (std::memcmp(marker, "fLaC", 4) != 0) {
        if (std::memcmp(marker, "ID3", 3) != 0) {
            return "Not a valid FLAC file: missing fLaC marker";
        }

        uint8_t header[kId3v2HeaderSize]{};
        if (!ReadAt(file, 0, header, sizeof(header))) {
            return "Could not read ID3v2 tag header";
        }
        for (size_t i = 6; i < kId3v2HeaderSize; ++i) {
            if ((header[i] & 0x80u) != 0) {
                return "Invalid ID3v2 tag length";
            }
        }
        const uint32_t tagSize =
            (static_cast<uint32_t>(header[6]) << 21) |
            (static_cast<uint32_t>(header[7]) << 14) |
            (static_cast<uint32_t>(header[8]) << 7) |
            static_cast<uint32_t>(header[9]);
        // ID3v2.4 allows a 10-byte footer when flag 0x10 is set.
        streamOffset = kId3v2HeaderSize + tagSize +
                       ((header[5] & 0x10u) != 0 ? 10u : 0u);

        if (!ReadAt(file, streamOffset, marker, sizeof(marker)) ||
            std::memcmp(marker, "fLaC", 4) != 0) {
            return "Not a valid FLAC file: missing fLaC marker";
        }
    }

    uint8_t blockHeader[4]{};
    if (!ReadAt(file, streamOffset + 4, blockHeader, sizeof(blockHeader))) {
        return "Could not read FLAC STREAMINFO header";
    }
    const uint8_t blockType = blockHeader[0] & 0x7Fu;
    const uint32_t blockLength = ReadU24BE(blockHeader + 1);
    if (blockType != 0) {
        return "FLAC first metadata block is not STREAMINFO";
    }
    if (blockLength != 34) {
        return "FLAC STREAMINFO block has an invalid length";
    }

    uint8_t payload[34]{};
    if (!ReadAt(file, streamOffset + 8, payload, sizeof(payload))) {
        return "Could not read FLAC STREAMINFO block";
    }

    // Payload after the 10-byte block/sample-size fields:
    //   20 bits sample rate, 3 bits channels-1, 5 bits bits-per-sample-1,
    //   36 bits total samples.
    const uint64_t packed = ReadU64BE(payload + 10);
    out.sampleRate = static_cast<uint32_t>(packed >> 44);
    out.channelCount = static_cast<uint32_t>((packed >> 41) & 0x7u) + 1;
    out.bitsPerSample = static_cast<uint32_t>((packed >> 36) & 0x1Fu) + 1;
    out.totalSamples = packed & 0xFFFFFFFFFULL;
    out.streamOffset = streamOffset;

    if (out.sampleRate == 0 || out.channelCount == 0 ||
        out.channelCount > 8 || out.bitsPerSample < 4 ||
        out.bitsPerSample > 32) {
        return "Invalid FLAC STREAMINFO block (field out of range)";
    }
    return {};
}

FLAC__StreamDecoderWriteStatus FlacWriteCallback(
    const FLAC__StreamDecoder* /*decoder*/, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* clientData) {
    auto* self = static_cast<FlacReader*>(clientData);
    if (frame == nullptr || buffer == nullptr) {
        self->FlagDecodeError();
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    const bool ok = self->PushDecodedFrame(frame->header.channels,
                                           frame->header.bits_per_sample,
                                           frame->header.sample_rate,
                                           frame->header.blocksize,
                                           buffer);
    return ok ? FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE
              : FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
}

FLAC__StreamDecoderReadStatus FlacReadCallback(
    const FLAC__StreamDecoder* /*decoder*/, FLAC__byte buffer[], size_t* bytes,
    void* clientData) {
    return static_cast<FLAC__StreamDecoderReadStatus>(
        static_cast<FlacReader*>(clientData)->IoRead(buffer, bytes));
}

FLAC__StreamDecoderSeekStatus FlacSeekCallback(
    const FLAC__StreamDecoder* /*decoder*/, FLAC__uint64 absoluteByteOffset,
    void* clientData) {
    return static_cast<FLAC__StreamDecoderSeekStatus>(
        static_cast<FlacReader*>(clientData)->IoSeek(absoluteByteOffset));
}

FLAC__StreamDecoderTellStatus FlacTellCallback(
    const FLAC__StreamDecoder* /*decoder*/, FLAC__uint64* absoluteByteOffset,
    void* clientData) {
    return static_cast<FLAC__StreamDecoderTellStatus>(
        static_cast<FlacReader*>(clientData)->IoTell(absoluteByteOffset));
}

FLAC__StreamDecoderLengthStatus FlacLengthCallback(
    const FLAC__StreamDecoder* /*decoder*/, FLAC__uint64* streamLength,
    void* clientData) {
    return static_cast<FLAC__StreamDecoderLengthStatus>(
        static_cast<FlacReader*>(clientData)->IoLength(streamLength));
}

FLAC__bool FlacEofCallback(const FLAC__StreamDecoder* /*decoder*/,
                           void* clientData) {
    return static_cast<FlacReader*>(clientData)->IoEof() != 0;
}

void FlacErrorCallback(const FLAC__StreamDecoder* /*decoder*/,
                       FLAC__StreamDecoderErrorStatus /*status*/,
                       void* clientData) {
    static_cast<FlacReader*>(clientData)->FlagDecodeError();
}

} // namespace

std::string ParseFlacFormat(void* handle, FlacFormat& out) {
    return ParseFlacStreamInfo(static_cast<HANDLE>(handle), out);
}

std::string ParseFlacFormat(const std::wstring& path, FlacFormat& out) {
    if (path.empty()) {
        return "Empty path";
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return "Could not open FLAC file: " + core::WideToUtf8(path);
    }

    std::string error = ParseFlacFormat(static_cast<void*>(file), out);
    CloseHandle(file);
    return error;
}

bool AppendFlacFramePcm(std::vector<uint8_t>& out,
                        const int32_t* const* channels, uint32_t channelCount,
                        uint32_t blockSize, uint32_t bitsPerSample) {
    if (channels == nullptr || channelCount == 0 || blockSize == 0) {
        return false;
    }
    uint32_t bytesPerSample = 0;
    switch (bitsPerSample) {
        case 8:
        case 16:
        case 24:
        case 32:
            bytesPerSample = bitsPerSample / 8;
            break;
        default:
            return false;
    }

    out.reserve(out.size() +
                static_cast<size_t>(blockSize) * channelCount * bytesPerSample);
    for (uint32_t i = 0; i < blockSize; ++i) {
        for (uint32_t ch = 0; ch < channelCount; ++ch) {
            const int32_t sample = channels[ch][i];
            // Samples are right-aligned int32 from libFLAC and are stored
            // without scaling: packing only, no DSP.
            // The masks read the two's-complement bit pattern, which is the
            // PCM payload itself; no signed arithmetic is intended.
            // NOLINTBEGIN(bugprone-signed-bitwise)
            switch (bitsPerSample) {
                case 8:
                    // 8-bit FLAC is signed; the PCM wire format is unsigned.
                    out.push_back(
                        static_cast<uint8_t>(sample + 128));
                    break;
                case 16:
                    out.push_back(static_cast<uint8_t>(sample & 0xFF));
                    out.push_back(
                        static_cast<uint8_t>((sample >> 8) & 0xFF));
                    break;
                case 24:
                    out.push_back(static_cast<uint8_t>(sample & 0xFF));
                    out.push_back(
                        static_cast<uint8_t>((sample >> 8) & 0xFF));
                    out.push_back(
                        static_cast<uint8_t>((sample >> 16) & 0xFF));
                    break;
                case 32: {
                    const auto bits =
                        static_cast<uint32_t>(sample);
                    out.push_back(static_cast<uint8_t>(bits & 0xFF));
                    out.push_back(
                        static_cast<uint8_t>((bits >> 8) & 0xFF));
                    out.push_back(
                        static_cast<uint8_t>((bits >> 16) & 0xFF));
                    out.push_back(
                        static_cast<uint8_t>((bits >> 24) & 0xFF));
                    break;
                }
                default:
                    // bitsPerSample is validated above; fail loudly rather
                    // than emit a partially packed frame if that ever drifts.
                    return false;
            }
            // NOLINTEND(bugprone-signed-bitwise)
        }
    }
    return true;
}

FlacReader::FlacReader(FlacReader&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    decoder_ = other.decoder_;
    file_ = other.file_;
    streamOffset_ = other.streamOffset_;
    streamLength_ = other.streamLength_;
    fifo_ = std::move(other.fifo_);
    fifoStart_ = other.fifoStart_;
    eof_ = other.eof_;
    error_ = other.error_;
    bytesPerFrame_ = other.bytesPerFrame_;
    bitsPerSample_ = other.bitsPerSample_;
    channelCount_ = other.channelCount_;
    sampleRate_ = other.sampleRate_;
    other.decoder_ = nullptr;
    other.file_ = nullptr;
    other.streamOffset_ = 0;
    other.streamLength_ = 0;
    other.fifoStart_ = 0;
    other.eof_ = false;
    other.error_ = false;
}

FlacReader::~FlacReader() {
    Close();
}

std::string FlacReader::Open(void* handle, const FlacFormat& fmt,
                             uint64_t startSample) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();

    if (handle == nullptr || fmt.channelCount == 0 ||
        fmt.bitsPerSample == 0 || fmt.sampleRate == 0) {
        return "Invalid FLAC format for decoding";
    }
    channelCount_ = fmt.channelCount;
    bitsPerSample_ = fmt.bitsPerSample;
    sampleRate_ = fmt.sampleRate;
    bytesPerFrame_ = fmt.channelCount * (fmt.bitsPerSample / 8);
    streamOffset_ = fmt.streamOffset;

    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(handle),
                         GetCurrentProcess(), &duplicate, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return "Could not duplicate FLAC file handle";
    }
    file_ = duplicate;

    uint64_t totalSize = 0;
    if (!FileSize(static_cast<HANDLE>(file_), totalSize) ||
        streamOffset_ > totalSize) {
        CloseLocked();
        return "Could not determine FLAC stream size";
    }
    streamLength_ = totalSize - streamOffset_;

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(streamOffset_);
    if (!SetFilePointerEx(static_cast<HANDLE>(file_), position, nullptr,
                          FILE_BEGIN)) {
        CloseLocked();
        return "Could not seek to the FLAC stream marker";
    }

    auto* decoder = FLAC__stream_decoder_new();
    if (decoder == nullptr) {
        CloseLocked();
        return "Could not allocate FLAC decoder";
    }
    decoder_ = decoder;

    const FLAC__StreamDecoderInitStatus initStatus =
        FLAC__stream_decoder_init_stream(
            decoder, &FlacReadCallback, &FlacSeekCallback, &FlacTellCallback,
            &FlacLengthCallback, &FlacEofCallback, &FlacWriteCallback,
            nullptr, &FlacErrorCallback, this);
    if (initStatus != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(decoder);
        decoder_ = nullptr;
        CloseLocked();
        return "Could not open FLAC file for decoding";
    }

    if (FLAC__stream_decoder_process_until_end_of_metadata(decoder) == 0) {
        CloseLocked();
        return "Could not read FLAC metadata";
    }

    if (startSample > 0 &&
        FLAC__stream_decoder_seek_absolute(decoder, startSample) == 0) {
        // Do not report success with the decoder desynchronized: a caller
        // that asked to start at startSample must not read audio from an
        // unspecified point. Flush first (libFLAC leaves SEEK_ERROR behind,
        // which finish() tolerates but flushing restores the normal state),
        // then close so IsOpen() reports the failure unambiguously.
        FLAC__stream_decoder_flush(decoder);
        CloseLocked();
        return "Could not seek to the requested FLAC start position";
    }
    return {};
}

std::string FlacReader::Open(const std::wstring& path, const FlacFormat& fmt,
                             uint64_t startSample) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return "Could not open FLAC file for decoding";
    }

    std::string error =
        Open(static_cast<void*>(file), fmt, startSample);
    CloseHandle(file);
    return error;
}

void FlacReader::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
}

void FlacReader::CloseLocked() {
    if (decoder_ != nullptr) {
        auto* decoder = static_cast<FLAC__StreamDecoder*>(decoder_);
        FLAC__stream_decoder_finish(decoder);
        FLAC__stream_decoder_delete(decoder);
        decoder_ = nullptr;
    }
    if (file_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
    streamOffset_ = 0;
    streamLength_ = 0;
    fifo_.clear();
    fifoStart_ = 0;
    eof_ = false;
    error_ = false;
}

uint32_t FlacReader::ReadFrames(uint8_t* buffer, uint32_t maxFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (decoder_ == nullptr || error_ || buffer == nullptr) {
        return 0;
    }

    auto* decoder = static_cast<FLAC__StreamDecoder*>(decoder_);
    while (FifoFramesLocked() < maxFrames && !eof_ && !error_) {
        const FLAC__bool ok = FLAC__stream_decoder_process_single(decoder);
        if (FLAC__stream_decoder_get_state(decoder) ==
            FLAC__STREAM_DECODER_END_OF_STREAM) {
            eof_ = true;
            break;
        }
        if (ok == 0) {
            error_ = true;
            break;
        }
    }

    const uint32_t available = FifoFramesLocked();
    const uint32_t framesToCopy = std::min(maxFrames, available);
    if (framesToCopy == 0) {
        return 0;
    }
    std::memcpy(buffer, fifo_.data() + fifoStart_,
                static_cast<size_t>(framesToCopy) * bytesPerFrame_);
    fifoStart_ += static_cast<size_t>(framesToCopy) * bytesPerFrame_;
    if (fifoStart_ == fifo_.size()) {
        fifo_.clear();
        fifoStart_ = 0;
    } else if (fifoStart_ > 65536) {
        fifo_.erase(fifo_.begin(),
                    fifo_.begin() + static_cast<ptrdiff_t>(fifoStart_));
        fifoStart_ = 0;
    }
    return framesToCopy;
}

bool FlacReader::Seek(uint64_t sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    fifo_.clear();
    fifoStart_ = 0;
    eof_ = false;
    error_ = false;
    if (decoder_ == nullptr) {
        return true;
    }
    auto* decoder = static_cast<FLAC__StreamDecoder*>(decoder_);
    if (FLAC__stream_decoder_seek_absolute(decoder, sample) == 0) {
        FLAC__stream_decoder_flush(decoder);
        return false;
    }
    return true;
}

bool FlacReader::IsOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return decoder_ != nullptr;
}

bool FlacReader::HadError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool FlacReader::ReachedEnd() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return eof_;
}

bool FlacReader::PushDecodedFrame(uint32_t frameChannels,
                                  uint32_t frameBitsPerSample,
                                  uint32_t frameSampleRate,
                                  uint32_t blockSize,
                                  const int32_t* const* buffer) {
    // Called synchronously from the decoding thread while it already holds
    // mutex_; must not lock. Rejects mid-stream format changes: the Player
    // negotiates one exclusive-mode format up front, so a frame at another
    // rate, channel count, or bit depth ends the stream cleanly instead of
    // being resampled or converted.
    if (buffer == nullptr || frameChannels != channelCount_ ||
        frameBitsPerSample != bitsPerSample_ ||
        frameSampleRate != sampleRate_) {
        error_ = true;
        return false;
    }
    if (!AppendFlacFramePcm(fifo_, buffer, channelCount_, blockSize,
                            bitsPerSample_)) {
        error_ = true;
        return false;
    }
    return true;
}

void FlacReader::FlagDecodeError() {
    // Same synchronous-callback context as PushDecodedFrame: no locking.
    error_ = true;
}

int FlacReader::IoRead(uint8_t* buffer, size_t* bytes) {
    // Status values mirror FLAC__StreamDecoderReadStatus: 0 continue,
    // 1 end of stream, 2 abort.
    if (file_ == nullptr || bytes == nullptr) {
        error_ = true;
        return 2;
    }
    if (*bytes == 0) {
        return 0;
    }

    const DWORD requested = static_cast<DWORD>(
        std::min<size_t>(*bytes, static_cast<size_t>(MAXDWORD)));
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(file_), buffer, requested, &read,
                  nullptr)) {
        error_ = true;
        return 2;
    }
    *bytes = read;
    return read == 0 ? 1 : 0;
}

int FlacReader::IoSeek(uint64_t absoluteByteOffset) {
    // Status values mirror FLAC__StreamDecoderSeekStatus: 0 ok, 1 error.
    if (file_ == nullptr || absoluteByteOffset > streamLength_) {
        return 1;
    }

    LARGE_INTEGER position{};
    position.QuadPart =
        static_cast<LONGLONG>(streamOffset_ + absoluteByteOffset);
    return SetFilePointerEx(static_cast<HANDLE>(file_), position, nullptr,
                            FILE_BEGIN)
               ? 0
               : 1;
}

int FlacReader::IoTell(uint64_t* absoluteByteOffset) {
    // Status values mirror FLAC__StreamDecoderTellStatus: 0 ok, 1 error.
    if (file_ == nullptr || absoluteByteOffset == nullptr) {
        return 1;
    }

    LARGE_INTEGER zero{};
    LARGE_INTEGER position{};
    if (!SetFilePointerEx(static_cast<HANDLE>(file_), zero, &position,
                          FILE_CURRENT) ||
        position.QuadPart < static_cast<LONGLONG>(streamOffset_)) {
        return 1;
    }
    *absoluteByteOffset = static_cast<uint64_t>(position.QuadPart) -
                          streamOffset_;
    return 0;
}

int FlacReader::IoLength(uint64_t* streamLength) {
    // Status values mirror FLAC__StreamDecoderLengthStatus: 0 ok, 1 error.
    if (file_ == nullptr || streamLength == nullptr) {
        return 1;
    }
    *streamLength = streamLength_;
    return 0;
}

int FlacReader::IoEof() {
    if (file_ == nullptr) {
        return 1;
    }

    LARGE_INTEGER zero{};
    LARGE_INTEGER position{};
    if (!SetFilePointerEx(static_cast<HANDLE>(file_), zero, &position,
                          FILE_CURRENT)) {
        return 1;
    }
    return position.QuadPart >=
                   static_cast<LONGLONG>(streamOffset_ + streamLength_)
               ? 1
               : 0;
}

uint32_t FlacReader::FifoFramesLocked() const {
    if (bytesPerFrame_ == 0 || fifo_.size() <= fifoStart_) {
        return 0;
    }
    return static_cast<uint32_t>((fifo_.size() - fifoStart_) / bytesPerFrame_);
}

} // namespace kessoku::audio

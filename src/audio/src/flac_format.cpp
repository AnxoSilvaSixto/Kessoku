#include "kessoku/audio/flac_format.h"

#define NOMINMAX
#include <windows.h>

#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cstring>

namespace kessoku::audio {

namespace {

// libFLAC on Windows expects a UTF-8 filename and converts to _wfopen
// internally (see FLAC/metadata.h and FLAC/stream_decoder.h docs).
// Returns empty string on success.
std::string WideToUtf8(const std::wstring& path, std::string& out) {
    if (path.empty()) {
        return "Empty path";
    }
    int required = WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                       static_cast<int>(path.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return "Could not convert path to UTF-8";
    }
    out.assign(static_cast<size_t>(required), '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                      static_cast<int>(path.size()),
                                      out.data(), required, nullptr, nullptr);
    if (written != required) {
        return "Could not convert path to UTF-8";
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
                                           frame->header.blocksize,
                                           buffer);
    return ok ? FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE
              : FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
}

void FlacErrorCallback(const FLAC__StreamDecoder* /*decoder*/,
                       FLAC__StreamDecoderErrorStatus /*status*/,
                       void* clientData) {
    static_cast<FlacReader*>(clientData)->FlagDecodeError();
}

} // namespace

std::string ParseFlacFormat(const std::wstring& path, FlacFormat& out) {
    std::string utf8Path;
    std::string convError = WideToUtf8(path, utf8Path);
    if (!convError.empty()) {
        return convError;
    }

    FLAC__StreamMetadata streamInfo{};
    if (FLAC__metadata_get_streaminfo(utf8Path.c_str(), &streamInfo) == 0) {
        return "Not a valid FLAC file or STREAMINFO block unreadable: " +
               utf8Path;
    }
    if (streamInfo.type != FLAC__METADATA_TYPE_STREAMINFO) {
        return "FLAC file has no STREAMINFO block: " + utf8Path;
    }

    const FLAC__StreamMetadata_StreamInfo& si = streamInfo.data.stream_info;
    out.sampleRate = si.sample_rate;
    out.channelCount = si.channels;
    out.bitsPerSample = si.bits_per_sample;
    out.totalSamples = si.total_samples;

    if (out.sampleRate == 0 || out.channelCount == 0 ||
        out.bitsPerSample == 0) {
        return "Invalid FLAC STREAMINFO block (zero field): " + utf8Path;
    }
    return {};
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
            }
        }
    }
    return true;
}

FlacReader::FlacReader(FlacReader&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    decoder_ = other.decoder_;
    fifo_ = std::move(other.fifo_);
    fifoStart_ = other.fifoStart_;
    eof_ = other.eof_;
    error_ = other.error_;
    bytesPerFrame_ = other.bytesPerFrame_;
    bitsPerSample_ = other.bitsPerSample_;
    channelCount_ = other.channelCount_;
    other.decoder_ = nullptr;
    other.fifoStart_ = 0;
    other.eof_ = false;
    other.error_ = false;
}

FlacReader::~FlacReader() {
    Close();
}

std::string FlacReader::Open(const std::wstring& path, const FlacFormat& fmt,
                             uint64_t startSample) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();

    if (fmt.channelCount == 0 || fmt.bitsPerSample == 0 ||
        fmt.sampleRate == 0) {
        return "Invalid FLAC format for decoding";
    }
    channelCount_ = fmt.channelCount;
    bitsPerSample_ = fmt.bitsPerSample;
    bytesPerFrame_ = fmt.channelCount * (fmt.bitsPerSample / 8);

    std::string utf8Path;
    std::string convError = WideToUtf8(path, utf8Path);
    if (!convError.empty()) {
        return convError;
    }

    auto* decoder = FLAC__stream_decoder_new();
    if (decoder == nullptr) {
        return "Could not allocate FLAC decoder";
    }
    decoder_ = decoder;

    const FLAC__StreamDecoderInitStatus initStatus =
        FLAC__stream_decoder_init_file(decoder, utf8Path.c_str(),
                                       &FlacWriteCallback, nullptr,
                                       &FlacErrorCallback, this);
    if (initStatus != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        CloseLocked();
        return "Could not open FLAC file for decoding";
    }

    if (FLAC__stream_decoder_process_until_end_of_metadata(decoder) == 0) {
        CloseLocked();
        return "Could not read FLAC metadata";
    }

    if (startSample > 0) {
        if (FLAC__stream_decoder_seek_absolute(decoder, startSample) == 0) {
            FLAC__stream_decoder_flush(decoder);
        }
    }
    return {};
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

bool FlacReader::PushDecodedFrame(uint32_t frameChannels,
                                  uint32_t frameBitsPerSample,
                                  uint32_t blockSize,
                                  const int32_t* const* buffer) {
    // Called synchronously from the decoding thread while it already holds
    // mutex_; must not lock. Rejects mid-stream format changes: the Player
    // negotiates one exclusive-mode format up front, so a frame in any other
    // format ends the stream cleanly instead of being converted.
    if (buffer == nullptr || frameChannels != channelCount_ ||
        frameBitsPerSample != bitsPerSample_) {
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

uint32_t FlacReader::FifoFramesLocked() const {
    if (bytesPerFrame_ == 0 || fifo_.size() <= fifoStart_) {
        return 0;
    }
    return static_cast<uint32_t>((fifo_.size() - fifoStart_) / bytesPerFrame_);
}

} // namespace kessoku::audio

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace kessoku::audio {

// FLAC stream parameters from the STREAMINFO metadata block.
struct FlacFormat {
    uint32_t sampleRate = 0;
    uint32_t channelCount = 0;
    uint32_t bitsPerSample = 0;
    uint64_t totalSamples = 0; // 0 means "unknown"
    // Byte offset of the "fLaC" marker. Zero for a bare stream; non-zero
    // when a leading ID3v2 tag was skipped. The decoder uses it to seek to
    // the same bytes the format parser validated.
    uint64_t streamOffset = 0;
};

// Read STREAMINFO from an already-open handle. The handle must be readable
// and seekable; it is not closed. Returns an error message on failure.
std::string ParseFlacFormat(void* handle, FlacFormat& out);

// Path wrapper. Opens the path, then calls the handle overload. Used by
// tests and by callers that already established containment.
// Returns an error message on failure (empty string on success).
std::string ParseFlacFormat(const std::wstring& path, FlacFormat& out);

// Append one decoded FLAC frame to `out`, interleaved and packed into the
// little-endian PCM wire format for the given bitsPerSample.
// `channels` is channel-major int32 (libFLAC write-callback layout) with
// right-aligned samples, `blockSize` samples per channel.
// Only bitsPerSample in {8, 16, 24, 32} is accepted; anything else returns
// false so the caller can take the clean FormatNotSupported path.
// 8-bit FLAC samples are signed and pack to unsigned PCM, matching WAV.
// This interleaving/packing is the only transformation applied to decoded
// samples: no resampling, normalization, DSP, or mixing.
bool AppendFlacFramePcm(std::vector<uint8_t>& out, const int32_t* const* channels,
                        uint32_t channelCount, uint32_t blockSize,
                        uint32_t bitsPerSample);

// Pull-model FLAC decoder for the Player render thread.
//
// All decoding happens synchronously inside ReadFrames (called once per
// WASAPI buffer) because libFLAC delivers variable-size blocks through its
// write callback while WASAPI consumes fixed bufferFrameCount_ chunks; the
// internal FIFO bridges that granularity mismatch without any sample
// conversion. Seek() is sample-accurate via FLAC__stream_decoder_seek_absolute.
// Methods are internally synchronized so Seek() from the API thread cannot
// race the render thread's decode calls.
class FlacReader {
public:
    FlacReader() = default;
    FlacReader(const FlacReader&) = delete;
    FlacReader& operator=(const FlacReader&) = delete;
    FlacReader(FlacReader&& other) noexcept;
    FlacReader& operator=(FlacReader&&) = delete;
    ~FlacReader();

    // Open the file and consume metadata, then position at startSample
    // (sample-accurate seek; 0 = stream start). Closes any previous stream.
    // The handle overload duplicates `handle` and reads only from that file
    // object, so containment validated on the handle still holds here.
    // Returns an error message on failure (empty string on success).
    std::string Open(void* handle, const FlacFormat& fmt,
                     uint64_t startSample);
    std::string Open(const std::wstring& path, const FlacFormat& fmt,
                     uint64_t startSample);

    void Close();

    // Fill `buffer` with up to maxFrames of interleaved packed PCM.
    // Returns frames written; 0 means end of stream or decode error
    // (distinguish via HadError()).
    uint32_t ReadFrames(uint8_t* buffer, uint32_t maxFrames);

    // Sample-accurate reposition; clears buffered audio. On failure the
    // decoder is flushed back to a usable state and false is returned.
    // With no open stream this is a no-op returning true (the pending
    // position is applied when Open() runs).
    bool Seek(uint64_t sample);

    bool IsOpen() const;
    bool HadError() const;

    // True once the decoder reached a clean end of stream. Needed separately
    // from HadError(): a file whose STREAMINFO total_samples overstates the
    // real sample count ends without a decode error, but playback must still
    // finish instead of waiting for frames that will never arrive.
    bool ReachedEnd() const;

    // libFLAC C-callback entry points (called synchronously from inside
    // ReadFrames/Open on the decoding thread). Public only because C
    // callbacks cannot be private members; not part of the usable API.
    bool PushDecodedFrame(uint32_t frameChannels, uint32_t frameBitsPerSample,
                          uint32_t frameSampleRate, uint32_t blockSize,
                          const int32_t* const* buffer);
    void FlagDecodeError();

    // Handle-backed libFLAC I/O callbacks. Public only because C callbacks
    // cannot be private members; not part of the usable API. The return
    // values match the corresponding FLAC__StreamDecoder*Status enums.
    int IoRead(uint8_t* buffer, size_t* bytes);
    int IoSeek(uint64_t absoluteByteOffset);
    int IoTell(uint64_t* absoluteByteOffset);
    int IoLength(uint64_t* streamLength);
    int IoEof();

private:
    void CloseLocked();
    uint32_t FifoFramesLocked() const;

    void* decoder_ = nullptr; // FLAC__StreamDecoder*, guarded by mutex_
    void* file_ = nullptr;    // duplicated HANDLE, guarded by mutex_
    uint64_t streamOffset_ = 0;
    uint64_t streamLength_ = 0;
    std::vector<uint8_t> fifo_;
    size_t fifoStart_ = 0;
    bool eof_ = false;
    bool error_ = false;
    uint32_t bytesPerFrame_ = 0;
    uint32_t bitsPerSample_ = 0;
    uint32_t channelCount_ = 0;
    uint32_t sampleRate_ = 0;
    mutable std::mutex mutex_;
};

} // namespace kessoku::audio

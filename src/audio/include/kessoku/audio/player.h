#pragma once

#include "kessoku/core/result.h"
#include "kessoku/audio/flac_format.h"
#include "kessoku/audio/wav_format.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace kessoku::audio {

enum class PlaybackState {
    Stopped,
    Playing,
    Paused,
    DeviceLost
};

enum class SourceKind {
    Wav,
    Flac
};

class Player {
public:
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    Player(Player&& other) noexcept
        : pEnumerator_(other.pEnumerator_),
          pDevice_(other.pDevice_),
          pAudioClient_(other.pAudioClient_),
          pRenderClient_(other.pRenderClient_),
          hEvent_(other.hEvent_),
          renderThread_(std::move(other.renderThread_)),
          wavPath_(std::move(other.wavPath_)),
          hFile_(other.hFile_),
          dataChunkOffset_(other.dataChunkOffset_),
          dataChunkSize_(other.dataChunkSize_),
          filePosition_(other.filePosition_),
          format_(other.format_),
          flacFormat_(other.flacFormat_),
          sourceKind_(other.sourceKind_),
          flacReader_(std::move(other.flacReader_)),
          totalFrames_(other.totalFrames_),
          bufferFrameCount_(other.bufferFrameCount_),
          bytesPerFrame_(other.bytesPerFrame_),
          currentFrame_(other.currentFrame_),
          ownsCom_(other.ownsCom_),
          state_(other.state_.load()) {
        other.pEnumerator_ = nullptr;
        other.pDevice_ = nullptr;
        other.pAudioClient_ = nullptr;
        other.pRenderClient_ = nullptr;
        other.hEvent_ = nullptr;
        other.hFile_ = nullptr;
        other.currentFrame_ = 0;
        other.ownsCom_ = false;
        other.state_.store(PlaybackState::Stopped);
    }

    // Initialize the player for a specific audio file (.wav or .flac).
    // Parses the file format, enumerates the default render device,
    // and negotiates exclusive-mode format support.
    // Does NOT start playback.
    static core::Result<Player> Create(std::wstring_view wavPath);

    // Start playback from the current position.
    core::Status Play();

    // Pause playback (stops feeding the buffer; stream remains open).
    core::Status Pause();

    // Resume playback after Pause.
    core::Status Resume();

    // Seek to a sample offset (in frames) from the start of the file.
    core::Status Seek(uint32_t frameOffset);

    // Stop playback and release all resources.
    core::Status Stop();

    // Get the current playback state.
    PlaybackState GetState() const noexcept;

    // Get the current playback position in frames.
    uint32_t GetPosition() const noexcept;

    // Get the total duration in frames.
    uint32_t GetTotalFrames() const noexcept;

    // Get the sample rate.
    uint32_t GetSampleRate() const noexcept;

    ~Player();

private:
    explicit Player(std::wstring wavPath, WavFormat format, uint32_t totalFrames,
                    SourceKind sourceKind, FlacFormat flacFormat)
        : wavPath_(std::move(wavPath)),
          format_(format),
          flacFormat_(flacFormat),
          sourceKind_(sourceKind),
          totalFrames_(totalFrames),
          state_(PlaybackState::Stopped) {}

    // Internal: render thread entry point (called by std::thread).
    void RenderThreadEntry();

    // Fill one buffer from the WAV file and push it to the audio device.
    // Returns true if there is more data to play, false if end of file.
    bool RenderIteration();

    // Handle device invalidation: transition to DeviceLost state.
    void HandleDeviceLost();

    // Open the WAV file for reading from the beginning (or from seek position).
    void OpenWavFile();

    // Read frames from the WAV file into a buffer.
    uint32_t ReadFrames(uint8_t* buffer, uint32_t maxFrames);

    // Open the FLAC stream for decoding from the current position.
    void OpenFlacFile();

    // COM pointers
    void* pEnumerator_ = nullptr;
    void* pDevice_ = nullptr;
    void* pAudioClient_ = nullptr;
    void* pRenderClient_ = nullptr;

    // Event handle for event-driven buffering
    void* hEvent_ = nullptr;

    // Render thread
    std::thread renderThread_;

    // WAV file
    std::wstring wavPath_;
    void* hFile_ = nullptr; // HANDLE to the WAV file
    uint64_t dataChunkOffset_ = 0;
    uint64_t dataChunkSize_ = 0;
    // Byte offset into the data chunk; mirrors currentFrame_ for WAV.
    // Guarded by positionMutex_ (see below).
    uint64_t filePosition_ = 0;

    WavFormat format_;
    FlacFormat flacFormat_;
    SourceKind sourceKind_ = SourceKind::Wav;
    FlacReader flacReader_;
    uint32_t totalFrames_;
    uint32_t bufferFrameCount_ = 0;
    uint32_t bytesPerFrame_ = 0;

    // Playback position, shared between the API thread (Play/Seek/Stop/
    // GetPosition) and the render thread (Open*/RenderIteration/ReadFrames).
    // Guarded by positionMutex_, which also serializes the WAV file handle
    // reposition in Seek() against reads in ReadFrames(). FlacReader has
    // its own internal lock for decoder state; lock order is always
    // positionMutex_ -> FlacReader, never the reverse.
    mutable std::mutex positionMutex_;
    // File read state (guarded by positionMutex_, NOT render-thread-only:
    // Seek() writes it from the API thread while RenderIteration() advances
    // it on the render thread).
    uint32_t currentFrame_ = 0;

    // True while this instance owes CoUninitialize() for the apartment
    // entered during Create(). Guards Stop() idempotency: COM must be
    // uninitialized exactly once per successful Create().
    bool ownsCom_ = false;

    // Read/written from both the API thread and the render thread without
    // any higher-level lock, so atomic. Load/store only; transitions are
    // independent (no read-modify-write), so no CAS loop is needed.
    std::atomic<PlaybackState> state_ = PlaybackState::Stopped;
};

} // namespace kessoku::audio

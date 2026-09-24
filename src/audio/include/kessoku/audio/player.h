#pragma once

#include "kessoku/core/result.h"
#include "kessoku/audio/wav_format.h"

#include <cstdint>
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
          totalFrames_(other.totalFrames_),
          bufferFrameCount_(other.bufferFrameCount_),
          bytesPerFrame_(other.bytesPerFrame_),
          currentFrame_(other.currentFrame_),
          state_(other.state_) {
        other.pEnumerator_ = nullptr;
        other.pDevice_ = nullptr;
        other.pAudioClient_ = nullptr;
        other.pRenderClient_ = nullptr;
        other.hEvent_ = nullptr;
        other.hFile_ = nullptr;
        other.currentFrame_ = 0;
        other.state_ = PlaybackState::Stopped;
    }

    // Initialize the player for a specific WAV file.
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
    explicit Player(std::wstring wavPath, WavFormat format, uint32_t totalFrames)
        : wavPath_(std::move(wavPath)),
          format_(format),
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
    uint64_t filePosition_ = 0;

    WavFormat format_;
    uint32_t totalFrames_;
    uint32_t bufferFrameCount_ = 0;
    uint32_t bytesPerFrame_ = 0;

    // File read state (accessed only from render thread)
    uint32_t currentFrame_ = 0;

    PlaybackState state_ = PlaybackState::Stopped;
};

} // namespace kessoku::audio

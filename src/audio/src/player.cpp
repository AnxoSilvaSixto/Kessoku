#include "kessoku/audio/player.h"
#include "kessoku/audio/flac_format.h"
#include "kessoku/audio/wav_format.h"

#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <cwctype>
#include <exception>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kReftimesPerSec = 10000000;

kessoku::core::ErrorCode HResultToErrorCode(HRESULT hr) {
    switch (hr) {
        case AUDCLNT_E_UNSUPPORTED_FORMAT:
            return kessoku::core::ErrorCode::FormatNotSupported;
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
            return kessoku::core::ErrorCode::ExclusiveModeUnavailable;
        case AUDCLNT_E_DEVICE_IN_USE:
            return kessoku::core::ErrorCode::ExclusiveModeUnavailable;
        case AUDCLNT_E_DEVICE_INVALIDATED:
            return kessoku::core::ErrorCode::AudioInitFailed;
        case E_POINTER:
        case E_INVALIDARG:
            return kessoku::core::ErrorCode::AudioInitFailed;
        default:
            return kessoku::core::ErrorCode::AudioInitFailed;
    }
}

// True when an HRESULT means the endpoint is gone for good (unplugged,
// reconfigured, disabled, removed) or its service died. Both must land the
// player in DeviceLost, distinct from a clean Stopped exit. Sourced from
// Microsoft Learn "Recovering from an Invalid-Device Error" (INVALIDATED)
// and the IAudioClient::GetCurrentPadding remarks (SERVICE_NOT_RUNNING);
// RenderIteration already mapped INVALIDATED, this extends the same treatment
// to the service-death code on every IAudioClient call site that can report
// it, so a dead service cannot decay into Stopped or spin forever.
bool IsDeviceGone(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

WAVEFORMATEXTENSIBLE BuildWavExtensible(const kessoku::audio::WavFormat& fmt) {
    WAVEFORMATEXTENSIBLE wfx{};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = fmt.channelCount;
    wfx.Format.nSamplesPerSec = fmt.sampleRate;
    wfx.Format.wBitsPerSample = fmt.bitsPerSample;
    wfx.Format.nBlockAlign =
        static_cast<WORD>(fmt.channelCount * (fmt.bitsPerSample / 8));
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = 22;

    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    // Channel mask follows the FLAC standard channel ordering (RFC 9639
    // section 9.1.3), which maps 1:1 onto WAVEFORMATEXTENSIBLE speaker
    // positions. The mask selects speaker assignment only; samples pass
    // through untouched. Bit count always equals nChannels, as required
    // by Microsoft's WAVEFORMATEXTENSIBLE docs (drivers reject mismatches).
    switch (fmt.channelCount) {
        case 1:
            wfx.dwChannelMask = KSAUDIO_SPEAKER_MONO;
            break;
        case 2:
            wfx.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
            break;
        case 3:
            // L, R, C.
            wfx.dwChannelMask = KSAUDIO_SPEAKER_3POINT0;
            break;
        case 4:
            wfx.dwChannelMask = KSAUDIO_SPEAKER_QUAD;
            break;
        case 5:
            // FL, FR, FC, BL, BR. No KSAUDIO_SPEAKER_* constant matches
            // the FLAC 5-channel order (5POINT0 uses side instead of back),
            // so spell out the speaker bits explicitly.
            wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                SPEAKER_FRONT_CENTER | SPEAKER_BACK_LEFT |
                                SPEAKER_BACK_RIGHT;
            break;
        case 6:
            wfx.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
            break;
        case 7:
            // FL, FR, FC, LFE, BC, SL, SR. No standard constant matches.
            wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                SPEAKER_BACK_CENTER | SPEAKER_SIDE_LEFT |
                                SPEAKER_SIDE_RIGHT;
            break;
        case 8:
            // FL, FR, FC, LFE, BL, BR, SL, SR. This is 7POINT1_SURROUND
            // (0x63F), not 7POINT1 (0xFF, wide with FLC/FRC).
            wfx.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
            break;
        default:
            // Unreachable: Create() rejects channel counts outside 1-8
            // before this runs. Zero (DIRECTOUT) keeps the format invalid
            // so device negotiation fails cleanly instead of mislabeling
            // channels with a stereo mask.
            wfx.dwChannelMask = KSAUDIO_SPEAKER_DIRECTOUT;
            break;
    }

    wfx.Samples.wValidBitsPerSample = fmt.bitsPerSample;
    return wfx;
}

// Find the 'data' chunk in a WAV file and return its offset and size.
bool FindDataChunk(const std::wstring& path, uint64_t& outOffset, uint64_t& outSize) {
    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    // Read and validate RIFF header
    char riff[12]{};
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, riff, 12, &bytesRead, nullptr) || bytesRead < 12) {
        CloseHandle(hFile);
        return false;
    }
    if (std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        CloseHandle(hFile);
        return false;
    }

    // Scan chunks. `pos` tracks the chunk header offset; the file read
    // cursor advances past id+size on each read, so skipping a chunk
    // means header (8 bytes) + payload (+1 pad byte when odd).
    LARGE_INTEGER pos;
    pos.QuadPart = 12; // Skip RIFF header
    SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN);

    while (true) {
        char chunkId[4]{};
        uint32_t chunkSize = 0;

        if (!ReadFile(hFile, chunkId, 4, &bytesRead, nullptr) || bytesRead < 4) {
            CloseHandle(hFile);
            return false;
        }
        if (!ReadFile(hFile, &chunkSize, 4, &bytesRead, nullptr) || bytesRead < 4) {
            CloseHandle(hFile);
            return false;
        }

        if (chunkSize > 1024 * 1024 * 100) { // Sanity: > 100MB
            CloseHandle(hFile);
            return false;
        }

        if (std::memcmp(chunkId, "data", 4) == 0) {
            outOffset = static_cast<uint64_t>(pos.QuadPart + 8);
            outSize = chunkSize;
            CloseHandle(hFile);
            return true;
        }

        // Skip chunk payload (align to 2 bytes)
        pos.QuadPart += 8 + chunkSize;
        if (chunkSize % 2 != 0) {
            pos.QuadPart += 1;
        }
        SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN);
    }
}

// Case-insensitive ".flac" extension check.
// Takes a view because Create() receives one; the view may not be
// NUL-terminated, so compare character by character.
bool HasFlacExtension(std::wstring_view path) {
    if (path.size() < 5) {
        return false;
    }
    const wchar_t* ext = path.data() + path.size() - 5;
    return ext[0] == L'.' && std::towlower(ext[1]) == L'f' &&
           std::towlower(ext[2]) == L'l' && std::towlower(ext[3]) == L'a' &&
           std::towlower(ext[4]) == L'c';
}

} // namespace

namespace kessoku::audio {

void Player::AssertCallingThread() const noexcept {
    if (ownerThreadId_ == 0) {
        return;
    }
    assert(ownerThreadId_ == ::GetCurrentThreadId() &&
           "Player public API must be called from the thread that called "
           "Create()");
}

core::Result<Player> Player::Create(std::wstring_view wavPath) {
    const bool isFlac = HasFlacExtension(wavPath);
    const char* formatKind = isFlac ? "FLAC" : "WAV";

    WavFormat fileFormat{};
    FlacFormat flacFormat{};
    const SourceKind sourceKind =
        isFlac ? SourceKind::Flac : SourceKind::Wav;
    uint32_t totalFrames = 0;
    uint64_t dataChunkOffset = 0;
    uint64_t dataChunkSize = 0;

    if (isFlac) {
        std::string parseError =
            ParseFlacFormat(std::wstring(wavPath), flacFormat);
        if (!parseError.empty()) {
            return core::Result<Player>::Err(
                core::ErrorCode::AudioInitFailed,
                "FLAC format parse failed: " + parseError);
        }
        // Only bit depths with an exact PCM wire container are playable.
        // Anything else would need bit-depth conversion, which the
        // bit-perfect requirement forbids, so fail the same clean
        // FormatNotSupported path device negotiation failures use.
        if (flacFormat.bitsPerSample != 8 &&
            flacFormat.bitsPerSample != 16 &&
            flacFormat.bitsPerSample != 24 &&
            flacFormat.bitsPerSample != 32) {
            return core::Result<Player>::Err(
                core::ErrorCode::FormatNotSupported,
                "FLAC bit depth has no exact exclusive-mode PCM container");
        }
        if (flacFormat.channelCount == 0 || flacFormat.channelCount > 8) {
            return core::Result<Player>::Err(
                core::ErrorCode::FormatNotSupported,
                "FLAC channel count not supported in exclusive mode");
        }
        if (flacFormat.totalSamples == 0 ||
            flacFormat.totalSamples > UINT32_MAX) {
            return core::Result<Player>::Err(
                core::ErrorCode::AudioInitFailed,
                "FLAC total sample count unknown or too large");
        }
        fileFormat.sampleRate = flacFormat.sampleRate;
        fileFormat.bitsPerSample =
            static_cast<uint16_t>(flacFormat.bitsPerSample);
        fileFormat.channelCount =
            static_cast<uint16_t>(flacFormat.channelCount);
        fileFormat.blockAlign = static_cast<uint16_t>(
            flacFormat.channelCount * (flacFormat.bitsPerSample / 8));
        fileFormat.byteRate =
            flacFormat.sampleRate * fileFormat.blockAlign;
        totalFrames = static_cast<uint32_t>(flacFormat.totalSamples);
    } else {
        std::string parseError =
            ParseWavFormat(std::wstring(wavPath), fileFormat);
        if (!parseError.empty()) {
            return core::Result<Player>::Err(
                core::ErrorCode::AudioInitFailed,
                "WAV format parse failed: " + parseError);
        }
        // Same exact-container rule as FLAC: anything outside 8/16/24/32
        // would need bit-depth conversion, which bit-perfect forbids.
        if (fileFormat.bitsPerSample != 8 &&
            fileFormat.bitsPerSample != 16 &&
            fileFormat.bitsPerSample != 24 &&
            fileFormat.bitsPerSample != 32) {
            return core::Result<Player>::Err(
                core::ErrorCode::FormatNotSupported,
                "WAV bit depth has no exact exclusive-mode PCM container");
        }
        if (fileFormat.channelCount == 0 || fileFormat.channelCount > 8) {
            return core::Result<Player>::Err(
                core::ErrorCode::FormatNotSupported,
                "WAV channel count not supported in exclusive mode");
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        hr = S_OK;
    }
    if (FAILED(hr)) {
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "COM initialization failed");
    }

    IMMDeviceEnumerator* pEnumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&pEnumerator));
    if (FAILED(hr) || pEnumerator == nullptr) {
        CoUninitialize();
        return core::Result<Player>::Err(
            core::ErrorCode::DeviceNotFound,
            "Could not create device enumerator");
    }

    IMMDevice* pDevice = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr) || pDevice == nullptr) {
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            core::ErrorCode::DeviceNotFound,
            "Could not get default render device");
    }

    IAudioClient* pAudioClient = nullptr;
    hr = pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&pAudioClient));
    if (FAILED(hr) || pAudioClient == nullptr) {
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not activate audio client");
    }

    WAVEFORMATEXTENSIBLE wfxExt = BuildWavExtensible(fileFormat);

    WAVEFORMATEX* pClosest = nullptr;
    hr = pAudioClient->IsFormatSupported(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        reinterpret_cast<WAVEFORMATEX*>(&wfxExt),
        &pClosest);

    if (pClosest) {
        CoTaskMemFree(pClosest);
        pClosest = nullptr;
    }

    if (hr == S_OK) {
        // format supported
    } else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            core::ErrorCode::FormatNotSupported,
            std::string("Device does not support this ") + formatKind +
                " format in exclusive mode");
    } else {
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            HResultToErrorCode(hr),
            "IsFormatSupported failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
    }

    REFERENCE_TIME hnsMinPeriod = 0;
    hr = pAudioClient->GetDevicePeriod(&hnsMinPeriod, nullptr);
    if (FAILED(hr)) {
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not get device period");
    }

    REFERENCE_TIME hnsBufferDuration = hnsMinPeriod;

    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsBufferDuration,
        hnsBufferDuration,
        reinterpret_cast<WAVEFORMATEX*>(&wfxExt),
        nullptr);

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 nFrames = 0;
        hr = pAudioClient->GetBufferSize(&nFrames);
        if (SUCCEEDED(hr)) {
            hnsBufferDuration = static_cast<REFERENCE_TIME>(
                (10000.0 * 1000.0 / static_cast<double>(fileFormat.sampleRate) *
                 static_cast<double>(nFrames)) + 0.5);
        }
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();

        // Re-enter COM: the teardown above balanced the CoInitializeEx at
        // the top of Create(), so the re-created device objects below need
        // a fresh apartment (otherwise CoCreateInstance fails with
        // CO_E_NOTINITIALIZED and every device needing buffer-size
        // alignment becomes unusable).
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            hr = S_OK;
        }
        if (FAILED(hr)) {
            return core::Result<Player>::Err(
                core::ErrorCode::AudioInitFailed,
                "Re-init failed after buffer alignment");
        }

        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&pEnumerator));
        if (FAILED(hr)) {
            return core::Result<Player>::Err(
                core::ErrorCode::AudioInitFailed,
                "Re-init failed after buffer alignment");
        }

        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
        if (FAILED(hr)) {
            pEnumerator->Release();
            CoUninitialize();
            return core::Result<Player>::Err(
                core::ErrorCode::DeviceNotFound,
                "Could not get default render device (re-init)");
        }

        hr = pDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&pAudioClient));
        if (FAILED(hr)) {
            pDevice->Release();
            pEnumerator->Release();
            CoUninitialize();
            return core::Result<Player>::Err(
                core::ErrorCode::AudioInitFailed,
                "Could not activate audio client (re-init)");
        }

        hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            hnsBufferDuration,
            hnsBufferDuration,
            reinterpret_cast<WAVEFORMATEX*>(&wfxExt),
            nullptr);
    }

    if (FAILED(hr)) {
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            HResultToErrorCode(hr),
            "IAudioClient::Initialize failed: 0x" +
                std::to_string(static_cast<unsigned long>(hr)));
    }

    UINT32 bufferFrameCount = 0;
    hr = pAudioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not get buffer size");
    }

    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (hEvent == nullptr) {
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not create event handle");
    }

    hr = pAudioClient->SetEventHandle(hEvent);
    if (FAILED(hr)) {
        CloseHandle(hEvent);
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not set event handle");
    }

    IAudioRenderClient* pRenderClient = nullptr;
    hr = pAudioClient->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(&pRenderClient));
    if (FAILED(hr) || pRenderClient == nullptr) {
        CloseHandle(hEvent);
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        CoUninitialize();
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not get IAudioRenderClient");
    }

    // Find data chunk offset and size (WAV only; FLAC total frames come
    // from STREAMINFO and decoding starts at the stream head).
    uint32_t bytesPerFrame =
        fileFormat.channelCount * (fileFormat.bitsPerSample / 8);
    if (!isFlac) {
        if (!FindDataChunk(std::wstring(wavPath), dataChunkOffset,
                           dataChunkSize)) {
            pRenderClient->Release();
            CloseHandle(hEvent);
            pAudioClient->Release();
            pDevice->Release();
            pEnumerator->Release();
            CoUninitialize();
            return core::Result<Player>::Err(
                core::ErrorCode::AudioInitFailed,
                "Could not find data chunk in WAV file");
        }
        totalFrames = static_cast<uint32_t>(dataChunkSize / bytesPerFrame);
    }

    Player player(std::wstring(wavPath), fileFormat, totalFrames, sourceKind,
                  flacFormat);
    player.pEnumerator_ = pEnumerator;
    player.pDevice_ = pDevice;
    player.pAudioClient_ = pAudioClient;
    player.pRenderClient_ = pRenderClient;
    player.hEvent_ = hEvent;
    player.bufferFrameCount_ = bufferFrameCount;
    player.bytesPerFrame_ = bytesPerFrame;
    player.dataChunkOffset_ = dataChunkOffset;
    player.dataChunkSize_ = dataChunkSize;
    player.ownsCom_ = true;
    player.ownerThreadId_ = static_cast<uint32_t>(::GetCurrentThreadId());

    return core::Result<Player>::Ok(std::move(player));
}

core::Status Player::Play() {
    AssertCallingThread();
    if (state_.load() != PlaybackState::Stopped &&
        state_.load() != PlaybackState::Paused) {
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Player not in Stopped or Paused state");
    }

    if (state_.load() == PlaybackState::Paused) {
        // The render thread from the original Play() is still alive (and
        // joinable); resuming it has the same effect as Resume() without
        // move-assigning over a joinable std::thread (std::terminate).
        return Resume();
    }

    {
        std::lock_guard<std::mutex> lock(positionMutex_);
        currentFrame_ = 0;
        filePosition_ = 0;
    }

    if (renderThread_.joinable()) {
        // Natural end-of-stream leaves a finished-but-joinable thread
        // behind with no Stop() in between. Join it before spawning the
        // replacement; move-assigning over a joinable thread terminates.
        renderThread_.join();
    }

    // Publish Playing before the new thread can observe state: the store is
    // sequenced before thread construction, and construction synchronizes
    // with the start of the new thread, so the thread can only ever observe
    // Playing (or a later transition) — never the stale Stopped that would
    // make it break immediately and leave Playing with a dead thread. A
    // throwing constructor restores Stopped so no Playing-with-no-thread
    // state escapes. The join-before-reassign logic above is unchanged.
    state_ = PlaybackState::Playing;
    try {
        renderThread_ = std::thread(&Player::RenderThreadEntry, this);
    } catch (...) {
        state_ = PlaybackState::Stopped;
        throw;
    }
    return core::Result<void>::Ok();
}

core::Status Player::Pause() {
    AssertCallingThread();
    if (state_.load() != PlaybackState::Playing) {
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Player not in Playing state");
    }

    IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
    pAc->Stop();

    state_ = PlaybackState::Paused;
    return core::Result<void>::Ok();
}

core::Status Player::Resume() {
    AssertCallingThread();
    if (state_.load() != PlaybackState::Paused) {
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Player not in Paused state");
    }

    IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
    HRESULT hr = pAc->Start();
    if (FAILED(hr)) {
        if (IsDeviceGone(hr)) {
            HandleDeviceLost();
        }
        return core::Result<void>::Err(
            HResultToErrorCode(hr),
            "IAudioClient::Start failed");
    }

    state_ = PlaybackState::Playing;
    // Wake the render thread promptly: while Paused it polls hEvent_ with
    // a short timeout, and Start() alone only signals on the next buffer
    // period. Auto-reset event; harmless if the thread is not waiting.
    if (hEvent_) {
        SetEvent(reinterpret_cast<HANDLE>(hEvent_));
    }
    return core::Result<void>::Ok();
}

core::Status Player::Seek(uint32_t frameOffset) {
    AssertCallingThread();
    if (state_.load() == PlaybackState::Stopped) {
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Cannot seek while stopped");
    }

    if (sourceKind_ == SourceKind::Flac) {
        frameOffset = std::min(frameOffset, totalFrames_);

        // Pause, reposition, resume. The position update and decoder seek
        // hold positionMutex_ so a concurrent RenderIteration (which holds
        // the same mutex across its decode + currentFrame_ update) cannot
        // interleave and clobber the seek with a stale read-modify-write.
        // Lock order is always positionMutex_ -> FlacReader.
        Pause();
        {
            std::lock_guard<std::mutex> lock(positionMutex_);
            currentFrame_ = frameOffset;
            if (!flacReader_.Seek(frameOffset)) {
                // Decoder was flushed back to a usable state; the stream
                // stays paused so the caller can Resume() or Stop().
                return core::Result<void>::Err(
                    core::ErrorCode::AudioInitFailed,
                    "FLAC seek failed");
            }
        }
        Resume();

        return core::Result<void>::Ok();
    }

    frameOffset = std::min(frameOffset, totalFrames_);

    // Pause, reposition, resume (same serialization as FLAC above).
    // Repositions the file handle as well: filePosition_ alone is not
    // enough because ReadFrames reads sequentially from the handle.
    Pause();
    {
        std::lock_guard<std::mutex> lock(positionMutex_);
        currentFrame_ = frameOffset;
        filePosition_ = static_cast<uint64_t>(frameOffset) * bytesPerFrame_;
        if (hFile_) {
            LARGE_INTEGER pos;
            pos.QuadPart = static_cast<LONGLONG>(dataChunkOffset_ +
                                                 filePosition_);
            SetFilePointerEx(hFile_, pos, nullptr, FILE_BEGIN);
        }
    }
    Resume();

    return core::Result<void>::Ok();
}

core::Status Player::Stop() {
    AssertCallingThread();
    if (state_.load() != PlaybackState::Stopped) {
        IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
        pAc->Stop();

        // Make the Stop intent visible to the render thread before joining.
        // The thread tolerates an indefinite Paused wait (client stopped, no
        // event), so without this it cannot tell "Stop() was called" from
        // "intentional pause" and the join would hang. Preserve DeviceLost:
        // a lost device already exited (or is exiting) the thread on its own.
        // Signaling hEvent_ wakes a thread blocked in WaitForSingleObject
        // promptly instead of waiting out the full 2s Playing timeout.
        // Cleanup below (releases, CoUninitialize) is unchanged.
        const PlaybackState stopFrom = state_.load();
        if (stopFrom == PlaybackState::Playing ||
            stopFrom == PlaybackState::Paused) {
            state_.store(PlaybackState::Stopped);
        }
        if (hEvent_) {
            SetEvent(reinterpret_cast<HANDLE>(hEvent_));
        }

        if (renderThread_.joinable()) {
            renderThread_.join();
        }
    } else if (renderThread_.joinable() &&
               renderThread_.get_id() != std::this_thread::get_id()) {
        // End-of-stream path: the render thread finished on its own and
        // left a joinable handle behind.
        renderThread_.join();
    }

    // Release everything held, whether playback ran or Create() only
    // negotiated the device. A successful Create() without Play() leaves
    // the exclusive stream initialized; releasing it here keeps Stop()
    // idempotent and lets a later Create() re-acquire the device.

    if (pRenderClient_) {
        reinterpret_cast<IAudioRenderClient*>(pRenderClient_)->Release();
        pRenderClient_ = nullptr;
    }
    if (pAudioClient_) {
        reinterpret_cast<IAudioClient*>(pAudioClient_)->Release();
        pAudioClient_ = nullptr;
    }
    if (pDevice_) {
        reinterpret_cast<IMMDevice*>(pDevice_)->Release();
        pDevice_ = nullptr;
    }
    if (pEnumerator_) {
        reinterpret_cast<IMMDeviceEnumerator*>(pEnumerator_)->Release();
        pEnumerator_ = nullptr;
    }

    if (hEvent_) {
        CloseHandle(hEvent_);
        hEvent_ = nullptr;
    }

    if (hFile_) {
        CloseHandle(hFile_);
        hFile_ = nullptr;
    }

    flacReader_.Close();

    if (ownsCom_) {
        CoUninitialize();
        ownsCom_ = false;
    }

    state_ = PlaybackState::Stopped;
    {
        std::lock_guard<std::mutex> lock(positionMutex_);
        currentFrame_ = 0;
        filePosition_ = 0;
    }
    return core::Result<void>::Ok();
}

PlaybackState Player::GetState() const noexcept {
    AssertCallingThread();
    return state_.load();
}

uint32_t Player::GetPosition() const noexcept {
    AssertCallingThread();
    std::lock_guard<std::mutex> lock(positionMutex_);
    return currentFrame_;
}

uint32_t Player::GetTotalFrames() const noexcept {
    AssertCallingThread();
    return totalFrames_;
}

uint32_t Player::GetSampleRate() const noexcept {
    AssertCallingThread();
    return format_.sampleRate;
}

void Player::RenderThreadEntry() {
    if (sourceKind_ == SourceKind::Flac) {
        OpenFlacFile();
        if (!flacReader_.IsOpen()) {
            state_ = PlaybackState::Stopped;
            return;
        }
    } else {
        OpenWavFile();
        {
            std::lock_guard<std::mutex> lock(positionMutex_);
            if (!hFile_) {
                state_ = PlaybackState::Stopped;
                return;
            }
        }
    }

    // Fill the first buffer before starting.
    RenderIteration();

    IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
    HRESULT hr = pAc->Start();
    if (FAILED(hr)) {
        // A dead device or dead audio service at startup must stay
        // DeviceLost, distinct from a clean Stopped exit (same rule as the
        // loop-exit preservation below).
        if (IsDeviceGone(hr) ||
            state_.load() == PlaybackState::DeviceLost) {
            HandleDeviceLost();
        } else {
            state_ = PlaybackState::Stopped;
        }
        return;
    }

    while (true) {
        const PlaybackState loopState = state_.load();
        if (loopState == PlaybackState::Stopped ||
            loopState == PlaybackState::DeviceLost) {
            break;
        }
        if (loopState == PlaybackState::Paused) {
            // Intentional pause: Pause() stopped the client, so the buffer
            // event will not re-signal until Resume() calls Start(). A lack
            // of event here is expected, never device loss. Poll with a
            // short timeout so Resume()/Stop() (both SetEvent hEvent_) are
            // noticed promptly, and probe for unplug-while-paused so a
            // genuine invalidation still lands in DeviceLost.
            WaitForSingleObject(
                reinterpret_cast<HANDLE>(hEvent_), 100);
            UINT32 pad = 0;
            const HRESULT qhr = pAc->GetCurrentPadding(&pad);
            if (IsDeviceGone(qhr)) {
                HandleDeviceLost();
                break;
            }
            continue;
        }

        DWORD waitResult = WaitForSingleObject(
            reinterpret_cast<HANDLE>(hEvent_), 2000);

        if (waitResult == WAIT_OBJECT_0) {
            // Pause()/Stop() may have raced with the signal; re-check
            // before touching the buffer so a pause is never rendered
            // past and a stop is never mistaken for data.
            const PlaybackState signaledState = state_.load();
            if (signaledState == PlaybackState::Paused) {
                continue;
            }
            if (signaledState == PlaybackState::Stopped ||
                signaledState == PlaybackState::DeviceLost) {
                break;
            }
            if (!RenderIteration()) {
                break;
            }
            if (state_.load() == PlaybackState::DeviceLost) {
                break;
            }
            continue;
        }

        // Timeout or wait failure: never assume loss. Pause() stops the
        // client (no more events) and sets Paused; Stop() now stores
        // Stopped and signals before joining. Re-check state first.
        const PlaybackState timeoutState = state_.load();
        if (timeoutState == PlaybackState::Paused) {
            continue;
        }
        if (timeoutState == PlaybackState::Stopped ||
            timeoutState == PlaybackState::DeviceLost) {
            break;
        }
        // Still Playing: distinguish "we stopped the client ourselves"
        // from "the device is actually gone" with a non-destructive probe.
        // GetCurrentPadding reports a dead device or dead audio service via
        // IsDeviceGone (Microsoft Learn: Recovering from an Invalid-Device
        // Error + GetCurrentPadding remarks; RenderIteration treats the same
        // codes from GetBuffer / ReleaseBuffer as loss). Anything else is
        // scheduling jitter or a Stop() race whose state store lands on the
        // next iteration, so keep waiting instead of declaring loss.
        {
            UINT32 pad = 0;
            const HRESULT qhr = pAc->GetCurrentPadding(&pad);
            if (IsDeviceGone(qhr)) {
                HandleDeviceLost();
                break;
            }
        }
    }

    // Drain only on a clean Playing exit (end-of-stream): let the last
    // buffer play, then stop the client. On a Stop()-requested exit the
    // client is already stopped (Stop() did it before joining), and on a
    // DeviceLost exit the device is dead — calling into it again only delays
    // the join, so skip straight to the exit-reason preservation below.
    // A Paused thread never reaches here on its own (only via Stop/DeviceLost
    // above); defensively, any non-Playing reason is left untouched so
    // DeviceLost stays distinct from Stopped. EOS is Playing -> Stopped.
    if (state_.load() == PlaybackState::Playing) {
        REFERENCE_TIME hnsPeriod = 0;
        pAc->GetDevicePeriod(nullptr, &hnsPeriod);
        Sleep(static_cast<DWORD>(hnsPeriod / 10000));

        pAc->Stop();

        state_ = PlaybackState::Stopped;
    }
}

Player::~Player() {
    AssertCallingThread();
    Stop();
}

void Player::OpenWavFile() {
    // Holds positionMutex_ across handle creation + seek so a concurrent
    // Seek() (same mutex) cannot reposition a half-opened handle or miss
    // the new filePosition_.
    std::lock_guard<std::mutex> lock(positionMutex_);
    if (hFile_) {
        CloseHandle(hFile_);
        hFile_ = nullptr;
    }

    void* newFile = CreateFileW(
        wavPath_.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (newFile == INVALID_HANDLE_VALUE) {
        hFile_ = nullptr;
        return;
    }
    hFile_ = newFile;

    // Seek to data chunk start + filePosition_
    LARGE_INTEGER dataStart;
    dataStart.QuadPart = static_cast<LONGLONG>(dataChunkOffset_);
    SetFilePointerEx(hFile_, dataStart, nullptr, FILE_BEGIN);

    LARGE_INTEGER filePos;
    filePos.QuadPart = static_cast<LONGLONG>(filePosition_);
    SetFilePointerEx(hFile_, filePos, nullptr, FILE_CURRENT);
}

void Player::OpenFlacFile() {
    // Open failure is reported via IsOpen(), checked by RenderThreadEntry;
    // there is no error channel back from the render thread.
    // Reads currentFrame_ under lock; holds the lock across Open so a
    // concurrent Seek() serializes after the open instead of seeking a
    // decoder that is about to be replaced.
    std::lock_guard<std::mutex> lock(positionMutex_);
    flacReader_.Open(wavPath_, flacFormat_, currentFrame_);
}

uint32_t Player::ReadFrames(uint8_t* buffer, uint32_t maxFrames) {
    // Caller (RenderIteration) holds positionMutex_. Must not lock here:
    // the same non-recursive mutex is already held. Serializes the
    // ReadFile + filePosition_ advance against Seek()'s reposition.
    if (!hFile_ || bytesPerFrame_ == 0) return 0;

    uint64_t remaining = dataChunkSize_ - filePosition_;
    uint32_t bytesAvailable = static_cast<uint32_t>(
        remaining > UINT32_MAX ? UINT32_MAX : remaining);
    uint32_t framesAvailable = bytesAvailable / bytesPerFrame_;
    uint32_t framesToRead = std::min(maxFrames, framesAvailable);

    if (framesToRead == 0) return 0;

    DWORD bytesRead = 0;
    if (!ReadFile(hFile_, buffer, framesToRead * bytesPerFrame_, &bytesRead, nullptr)) {
        return 0;
    }

    filePosition_ += bytesRead;
    return bytesRead / bytesPerFrame_;
}

bool Player::RenderIteration() {
    IAudioRenderClient* pRC =
        reinterpret_cast<IAudioRenderClient*>(pRenderClient_);

    BYTE* pData = nullptr;
    HRESULT hr = pRC->GetBuffer(bufferFrameCount_, &pData);
    if (FAILED(hr) || pData == nullptr) {
        if (IsDeviceGone(hr)) {
            HandleDeviceLost();
        }
        return false;
    }

    // Zero out buffer (silence for EOF).
    std::memset(pData, 0, bufferFrameCount_ * bytesPerFrame_);

    // Read frames from the source: WAV file bytes or decoded FLAC samples.
    // The remaining-count computation, decode, and currentFrame_ advance
    // hold positionMutex_ as one critical section so Seek() cannot slip a
    // store between the read and the += and lose either update.
    uint32_t framesRead = 0;
    bool more = false;
    {
        std::lock_guard<std::mutex> lock(positionMutex_);
        uint32_t remaining = (currentFrame_ < totalFrames_)
                                 ? (totalFrames_ - currentFrame_)
                                 : 0;
        uint32_t toRead = std::min(bufferFrameCount_, remaining);
        if (sourceKind_ == SourceKind::Flac) {
            framesRead = flacReader_.ReadFrames(pData, toRead);
        } else {
            framesRead = ReadFrames(pData, toRead);
        }

        currentFrame_ += framesRead;
        more = (currentFrame_ < totalFrames_);
    }

    DWORD flags = 0;
    if (framesRead == 0) {
        flags = AUDCLNT_BUFFERFLAGS_SILENT;
    }

    hr = pRC->ReleaseBuffer(framesRead, flags);
    if (FAILED(hr)) {
        if (IsDeviceGone(hr)) {
            HandleDeviceLost();
        }
        return false;
    }

    return more;
}

void Player::HandleDeviceLost() {
    state_ = PlaybackState::DeviceLost;
}

} // namespace kessoku::audio

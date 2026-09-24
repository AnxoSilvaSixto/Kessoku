#include "kessoku/audio/player.h"
#include "kessoku/audio/wav_format.h"

#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <array>
#include <cstring>
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

    switch (fmt.bitsPerSample) {
        case 8:
            wfx.dwChannelMask = KSAUDIO_SPEAKER_MONO;
            break;
        case 16:
        case 24:
        case 32:
            if (fmt.channelCount == 1)
                wfx.dwChannelMask = KSAUDIO_SPEAKER_MONO;
            else if (fmt.channelCount == 2)
                wfx.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
            else if (fmt.channelCount == 4)
                wfx.dwChannelMask = KSAUDIO_SPEAKER_QUAD;
            else if (fmt.channelCount == 6)
                wfx.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
            else if (fmt.channelCount == 8)
                wfx.dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
            else
                wfx.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
            break;
        default:
            wfx.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
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

    // Scan chunks
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
            outOffset = pos.QuadPart;
            outSize = chunkSize;
            CloseHandle(hFile);
            return true;
        }

        // Skip chunk (align to 2 bytes)
        pos.QuadPart += chunkSize;
        if (chunkSize % 2 != 0) {
            pos.QuadPart += 1;
        }
        SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN);
    }
}

} // namespace

namespace kessoku::audio {

core::Result<Player> Player::Create(std::wstring_view wavPath) {
    WavFormat fileFormat{};
    std::string parseError = ParseWavFormat(std::wstring(wavPath), fileFormat);
    if (!parseError.empty()) {
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "WAV format parse failed: " + parseError);
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
            "Device does not support this WAV format in exclusive mode");
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

    // Find data chunk offset and size.
    uint64_t dataChunkOffset = 0;
    uint64_t dataChunkSize = 0;
    if (!FindDataChunk(std::wstring(wavPath), dataChunkOffset, dataChunkSize)) {
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

    uint32_t bytesPerFrame = fileFormat.channelCount * (fileFormat.bitsPerSample / 8);
    uint32_t totalFrames = static_cast<uint32_t>(dataChunkSize / bytesPerFrame);

    Player player(std::wstring(wavPath), fileFormat, totalFrames);
    player.pEnumerator_ = pEnumerator;
    player.pDevice_ = pDevice;
    player.pAudioClient_ = pAudioClient;
    player.pRenderClient_ = pRenderClient;
    player.hEvent_ = hEvent;
    player.bufferFrameCount_ = bufferFrameCount;
    player.bytesPerFrame_ = bytesPerFrame;
    player.dataChunkOffset_ = dataChunkOffset;
    player.dataChunkSize_ = dataChunkSize;

    return core::Result<Player>::Ok(std::move(player));
}

core::Status Player::Play() {
    if (state_ != PlaybackState::Stopped && state_ != PlaybackState::Paused) {
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Player not in Stopped or Paused state");
    }

    if (state_ == PlaybackState::Stopped) {
        currentFrame_ = 0;
        filePosition_ = 0;
    }

    renderThread_ = std::thread(&Player::RenderThreadEntry, this);

    state_ = PlaybackState::Playing;
    return core::Result<void>::Ok();
}

core::Status Player::Pause() {
    if (state_ != PlaybackState::Playing) {
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
    if (state_ != PlaybackState::Paused) {
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Player not in Paused state");
    }

    IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
    HRESULT hr = pAc->Start();
    if (FAILED(hr)) {
        return core::Result<void>::Err(
            HResultToErrorCode(hr),
            "IAudioClient::Start failed");
    }

    state_ = PlaybackState::Playing;
    return core::Result<void>::Ok();
}

core::Status Player::Seek(uint32_t frameOffset) {
    if (state_ == PlaybackState::Stopped) {
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Cannot seek while stopped");
    }

    frameOffset = std::min(frameOffset, totalFrames_);

    // Pause, reposition, resume.
    Pause();
    currentFrame_ = frameOffset;
    filePosition_ = static_cast<uint64_t>(frameOffset) * bytesPerFrame_;
    Resume();

    return core::Result<void>::Ok();
}

core::Status Player::Stop() {
    if (state_ == PlaybackState::Stopped) {
        return core::Result<void>::Ok();
    }

    IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
    pAc->Stop();

    if (renderThread_.joinable()) {
        renderThread_.join();
    }

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

    CoUninitialize();

    state_ = PlaybackState::Stopped;
    currentFrame_ = 0;
    filePosition_ = 0;
    return core::Result<void>::Ok();
}

PlaybackState Player::GetState() const noexcept {
    return state_;
}

uint32_t Player::GetPosition() const noexcept {
    return currentFrame_;
}

uint32_t Player::GetTotalFrames() const noexcept {
    return totalFrames_;
}

uint32_t Player::GetSampleRate() const noexcept {
    return format_.sampleRate;
}

void Player::RenderThreadEntry() {
    OpenWavFile();
    if (!hFile_) {
        state_ = PlaybackState::Stopped;
        return;
    }

    // Fill the first buffer before starting.
    RenderIteration();

    IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
    HRESULT hr = pAc->Start();
    if (FAILED(hr)) {
        state_ = PlaybackState::Stopped;
        return;
    }

    while (state_ == PlaybackState::Playing) {
        DWORD waitResult = WaitForSingleObject(
            reinterpret_cast<HANDLE>(hEvent_), 2000);

        if (waitResult != WAIT_OBJECT_0) {
            HandleDeviceLost();
            break;
        }

        if (!RenderIteration()) {
            break;
        }
    }

    // Wait for last buffer to play.
    REFERENCE_TIME hnsPeriod = 0;
    pAc->GetDevicePeriod(nullptr, &hnsPeriod);
    Sleep(static_cast<DWORD>(hnsPeriod / 10000));

    pAc->Stop();

    state_ = PlaybackState::Stopped;
}

Player::~Player() {
    if (state_ != PlaybackState::Stopped) {
        Stop();
    }
}

void Player::OpenWavFile() {
    if (hFile_) {
        CloseHandle(hFile_);
    }

    hFile_ = CreateFileW(
        wavPath_.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile_ == INVALID_HANDLE_VALUE) {
        hFile_ = nullptr;
        return;
    }

    // Seek to data chunk start + filePosition_
    LARGE_INTEGER dataStart;
    dataStart.QuadPart = static_cast<LONGLONG>(dataChunkOffset_);
    SetFilePointerEx(hFile_, dataStart, nullptr, FILE_BEGIN);

    LARGE_INTEGER filePos;
    filePos.QuadPart = static_cast<LONGLONG>(filePosition_);
    SetFilePointerEx(hFile_, filePos, nullptr, FILE_CURRENT);
}

uint32_t Player::ReadFrames(uint8_t* buffer, uint32_t maxFrames) {
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
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            HandleDeviceLost();
        }
        return false;
    }

    // Zero out buffer (silence for EOF).
    std::memset(pData, 0, bufferFrameCount_ * bytesPerFrame_);

    // Read frames from WAV file.
    uint32_t framesRead = ReadFrames(
        pData,
        std::min(bufferFrameCount_,
                 totalFrames_ - currentFrame_));

    currentFrame_ += framesRead;

    DWORD flags = 0;
    if (framesRead == 0) {
        flags = AUDCLNT_BUFFERFLAGS_SILENT;
    }

    hr = pRC->ReleaseBuffer(framesRead, flags);
    if (FAILED(hr)) {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            HandleDeviceLost();
        }
        return false;
    }

    return (currentFrame_ < totalFrames_);
}

void Player::HandleDeviceLost() {
    state_ = PlaybackState::DeviceLost;
}

} // namespace kessoku::audio

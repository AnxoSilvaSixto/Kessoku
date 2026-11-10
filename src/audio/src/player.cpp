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
#include <cmath>
#include <cstring>
#include <cwctype>
#include <exception>
#include <string>
#include <vector>

namespace {

kessoku::core::ErrorCode HResultToErrorCode(HRESULT hr) {
    switch (hr) {
        case AUDCLNT_E_UNSUPPORTED_FORMAT:
            return kessoku::core::ErrorCode::FormatNotSupported;
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        case AUDCLNT_E_DEVICE_IN_USE:
            return kessoku::core::ErrorCode::ExclusiveModeUnavailable;
        case AUDCLNT_E_DEVICE_INVALIDATED:
        case E_POINTER:
        case E_INVALIDARG:
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
// Also rejects a header overstating its data: the declared data range
// must fit inside the actual file, otherwise the file is truncated and
// this returns false so Create() fails cleanly before any audio setup.
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

        if (std::memcmp(chunkId, "data", 4) == 0) {
            // The declared data range must fit inside the actual file.
            // A header overstating its data means a truncated file; reject
            // it here (same open handle, no extra opens) so Create() fails
            // cleanly. Reads past real EOF return success with 0 bytes, so
            // this cannot be detected reliably at playback time.
            const uint64_t dataEnd =
                static_cast<uint64_t>(pos.QuadPart + 8) + chunkSize;
            LARGE_INTEGER fileSize{};
            if (!GetFileSizeEx(hFile, &fileSize) ||
                dataEnd > static_cast<uint64_t>(fileSize.QuadPart)) {
                CloseHandle(hFile);
                return false;
            }
            outOffset = static_cast<uint64_t>(pos.QuadPart + 8);
            outSize = chunkSize;
            CloseHandle(hFile);
            return true;
        }

        // Skip chunk payload (align to 2 bytes). Widen before adding:
        // chunkSize is uint32_t, so 8 + chunkSize would wrap for a bogus
        // size near UINT32_MAX and misalign every following chunk header.
        pos.QuadPart += static_cast<LONGLONG>(8) +
                        static_cast<LONGLONG>(chunkSize);
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
        // PCM block alignment must match the channel/bit-depth layout used
        // below. A mismatch means the declared sample stride is not
        // channels * bytes-per-sample; reading at the derived stride would
        // play misaligned samples, so reject rather than emit garbage.
        const uint16_t expectedBlockAlign = static_cast<uint16_t>(
            fileFormat.channelCount * (fileFormat.bitsPerSample / 8));
        if (fileFormat.blockAlign != expectedBlockAlign) {
            return core::Result<Player>::Err(
                core::ErrorCode::FormatNotSupported,
                "WAV block alignment does not match its channel and bit "
                "depth");
        }
    }

    // Validate the WAV data chunk before touching audio hardware: an
    // overstated header (truncated file) fails here with a clean error
    // instead of hanging or failing mid-playback. FindDataChunk already
    // rejects a declared range extending beyond the actual file size.
    uint32_t bytesPerFrame =
        fileFormat.channelCount * (fileFormat.bitsPerSample / 8);
    if (!isFlac) {
        if (!FindDataChunk(std::wstring(wavPath), dataChunkOffset,
                           dataChunkSize)) {
            return core::Result<Player>::Err(
                core::ErrorCode::AudioInitFailed,
                "Invalid WAV data chunk (not found or extends beyond end "
                "of file)");
        }
        totalFrames = static_cast<uint32_t>(dataChunkSize / bytesPerFrame);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // A thread must balance every successful CoInitializeEx() with one
    // CoUninitialize(), including a call that returns S_FALSE (already
    // initialized with the same model). RPC_E_CHANGED_MODE means another
    // apartment already owns this thread: COM is usable, but that call did
    // not increment the count, so teardown must not decrement it.
    bool ownsComInit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) {
        hr = S_OK;
    }
    if (FAILED(hr)) {
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "COM initialization failed");
    }
    const auto uninitIfOwned = [&ownsComInit] {
        if (ownsComInit) {
            CoUninitialize();
            ownsComInit = false;
        }
    };

    IMMDeviceEnumerator* pEnumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&pEnumerator));
    if (FAILED(hr) || pEnumerator == nullptr) {
        uninitIfOwned();
        return core::Result<Player>::Err(
            core::ErrorCode::DeviceNotFound,
            "Could not create device enumerator");
    }

    IMMDevice* pDevice = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr) || pDevice == nullptr) {
        pEnumerator->Release();
        uninitIfOwned();
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
        uninitIfOwned();
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
        uninitIfOwned();
        return core::Result<Player>::Err(
            core::ErrorCode::FormatNotSupported,
            std::string("Device does not support this ") + formatKind +
                " format in exclusive mode");
    } else {
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        uninitIfOwned();
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
        uninitIfOwned();
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
            hnsBufferDuration = static_cast<REFERENCE_TIME>(std::llround(
                10000.0 * 1000.0 / static_cast<double>(fileFormat.sampleRate) *
                static_cast<double>(nFrames)));
        }
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        uninitIfOwned();

        // Re-enter COM: the teardown above balanced the CoInitializeEx at
        // the top of Create(), so the re-created device objects below need
        // a fresh apartment (otherwise CoCreateInstance fails with
        // CO_E_NOTINITIALIZED and every device needing buffer-size
        // alignment becomes unusable). When the caller's apartment owns the
        // thread (RPC_E_CHANGED_MODE), nothing was balanced and the re-init
        // is not counted either.
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ownsComInit = SUCCEEDED(hr);
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
            uninitIfOwned();
            return core::Result<Player>::Err(
                core::ErrorCode::AudioInitFailed,
                "Re-init failed after buffer alignment");
        }

        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
        if (FAILED(hr)) {
            pEnumerator->Release();
            uninitIfOwned();
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
            uninitIfOwned();
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
        uninitIfOwned();
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
        uninitIfOwned();
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not get buffer size");
    }

    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    HANDLE hWakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (hEvent == nullptr || hWakeEvent == nullptr) {
        if (hEvent != nullptr) CloseHandle(hEvent);
        if (hWakeEvent != nullptr) CloseHandle(hWakeEvent);
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        uninitIfOwned();
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not create event handles");
    }

    hr = pAudioClient->SetEventHandle(hEvent);
    if (FAILED(hr)) {
        CloseHandle(hEvent);
        CloseHandle(hWakeEvent);
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        uninitIfOwned();
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
        CloseHandle(hWakeEvent);
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        uninitIfOwned();
        return core::Result<Player>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not get IAudioRenderClient");
    }

    // Data chunk offset/size (WAV) were validated above, before any audio
    // setup; FLAC total frames come from STREAMINFO and decoding starts at
    // the stream head.
    Player player(std::wstring(wavPath), fileFormat, totalFrames, sourceKind,
                  flacFormat);
    player.pEnumerator_ = pEnumerator;
    player.pDevice_ = pDevice;
    player.pAudioClient_ = pAudioClient;
    player.pRenderClient_ = pRenderClient;
    player.hEvent_ = hEvent;
    player.hWakeEvent_ = hWakeEvent;
    player.bufferFrameCount_ = bufferFrameCount;
    player.bytesPerFrame_ = bytesPerFrame;
    player.dataChunkOffset_ = dataChunkOffset;
    player.dataChunkSize_ = dataChunkSize;
    player.ownsCom_ = ownsComInit;
    ownsComInit = false; // ownership transferred to the Player
    player.ownerThreadId_ = static_cast<uint32_t>(::GetCurrentThreadId());

    return core::Result<Player>::Ok(std::move(player));
}

core::Status Player::Play() {
    AssertCallingThread();

    // Stop() releases the exclusive-mode client, so this Player cannot be
    // restarted afterwards; callers must Create() a new one. Natural
    // end-of-stream also ends in Stopped but keeps these resources, so the
    // state check below cannot tell the two cases apart.
    if (pAudioClient_ == nullptr || pRenderClient_ == nullptr ||
        hEvent_ == nullptr || hWakeEvent_ == nullptr) {
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Player resources released; create a new Player");
    }

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
    // throwing constructor restores Stopped and reports Err through the
    // normal Result channel so no exception escapes Play(). The
    // join-before-reassign logic above is unchanged.
    state_ = PlaybackState::Playing;
    try {
        renderThread_ = std::thread(&Player::RenderThreadEntry, this);
    } catch (const std::exception& ex) {
        state_ = PlaybackState::Stopped;
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            std::string("Could not start render thread: ") + ex.what());
    } catch (...) {
        state_ = PlaybackState::Stopped;
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Could not start render thread");
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

    // Publish Paused and stop the client while no render pass can be in
    // flight. Taking renderMutex_ before Stop() puts the pause boundary
    // between iterations instead of in the middle of one, so no
    // GetBuffer/ReleaseBuffer can straddle the stopped stream.
    std::lock_guard<std::mutex> renderLock(renderMutex_);

    // Re-check under the lock: the render thread latches Stopped (or
    // DeviceLost) under this same lock when the stream ends. If it won that
    // race after the check above, publishing Paused here would leave a
    // paused player with no render thread, and Resume() could never revive
    // it: Start() would succeed and the state would read Playing while no
    // thread ever fills the buffer again.
    if (state_.load() != PlaybackState::Playing) {
        return core::Result<void>::Err(
            core::ErrorCode::AudioInitFailed,
            "Player not in Playing state");
    }
    state_ = PlaybackState::Paused;

    IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
    pAc->Stop();

    // Wake a thread that was blocked in the Playing wait so it observes
    // Paused promptly. This is the control event, never the audio engine's
    // buffer event.
    if (hWakeEvent_) {
        SetEvent(reinterpret_cast<HANDLE>(hWakeEvent_));
    }

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

    // Wake the render thread out of the Paused poll. Do NOT touch hEvent_:
    // if the engine signaled a buffer while paused (including around
    // Start()), that notification must stay pending so the next Playing
    // wait consumes it and fills the buffer. Setting the engine event here
    // instead could coalesce with that notification, leaving the stream
    // with an empty buffer and no further event — the post-Resume stall.
    if (hWakeEvent_) {
        SetEvent(reinterpret_cast<HANDLE>(hWakeEvent_));
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
        // Lock order is always positionMutex_ -> FlacReader. While Paused
        // the render thread only polls (it never decodes), so repositioning
        // needs no Pause()/Resume() pair and the state stays Paused.
        const bool wasPaused = (state_.load() == PlaybackState::Paused);
        if (!wasPaused) {
            auto pauseStatus = Pause();
            if (pauseStatus.IsErr()) {
                return pauseStatus;
            }
        }
        {
            std::lock_guard<std::mutex> lock(positionMutex_);
            // libFLAC rejects seek_absolute(sample >= total_samples), but
            // totalFrames_ is a legitimate end position here: the seek bar's
            // maximum maps to it, and no frame is decoded there. The next
            // RenderIteration sees zero remaining and ends the stream, so
            // skip the decoder seek instead of turning "seek to end" into an
            // error. A later backwards seek repositions the decoder and
            // clears its buffer anyway.
            if (frameOffset < totalFrames_ && !flacReader_.Seek(frameOffset)) {
                // A failed seek leaves libFLAC flushed at an indeterminate
                // file position. Publishing frameOffset here would make
                // GetPosition() disagree with the audio decoded after a
                // later Resume(), so restore the last known-good frame
                // instead. FlacReader::Seek flushes on failure, which puts
                // the decoder back in a state seek_absolute accepts.
                if (!flacReader_.Seek(currentFrame_)) {
                    // Neither the target nor the previous frame is
                    // reachable: the decoder is unusable. End the stream
                    // rather than leave a Paused player that would resume
                    // at an unknown position. Pause() already stopped the
                    // client, and the render thread exits its poll on the
                    // next state check; Stop() then joins it.
                    state_ = PlaybackState::Stopped;
                    return core::Result<void>::Err(
                        core::ErrorCode::AudioInitFailed,
                        "FLAC seek failed; stream stopped");
                }
                // Decoder is back at the unchanged currentFrame_; the
                // stream stays paused so the caller can Resume() or Stop().
                return core::Result<void>::Err(
                    core::ErrorCode::AudioInitFailed,
                    "FLAC seek failed");
            }
            currentFrame_ = frameOffset;
        }
        if (!wasPaused) {
            auto resumeStatus = Resume();
            if (resumeStatus.IsErr()) {
                return resumeStatus;
            }
        }

        return core::Result<void>::Ok();
    }

    frameOffset = std::min(frameOffset, totalFrames_);

    // Pause, reposition, resume (same serialization as FLAC above).
    // Repositions the file handle as well: filePosition_ alone is not
    // enough because ReadFrames reads sequentially from the handle.
    // Same Paused shortcut as FLAC: the render thread is polling, not
    // reading, so skip Pause()/Resume() and stay Paused.
    const bool wasPaused = (state_.load() == PlaybackState::Paused);
    if (!wasPaused) {
        auto pauseStatus = Pause();
        if (pauseStatus.IsErr()) {
            return pauseStatus;
        }
    }
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
    if (!wasPaused) {
        auto resumeStatus = Resume();
        if (resumeStatus.IsErr()) {
            return resumeStatus;
        }
    }

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
        // Signaling the control event wakes a thread blocked in either wait
        // promptly instead of waiting out the full timeout. Cleanup below
        // (releases, CoUninitialize) is unchanged.
        const PlaybackState stopFrom = state_.load();
        if (stopFrom == PlaybackState::Playing ||
            stopFrom == PlaybackState::Paused) {
            state_.store(PlaybackState::Stopped);
        }
        if (hWakeEvent_) {
            SetEvent(reinterpret_cast<HANDLE>(hWakeEvent_));
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

    if (hWakeEvent_) {
        CloseHandle(hWakeEvent_);
        hWakeEvent_ = nullptr;
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

uint32_t Player::GetPosition() const {
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

    // Fill the first buffer before starting. A false result means the
    // stream already ended (or hit a terminal buffer error) before Start():
    // RenderIteration latched Stopped/DeviceLost under renderMutex_ and the
    // client must not be started.
    if (!RenderIteration()) {
        return;
    }

    IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
    {
        // Serialize Start with Pause(): Pause() publishes Paused and stops
        // the client under renderMutex_. If it lands between the first fill
        // and this call, starting here would leave the client running while
        // the player is Paused. Resume() starts the client itself.
        std::lock_guard<std::mutex> renderLock(renderMutex_);
        if (state_.load() == PlaybackState::Playing) {
            HRESULT hr = pAc->Start();
            if (FAILED(hr)) {
                // A dead device or dead audio service at startup must stay
                // DeviceLost, distinct from a clean Stopped exit (same rule
                // as the loop-exit preservation below).
                if (IsDeviceGone(hr) ||
                    state_.load() == PlaybackState::DeviceLost) {
                    HandleDeviceLost();
                } else {
                    state_ = PlaybackState::Stopped;
                }
                return;
            }
        }
    }

    // hEvent_ is signaled by the audio engine when a buffer is ready (and
    // on device invalidation). hWakeEvent_ is this Player's private control
    // event for Pause()/Resume()/Stop(). They must stay separate: if the
    // Paused poll waited on hEvent_, it could consume a buffer
    // notification that arrives around Stop/Start, and an exclusive-mode
    // event-driven stream left with no notification and an empty buffer
    // never asks for data again (playback stalls after Resume()).
    HANDLE waitHandles[2] = {
        reinterpret_cast<HANDLE>(hEvent_),
        reinterpret_cast<HANDLE>(hWakeEvent_)};

    while (true) {
        const PlaybackState loopState = state_.load();
        if (loopState == PlaybackState::Stopped ||
            loopState == PlaybackState::DeviceLost) {
            break;
        }
        if (loopState == PlaybackState::Paused) {
            // Intentional pause: the client is stopped, so hEvent_ may or
            // may not have a notification pending. Do not consume it here;
            // leaving it pending is what lets Resume() restart the stream
            // without losing a buffer. Wait only on the control event, with
            // a timeout so unplug-while-paused still lands in DeviceLost.
            WaitForSingleObject(waitHandles[1], 100);
            UINT32 pad = 0;
            const HRESULT qhr = pAc->GetCurrentPadding(&pad);
            if (IsDeviceGone(qhr)) {
                HandleDeviceLost();
                break;
            }
            continue;
        }

        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 2000);

        if (waitResult == WAIT_OBJECT_0 + 1) {
            // Control wake (Pause/Resume/Stop): re-check the state at the
            // top instead of touching the buffer.
            continue;
        }

        if (waitResult == WAIT_OBJECT_0) {
            // Pause()/Stop() may have raced with the signal; re-check
            // before touching the buffer so a pause is never rendered
            // past and a stop is never mistaken for data.
            const PlaybackState signaledState = state_.load();
            if (signaledState == PlaybackState::Paused) {
                // Pause() won the race after this auto-reset buffer event
                // was consumed. The notification is one-shot: the engine
                // will not signal again for this buffer, and a later
                // Resume() would start the client and then wait for a
                // buffer event that was already consumed here, leaving the
                // stream Playing with no render pass ever running again.
                // Put the notification back so the next Playing wait
                // services it. The paused poll never waits on hEvent_, so
                // it stays pending across the pause.
                SetEvent(reinterpret_cast<HANDLE>(hEvent_));
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
        // client (no more events) and sets Paused; Stop() stores Stopped
        // and signals the control event before joining. Re-check state
        // first.
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

    // Every exit above either preserved a terminal state already latched by
    // Stop()/Pause() (or the device-loss paths), or came from a
    // RenderIteration() that latched Stopped/DeviceLost itself while still
    // holding renderMutex_. Nothing is left to drain here.
}

Player::~Player() {
    AssertCallingThread();
    // Destructors cannot report failures; Stop() is best-effort cleanup here.
    (void)Stop();
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
        audioError_ = false;
        return;
    }
    hFile_ = newFile;
    audioError_ = false;

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
    audioError_ = false;
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

    const uint32_t bytesRequested = framesToRead * bytesPerFrame_;
    DWORD bytesRead = 0;
    if (!ReadFile(hFile_, buffer, bytesRequested, &bytesRead, nullptr)) {
        audioError_ = true;
        return 0;
    }
    // A short read means the file ended before the data chunk Create()
    // validated (it shrank after that check). Reading on would replay
    // misaligned partial frames or, at a clean EOF, spin forever writing
    // silence while more stays true. Flag it like any other read failure.
    if (bytesRead != bytesRequested) {
        audioError_ = true;
    }

    filePosition_ += bytesRead;
    return bytesRead / bytesPerFrame_;
}

bool Player::RenderIteration() {
    std::lock_guard<std::mutex> renderLock(renderMutex_);

    // Pause() holds this same lock across its state store and Stop(), so an
    // iteration cannot start after Paused is published and cannot straddle
    // the stopped stream. Returning true keeps the caller's loop alive; it
    // observes Paused on its next top-of-loop check.
    if (state_.load() != PlaybackState::Playing) {
        return true;
    }

    IAudioRenderClient* pRC =
        reinterpret_cast<IAudioRenderClient*>(pRenderClient_);

    BYTE* pData = nullptr;
    HRESULT hr = pRC->GetBuffer(bufferFrameCount_, &pData);
    if (FAILED(hr) || pData == nullptr) {
        FinishStreamLocked(IsDeviceGone(hr));
        return false;
    }

    // Zero out buffer (silence for EOF). Widen before multiplying so the
    // byte count cannot wrap in 32 bits.
    std::memset(pData, 0,
                static_cast<size_t>(bufferFrameCount_) * bytesPerFrame_);

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

        // Distinguish "stream ended" from "decode/read error": when
        // ReadFrames returns 0 without advancing currentFrame_, the
        // natural more = (currentFrame_ < totalFrames_) would still be
        // true and the render loop would spin forever writing silence.
        // That happens on a flagged read/decode error, and also when a
        // FLAC stream reaches a clean EOS before its STREAMINFO-declared
        // total_samples (overstated header / truncated frames). Treat both
        // as end-of-stream so the loop exits cleanly.
        const bool sourceEnded =
            sourceKind_ == SourceKind::Flac
                ? (flacReader_.HadError() || flacReader_.ReachedEnd())
                : audioError_;
        if (framesRead == 0 && sourceEnded) {
            more = false;
        }
    }

    DWORD flags = 0;
    if (framesRead == 0) {
        flags = AUDCLNT_BUFFERFLAGS_SILENT;
    }

    // Exclusive-mode, event-driven contract
    // (IAudioRenderClient::ReleaseBuffer): NumFramesWritten must equal
    // NumFramesRequested (bufferFrameCount_) from the preceding GetBuffer;
    // a short count fails with AUDCLNT_E_BUFFER_SIZE_ERROR. The buffer is
    // already pre-silenced via memset above, so releasing the full size
    // pads a partial tail with silence. flags is unchanged: SILENT exactly
    // when framesRead == 0.
    const UINT32 numFramesWritten = bufferFrameCount_;
    assert(numFramesWritten == bufferFrameCount_);
    assert(framesRead <= bufferFrameCount_);

    hr = pRC->ReleaseBuffer(numFramesWritten, flags);
    if (FAILED(hr)) {
        FinishStreamLocked(IsDeviceGone(hr));
        return false;
    }

    if (!more) {
        // End of stream. Latch the terminal state while still holding
        // renderMutex_: Pause() publishes Paused under this same lock, so
        // it cannot land between this last render pass and the state store.
        // Otherwise a Pause() racing the end of the track could leave the
        // player Paused with no render thread and no way for Resume() to
        // restart the buffer-event stream.
        FinishStreamLocked(false);
    }
    return more;
}

void Player::FinishStreamLocked(bool deviceGone) {
    if (deviceGone) {
        // Dead endpoint: do not call into it again, and keep DeviceLost
        // distinct from a clean end of stream.
        state_ = PlaybackState::DeviceLost;
        return;
    }

    // Let the last buffer play before stopping the client so the final
    // audio is not truncated. A concurrent Stop() also stores Stopped and
    // then joins this thread before releasing the client, so the calls
    // below remain safe and Stop() itself is idempotent.
    IAudioClient* pAc = reinterpret_cast<IAudioClient*>(pAudioClient_);
    REFERENCE_TIME hnsPeriod = 0;
    pAc->GetDevicePeriod(nullptr, &hnsPeriod);
    Sleep(static_cast<DWORD>(hnsPeriod / 10000));
    pAc->Stop();

    state_ = PlaybackState::Stopped;
}

void Player::HandleDeviceLost() {
    state_ = PlaybackState::DeviceLost;
}

} // namespace kessoku::audio

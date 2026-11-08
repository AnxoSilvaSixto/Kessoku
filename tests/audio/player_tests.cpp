#include "kessoku/audio/player.h"
#include "kessoku/audio/flac_format.h"
#include "kessoku/audio/wav_format.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <audioclient.h>

#include <FLAC/stream_encoder.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void Check(bool condition, const char* testName, const char* desc,
           int line) {
    if (!condition) {
        ++gFailures;
        fprintf(stderr, "FAIL: %s (%s) at line %d\n", testName, desc, line);
    } else {
        printf("PASS: %s (%s)\n", testName, desc);
    }
}

#define CHECK(cond, desc) Check((cond), __func__, (desc), __LINE__)

// True when Player::Create failed for an environment reason (no default
// device, device rejects this format in exclusive mode, or exclusive mode is
// busy/disallowed) rather than a product regression. Callers print an honest
// SKIP instead of failing the test. Any other code (notably AudioInitFailed)
// is a genuine failure and must still hit CHECK. The code distinction comes
// from player.cpp: DeviceNotFound for enumerator/endpoint failures,
// FormatNotSupported for IsFormatSupported rejection, ExclusiveModeUnavailable
// via HResultToErrorCode for EXCLUSIVE_MODE_NOT_ALLOWED/DEVICE_IN_USE.
bool IsEnvironmentSkipCode(kessoku::core::ErrorCode code) {
    return code == kessoku::core::ErrorCode::DeviceNotFound ||
           code == kessoku::core::ErrorCode::FormatNotSupported ||
           code == kessoku::core::ErrorCode::ExclusiveModeUnavailable;
}

// Minimal WAV file builder: creates a PCM WAV file with given parameters.
bool CreateTestWav(std::wstring_view path, uint32_t sampleRate,
                   uint16_t bitsPerSample, uint16_t channelCount,
                   uint32_t numFrames) {
    HANDLE hFile = CreateFileW(
        path.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    uint32_t bytesPerFrame = channelCount * (bitsPerSample / 8);
    uint32_t dataChunkSize = numFrames * bytesPerFrame;
    uint32_t chunkSize = 36 + dataChunkSize; // RIFF size

    // RIFF header
    char riffHeader[] = "RIFF";
    DWORD chunkSizeBytes = chunkSize;
    char waveHeader[] = "WAVE";
    WriteFile(hFile, riffHeader, 4, nullptr, nullptr);
    WriteFile(hFile, &chunkSizeBytes, 4, nullptr, nullptr);
    WriteFile(hFile, waveHeader, 4, nullptr, nullptr);

    // fmt chunk
    char fmtId[] = "fmt ";
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1; // PCM
    WriteFile(hFile, fmtId, 4, nullptr, nullptr);
    WriteFile(hFile, &fmtSize, 4, nullptr, nullptr);
    WriteFile(hFile, &audioFormat, 2, nullptr, nullptr);
    WriteFile(hFile, &channelCount, 2, nullptr, nullptr);
    WriteFile(hFile, &sampleRate, 4, nullptr, nullptr);
    uint32_t byteRate = sampleRate * bytesPerFrame;
    WriteFile(hFile, &byteRate, 4, nullptr, nullptr);
    uint16_t blockAlign = static_cast<uint16_t>(bytesPerFrame);
    WriteFile(hFile, &blockAlign, 2, nullptr, nullptr);
    WriteFile(hFile, &bitsPerSample, 2, nullptr, nullptr);

    // data chunk header
    char dataId[] = "data";
    WriteFile(hFile, dataId, 4, nullptr, nullptr);
    WriteFile(hFile, &dataChunkSize, 4, nullptr, nullptr);

    // Write silence (zero bytes) for all frames
    std::vector<uint8_t> silence(bytesPerFrame, 0);
    for (uint32_t i = 0; i < numFrames; ++i) {
        WriteFile(hFile, silence.data(), bytesPerFrame, nullptr, nullptr);
    }

    CloseHandle(hFile);
    return true;
}

// Create a temp WAV file and return its path.
std::wstring CreateTempWav(std::wstring_view prefix, uint32_t sampleRate,
                           uint16_t bitsPerSample, uint16_t channels,
                           uint32_t frames) {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, buf);
    if (len == 0 || len > MAX_PATH) return {};

    std::wstring tempDir = std::wstring(buf) + std::wstring(prefix);
    if (!CreateDirectoryW(tempDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }

    std::wstring name = tempDir + L"\\test.wav";
    if (CreateTestWav(name, sampleRate, bitsPerSample, channels, frames)) {
        return name;
    }
    return {};
}

// Clean up a temp WAV file and its parent directory.
void CleanupTempWav(std::wstring_view path) {
    DeleteFileW(path.data());
    std::wstring dir = std::wstring(path).substr(
        0, std::wstring(path).find_last_of(L'\\'));
    RemoveDirectoryW(dir.c_str());
}

// Minimal FLAC file builder using libFLAC: encodes a linear ramp
// (value = baseValue + frameIndex * step, same in every channel) with the
// given parameters. Silence when baseValue == step == 0. The caller must
// keep values inside the bit depth's range. Returns false if the encoder
// rejects the parameters.
bool CreateTestFlac(std::wstring_view path, uint32_t sampleRate,
                    uint32_t bitsPerSample, uint32_t channelCount,
                    uint32_t numFrames, FLAC__int32 baseValue = 0,
                    FLAC__int32 step = 0) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, std::wstring(path).c_str(), L"w+b") != 0 ||
        file == nullptr) {
        return false;
    }

    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (encoder == nullptr) {
        fclose(file);
        return false;
    }

    bool ok = true;
    ok = ok && FLAC__stream_encoder_set_channels(encoder, channelCount) != 0;
    ok = ok &&
         FLAC__stream_encoder_set_bits_per_sample(encoder, bitsPerSample) != 0;
    ok = ok && FLAC__stream_encoder_set_sample_rate(encoder, sampleRate) != 0;
    ok = ok && FLAC__stream_encoder_set_total_samples_estimate(
                     encoder, numFrames) != 0;
    ok = ok && FLAC__stream_encoder_set_compression_level(encoder, 0) != 0;
    if (ok) {
        // On success the encoder owns `file` and closes it on finish().
        ok = FLAC__stream_encoder_init_FILE(encoder, file, nullptr,
                                            nullptr) ==
             FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    }
    if (ok && numFrames > 0) {
        std::vector<FLAC__int32> frame(
            static_cast<size_t>(channelCount) * 4096, 0);
        uint32_t remaining = numFrames;
        uint32_t frameIndex = 0;
        while (ok && remaining > 0) {
            uint32_t chunk = remaining > 4096 ? 4096 : remaining;
            for (uint32_t i = 0; i < chunk; ++i) {
                FLAC__int32 value =
                    baseValue +
                    static_cast<FLAC__int32>(frameIndex + i) * step;
                for (uint32_t c = 0; c < channelCount; ++c) {
                    frame[static_cast<size_t>(i) * channelCount + c] = value;
                }
            }
            ok = FLAC__stream_encoder_process_interleaved(
                     encoder, frame.data(), chunk) != 0;
            frameIndex += chunk;
            remaining -= chunk;
        }
    }
    if (ok) {
        ok = FLAC__stream_encoder_finish(encoder) != 0;
    }
    FLAC__stream_encoder_delete(encoder);
    if (!ok) {
        // Init or encode failed: the encoder never took ownership.
        fclose(file);
        DeleteFileW(std::wstring(path).c_str());
    }
    return ok;
}

// Create a temp FLAC file and return its path.
std::wstring CreateTempFlac(std::wstring_view prefix, uint32_t sampleRate,
                            uint32_t bitsPerSample, uint32_t channels,
                            uint32_t frames, FLAC__int32 baseValue = 0,
                            FLAC__int32 step = 0) {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, buf);
    if (len == 0 || len > MAX_PATH) return {};

    std::wstring tempDir = std::wstring(buf) + std::wstring(prefix);
    if (!CreateDirectoryW(tempDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }

    std::wstring name = tempDir + L"\\test.flac";
    if (CreateTestFlac(name, sampleRate, bitsPerSample, channels, frames,
                       baseValue, step)) {
        return name;
    }
    return {};
}

// Clean up a temp FLAC file and its parent directory.
void CleanupTempFlac(std::wstring_view path) {
    DeleteFileW(path.data());
    std::wstring dir = std::wstring(path).substr(
        0, std::wstring(path).find_last_of(L'\\'));
    RemoveDirectoryW(dir.c_str());
}

// Query the exclusive-mode event-driven buffer size for a PCM format,
// mirroring Player::Create negotiation (IsFormatSupported ->
// GetDevicePeriod -> Initialize -> GetBufferSize, with the
// BUFFER_SIZE_NOT_ALIGNED retry). Returns false when there is no usable
// device/format (caller must SKIP, not FAIL). Releases everything it
// acquires; balances CoInitializeEx. Never assumes a buffer size.
bool QueryExclusiveBufferFrames(uint32_t sampleRate, uint16_t bitsPerSample,
                                uint16_t channelCount, UINT32& outFrames) {
    outFrames = 0;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        hr = S_OK;
    }
    if (FAILED(hr)) {
        return false;
    }
    const bool didInit = (hr == S_OK);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;

    auto cleanup = [&](bool uninit) {
        if (pAudioClient != nullptr) {
            pAudioClient->Release();
            pAudioClient = nullptr;
        }
        if (pDevice != nullptr) {
            pDevice->Release();
            pDevice = nullptr;
        }
        if (pEnumerator != nullptr) {
            pEnumerator->Release();
            pEnumerator = nullptr;
        }
        if (uninit && didInit) {
            CoUninitialize();
        }
    };

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&pEnumerator));
    if (FAILED(hr) || pEnumerator == nullptr) {
        cleanup(true);
        return false;
    }

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr) || pDevice == nullptr) {
        cleanup(true);
        return false;
    }

    hr = pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&pAudioClient));
    if (FAILED(hr) || pAudioClient == nullptr) {
        cleanup(true);
        return false;
    }

    WAVEFORMATEXTENSIBLE wfx{};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = channelCount;
    wfx.Format.nSamplesPerSec = sampleRate;
    wfx.Format.wBitsPerSample = bitsPerSample;
    wfx.Format.nBlockAlign =
        static_cast<WORD>(channelCount * (bitsPerSample / 8));
    wfx.Format.nAvgBytesPerSec =
        wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = 22;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    wfx.dwChannelMask =
        (channelCount == 1) ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
    wfx.Samples.wValidBitsPerSample = bitsPerSample;

    WAVEFORMATEX* pClosest = nullptr;
    hr = pAudioClient->IsFormatSupported(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        reinterpret_cast<WAVEFORMATEX*>(&wfx), &pClosest);
    if (pClosest != nullptr) {
        CoTaskMemFree(pClosest);
        pClosest = nullptr;
    }
    if (hr != S_OK) {
        cleanup(true);
        return false;
    }

    REFERENCE_TIME hnsMinPeriod = 0;
    hr = pAudioClient->GetDevicePeriod(&hnsMinPeriod, nullptr);
    if (FAILED(hr)) {
        cleanup(true);
        return false;
    }

    REFERENCE_TIME hnsBufferDuration = hnsMinPeriod;
    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsBufferDuration, hnsBufferDuration,
        reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 nFrames = 0;
        hr = pAudioClient->GetBufferSize(&nFrames);
        if (SUCCEEDED(hr)) {
            hnsBufferDuration = static_cast<REFERENCE_TIME>(std::llround(
                10000.0 * 1000.0 / static_cast<double>(sampleRate) *
                static_cast<double>(nFrames)));
        }
        cleanup(true);

        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            hr = S_OK;
        }
        if (FAILED(hr)) {
            return false;
        }
        const bool didInit2 = (hr == S_OK);

        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&pEnumerator));
        if (FAILED(hr) || pEnumerator == nullptr) {
            if (didInit2) {
                CoUninitialize();
            }
            pEnumerator = nullptr;
            return false;
        }
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                 &pDevice);
        if (FAILED(hr) || pDevice == nullptr) {
            if (pEnumerator != nullptr) {
                pEnumerator->Release();
                pEnumerator = nullptr;
            }
            if (didInit2) {
                CoUninitialize();
            }
            pDevice = nullptr;
            return false;
        }
        hr = pDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&pAudioClient));
        if (FAILED(hr) || pAudioClient == nullptr) {
            if (pDevice != nullptr) {
                pDevice->Release();
                pDevice = nullptr;
            }
            if (pEnumerator != nullptr) {
                pEnumerator->Release();
                pEnumerator = nullptr;
            }
            if (didInit2) {
                CoUninitialize();
            }
            pAudioClient = nullptr;
            return false;
        }
        hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            hnsBufferDuration, hnsBufferDuration,
            reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
        if (FAILED(hr)) {
            cleanup(false);
            if (didInit2) {
                CoUninitialize();
            }
            if (didInit) {
                CoUninitialize();
            }
            return false;
        }
        // Fall through to GetBufferSize with both inits outstanding;
        // both are balanced below.
        UINT32 bufferFrames = 0;
        hr = pAudioClient->GetBufferSize(&bufferFrames);
        cleanup(false);
        if (didInit2) {
            CoUninitialize();
        }
        if (didInit) {
            CoUninitialize();
        }
        if (FAILED(hr) || bufferFrames == 0) {
            return false;
        }
        outFrames = bufferFrames;
        return true;
    }

    if (FAILED(hr)) {
        cleanup(true);
        return false;
    }

    UINT32 bufferFrames = 0;
    hr = pAudioClient->GetBufferSize(&bufferFrames);
    cleanup(true);
    if (FAILED(hr) || bufferFrames == 0) {
        return false;
    }
    outFrames = bufferFrames;
    return true;
}

// Test 2c: RIFF puts no size cap on non-fmt chunks. Encoders routinely emit
// a LIST/JUNK/id3 chunk before fmt, and rejecting it by an arbitrary 1MB
// sanity limit refuses a perfectly valid WAV.
void RunBigLeadingChunkTest() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, buf);
    if (len == 0 || len > MAX_PATH) {
        return;
    }
    std::wstring tempDir = std::wstring(buf) + L"\\kessoku_test_player2c_";
    std::wstring name = tempDir + L"\\big_junk.wav";
    if (!CreateDirectoryW(tempDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return;
    }

    HANDLE hFile = CreateFileW(
        name.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(tempDir.c_str());
        return;
    }

    char riffId[] = "RIFF";
    uint32_t riffSize = 0xFFFFFFFF; // not validated by parse
    char waveId[] = "WAVE";
    WriteFile(hFile, riffId, 4, nullptr, nullptr);
    WriteFile(hFile, &riffSize, 4, nullptr, nullptr);
    WriteFile(hFile, waveId, 4, nullptr, nullptr);

    // 1MB + 1 payload bytes (odd) plus its pad byte.
    const uint32_t junkSize = 1024 * 1024 + 1;
    char junkId[] = "JUNK";
    WriteFile(hFile, junkId, 4, nullptr, nullptr);
    WriteFile(hFile, &junkSize, 4, nullptr, nullptr);
    std::vector<uint8_t> junk(junkSize + 1, 0);
    WriteFile(hFile, junk.data(), static_cast<DWORD>(junk.size()), nullptr,
              nullptr);

    char fmtId[] = "fmt ";
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;
    uint16_t channels = 2;
    uint32_t sampleRate = 44100;
    uint32_t byteRate = 176400;
    uint16_t blockAlign = 4;
    uint16_t bitsPerSample = 16;
    WriteFile(hFile, fmtId, 4, nullptr, nullptr);
    WriteFile(hFile, &fmtSize, 4, nullptr, nullptr);
    WriteFile(hFile, &audioFormat, 2, nullptr, nullptr);
    WriteFile(hFile, &channels, 2, nullptr, nullptr);
    WriteFile(hFile, &sampleRate, 4, nullptr, nullptr);
    WriteFile(hFile, &byteRate, 4, nullptr, nullptr);
    WriteFile(hFile, &blockAlign, 2, nullptr, nullptr);
    WriteFile(hFile, &bitsPerSample, 2, nullptr, nullptr);
    CloseHandle(hFile);

    kessoku::audio::WavFormat fmt{};
    std::string err = kessoku::audio::ParseWavFormat(name, fmt);
    CHECK(err.empty(), "WAV with a >1MB leading chunk parses");
    CHECK(fmt.sampleRate == 44100 && fmt.channelCount == 2 &&
              fmt.bitsPerSample == 16,
          "WAV format fields after a >1MB leading chunk");

    DeleteFileW(name.data());
    RemoveDirectoryW(tempDir.c_str());
}

// Test 25: FindDataChunk applied an arbitrary 100MB "sanity" cap to every
// chunk, including 'data'. A 16-bit/44.1kHz stereo WAV passes 100MB after
// ~10 minutes, and 24-bit/96kHz after ~3 minutes, so the cap rejected
// ordinary music files before any audio setup.
void RunLargeDataChunkTest() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, buf);
    if (len == 0 || len > MAX_PATH) {
        return;
    }
    std::wstring tempDir = std::wstring(buf) + L"\\kessoku_test_player25_";
    if (!CreateDirectoryW(tempDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return;
    }
    std::wstring wavPath = tempDir + L"\\long.wav";

    const uint32_t bytesPerFrame = 2 * 2; // 16-bit, 2ch
    const uint32_t dataChunkSize = 100u * 1024u * 1024u + 4u;
    const uint32_t expectedFrames = dataChunkSize / bytesPerFrame;

    HANDLE hFile = CreateFileW(
        wavPath.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        const uint32_t riffSize = 36u + dataChunkSize;

        char riffId[] = "RIFF";
        char waveId[] = "WAVE";
        WriteFile(hFile, riffId, 4, nullptr, nullptr);
        WriteFile(hFile, &riffSize, 4, nullptr, nullptr);
        WriteFile(hFile, waveId, 4, nullptr, nullptr);

        char fmtId[] = "fmt ";
        uint32_t fmtSize = 16;
        uint16_t audioFormat = 1;
        uint16_t channels = 2;
        uint32_t sampleRate = 44100;
        uint32_t byteRate = sampleRate * bytesPerFrame;
        uint16_t blockAlign = static_cast<uint16_t>(bytesPerFrame);
        uint16_t bitsPerSample = 16;
        WriteFile(hFile, fmtId, 4, nullptr, nullptr);
        WriteFile(hFile, &fmtSize, 4, nullptr, nullptr);
        WriteFile(hFile, &audioFormat, 2, nullptr, nullptr);
        WriteFile(hFile, &channels, 2, nullptr, nullptr);
        WriteFile(hFile, &sampleRate, 4, nullptr, nullptr);
        WriteFile(hFile, &byteRate, 4, nullptr, nullptr);
        WriteFile(hFile, &blockAlign, 2, nullptr, nullptr);
        WriteFile(hFile, &bitsPerSample, 2, nullptr, nullptr);

        char dataId[] = "data";
        WriteFile(hFile, dataId, 4, nullptr, nullptr);
        WriteFile(hFile, &dataChunkSize, 4, nullptr, nullptr);

        // Extend to the declared size without writing 100MB of zeros: the
        // parser and Create() only read headers, and NTFS serves the
        // unwritten tail as zeros.
        LARGE_INTEGER finalSize;
        finalSize.QuadPart = 44 + static_cast<LONGLONG>(dataChunkSize);
        SetFilePointerEx(hFile, finalSize, nullptr, FILE_BEGIN);
        SetEndOfFile(hFile);
        CloseHandle(hFile);
    }
    CHECK(!wavPath.empty(), "Create >100MB-data-chunk WAV fixture");

    if (!wavPath.empty()) {
        auto result = kessoku::audio::Player::Create(wavPath);
        if (result.IsErr() &&
            IsEnvironmentSkipCode(result.GetError().code)) {
            printf("SKIP: large-data-chunk test "
                   "(no compatible exclusive-mode audio device)\n");
        } else {
            CHECK(result.IsOk(),
                  "Player::Create accepts a data chunk over 100MB");
        }
        if (result.IsOk()) {
            CHECK(result.Value().GetTotalFrames() == expectedFrames,
                  "Total frames match the >100MB data chunk");
            (void)result.Value().Stop();
        }

        DeleteFileW(wavPath.data());
        RemoveDirectoryW(tempDir.c_str());
    }
}

// Test 26: a PCM WAV whose block alignment disagrees with its channel and
// bit-depth fields cannot be read at the derived sample stride. Playing it
// would emit misaligned samples, so Create() must reject it instead.
void RunBlockAlignMismatchTest() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, buf);
    if (len == 0 || len > MAX_PATH) {
        return;
    }
    std::wstring tempDir = std::wstring(buf) + L"\\kessoku_test_player26_";
    if (!CreateDirectoryW(tempDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return;
    }
    std::wstring wavPath = tempDir + L"\\bad_align.wav";

    HANDLE hFile = CreateFileW(
        wavPath.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        const uint16_t channels = 2;
        const uint16_t bitsPerSample = 16;
        const uint16_t declaredBlockAlign = 8; // correct value is 4
        const uint32_t sampleRate = 44100;
        const uint32_t dataChunkSize = 3200;
        const uint32_t riffSize = 36u + dataChunkSize;

        char riffId[] = "RIFF";
        char waveId[] = "WAVE";
        WriteFile(hFile, riffId, 4, nullptr, nullptr);
        WriteFile(hFile, &riffSize, 4, nullptr, nullptr);
        WriteFile(hFile, waveId, 4, nullptr, nullptr);

        char fmtId[] = "fmt ";
        uint32_t fmtSize = 16;
        uint16_t audioFormat = 1;
        uint32_t byteRate = sampleRate * declaredBlockAlign;
        WriteFile(hFile, fmtId, 4, nullptr, nullptr);
        WriteFile(hFile, &fmtSize, 4, nullptr, nullptr);
        WriteFile(hFile, &audioFormat, 2, nullptr, nullptr);
        WriteFile(hFile, &channels, 2, nullptr, nullptr);
        WriteFile(hFile, &sampleRate, 4, nullptr, nullptr);
        WriteFile(hFile, &byteRate, 4, nullptr, nullptr);
        WriteFile(hFile, &declaredBlockAlign, 2, nullptr, nullptr);
        WriteFile(hFile, &bitsPerSample, 2, nullptr, nullptr);

        char dataId[] = "data";
        WriteFile(hFile, dataId, 4, nullptr, nullptr);
        WriteFile(hFile, &dataChunkSize, 4, nullptr, nullptr);
        std::vector<uint8_t> payload(dataChunkSize, 0);
        WriteFile(hFile, payload.data(), dataChunkSize, nullptr, nullptr);
        CloseHandle(hFile);
    }
    CHECK(!wavPath.empty(), "Create mismatched-blockAlign WAV fixture");

    if (!wavPath.empty()) {
        auto result = kessoku::audio::Player::Create(wavPath);
        CHECK(result.IsErr(),
              "Player::Create rejects a mismatched WAV blockAlign");
        if (result.IsErr()) {
            CHECK(result.GetError().code ==
                      kessoku::core::ErrorCode::FormatNotSupported,
                  "Mismatched blockAlign is FormatNotSupported");
        }

        DeleteFileW(wavPath.data());
        RemoveDirectoryW(tempDir.c_str());
    }
}

// Test 27: Create() must not uninitialize a COM apartment it did not
// initialize. On a thread already initialized as MTA, the internal
// CoInitializeEx() returns RPC_E_CHANGED_MODE without incrementing the
// thread's init count. Stop() must leave that apartment initialized; an
// unconditional CoUninitialize() would consume the caller's own init and
// unbalance the caller's matching CoUninitialize().
void RunComApartmentOwnershipTest() {
    std::wstring wavPath = CreateTempWav(
        L"\\kessoku_test_player27_", 44100, 16, 2, 44100);
    CHECK(!wavPath.empty(), "Create temp WAV for COM ownership test");
    if (wavPath.empty()) {
        return;
    }

    // Requires COM uninitialized on this thread so the MTA init succeeds
    // outright; otherwise the precondition cannot be established.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr != S_OK) {
        printf("SKIP: COM ownership test "
               "(thread already COM-initialized)\n");
        CleanupTempWav(wavPath);
        return;
    }

    auto result = kessoku::audio::Player::Create(wavPath);
    if (result.IsErr()) {
        if (IsEnvironmentSkipCode(result.GetError().code)) {
            printf("SKIP: COM ownership test "
                   "(no compatible exclusive-mode audio device)\n");
        } else {
            CHECK(result.IsOk(),
                  "Player::Create succeeds on an MTA-initialized thread");
        }
    } else {
        auto& player = result.Value();
        CHECK(player.Stop().IsOk(),
              "Stop succeeds for a Player created on an MTA thread");

        APTTYPE apartmentType = APTTYPE_CURRENT;
        APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
        CHECK(SUCCEEDED(CoGetApartmentType(&apartmentType, &qualifier)),
              "Stop() leaves the caller-owned COM apartment initialized");
    }

    // Balance only if COM is still initialized: the buggy path has already
    // consumed this thread's init, and a second CoUninitialize() would be
    // equally unbalanced.
    APTTYPE apartmentType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    if (SUCCEEDED(CoGetApartmentType(&apartmentType, &qualifier))) {
        CoUninitialize();
    }

    CleanupTempWav(wavPath);
}

// Test 28: a FLAC whose STREAMINFO claims more samples than the stream
// actually contains must stop at the real end of stream. The decoder reaches
// a clean EOS while currentFrame_ < totalFrames_; treating that as "more data
// coming" leaves the render loop writing silence in Playing forever.
//
// Patch the 36-bit total-samples field in STREAMINFO. Layout: "fLaC",
// block header (1 byte type + 3-byte length), then STREAMINFO. The packed
// sample-rate/channels/depth/total-samples field starts 10 bytes into
// STREAMINFO, so total_samples occupies file offsets 18..25 (big-endian,
// low 36 bits).
bool PatchFlacTotalSamples(std::wstring_view path, uint64_t totalSamples) {
    HANDLE hFile = CreateFileW(path.data(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    constexpr DWORD kPackedFieldOffset = 18;
    constexpr uint64_t kTotalSamplesMask = 0xFFFFFFFFFULL; // low 36 bits

    uint8_t header[kPackedFieldOffset + 8]{};
    DWORD bytesRead = 0;
    bool ok = false;
    if (ReadFile(hFile, header, sizeof(header), &bytesRead, nullptr) &&
        bytesRead == sizeof(header) &&
        std::memcmp(header, "fLaC", 4) == 0 &&
        (static_cast<unsigned>(header[4]) & 0x7Fu) == 0 /* STREAMINFO */) {
        uint64_t packed = 0;
        for (DWORD i = 0; i < 8; ++i) {
            packed = (packed << uint64_t{8}) |
                     static_cast<uint64_t>(header[kPackedFieldOffset + i]);
        }
        packed = (packed & ~kTotalSamplesMask) |
                 (totalSamples & kTotalSamplesMask);

        uint8_t patched[8]{};
        for (int i = 7; i >= 0; --i) {
            patched[i] = static_cast<uint8_t>(packed & uint64_t{0xFF});
            packed >>= uint64_t{8};
        }

        LARGE_INTEGER offset;
        offset.QuadPart = kPackedFieldOffset;
        ok = SetFilePointerEx(hFile, offset, nullptr, FILE_BEGIN) != 0 &&
             WriteFile(hFile, patched, sizeof(patched), nullptr, nullptr) != 0;
    }

    CloseHandle(hFile);
    return ok;
}

void RunPrematureFlacEndTest() {
    std::wstring flacPath = CreateTempFlac(
        L"\\kessoku_test_player28_", 44100, 16, 2, 44100);
    CHECK(!flacPath.empty(), "Create temp FLAC for premature-end test");
    if (flacPath.empty()) {
        return;
    }

    const bool patched = PatchFlacTotalSamples(flacPath, 88200);
    CHECK(patched, "Patch FLAC STREAMINFO total samples");

    if (patched) {
        auto result = kessoku::audio::Player::Create(flacPath);
        if (result.IsErr() &&
            IsEnvironmentSkipCode(result.GetError().code)) {
            printf("SKIP: premature FLAC end test "
                   "(no compatible exclusive-mode audio device)\n");
        } else {
            CHECK(result.IsOk(),
                  "Player::Create succeeds for patched FLAC");
        }

        if (result.IsOk()) {
            auto& player = result.Value();
            auto playStatus = player.Play();
            CHECK(playStatus.IsOk(), "Play() for premature-end FLAC");

            // One second of real audio against a two-second declared total.
            // With the defect the render loop never leaves Playing.
            bool leftPlaying = false;
            for (int i = 0; i < 100; ++i) {
                if (player.GetState() !=
                    kessoku::audio::PlaybackState::Playing) {
                    leftPlaying = true;
                    break;
                }
                Sleep(50);
            }
            CHECK(leftPlaying,
                  "Player leaves Playing after the real FLAC end");
            if (leftPlaying) {
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Stopped,
                      "Premature FLAC end lands in Stopped");
            }
            (void)player.Stop();
        }

        CleanupTempFlac(flacPath);
    }
}

// Test 29: a WAV truncated after Create() must end playback instead of
// spinning in Playing. Create() validates the data chunk against the file
// size, but the file can shrink before Play() opens it. ReadFile at EOF
// succeeds with zero bytes, which ReadFrames reported as "no frames" without
// flagging an error, so more stayed true and the render loop waited forever
// for samples that no longer exist.
bool TruncateFile(std::wstring_view path, uint64_t newSize) {
    HANDLE hFile = CreateFileW(path.data(), GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(newSize);
    const bool ok = SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN) != 0 &&
                    SetEndOfFile(hFile) != 0;
    CloseHandle(hFile);
    return ok;
}

void RunWavTruncatedAfterCreateTest() {
    std::wstring wavPath = CreateTempWav(
        L"\\kessoku_test_player29_", 44100, 16, 2, 44100);
    CHECK(!wavPath.empty(),
          "Create temp WAV for post-create truncation test");
    if (wavPath.empty()) {
        return;
    }

    auto result = kessoku::audio::Player::Create(wavPath);
    if (result.IsErr() && IsEnvironmentSkipCode(result.GetError().code)) {
        printf("SKIP: post-create truncation test "
               "(no compatible exclusive-mode audio device)\n");
    } else {
        CHECK(result.IsOk(), "Player::Create succeeds before truncation");
    }

    if (result.IsOk()) {
        auto& player = result.Value();
        // Keep the RIFF/fmt/data headers (44 bytes) and drop every sample;
        // Create() has already validated the now-stale data chunk size.
        const bool truncated = TruncateFile(wavPath, 44);
        CHECK(truncated, "Truncate WAV after Create()");

        auto playStatus = player.Play();
        CHECK(playStatus.IsOk(), "Play() for post-create truncated WAV");

        bool leftPlaying = false;
        for (int i = 0; i < 60; ++i) {
            if (player.GetState() !=
                kessoku::audio::PlaybackState::Playing) {
                leftPlaying = true;
                break;
            }
            Sleep(50);
        }
        CHECK(leftPlaying,
              "Player leaves Playing after post-create WAV truncation");
        if (leftPlaying) {
            CHECK(player.GetState() ==
                      kessoku::audio::PlaybackState::Stopped,
                  "Truncated WAV lands in Stopped");
        }
        (void)player.Stop();
    }

    CleanupTempWav(wavPath);
}

// Test 30: seeking to the end of a FLAC must succeed. libFLAC rejects
// seek_absolute(sample >= total_samples), but totalFrames is a legitimate
// end position: the seek bar's maximum maps to exactly GetTotalFrames(),
// and no frame is decoded there. Without this, dragging the thumb fully
// right reports "seek failed" and leaves the transport paused, while the
// same position on a WAV is accepted.
void RunFlacSeekToEndTest() {
    std::wstring flacPath = CreateTempFlac(
        L"\\kessoku_test_player30_", 44100, 16, 2, 4410);
    CHECK(!flacPath.empty(), "Create temp FLAC for seek-to-end test");
    if (flacPath.empty()) {
        return;
    }

    auto result = kessoku::audio::Player::Create(flacPath);
    if (result.IsErr() && IsEnvironmentSkipCode(result.GetError().code)) {
        printf("SKIP: FLAC seek-to-end test "
               "(no compatible exclusive-mode audio device)\n");
    } else {
        CHECK(result.IsOk(), "Player::Create for FLAC seek-to-end test");
    }

    if (result.IsOk()) {
        auto& player = result.Value();
        CHECK(player.Play().IsOk(), "Play() for FLAC seek-to-end test");
        Sleep(50);

        const uint32_t total = player.GetTotalFrames();
        CHECK(total > 0, "FLAC total frames is non-zero");

        CHECK(player.Pause().IsOk(), "Pause() before FLAC seek-to-end");
        CHECK(player.GetState() == kessoku::audio::PlaybackState::Paused,
              "FLAC state is Paused before seek-to-end");

        auto seekStatus = player.Seek(total);
        CHECK(seekStatus.IsOk(), "FLAC Seek(totalFrames) succeeds");
        CHECK(player.GetPosition() == total,
              "FLAC position is totalFrames after seek-to-end");
        CHECK(player.GetState() == kessoku::audio::PlaybackState::Paused,
              "FLAC state stays Paused after seek-to-end");

        (void)player.Stop();
    }

    CleanupTempFlac(flacPath);
}

// Test 31: Pause() racing natural end-of-stream must not leave a dead
// paused render thread. Regression: RenderIteration() released renderMutex_
// before the render thread latched Stopped. A Pause() blocked on that mutex
// during the final render pass could publish Paused after the last buffer
// was written; the render thread then observed Paused, skipped its exit
// transition, and ended. Resume() flipped the state back to Playing, but no
// thread existed to fill the buffer again. This is a one-sided guard:
// hitting the interleaving depends on scheduling, but a reported failure is
// always a real dead render thread.
void RunPauseAtEndOfStreamTest() {
    std::wstring wavPath = CreateTempWav(
        L"\\kessoku_test_player31_", 44100, 16, 2, 4410); // 0.1 s
    CHECK(!wavPath.empty(), "Create temp WAV for pause-at-EOS test");
    if (wavPath.empty()) {
        return;
    }

    bool skip = false;
    for (int attempt = 0; attempt < 8 && !skip; ++attempt) {
        auto result = kessoku::audio::Player::Create(wavPath);
        if (result.IsErr()) {
            if (IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: pause-at-EOS test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(), "Player::Create for pause-at-EOS test");
            }
            break;
        }

        auto player = std::move(result.Value());
        if (!player.Play().IsOk()) {
            CHECK(false, "Play() for pause-at-EOS test");
            (void)player.Stop();
            break;
        }

        const uint32_t total = player.GetTotalFrames();
        bool paused = false;

        // Pause once the last ~1024 frames remain. Pause() either wins
        // (state Paused) or loses to end-of-stream (Err with the state
        // already Stopped).
        while (!skip) {
            const auto state = player.GetState();
            if (state == kessoku::audio::PlaybackState::Stopped) {
                break;
            }
            if (state == kessoku::audio::PlaybackState::DeviceLost) {
                printf("SKIP: pause-at-EOS test (device lost)\n");
                skip = true;
                break;
            }
            if (state != kessoku::audio::PlaybackState::Playing) {
                CHECK(false, "Unexpected state before pause at EOS");
                break;
            }
            if (total > 1024 && player.GetPosition() < total - 1024) {
                Sleep(1);
                continue;
            }

            auto pauseStatus = player.Pause();
            if (pauseStatus.IsOk()) {
                paused = true;
                break;
            }
            if (player.GetState() == kessoku::audio::PlaybackState::Stopped) {
                break;
            }
            if (player.GetState() ==
                kessoku::audio::PlaybackState::DeviceLost) {
                printf("SKIP: pause-at-EOS test (device lost)\n");
                skip = true;
                break;
            }
            CHECK(false, "Pause() near EOS fails unexpectedly");
            break;
        }

        if (paused) {
            CHECK(player.GetState() == kessoku::audio::PlaybackState::Paused,
                  "State is Paused after pause near EOS");
            CHECK(player.Resume().IsOk(), "Resume() after pause near EOS");

            // At most ~1024 frames remain, so a live render thread reaches
            // Stopped well inside this window. The dead render thread left
            // by the old race stays Playing.
            bool stopped = false;
            for (int i = 0; i < 100; ++i) {
                const auto state = player.GetState();
                if (state == kessoku::audio::PlaybackState::Stopped) {
                    stopped = true;
                    break;
                }
                if (state == kessoku::audio::PlaybackState::DeviceLost) {
                    printf("SKIP: pause-at-EOS test (device lost)\n");
                    skip = true;
                    break;
                }
                Sleep(20);
            }
            CHECK(stopped || skip,
                  "Track reaches Stopped after Resume near EOS");
        }

        (void)player.Stop();
    }

    CleanupTempWav(wavPath);
}

// Test 32: a failed FLAC seek must not move the reported position. libFLAC
// flushes the decoder to an indeterminate file position when seek_absolute()
// fails, so publishing the requested target in currentFrame_ would make
// GetPosition() disagree with the audio a later Resume() decodes (or with
// the clean end-of-stream the flushed decoder hits). The player must restore
// the last known-good frame instead and report failure with the position
// unchanged.
void RunFlacSeekFailureTest() {
    std::wstring flacPath = CreateTempFlac(
        L"\\kessoku_test_player32_", 44100, 16, 2, 441000);
    CHECK(!flacPath.empty(), "Create temp FLAC for seek-failure test");
    if (flacPath.empty()) {
        return;
    }

    // Keep STREAMINFO (still declaring 441000 samples) and roughly half the
    // audio. Seeking to sample 400000 then has no frame to land on inside
    // the surviving data and fails deterministically.
    uint64_t fileSize = 0;
    {
        HANDLE hFile = CreateFileW(flacPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size;
            if (GetFileSizeEx(hFile, &size)) {
                fileSize = static_cast<uint64_t>(size.QuadPart);
            }
            CloseHandle(hFile);
        }
    }
    CHECK(fileSize > 1024, "FLAC file is large enough to truncate");
    if (fileSize > 1024) {
        CHECK(TruncateFile(flacPath, fileSize / 2),
              "Truncate FLAC after its STREAMINFO block");
    }

    auto result = kessoku::audio::Player::Create(flacPath);
    if (result.IsErr() && IsEnvironmentSkipCode(result.GetError().code)) {
        printf("SKIP: FLAC seek-failure test "
               "(no compatible exclusive-mode audio device)\n");
    } else {
        CHECK(result.IsOk(), "Player::Create for FLAC seek-failure test");
    }

    if (result.IsOk()) {
        auto& player = result.Value();
        CHECK(player.Play().IsOk(), "Play() for FLAC seek-failure test");
        Sleep(50);
        CHECK(player.Pause().IsOk(), "Pause() before failed FLAC seek");

        const uint32_t total = player.GetTotalFrames();
        const uint32_t posBefore = player.GetPosition();
        CHECK(total == 441000, "STREAMINFO total survives truncation");
        CHECK(posBefore < 400000, "Playback position is before the cut");

        auto seekStatus = player.Seek(400000);
        CHECK(seekStatus.IsErr(), "Seek past the truncated audio fails");
        CHECK(player.GetPosition() == posBefore,
              "Failed seek leaves the reported position unchanged");
        CHECK(player.GetState() == kessoku::audio::PlaybackState::Paused,
              "Player stays Paused after a failed seek");

        // Restoring the reported number is not enough: the decoder itself
        // must be back at posBefore, so playback has to advance from there.
        CHECK(player.Resume().IsOk(), "Resume() after failed FLAC seek");
        bool advanced = false;
        for (int i = 0; i < 80; ++i) {
            if (player.GetState() != kessoku::audio::PlaybackState::Playing) {
                break;
            }
            if (player.GetPosition() > posBefore) {
                advanced = true;
                break;
            }
            Sleep(25);
        }
        CHECK(advanced, "Playback advances from the restored position");

        (void)player.Stop();
    }

    CleanupTempFlac(flacPath);
}

} // namespace

int main() {
    std::wprintf(L"=== Player Tests ===\n\n");

    // --- Test 1: Create Player from a valid WAV file ---
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player1_", 44100, 16, 2, 44100); // 1 second
        CHECK(!wavPath.empty(), "Create temp WAV file");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: WAV lifecycle test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(),
                      "Player::Create succeeds for valid WAV");
            }

            if (result.IsOk()) {
                auto& player = result.Value();
                CHECK(player.GetSampleRate() == 44100,
                      "Sample rate matches WAV file");
                CHECK(player.GetTotalFrames() == 44100,
                      "Total frames matches WAV file");
                CHECK(player.GetPosition() == 0,
                      "Initial position is 0");
                CHECK(player.GetState() == kessoku::audio::PlaybackState::Stopped,
                      "Initial state is Stopped");

                (void)player.Stop();
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 2: WAV format parse rejects non-PCM ---
    {
        // Create a WAV with non-PCM format (MP3 = 0x55)
        wchar_t buf[MAX_PATH]{};
        DWORD len = GetTempPathW(MAX_PATH, buf);
        if (len > 0 && len <= MAX_PATH) {
            std::wstring tempDir = std::wstring(buf) + L"\\kessoku_test_player2_";
            std::wstring name = tempDir + L"\\test.wav";
            if (CreateDirectoryW(tempDir.c_str(), nullptr) ||
                GetLastError() == ERROR_ALREADY_EXISTS) {

                HANDLE hFile = CreateFileW(
                    name.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    char riffHeader[] = "RIFF";
                    uint32_t chunkSize = 40;
                    char waveHeader[] = "WAVE";
                    WriteFile(hFile, riffHeader, 4, nullptr, nullptr);
                    WriteFile(hFile, &chunkSize, 4, nullptr, nullptr);
                    WriteFile(hFile, waveHeader, 4, nullptr, nullptr);

                    char fmtId[] = "fmt ";
                    uint32_t fmtSize = 16;
                    uint16_t audioFormat = 0x55; // non-PCM
                    WriteFile(hFile, fmtId, 4, nullptr, nullptr);
                    WriteFile(hFile, &fmtSize, 4, nullptr, nullptr);
                    WriteFile(hFile, &audioFormat, 2, nullptr, nullptr);
                    uint16_t channels = 2;
                    WriteFile(hFile, &channels, 2, nullptr, nullptr);
                    uint32_t sampleRate = 44100;
                    WriteFile(hFile, &sampleRate, 4, nullptr, nullptr);
                    uint32_t byteRate = 176400;
                    WriteFile(hFile, &byteRate, 4, nullptr, nullptr);
                    uint16_t blockAlign = 4;
                    WriteFile(hFile, &blockAlign, 2, nullptr, nullptr);
                    uint16_t bitsPerSample = 16;
                    WriteFile(hFile, &bitsPerSample, 2, nullptr, nullptr);

                    char dataId[] = "data";
                    uint32_t dataSize = 0;
                    WriteFile(hFile, dataId, 4, nullptr, nullptr);
                    WriteFile(hFile, &dataSize, 4, nullptr, nullptr);

                    CloseHandle(hFile);

                    kessoku::audio::WavFormat fmt;
                    std::string err = kessoku::audio::ParseWavFormat(name, fmt);
                    CHECK(!err.empty(), "Non-PCM WAV format rejected");

                    DeleteFileW(name.data());
                    RemoveDirectoryW(tempDir.c_str());
                }
            }
        }
    }

    // --- Test 2b: odd-sized RIFF chunks are padded before the next header ---
    {
        wchar_t buf[MAX_PATH]{};
        DWORD len = GetTempPathW(MAX_PATH, buf);
        if (len > 0 && len <= MAX_PATH) {
            std::wstring tempDir =
                std::wstring(buf) + L"\\kessoku_test_player2b_";
            std::wstring name = tempDir + L"\\odd_chunk.wav";
            if (CreateDirectoryW(tempDir.c_str(), nullptr) ||
                GetLastError() == ERROR_ALREADY_EXISTS) {
                HANDLE hFile = CreateFileW(
                    name.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    char riffId[] = "RIFF";
                    uint32_t riffSize = 4 + (8 + 4) + (8 + 16);
                    char waveId[] = "WAVE";
                    WriteFile(hFile, riffId, 4, nullptr, nullptr);
                    WriteFile(hFile, &riffSize, 4, nullptr, nullptr);
                    WriteFile(hFile, waveId, 4, nullptr, nullptr);

                    // Three payload bytes plus the mandatory pad byte.
                    char junkId[] = "JUNK";
                    uint32_t junkSize = 3;
                    char junkPayload[] = { 'x', 'y', 'z', 0 };
                    WriteFile(hFile, junkId, 4, nullptr, nullptr);
                    WriteFile(hFile, &junkSize, 4, nullptr, nullptr);
                    WriteFile(hFile, junkPayload, 4, nullptr, nullptr);

                    char fmtId[] = "fmt ";
                    uint32_t fmtSize = 16;
                    uint16_t audioFormat = 1;
                    uint16_t channels = 2;
                    uint32_t sampleRate = 44100;
                    uint32_t byteRate = 176400;
                    uint16_t blockAlign = 4;
                    uint16_t bitsPerSample = 16;
                    WriteFile(hFile, fmtId, 4, nullptr, nullptr);
                    WriteFile(hFile, &fmtSize, 4, nullptr, nullptr);
                    WriteFile(hFile, &audioFormat, 2, nullptr, nullptr);
                    WriteFile(hFile, &channels, 2, nullptr, nullptr);
                    WriteFile(hFile, &sampleRate, 4, nullptr, nullptr);
                    WriteFile(hFile, &byteRate, 4, nullptr, nullptr);
                    WriteFile(hFile, &blockAlign, 2, nullptr, nullptr);
                    WriteFile(hFile, &bitsPerSample, 2, nullptr, nullptr);
                    CloseHandle(hFile);

                    kessoku::audio::WavFormat fmt{};
                    std::string err =
                        kessoku::audio::ParseWavFormat(name, fmt);
                    CHECK(err.empty(),
                          "WAV with odd-sized leading chunk parses");
                    CHECK(fmt.sampleRate == 44100 && fmt.channelCount == 2 &&
                              fmt.bitsPerSample == 16,
                          "WAV format fields after an odd-sized chunk");

                    DeleteFileW(name.data());
                    RemoveDirectoryW(tempDir.c_str());
                }
            }
        }
    }

    // --- Test 2c: a leading non-fmt chunk larger than 1MB is legal ---
    RunBigLeadingChunkTest();

    // --- Test 3: Play → Pause → Resume → Stop state transitions ---
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player3_", 22050, 16, 1, 22050); // 1 second mono
        CHECK(!wavPath.empty(), "Create temp WAV for state test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: WAV state test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(), "Player::Create for state test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                // Initial state: Stopped
                CHECK(player.GetState() == kessoku::audio::PlaybackState::Stopped,
                      "Initial state is Stopped");

                // Play
                auto playStatus = player.Play();
                CHECK(playStatus.IsOk(), "Play() succeeds");

                // Give it a moment to start
                Sleep(100);

                // State should be Playing
                CHECK(player.GetState() == kessoku::audio::PlaybackState::Playing,
                      "State is Playing after Play()");

                // Pause
                auto pauseStatus = player.Pause();
                CHECK(pauseStatus.IsOk(), "Pause() succeeds");
                CHECK(player.GetState() == kessoku::audio::PlaybackState::Paused,
                      "State is Paused after Pause()");

                // Resume
                auto resumeStatus = player.Resume();
                CHECK(resumeStatus.IsOk(), "Resume() succeeds");
                CHECK(player.GetState() == kessoku::audio::PlaybackState::Playing,
                      "State is Playing after Resume()");

                // Stop
                auto stopStatus = player.Stop();
                CHECK(stopStatus.IsOk(), "Stop() succeeds");
                CHECK(player.GetState() == kessoku::audio::PlaybackState::Stopped,
                      "State is Stopped after Stop()");
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 4: GetPosition reports non-zero during playback ---
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player4_", 44100, 16, 2, 44100);
        CHECK(!wavPath.empty(), "Create temp WAV for position test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: WAV position test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(), "Player::Create for position test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                auto playStatus = player.Play();
                CHECK(playStatus.IsOk(), "Play() for position test");

                // Wait for some playback
                Sleep(200);

                uint32_t pos = player.GetPosition();
                CHECK(pos > 0, "Position is non-zero during playback");
                CHECK(pos <= 44100, "Position is within total frames");

                (void)player.Stop();
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 5: Seek repositions playback ---
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player5_", 44100, 16, 2, 44100);
        CHECK(!wavPath.empty(), "Create temp WAV for seek test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: WAV seek test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(), "Player::Create for seek test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                auto playStatus = player.Play();
                CHECK(playStatus.IsOk(), "Play() for seek test");

                Sleep(100);

                uint32_t posBefore = player.GetPosition();
                CHECK(posBefore <= 44100,
                      "Position before seek is within total frames");

                // Seek to frame 10000
                auto seekStatus = player.Seek(10000);
                CHECK(seekStatus.IsOk(), "Seek() succeeds");
                // Seek() auto-resumes when called while Playing, so the
                // render thread may legitimately advance past the target
                // before this read. Exactness is asserted by the paused-seek
                // tests; here only the reposition itself is required.
                const uint32_t posAfterSeek = player.GetPosition();
                CHECK(posAfterSeek >= 10000,
                      "Position is at least 10000 after Seek(10000)");
                CHECK(posAfterSeek < player.GetTotalFrames(),
                      "Position is within total frames after Seek(10000)");

                (void)player.Stop();
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 6: Device loss state transition ---
    // GAP (explicit, per test-fidelity constraints): there is currently no
    // way to drive Player into PlaybackState::DeviceLost deterministically.
    // HandleDeviceLost() is private, Player exposes no test hook, friend, or
    // HRESULT-injection seam, and the only production triggers are real device
    // invalidation (AUDCLNT_E_DEVICE_INVALIDATED / SERVICE_NOT_RUNNING on the
    // render thread) — i.e. real hardware manipulation. The previous
    // enum-distinctness CHECKs were true by construction for a plain enum
    // class regardless of anything Player does at runtime, so they have been
    // removed rather than left in place uncommented. Deterministic coverage of
    // the DeviceLost transition needs a production-code seam (e.g. a
    // test-only entry point that invokes the device-gone path); adding one is
    // out of scope here — this is reported as a named gap instead.
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player6_", 44100, 16, 2, 44100);
        CHECK(!wavPath.empty(), "Create temp WAV for device loss test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: device-loss test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(), "Player::Create for device loss test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                printf("SKIP: device-loss coverage (no seam to drive Player "
                       "into DeviceLost without real hardware manipulation)\n");

                (void)player.Stop();
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 7: Stop is idempotent ---
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player7_", 44100, 16, 2, 44100);
        CHECK(!wavPath.empty(), "Create temp WAV for idempotent stop test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: idempotent-stop test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(),
                      "Player::Create for idempotent stop test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                // Stop when already stopped should succeed
                auto stopStatus1 = player.Stop();
                CHECK(stopStatus1.IsOk(), "Stop() when already stopped succeeds");

                // Another stop should also succeed
                auto stopStatus2 = player.Stop();
                CHECK(stopStatus2.IsOk(), "Second Stop() succeeds");

                CHECK(player.GetState() == kessoku::audio::PlaybackState::Stopped,
                      "State remains Stopped after double Stop()");

                // Stop() released the exclusive stream, so restarting this
                // Player must fail cleanly rather than touch released state.
                CHECK(player.Play().IsErr(), "Play() after Stop() returns Err");
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 8: Invalid WAV path returns Err ---
    {
        auto result = kessoku::audio::Player::Create(
            L"C:\\this\\path\\does\\not\\exist\\file.wav");
        CHECK(result.IsErr(), "Player::Create fails for nonexistent file");

        if (result.IsErr()) {
            CHECK(result.GetError().code ==
                      kessoku::core::ErrorCode::AudioInitFailed ||
                  result.GetError().code ==
                      kessoku::core::ErrorCode::DeviceNotFound,
                  "Error code is AudioInitFailed or DeviceNotFound");
        }
    }

    // --- Test 9: FLAC STREAMINFO probe extracts the file format ---
    {
        std::wstring flacPath = CreateTempFlac(
            L"\\kessoku_test_flac9_", 44100, 16, 2, 44100); // 1 second
        CHECK(!flacPath.empty(), "Create temp FLAC file");

        if (!flacPath.empty()) {
            kessoku::audio::FlacFormat fmt;
            std::string err =
                kessoku::audio::ParseFlacFormat(flacPath, fmt);
            CHECK(err.empty(), "ParseFlacFormat succeeds for valid FLAC");
            CHECK(fmt.sampleRate == 44100,
                  "FLAC sample rate matches STREAMINFO");
            CHECK(fmt.channelCount == 2,
                  "FLAC channel count matches STREAMINFO");
            CHECK(fmt.bitsPerSample == 16,
                  "FLAC bit depth matches STREAMINFO");
            CHECK(fmt.totalSamples == 44100,
                  "FLAC total samples matches STREAMINFO");

            CleanupTempFlac(flacPath);
        }
    }

    // --- Test 10: FLAC frame packing is exact for 8/16/24/32-bit ---
    {
        // Stereo 16-bit: known samples, interleaved little-endian.
        {
            const int32_t left[] = {0x0102, -2};
            const int32_t right[] = {0x0304, 0x7FFF};
            const int32_t* channels[] = {left, right};
            std::vector<uint8_t> out;
            bool ok = kessoku::audio::AppendFlacFramePcm(out, channels, 2, 2,
                                                        16);
            CHECK(ok, "Pack 16-bit stereo frame");
            const uint8_t expected[] = {0x02, 0x01, 0x04, 0x03,
                                        0xFE, 0xFF, 0xFF, 0x7F};
            CHECK(out.size() == sizeof(expected),
                  "16-bit stereo packed size");
            CHECK(std::memcmp(out.data(), expected, sizeof(expected)) == 0,
                  "16-bit stereo packed bytes exact");
        }
        // Mono 8-bit: signed FLAC samples map to unsigned PCM.
        {
            const int32_t mono[] = {-128, 0, 127};
            const int32_t* channels[] = {mono};
            std::vector<uint8_t> out;
            bool ok = kessoku::audio::AppendFlacFramePcm(out, channels, 1, 3,
                                                        8);
            CHECK(ok, "Pack 8-bit mono frame");
            const uint8_t expected[] = {0x00, 0x80, 0xFF};
            CHECK(out.size() == sizeof(expected), "8-bit mono packed size");
            CHECK(std::memcmp(out.data(), expected, sizeof(expected)) == 0,
                  "8-bit mono packed bytes exact");
        }
        // Stereo 24-bit: low 3 bytes, little-endian.
        {
            const int32_t left[] = {0x010203};
            const int32_t right[] = {-1};
            const int32_t* channels[] = {left, right};
            std::vector<uint8_t> out;
            bool ok = kessoku::audio::AppendFlacFramePcm(out, channels, 2, 1,
                                                        24);
            CHECK(ok, "Pack 24-bit stereo frame");
            const uint8_t expected[] = {0x03, 0x02, 0x01, 0xFF, 0xFF, 0xFF};
            CHECK(out.size() == sizeof(expected),
                  "24-bit stereo packed size");
            CHECK(std::memcmp(out.data(), expected, sizeof(expected)) == 0,
                  "24-bit stereo packed bytes exact");
        }
        // Mono 32-bit: full word, little-endian.
        {
            const int32_t mono[] = {0x01020304};
            const int32_t* channels[] = {mono};
            std::vector<uint8_t> out;
            bool ok = kessoku::audio::AppendFlacFramePcm(out, channels, 1, 1,
                                                        32);
            CHECK(ok, "Pack 32-bit mono frame");
            const uint8_t expected[] = {0x04, 0x03, 0x02, 0x01};
            CHECK(out.size() == sizeof(expected),
                  "32-bit mono packed size");
            CHECK(std::memcmp(out.data(), expected, sizeof(expected)) == 0,
                  "32-bit mono packed bytes exact");
        }
        // Unsupported depth is rejected, never converted.
        {
            const int32_t mono[] = {0};
            const int32_t* channels[] = {mono};
            std::vector<uint8_t> out;
            CHECK(!kessoku::audio::AppendFlacFramePcm(out, channels, 1, 1,
                                                     12),
                  "Pack rejects 12-bit depth");
            CHECK(out.empty(), "Rejected pack appends nothing");
        }
    }

    // --- Test 11: Corrupted/truncated FLAC fails cleanly at Create() ---
    {
        wchar_t buf[MAX_PATH]{};
        DWORD len = GetTempPathW(MAX_PATH, buf);
        if (len > 0 && len <= MAX_PATH) {
            std::wstring tempDir =
                std::wstring(buf) + L"\\kessoku_test_flac11_";
            if (CreateDirectoryW(tempDir.c_str(), nullptr) ||
                GetLastError() == ERROR_ALREADY_EXISTS) {
                // Case A: garbage bytes with a .flac extension.
                std::wstring garbage = tempDir + L"\\garbage.flac";
                HANDLE hFile = CreateFileW(
                    garbage.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    const char junk[] = "this is not a FLAC file at all";
                    WriteFile(hFile, junk, sizeof(junk) - 1, nullptr,
                              nullptr);
                    CloseHandle(hFile);

                    auto result =
                        kessoku::audio::Player::Create(garbage);
                    CHECK(result.IsErr(),
                          "Player::Create fails for garbage FLAC");

                    DeleteFileW(garbage.data());
                }

                // Case B: valid FLAC header truncated mid-stream.
                std::wstring full = tempDir + L"\\full.flac";
                if (CreateTestFlac(full, 44100, 16, 2, 44100)) {
                    std::wstring truncated = tempDir + L"\\truncated.flac";
                    HANDLE hSrc = CreateFileW(
                        full.data(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    HANDLE hDst = CreateFileW(
                        truncated.data(), GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hSrc != INVALID_HANDLE_VALUE &&
                        hDst != INVALID_HANDLE_VALUE) {
                        uint8_t head[64]{};
                        DWORD bytesRead = 0;
                        if (ReadFile(hSrc, head, sizeof(head), &bytesRead,
                                     nullptr) &&
                            bytesRead > 0) {
                            WriteFile(hDst, head, bytesRead, nullptr,
                                      nullptr);
                        }
                    }
                    if (hSrc != INVALID_HANDLE_VALUE) CloseHandle(hSrc);
                    if (hDst != INVALID_HANDLE_VALUE) CloseHandle(hDst);

                    auto result =
                        kessoku::audio::Player::Create(truncated);
                    CHECK(result.IsErr(),
                          "Player::Create fails for truncated FLAC");

                    DeleteFileW(truncated.data());
                    DeleteFileW(full.data());
                }
                RemoveDirectoryW(tempDir.c_str());
            }
        }
    }

    // --- Test 12: FLAC bit depth without an exact PCM container is
    // --- FormatNotSupported (no conversion, no fallback) ---
    {
        std::wstring flacPath = CreateTempFlac(
            L"\\kessoku_test_flac12_", 44100, 12, 2, 4410);
        CHECK(!flacPath.empty(), "Create 12-bit temp FLAC file");

        if (!flacPath.empty()) {
            auto result = kessoku::audio::Player::Create(flacPath);
            CHECK(result.IsErr(),
                  "Player::Create fails for 12-bit FLAC");
            if (result.IsErr()) {
                CHECK(result.GetError().code ==
                          kessoku::core::ErrorCode::FormatNotSupported,
                      "Error code is FormatNotSupported");
            }

            CleanupTempFlac(flacPath);
        }
    }

    // --- Test 13: Create Player from a valid FLAC file ---
    {
        std::wstring flacPath = CreateTempFlac(
            L"\\kessoku_test_flac13_", 44100, 16, 2, 44100); // 1 second
        CHECK(!flacPath.empty(), "Create temp FLAC for lifecycle test");

        if (!flacPath.empty()) {
            auto result = kessoku::audio::Player::Create(flacPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: FLAC lifecycle test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(),
                      "Player::Create succeeds for valid FLAC");
            }

            if (result.IsOk()) {
                auto& player = result.Value();
                CHECK(player.GetSampleRate() == 44100,
                      "FLAC sample rate matches file");
                CHECK(player.GetTotalFrames() == 44100,
                      "FLAC total frames matches file");
                CHECK(player.GetPosition() == 0,
                      "FLAC initial position is 0");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Stopped,
                      "FLAC initial state is Stopped");

                (void)player.Stop();
            }

            CleanupTempFlac(flacPath);
        }
    }

    // --- Test 14: FLAC Play -> Pause -> Resume -> Stop ---
    {
        std::wstring flacPath = CreateTempFlac(
            L"\\kessoku_test_flac14_", 22050, 16, 1, 22050); // 1 second mono
        CHECK(!flacPath.empty(), "Create temp FLAC for state test");

        if (!flacPath.empty()) {
            auto result = kessoku::audio::Player::Create(flacPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: FLAC state test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(), "Player::Create for FLAC state test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Stopped,
                      "FLAC initial state is Stopped");

                auto playStatus = player.Play();
                CHECK(playStatus.IsOk(), "FLAC Play() succeeds");

                Sleep(100);

                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Playing,
                      "FLAC state is Playing after Play()");

                auto pauseStatus = player.Pause();
                CHECK(pauseStatus.IsOk(), "FLAC Pause() succeeds");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Paused,
                      "FLAC state is Paused after Pause()");

                auto resumeStatus = player.Resume();
                CHECK(resumeStatus.IsOk(), "FLAC Resume() succeeds");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Playing,
                      "FLAC state is Playing after Resume()");

                auto stopStatus = player.Stop();
                CHECK(stopStatus.IsOk(), "FLAC Stop() succeeds");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Stopped,
                      "FLAC state is Stopped after Stop()");
            }

            CleanupTempFlac(flacPath);
        }
    }

    // --- Test 15: FLAC Seek repositions playback ---
    {
        std::wstring flacPath = CreateTempFlac(
            L"\\kessoku_test_flac15_", 44100, 16, 2, 44100);
        CHECK(!flacPath.empty(), "Create temp FLAC for seek test");

        if (!flacPath.empty()) {
            auto result = kessoku::audio::Player::Create(flacPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: FLAC seek test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(), "Player::Create for FLAC seek test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                auto playStatus = player.Play();
                CHECK(playStatus.IsOk(), "Play() for FLAC seek test");

                Sleep(100);

                uint32_t posBefore = player.GetPosition();
                CHECK(posBefore <= 44100,
                      "FLAC position before seek is within total frames");

                auto seekStatus = player.Seek(10000);
                CHECK(seekStatus.IsOk(), "FLAC Seek() succeeds");
                // Same async-advance allowance as the WAV seek test above.
                const uint32_t posAfterSeek = player.GetPosition();
                CHECK(posAfterSeek >= 10000,
                      "FLAC position is at least 10000 after Seek(10000)");
                CHECK(posAfterSeek < player.GetTotalFrames(),
                      "FLAC position is within total frames after Seek(10000)");

                (void)player.Stop();
            }

            CleanupTempFlac(flacPath);
        }
    }

    // --- Test 16: FlacReader decodes bit-exact PCM with seek (no device) ---
    {
        // 9000 mono frames span several FLAC blocks, so sub-block reads
        // exercise the FIFO bridging. Ramp values stay inside int16 range.
        const uint32_t kFrames = 9000;
        const FLAC__int32 kBase = -9000;
        const FLAC__int32 kStep = 2;
        std::wstring flacPath = CreateTempFlac(
            L"\\kessoku_test_flac16_", 44100, 16, 1, kFrames, kBase, kStep);
        CHECK(!flacPath.empty(), "Create ramp FLAC file");

        if (!flacPath.empty()) {
            kessoku::audio::FlacFormat fmt;
            CHECK(kessoku::audio::ParseFlacFormat(flacPath, fmt).empty(),
                  "Probe ramp FLAC file");

            auto expectedBytes = [&](uint32_t first, uint32_t count) {
                std::vector<uint8_t> bytes;
                bytes.reserve(static_cast<size_t>(count) * 2);
                for (uint32_t i = 0; i < count; ++i) {
                    FLAC__int32 v =
                        kBase + static_cast<FLAC__int32>(first + i) * kStep;
                    const auto sample = static_cast<uint32_t>(v);
                    bytes.push_back(static_cast<uint8_t>(sample & 0xFFU));
                    bytes.push_back(
                        static_cast<uint8_t>((sample >> 8U) & 0xFFU));
                }
                return bytes;
            };

            kessoku::audio::FlacReader reader;
            CHECK(reader.Open(flacPath, fmt, 0).empty(),
                  "FlacReader::Open succeeds");
            CHECK(reader.IsOpen(), "FlacReader is open");

            std::vector<uint8_t> decoded;
            decoded.reserve(static_cast<size_t>(kFrames) * 2);
            std::vector<uint8_t> chunk(static_cast<size_t>(1000) * 2);
            uint32_t totalRead = 0;
            for (int n = 0; n < 9; ++n) {
                uint32_t got = reader.ReadFrames(chunk.data(), 1000);
                CHECK(got == 1000, "Read 1000 frames per call");
                decoded.insert(decoded.end(), chunk.begin(),
                               chunk.begin() +
                                   static_cast<std::ptrdiff_t>(got) * 2);
                totalRead += got;
            }
            CHECK(totalRead == kFrames, "Read all frames");
            CHECK(reader.ReadFrames(chunk.data(), 1000) == 0,
                  "Read past end returns 0");
            CHECK(!reader.HadError(), "No decode error");
            CHECK(decoded == expectedBytes(0, kFrames),
                  "Decoded PCM is bit-exact");

            CHECK(reader.Seek(500), "FlacReader::Seek(500) succeeds");
            uint32_t got = reader.ReadFrames(chunk.data(), 100);
            CHECK(got == 100, "Read 100 frames after seek");
            CHECK(std::memcmp(chunk.data(),
                              expectedBytes(500, 100).data(), 200) == 0,
                  "Seek(500) position is sample-accurate");

            CHECK(reader.Seek(0), "FlacReader::Seek(0) succeeds");
            got = reader.ReadFrames(chunk.data(), 10);
            CHECK(got == 10, "Read 10 frames after re-seek");
            CHECK(std::memcmp(chunk.data(), expectedBytes(0, 10).data(),
                              20) == 0,
                  "Seek(0) position is sample-accurate");

            reader.Close();
            CHECK(!reader.IsOpen(), "FlacReader closed");

            CleanupTempFlac(flacPath);
        }
    }

    // --- Test 17: Play() after natural end-of-stream without Stop() ---
    // Regression: the finished render thread stays joinable, so a second
    // Play() must join it before spawning a replacement instead of
    // move-assigning over it (std::terminate).
    {
        // 0.1s fixture finishes quickly; polling avoids a racy fixed Sleep.
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player17_", 44100, 16, 2, 4410);
        CHECK(!wavPath.empty(), "Create temp WAV for replay test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: replay test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(), "Player::Create for replay test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                CHECK(player.Play().IsOk(), "First Play() succeeds");

                bool finished = false;
                for (int i = 0; i < 100; ++i) {
                    if (player.GetState() ==
                        kessoku::audio::PlaybackState::Stopped) {
                        finished = true;
                        break;
                    }
                    Sleep(50);
                }
                CHECK(finished, "Track finishes naturally to Stopped");

                auto second = player.Play();
                CHECK(second.IsOk(),
                      "Second Play() after EOS succeeds (no terminate)");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Playing,
                      "State is Playing after replay");

                (void)player.Stop();
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 18: Play() while Paused resumes without spawning ---
    // Regression: renderThread_ is joinable for the whole Paused interval,
    // so Play() must resume the existing thread (like Resume()) instead of
    // move-assigning a new one over it (std::terminate).
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player18_", 44100, 16, 2, 44100);
        CHECK(!wavPath.empty(), "Create temp WAV for paused-replay test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: paused-replay test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(),
                      "Player::Create for paused-replay test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                CHECK(player.Play().IsOk(), "Play() succeeds");
                Sleep(100);
                CHECK(player.Pause().IsOk(), "Pause() succeeds");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Paused,
                      "State is Paused before second Play()");

                auto second = player.Play();
                CHECK(second.IsOk(),
                      "Play() while Paused succeeds (no terminate)");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Playing,
                      "State is Playing after Play() while Paused");

                (void)player.Stop();
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 19: Pause past the 2s buffer-event timeout still resumes ---
    // Regression: the render thread's WaitForSingleObject(hEvent_, 2000)
    // timeout fired on any Pause() held >= 2s (Pause stops the client, so
    // the event never re-signals), declared DeviceLost, and the thread exit
    // unconditionally stored Stopped. Resume() then failed and Play()
    // restarted from frame 0. A 10s fixture guarantees no natural EOS
    // during the 3s paused hold.
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player19_", 44100, 16, 2, 441000);
        CHECK(!wavPath.empty(), "Create temp WAV for long-pause test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: long-pause test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(), "Player::Create for long-pause test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                CHECK(player.Play().IsOk(), "Play() succeeds");
                Sleep(200);
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Playing,
                      "State is Playing before pause");

                CHECK(player.Pause().IsOk(), "Pause() succeeds");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Paused,
                      "State is Paused immediately after Pause()");
                const uint32_t pausedPos = player.GetPosition();

                Sleep(3000);

                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Paused,
                      "State is still Paused after 3s (no false DeviceLost/Stopped)");
                CHECK(player.GetPosition() == pausedPos,
                      "Position unchanged across 3s pause");

                auto resumeStatus = player.Resume();
                CHECK(resumeStatus.IsOk(),
                      "Resume() succeeds after 3s pause");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Playing,
                      "State is Playing after Resume()");
                CHECK(player.GetPosition() >= pausedPos,
                      "Resume continues from paused position (no restart)");
                CHECK(player.GetPosition() != 0 || pausedPos == 0,
                      "Resume did not restart from frame 0");

                // Resuming must restart the buffer-event stream, not just
                // flip the state: the render thread only advances the
                // position when the audio engine signals its event, so a
                // state-only Resume() would leave playback silent forever.
                const uint32_t resumedPos = player.GetPosition();
                Sleep(1000);
                CHECK(player.GetPosition() > resumedPos,
                      "Position advances after Resume()");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Playing,
                      "State is still Playing 1s after Resume()");

                (void)player.Stop();
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 20: Mid-stream FLAC decode error stops playback (hang fix) ---
    // A FLAC file with a corrupted frame mid-stream: the decoder hits bad
    // frame sync / CRC, sets error_, and ReadFrames returns 0. Without the
    // fix, currentFrame_ never advances and the render loop spins forever
    // writing silence. With the fix, HadError() is checked and playback
    // stops like natural EOS.
    {
        std::wstring flacPath = CreateTempFlac(
            L"\\kessoku_test_player20_", 44100, 16, 2, 10000);
        CHECK(!flacPath.empty(), "Create temp FLAC for decode-error test");

        if (!flacPath.empty()) {
            // Corrupt a frame in the middle of the file.
            // FLAC frame data starts after the metadata blocks. We corrupt
            // bytes around the middle of the file to trigger a decode error
            // after several good frames have been decoded.
            HANDLE hCorrupt = CreateFileW(
                flacPath.data(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hCorrupt != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER fileEnd;
                GetFileSizeEx(hCorrupt, &fileEnd);
                // Seek to roughly 60% into the file (past STREAMINFO + first
                // few frames, into the middle of frame data).
                LARGE_INTEGER seekPos;
                seekPos.QuadPart = fileEnd.QuadPart * 3 / 5;
                SetFilePointerEx(hCorrupt, seekPos, nullptr, FILE_BEGIN);
                // Overwrite 4 bytes with invalid frame sync pattern (0xFF 0xFB
                // is a sync pattern that won't match valid FLAC framing).
                unsigned char badBytes[] = { 0xFF, 0xFB, 0x00, 0x00 };
                DWORD written = 0;
                WriteFile(hCorrupt, badBytes, sizeof(badBytes), &written, nullptr);
                CloseHandle(hCorrupt);
            }

            auto result = kessoku::audio::Player::Create(flacPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: decode-error test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(),
                      "Player::Create for decode-error test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                CHECK(player.Play().IsOk(), "Play() succeeds for corrupted FLAC");

                // Poll with a bounded deadline. Without the fix, this hangs
                // forever (test binary never exits). With the fix, the player
                // stops within a second or two.
                bool stopped = false;
                for (int i = 0; i < 40; ++i) {
                    if (player.GetState() ==
                        kessoku::audio::PlaybackState::Stopped) {
                        stopped = true;
                        break;
                    }
                    Sleep(250);
                }
                CHECK(stopped,
                      "Corrupted FLAC stops within deadline (not hang)");

                (void)player.Stop();
            }

            CleanupTempFlac(flacPath);
        }
    }

    // --- Test 21: Truncated WAV (overstated data chunk) fails Create() ---
    // A WAV file whose data-chunk header declares more samples than are
    // actually present in the file. Create() rejects it deterministically
    // with a clean error before any audio setup — no playback, no polling.
    // (Runtime detection is inherently ambiguous here: ReadFile past real
    // EOF returns TRUE with 0 bytes, indistinguishable from a short wait,
    // so the header must be validated up front.)
    {
        // Create a WAV with header declaring 441000 frames (10s) but only
        // write 4410 frames (0.1s) of actual data. The file is truncated.
        wchar_t buf[MAX_PATH]{};
        DWORD len = GetTempPathW(MAX_PATH, buf);
        std::wstring tempDir;
        if (len > 0 && len <= MAX_PATH) {
            tempDir = std::wstring(buf) + L"\\kessoku_test_player21_";
            if (!CreateDirectoryW(tempDir.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                tempDir.clear();
            }
        }
        std::wstring wavPath;
        if (!tempDir.empty()) {
            wavPath = tempDir + L"\\test.wav";

            HANDLE hFile = CreateFileW(
                wavPath.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                uint32_t bytesPerFrame = 2 * 2; // 16-bit, 2ch
                uint32_t declaredDataSize = 441000 * bytesPerFrame; // 10s
                uint32_t chunkSize = 36 + declaredDataSize;

                char riffHeader[] = "RIFF";
                DWORD chunkSizeBytes = chunkSize;
                char waveHeader[] = "WAVE";
                WriteFile(hFile, riffHeader, 4, nullptr, nullptr);
                WriteFile(hFile, &chunkSizeBytes, 4, nullptr, nullptr);
                WriteFile(hFile, waveHeader, 4, nullptr, nullptr);

                char fmtId[] = "fmt ";
                uint32_t fmtSize = 16;
                uint16_t audioFormat = 1;
                WriteFile(hFile, fmtId, 4, nullptr, nullptr);
                WriteFile(hFile, &fmtSize, 4, nullptr, nullptr);
                WriteFile(hFile, &audioFormat, 2, nullptr, nullptr);
                uint16_t channels = 2;
                WriteFile(hFile, &channels, 2, nullptr, nullptr);
                uint32_t sampleRate = 44100;
                WriteFile(hFile, &sampleRate, 4, nullptr, nullptr);
                uint32_t byteRate = sampleRate * bytesPerFrame;
                WriteFile(hFile, &byteRate, 4, nullptr, nullptr);
                uint16_t blockAlign = 4;
                WriteFile(hFile, &blockAlign, 2, nullptr, nullptr);
                uint16_t bitsPerSample = 16;
                WriteFile(hFile, &bitsPerSample, 2, nullptr, nullptr);

                char dataId[] = "data";
                WriteFile(hFile, dataId, 4, nullptr, nullptr);
                WriteFile(hFile, &declaredDataSize, 4, nullptr, nullptr);

                // Only write 0.1s of actual data (4410 frames)
                std::vector<uint8_t> silence(bytesPerFrame, 0);
                for (uint32_t i = 0; i < 4410; ++i) {
                    WriteFile(hFile, silence.data(), bytesPerFrame, nullptr, nullptr);
                }

                CloseHandle(hFile);
            }
        }
        CHECK(!wavPath.empty(), "Create truncated WAV fixture");

        if (!wavPath.empty()) {
            // Deterministic rejection before any WASAPI setup: no device
            // needed, so no environment SKIP applies here.
            auto result = kessoku::audio::Player::Create(wavPath);
            CHECK(result.IsErr(),
                  "Player::Create fails for truncated WAV");
            if (result.IsErr()) {
                CHECK(result.GetError().code ==
                          kessoku::core::ErrorCode::AudioInitFailed,
                      "Error code is AudioInitFailed");
            }

            DeleteFileW(wavPath.data());
            RemoveDirectoryW(tempDir.c_str());
        }
    }

    // --- Test 22: Final partial buffer releases the full buffer size ---
    // Regression: RenderIteration released framesRead (short count) to an
    // exclusive-mode, event-driven stream. Microsoft's ReleaseBuffer
    // contract rejects that with AUDCLNT_E_BUFFER_SIZE_ERROR: the client
    // must release exactly NumFramesRequested from the preceding GetBuffer
    // (the buffer is pre-silenced via memset; only the reported count was
    // wrong). Every track whose length is not an exact multiple of the
    // negotiated buffer lost its final sub-buffer silently. Invisible to
    // state/position testing by construction (position advances before
    // ReleaseBuffer; failure still lands in Stopped), so this test
    // exercises misaligned WAV+FLAC fixtures to natural EOS while the
    // instrumented player log proves the final ReleaseBuffer carries the
    // full buffer size with S_OK. Buffer size is queried the way Player
    // itself does (GetBufferSize); never hardcoded or assumed.
    {
        const uint32_t kRate = 44100;
        const uint16_t kBits = 16;
        const uint16_t kCh = 2;
        UINT32 bufFrames = 0;
        if (!QueryExclusiveBufferFrames(kRate, kBits, kCh, bufFrames) ||
            bufFrames == 0) {
            printf("SKIP: misaligned-tail test "
                   "(no compatible exclusive-mode audio device)\n");
        } else {
            printf("[info] Test22 exclusive buffer=%u frames\n", bufFrames);
            uint32_t tail = bufFrames / 2;
            if (tail == 0) {
                tail = 1;
            }
            const uint32_t total = bufFrames * 2 + tail;
            // Confirm misalignment rather than assuming a size.
            CHECK(total % bufFrames != 0,
                  "Fixture length is NOT a multiple of buffer size");
            printf("[info] Test22 misaligned fixture total=%u "
                   "(total%%buf=%u)\n",
                   total, total % bufFrames);

            // WAV case: misaligned length plays to natural EOS.
            {
                std::wstring wavPath = CreateTempWav(
                    L"\\kessoku_test_player22_wav_", kRate, kBits, kCh,
                    total);
                CHECK(!wavPath.empty(),
                      "Create misaligned WAV fixture");

                if (!wavPath.empty()) {
                    auto result = kessoku::audio::Player::Create(wavPath);
                    if (result.IsErr() &&
                        IsEnvironmentSkipCode(result.GetError().code)) {
                        printf("SKIP: misaligned-WAV test "
                               "(no compatible exclusive-mode audio device)\n");
                    } else {
                        CHECK(result.IsOk(),
                              "Player::Create for misaligned WAV test");
                    }

                    if (result.IsOk()) {
                        auto player = std::move(result.Value());

                        CHECK(player.Play().IsOk(),
                              "Play() succeeds for misaligned WAV");

                        bool finished = false;
                        for (int i = 0; i < 100; ++i) {
                            if (player.GetState() ==
                                kessoku::audio::PlaybackState::Stopped) {
                                finished = true;
                                break;
                            }
                            Sleep(50);
                        }
                        CHECK(finished,
                              "Misaligned WAV finishes naturally to Stopped");
                        CHECK(player.GetPosition() == total,
                              "Misaligned WAV position reaches total");

                        (void)player.Stop();
                    }

                    CleanupTempWav(wavPath);
                }
            }

            // FLAC case: same misaligned length, decoded path.
            {
                std::wstring flacPath = CreateTempFlac(
                    L"\\kessoku_test_player22_flac_", kRate, kBits, kCh,
                    total);
                CHECK(!flacPath.empty(),
                      "Create misaligned FLAC fixture");

                if (!flacPath.empty()) {
                    auto result = kessoku::audio::Player::Create(flacPath);
                    if (result.IsErr() &&
                        IsEnvironmentSkipCode(result.GetError().code)) {
                        printf("SKIP: misaligned-FLAC test "
                               "(no compatible exclusive-mode audio device)\n");
                    } else {
                        CHECK(result.IsOk(),
                              "Player::Create for misaligned FLAC test");
                    }

                    if (result.IsOk()) {
                        auto player = std::move(result.Value());

                        CHECK(player.Play().IsOk(),
                              "Play() succeeds for misaligned FLAC");

                        bool finished = false;
                        for (int i = 0; i < 100; ++i) {
                            if (player.GetState() ==
                                kessoku::audio::PlaybackState::Stopped) {
                                finished = true;
                                break;
                            }
                            Sleep(50);
                        }
                        CHECK(finished,
                              "Misaligned FLAC finishes naturally to Stopped");
                        CHECK(player.GetPosition() == total,
                              "Misaligned FLAC position reaches total");

                        (void)player.Stop();
                    }

                    CleanupTempFlac(flacPath);
                }
            }
        }
    }

    // --- Test 23: WAV Seek while Paused preserves Paused (no auto-resume) ---
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player23_", 44100, 16, 2, 44100);
        CHECK(!wavPath.empty(), "Create temp WAV for paused-seek test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: WAV paused-seek test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(),
                      "Player::Create for WAV paused-seek test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                CHECK(player.Play().IsOk(), "Play() succeeds");
                Sleep(100);
                CHECK(player.Pause().IsOk(), "Pause() succeeds");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Paused,
                      "State is Paused before Seek()");

                auto seekStatus = player.Seek(10000);
                CHECK(seekStatus.IsOk(), "Seek() while Paused succeeds");
                CHECK(player.GetPosition() == 10000,
                      "Position is 10000 after paused Seek(10000)");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Paused,
                      "State stays Paused after Seek() (no auto-resume)");

                // Seek while Playing keeps playing (unchanged behavior).
                CHECK(player.Resume().IsOk() ||
                          player.GetState() ==
                              kessoku::audio::PlaybackState::Playing,
                      "Back to Playing for the second half");
                if (player.GetState() ==
                    kessoku::audio::PlaybackState::Paused) {
                    (void)player.Resume();
                }
                auto seekPlaying = player.Seek(20000);
                CHECK(seekPlaying.IsOk(), "Seek() while Playing succeeds");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Playing,
                      "State stays Playing after Seek() while Playing");
                // Playing seeks may advance before the read; exactness while
                // paused is covered above and in the FLAC paused-seek test.
                const uint32_t posAfterPlayingSeek = player.GetPosition();
                CHECK(posAfterPlayingSeek >= 20000,
                      "Position is at least 20000 after Seek(20000)");
                CHECK(posAfterPlayingSeek < player.GetTotalFrames(),
                      "Position is within total frames after Seek(20000)");

                (void)player.Stop();
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 24: FLAC Seek while Paused preserves Paused (no auto-resume) ---
    {
        std::wstring flacPath = CreateTempFlac(
            L"\\kessoku_test_flac24_", 44100, 16, 2, 44100);
        CHECK(!flacPath.empty(), "Create temp FLAC for paused-seek test");

        if (!flacPath.empty()) {
            auto result = kessoku::audio::Player::Create(flacPath);
            if (result.IsErr() &&
                IsEnvironmentSkipCode(result.GetError().code)) {
                printf("SKIP: FLAC paused-seek test "
                       "(no compatible exclusive-mode audio device)\n");
            } else {
                CHECK(result.IsOk(),
                      "Player::Create for FLAC paused-seek test");
            }

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                CHECK(player.Play().IsOk(), "FLAC Play() succeeds");
                Sleep(100);
                CHECK(player.Pause().IsOk(), "FLAC Pause() succeeds");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Paused,
                      "FLAC state is Paused before Seek()");

                auto seekStatus = player.Seek(10000);
                CHECK(seekStatus.IsOk(), "FLAC Seek() while Paused succeeds");
                CHECK(player.GetPosition() == 10000,
                      "FLAC position is 10000 after paused Seek(10000)");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Paused,
                      "FLAC state stays Paused after Seek()");

                if (player.GetState() ==
                    kessoku::audio::PlaybackState::Paused) {
                    (void)player.Resume();
                }
                auto seekPlaying = player.Seek(20000);
                CHECK(seekPlaying.IsOk(),
                      "FLAC Seek() while Playing succeeds");
                CHECK(player.GetState() ==
                          kessoku::audio::PlaybackState::Playing,
                      "FLAC state stays Playing after Seek() while Playing");

                (void)player.Stop();
            }

            CleanupTempFlac(flacPath);
        }
    }

    // --- Test 25: data chunk larger than 100MB is a valid WAV ---
    // Regression: FindDataChunk applied an arbitrary 100MB "sanity" cap to
    // every chunk, including 'data'. A 16-bit/44.1kHz stereo WAV passes
    // 100MB after ~10 minutes, and 24-bit/96kHz after ~3 minutes, so the cap
    // rejected ordinary music files before any audio setup.
    RunLargeDataChunkTest();

    // --- Test 26: mismatched WAV block alignment is rejected ---
    RunBlockAlignMismatchTest();

    // --- Test 27: Stop() must not consume a caller-owned COM apartment ---
    RunComApartmentOwnershipTest();

    // --- Test 28: playback stops at the real FLAC end even when STREAMINFO
    // --- declares more samples ---
    RunPrematureFlacEndTest();

    // --- Test 29: a WAV truncated after Create() ends instead of spinning ---
    RunWavTruncatedAfterCreateTest();

    // --- Test 30: seeking to the end of a FLAC succeeds ---
    RunFlacSeekToEndTest();

    // --- Test 31: Pause() racing natural end-of-stream must not leave a
    // --- dead paused render thread ---
    RunPauseAtEndOfStreamTest();

    // --- Test 32: a failed FLAC seek must leave the position unchanged ---
    RunFlacSeekFailureTest();

    std::wprintf(L"\n=== Results: %d failures ===\n", gFailures);
    return gFailures;
}

#include "kessoku/audio/player.h"
#include "kessoku/audio/flac_format.h"
#include "kessoku/audio/wav_format.h"

#include <windows.h>

#include <FLAC/stream_encoder.h>

#include <array>
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
            CHECK(result.IsOk(), "Player::Create succeeds for valid WAV");

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

                player.Stop();
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

    // --- Test 3: Play → Pause → Resume → Stop state transitions ---
    {
        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player3_", 22050, 16, 1, 22050); // 1 second mono
        CHECK(!wavPath.empty(), "Create temp WAV for state test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            CHECK(result.IsOk(), "Player::Create for state test");

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
            CHECK(result.IsOk(), "Player::Create for position test");

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                auto playStatus = player.Play();
                CHECK(playStatus.IsOk(), "Play() for position test");

                // Wait for some playback
                Sleep(200);

                uint32_t pos = player.GetPosition();
                CHECK(pos > 0, "Position is non-zero during playback");
                CHECK(pos <= 44100, "Position is within total frames");

                player.Stop();
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
            CHECK(result.IsOk(), "Player::Create for seek test");

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
                CHECK(player.GetPosition() == 10000,
                      "Position is 10000 after Seek(10000)");

                player.Stop();
            }

            CleanupTempWav(wavPath);
        }
    }

    // --- Test 6: Device loss state transition ---
    {
        // We can't easily force real device loss, but we can test that
        // the state machine handles the DeviceLost state correctly.
        // Create a player, then manually verify the state enum is distinct.

        std::wstring wavPath = CreateTempWav(
            L"\\kessoku_test_player6_", 44100, 16, 2, 44100);
        CHECK(!wavPath.empty(), "Create temp WAV for device loss test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            CHECK(result.IsOk(), "Player::Create for device loss test");

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                // Verify all states are distinct values
                CHECK(
                    static_cast<int>(kessoku::audio::PlaybackState::Stopped) !=
                    static_cast<int>(kessoku::audio::PlaybackState::Playing),
                    "Stopped != Playing");
                CHECK(
                    static_cast<int>(kessoku::audio::PlaybackState::Playing) !=
                    static_cast<int>(kessoku::audio::PlaybackState::Paused),
                    "Playing != Paused");
                CHECK(
                    static_cast<int>(kessoku::audio::PlaybackState::Paused) !=
                    static_cast<int>(kessoku::audio::PlaybackState::DeviceLost),
                    "Paused != DeviceLost");
                CHECK(
                    static_cast<int>(kessoku::audio::PlaybackState::Stopped) !=
                    static_cast<int>(kessoku::audio::PlaybackState::DeviceLost),
                    "Stopped != DeviceLost");

                player.Stop();
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
            CHECK(result.IsOk(), "Player::Create for idempotent stop test");

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
            CHECK(result.IsOk(), "Player::Create succeeds for valid FLAC");

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

                player.Stop();
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
            CHECK(result.IsOk(), "Player::Create for FLAC state test");

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
            CHECK(result.IsOk(), "Player::Create for FLAC seek test");

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
                CHECK(player.GetPosition() == 10000,
                      "FLAC position is 10000 after Seek(10000)");

                player.Stop();
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
                    bytes.push_back(static_cast<uint8_t>(v & 0xFF));
                    bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                }
                return bytes;
            };

            kessoku::audio::FlacReader reader;
            CHECK(reader.Open(flacPath, fmt, 0).empty(),
                  "FlacReader::Open succeeds");
            CHECK(reader.IsOpen(), "FlacReader is open");

            std::vector<uint8_t> decoded;
            decoded.reserve(static_cast<size_t>(kFrames) * 2);
            std::vector<uint8_t> chunk(1000 * 2);
            uint32_t totalRead = 0;
            for (int n = 0; n < 9; ++n) {
                uint32_t got = reader.ReadFrames(chunk.data(), 1000);
                CHECK(got == 1000, "Read 1000 frames per call");
                decoded.insert(decoded.end(), chunk.begin(),
                               chunk.begin() + got * 2);
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
            CHECK(result.IsOk(), "Player::Create for replay test");

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

                player.Stop();
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
            CHECK(result.IsOk(), "Player::Create for paused-replay test");

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

                player.Stop();
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
            CHECK(result.IsOk(), "Player::Create for long-pause test");

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

                player.Stop();
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
            CHECK(result.IsOk(), "Player::Create for decode-error test");

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

                player.Stop();
            }

            CleanupTempFlac(flacPath);
        }
    }

    // --- Test 21: Truncated WAV file stops playback (hang fix) ---
    // A WAV file whose data-chunk header declares more samples than are
    // actually present in the file. When the declared size exceeds what's
    // readable, the render loop would spin forever writing silence without
    // the fix. With the fix, the read failure is detected and playback
    // stops like natural EOS.
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
        CHECK(!wavPath.empty(), "Create truncated WAV for read-error test");

        if (!wavPath.empty()) {
            auto result = kessoku::audio::Player::Create(wavPath);
            CHECK(result.IsOk(), "Player::Create for truncated WAV test");

            if (result.IsOk()) {
                auto player = std::move(result.Value());

                CHECK(player.Play().IsOk(), "Play() succeeds for truncated WAV");

                // Poll with a bounded deadline. Without the fix, this hangs
                // forever. With the fix, playback stops within a few seconds.
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
                      "Truncated WAV stops within deadline (not hang)");

                player.Stop();
            }

            DeleteFileW(wavPath.data());
            std::wstring dir = tempDir;
            RemoveDirectoryW(dir.c_str());
        }
    }

    std::wprintf(L"\n=== Results: %d failures ===\n", gFailures);
    return gFailures;
}

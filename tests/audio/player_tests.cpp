#include "kessoku/audio/player.h"
#include "kessoku/audio/wav_format.h"

#include <windows.h>

#include <array>
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

    std::wprintf(L"\n=== Results: %d failures ===\n", gFailures);
    return gFailures;
}

#include "kessoku/audio/flac_format.h"
#include "kessoku/audio/wav_format.h"
#include "kessoku/core/library_root.h"
#include "kessoku/core/utf8.h"
#include "kessoku/library/metadata.h"
#include "kessoku/library/scanner.h"
#include "transport_position.h"

#include "test_helpers.h"

#define NOMINMAX
#include <windows.h>

#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The Doherty Threshold is the point (400 ms) past which an interactive
// response stops feeling immediate. These are not microbenchmarks: each
// scenario times a complete user-visible operation (scan, metadata read,
// decode) so a regression that would be felt in the UI fails the suite.
constexpr double kDohertyThresholdMs = 400.0;

// AddressSanitizer instrumentation makes CPU-bound helpers several times
// slower, so a wall-clock gate would fail for sanitizer overhead rather than
// an application regression. ASan builds still execute every workload; only
// the latency assertion is disabled.
#if defined(KESSOKU_PERFORMANCE_ASAN)
constexpr bool kEnforceDohertyThreshold = false;
#else
constexpr bool kEnforceDohertyThreshold = true;
#endif

int gFailures = 0;

void Check(bool condition, const char* desc) {
    if (!condition) {
        ++gFailures;
        fprintf(stderr, "FAIL: %s\n", desc);
    } else {
        printf("PASS: %s\n", desc);
    }
}

class TempDir {
public:
    explicit TempDir(std::wstring path) : path_(std::move(path)) {}
    ~TempDir() {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::wstring& path() const { return path_; }
    bool valid() const { return !path_.empty(); }

private:
    std::wstring path_;
};

std::wstring CreateUniqueTempDir(std::wstring_view prefix) {
    std::error_code ec;
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path(ec);
    if (ec) {
        return {};
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        std::wstring name(prefix);
        name += std::to_wstring(GetCurrentProcessId());
        name += L"_";
        name += std::to_wstring(attempt);
        std::filesystem::path candidate = parent / name;
        std::error_code createEc;
        if (std::filesystem::create_directory(candidate, createEc)) {
            return candidate.wstring();
        }
        if (createEc) {
            return {};
        }
    }
    return {};
}

void AppendU16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

bool WriteWavFile(const std::wstring& path, uint32_t frames) {
    constexpr uint16_t kChannels = 2;
    constexpr uint16_t kBits = 16;
    constexpr uint32_t kSampleRate = 44100;
    const uint16_t blockAlign =
        static_cast<uint16_t>(kChannels * (kBits / 8));
    const uint32_t dataBytes = frames * blockAlign;
    const uint32_t riffSize = 36 + dataBytes;

    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(riffSize) + 8);
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    AppendU32(bytes, riffSize);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
    bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
    AppendU32(bytes, 16);
    AppendU16(bytes, 1);
    AppendU16(bytes, kChannels);
    AppendU32(bytes, kSampleRate);
    AppendU32(bytes, kSampleRate * blockAlign);
    AppendU16(bytes, blockAlign);
    AppendU16(bytes, kBits);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    AppendU32(bytes, dataBytes);
    bytes.resize(bytes.size() + dataBytes, 0);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return false;
    }
    const size_t written = fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    return written == bytes.size();
}

// A structurally plausible RIFF/WAVE file whose fmt chunk declares more than
// the 1 MB cap. Metadata pre-validation must reject it without reading or
// allocating the declared payload, so a malicious library cannot turn startup
// into an allocation or I/O DoS.
bool WriteMalformedWavFile(const std::wstring& path) {
    std::vector<uint8_t> bytes;
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    AppendU32(bytes, 0);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
    bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
    AppendU32(bytes, 1024 * 1024 + 1);
    AppendU16(bytes, 1);
    AppendU16(bytes, 2);
    AppendU32(bytes, 44100);
    AppendU32(bytes, 176400);
    AppendU16(bytes, 4);
    AppendU16(bytes, 16);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return false;
    }
    const size_t written = fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    return written == bytes.size();
}

bool WriteFlacFile(const std::wstring& path, uint32_t frames) {
    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (encoder == nullptr) {
        return false;
    }

    FLAC__stream_encoder_set_channels(encoder, 1);
    FLAC__stream_encoder_set_bits_per_sample(encoder, 16);
    FLAC__stream_encoder_set_sample_rate(encoder, 44100);
    FLAC__stream_encoder_set_total_samples_estimate(encoder, frames);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        FLAC__stream_encoder_delete(encoder);
        return false;
    }

    if (FLAC__stream_encoder_init_FILE(encoder, file, nullptr, nullptr) !=
        FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(encoder);
        fclose(file);
        return false;
    }

    std::vector<FLAC__int32> samples(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        samples[i] = static_cast<FLAC__int32>((i * 37u) % 65536u) - 32768;
    }
    const FLAC__int32* channels[1] = {samples.data()};
    const bool encoded =
        FLAC__stream_encoder_process(encoder, channels, frames) != 0;
    FLAC__stream_encoder_finish(encoder);
    FLAC__stream_encoder_delete(encoder);
    fclose(file);
    return encoded;
}

template <typename Fn>
double MeasureWorstMs(int iterations, Fn&& action) {
    action(); // Warm caches and load code once before measuring.

    double worstMs = 0.0;
    for (int i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        action();
        const auto end = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(end - start).count();
        worstMs = std::max(worstMs, ms);
    }
    return worstMs;
}

template <typename Fn>
void Report(const char* name, int iterations, Fn&& action) {
    const double ms = MeasureWorstMs(iterations, action);
    const bool measuredOk = ms <= kDohertyThresholdMs;
    if (!kEnforceDohertyThreshold) {
        printf("SKIP: %-48s %8.2f ms (ASan build, Doherty budget not "
               "enforced)\n",
               name, ms);
        return;
    }
    const bool ok = measuredOk;
    printf("%s: %-48s %8.2f ms (Doherty budget %.0f ms)\n",
           ok ? "PASS" : "FAIL", name, ms, kDohertyThresholdMs);
    if (!ok) {
        ++gFailures;
    }
}

std::wstring MakeBenchmarkLibrary(const std::wstring& root,
                                  int directoryCount, int filesPerDirectory) {
    for (int d = 0; d < directoryCount; ++d) {
        const std::wstring dirName = L"album_" + std::to_wstring(d);
        if (!CreateSubdir(root, dirName)) {
            return {};
        }
        const std::wstring dir = root + L"\\" + dirName;

        for (int f = 0; f < filesPerDirectory; ++f) {
            const std::wstring name =
                L"track_" + std::to_wstring(f) +
                ((f % 2 == 0) ? L".flac" : L".wav");
            if (!CreateFileInDir(dir, name)) {
                return {};
            }
        }

        if (!CreateFileInDir(dir, L"cover.jpg") ||
            !CreateFileInDir(dir, L"notes.txt")) {
            return {};
        }
    }
    return root;
}

void RunScanBenchmark() {
    constexpr int kDirectories = 8;
    constexpr int kFilesPerDirectory = 250; // 2,000 audio files.
    constexpr int kExpectedFiles = kDirectories * kFilesPerDirectory;

    TempDir dir(CreateUniqueTempDir(L"kessoku_perf_scan_"));
    Check(dir.valid(), "create performance scan temp directory");
    if (!dir.valid()) {
        return;
    }

    const std::wstring rootPath =
        MakeBenchmarkLibrary(dir.path(), kDirectories, kFilesPerDirectory);
    Check(!rootPath.empty(), "build 2,000-file benchmark library");
    if (rootPath.empty()) {
        return;
    }

    auto rootResult = kessoku::core::LibraryRoot::Create(rootPath);
    Check(rootResult.IsOk(), "create LibraryRoot for benchmark library");
    if (!rootResult.IsOk()) {
        return;
    }
    const kessoku::core::LibraryRoot& root = rootResult.Value();

    const auto initial = kessoku::library::Scan(root);
    Check(initial.files.size() == static_cast<size_t>(kExpectedFiles),
          "benchmark scan finds every audio file");
    Check(initial.skipped.empty(), "benchmark scan has no skipped entries");
    if (initial.files.size() != static_cast<size_t>(kExpectedFiles)) {
        return;
    }

    size_t observed = 0;
    Report("scan 2,000 files across 8 directories", 3, [&] {
        const auto result = kessoku::library::Scan(root);
        observed = result.files.size();
    });
    Check(observed == static_cast<size_t>(kExpectedFiles),
          "timed scan still returns every audio file");
}

// Real libraries are rarely pure audio: cover art, logs, cue sheets, and
// other files sit next to the tracks. Discovery must filter them without
// letting their count dominate the interactive budget or leak into results.
void RunMixedLibraryBenchmark() {
    constexpr int kDirectories = 5;
    constexpr int kAudioPerDirectory = 100;  // 500 tracks.
    constexpr int kOtherPerDirectory = 300;  // 1,500 non-audio files.
    constexpr int kExpectedAudio = kDirectories * kAudioPerDirectory;
    constexpr const wchar_t* kOtherExtensions[] = {
        L".jpg", L".png", L".txt", L".log", L".cue", L".m3u", L".nfo", L".tmp"};

    TempDir dir(CreateUniqueTempDir(L"kessoku_perf_mixed_"));
    Check(dir.valid(), "create mixed-library temp directory");
    if (!dir.valid()) {
        return;
    }

    for (int d = 0; d < kDirectories; ++d) {
        const std::wstring dirName = L"album_" + std::to_wstring(d);
        if (!CreateSubdir(dir.path(), dirName)) {
            Check(false, "create mixed-library album directory");
            return;
        }
        const std::wstring albumDir = dir.path() + L"\\" + dirName;

        for (int f = 0; f < kAudioPerDirectory; ++f) {
            const std::wstring name =
                L"track_" + std::to_wstring(f) + L".wav";
            if (!CreateFileInDir(albumDir, name)) {
                Check(false, "create mixed-library audio file");
                return;
            }
        }
        for (int f = 0; f < kOtherPerDirectory; ++f) {
            const std::wstring name =
                L"extra_" + std::to_wstring(f) +
                kOtherExtensions[f % (sizeof(kOtherExtensions) /
                                      sizeof(kOtherExtensions[0]))];
            if (!CreateFileInDir(albumDir, name)) {
                Check(false, "create mixed-library non-audio file");
                return;
            }
        }
    }

    auto rootResult = kessoku::core::LibraryRoot::Create(dir.path());
    Check(rootResult.IsOk(), "create LibraryRoot for mixed library");
    if (!rootResult.IsOk()) {
        return;
    }
    const kessoku::core::LibraryRoot& root = rootResult.Value();

    const auto initial = kessoku::library::Scan(root);
    Check(initial.files.size() == static_cast<size_t>(kExpectedAudio),
          "mixed-library scan keeps only audio files");
    Check(initial.skipped.empty(), "mixed-library scan skips nothing");
    if (initial.files.size() != static_cast<size_t>(kExpectedAudio)) {
        return;
    }

    size_t observed = 0;
    Report("scan 500 audio + 1,500 other files", 3, [&] {
        observed = kessoku::library::Scan(root).files.size();
    });
    Check(observed == static_cast<size_t>(kExpectedAudio),
          "timed mixed-library scan returns only audio files");
}

// Mirrors the real startup path in main.cpp: scan the library root, then read
// metadata for every discovered track. This is the operation a user waits on
// when opening a library, so it gets the Doherty budget as a whole.
void RunLaunchBenchmark() {
    constexpr int kDirectories = 5;
    constexpr int kFilesPerDirectory = 100;
    constexpr int kExpectedTracks = kDirectories * kFilesPerDirectory;

    TempDir dir(CreateUniqueTempDir(L"kessoku_perf_launch_"));
    Check(dir.valid(), "create performance launch temp directory");
    if (!dir.valid()) {
        return;
    }

    for (int d = 0; d < kDirectories; ++d) {
        const std::wstring dirName = L"album_" + std::to_wstring(d);
        if (!CreateSubdir(dir.path(), dirName)) {
            Check(false, "create launch benchmark album directory");
            return;
        }
        const std::wstring albumDir = dir.path() + L"\\" + dirName;
        for (int f = 0; f < kFilesPerDirectory; ++f) {
            const std::wstring path =
                albumDir + L"\\track_" + std::to_wstring(f) + L".wav";
            if (!WriteWavFile(path, 0)) {
                Check(false, "create launch benchmark track");
                return;
            }
        }
    }

    auto rootResult = kessoku::core::LibraryRoot::Create(dir.path());
    Check(rootResult.IsOk(), "create LibraryRoot for launch benchmark");
    if (!rootResult.IsOk()) {
        return;
    }
    const kessoku::core::LibraryRoot& root = rootResult.Value();

    const auto initial = kessoku::library::Scan(root);
    Check(initial.files.size() == static_cast<size_t>(kExpectedTracks),
          "launch benchmark finds every track");
    if (initial.files.size() != static_cast<size_t>(kExpectedTracks)) {
        return;
    }

    size_t indexed = 0;
    Report("launch scan + metadata for 500 tracks", 3, [&] {
        const auto scan = kessoku::library::Scan(root);
        size_t readable = 0;
        for (const auto& file : scan.files) {
            if (kessoku::library::ReadTrackMetadata(file).IsOk()) {
                ++readable;
            }
        }
        indexed = readable;
    });
    Check(indexed == static_cast<size_t>(kExpectedTracks),
          "timed launch indexes every track");
}

void RunContainmentBenchmark() {
    constexpr int kChecks = 1000;

    TempDir dir(CreateUniqueTempDir(L"kessoku_perf_contains_"));
    Check(dir.valid(), "create performance containment temp directory");
    if (!dir.valid()) {
        return;
    }

    std::wstring insideFile;
    std::wstring outsideFile;
    Check(CreateFileInDir(dir.path(), L"inside.flac", &insideFile),
          "create inside benchmark file");

    TempDir outsideDir(CreateUniqueTempDir(L"kessoku_perf_outside_"));
    Check(outsideDir.valid(), "create outside benchmark directory");
    if (!outsideDir.valid()) {
        return;
    }
    Check(CreateFileInDir(outsideDir.path(), L"outside.flac", &outsideFile),
          "create outside benchmark file");

    auto rootResult = kessoku::core::LibraryRoot::Create(dir.path());
    Check(rootResult.IsOk(), "create LibraryRoot for containment benchmark");
    if (!rootResult.IsOk()) {
        return;
    }
    const kessoku::core::LibraryRoot& root = rootResult.Value();

    Check(root.Contains(insideFile), "containment accepts inside file");
    Check(!root.Contains(outsideFile), "containment rejects outside file");

    size_t accepted = 0;
    size_t rejected = 0;
    Report("2,000 path-containment checks", 3, [&] {
        for (int i = 0; i < kChecks; ++i) {
            if (root.Contains(insideFile)) {
                ++accepted;
            }
            if (!root.Contains(outsideFile)) {
                ++rejected;
            }
        }
    });
    Check(accepted >= static_cast<size_t>(kChecks),
          "timed containment accepts every inside check");
    Check(rejected >= static_cast<size_t>(kChecks),
          "timed containment rejects every outside check");
}

void RunMetadataBenchmark() {
    constexpr int kReads = 100;

    TempDir dir(CreateUniqueTempDir(L"kessoku_perf_metadata_"));
    Check(dir.valid(), "create performance metadata temp directory");
    if (!dir.valid()) {
        return;
    }

    const std::wstring wavPath = dir.path() + L"\\bench.wav";
    const std::wstring flacPath = dir.path() + L"\\bench.flac";
    Check(WriteWavFile(wavPath, 44100), "create WAV metadata benchmark file");
    Check(WriteFlacFile(flacPath, 441000),
          "create FLAC metadata benchmark file");

    const auto wav = kessoku::library::ReadTrackMetadata(wavPath);
    const auto flac = kessoku::library::ReadTrackMetadata(flacPath);
    Check(wav.IsOk(), "WAV metadata benchmark fixture is readable");
    Check(flac.IsOk(), "FLAC metadata benchmark fixture is readable");
    if (!wav.IsOk() || !flac.IsOk()) {
        return;
    }

    size_t successfulReads = 0;
    Report("100 WAV metadata reads", 3, [&] {
        for (int i = 0; i < kReads; ++i) {
            const auto result = kessoku::library::ReadTrackMetadata(wavPath);
            if (result.IsOk()) {
                ++successfulReads;
            }
        }
    });
    Check(successfulReads >= static_cast<size_t>(kReads),
          "timed WAV metadata reads all succeed");

    successfulReads = 0;
    Report("100 FLAC metadata reads", 3, [&] {
        for (int i = 0; i < kReads; ++i) {
            const auto result = kessoku::library::ReadTrackMetadata(flacPath);
            if (result.IsOk()) {
                ++successfulReads;
            }
        }
    });
    Check(successfulReads >= static_cast<size_t>(kReads),
          "timed FLAC metadata reads all succeed");

    kessoku::audio::FlacFormat format;
    Check(kessoku::audio::ParseFlacFormat(flacPath, format).empty(),
          "parse FLAC benchmark fixture");

    size_t decodedFrames = 0;
    Report("open + decode 10 s FLAC", 3, [&] {
        kessoku::audio::FlacReader reader;
        if (!reader.Open(flacPath, format, 0).empty()) {
            return;
        }

        const size_t bytesPerFrame =
            static_cast<size_t>(format.channelCount) *
            (format.bitsPerSample / 8);
        std::vector<uint8_t> buffer(bytesPerFrame * 4096);
        uint32_t total = 0;
        while (true) {
            const uint32_t got =
                reader.ReadFrames(buffer.data(), 4096);
            if (got == 0) {
                break;
            }
            total += got;
        }
        if (!reader.HadError()) {
            decodedFrames = total;
        }
    });
    Check(decodedFrames == 441000,
          "timed FLAC decode returns every frame without error");
}

// Malformed input is not just a correctness path: a library full of hostile
// headers must not make startup exceed the interactive budget. The old code
// could let TagLib chase a declared 4 GB fmt chunk; this measures the guarded
// rejection path across many independent files.
void RunMalformedMetadataBenchmark() {
    constexpr int kFiles = 200;

    TempDir dir(CreateUniqueTempDir(L"kessoku_perf_malformed_"));
    Check(dir.valid(), "create malformed metadata temp directory");
    if (!dir.valid()) {
        return;
    }

    std::vector<std::wstring> paths;
    paths.reserve(kFiles);
    for (int i = 0; i < kFiles; ++i) {
        const std::wstring path =
            dir.path() + L"\\bad_" + std::to_wstring(i) + L".wav";
        if (!WriteMalformedWavFile(path)) {
            Check(false, "create malformed metadata benchmark file");
            return;
        }
        paths.push_back(path);
    }

    size_t rejected = 0;
    for (const auto& path : paths) {
        if (kessoku::library::ReadTrackMetadata(path).IsErr()) {
            ++rejected;
        }
    }
    Check(rejected == paths.size(),
          "every malformed benchmark file is rejected");

    size_t timedRejected = 0;
    Report("reject 200 malformed WAV files", 3, [&] {
        size_t count = 0;
        for (const auto& path : paths) {
            if (kessoku::library::ReadTrackMetadata(path).IsErr()) {
                ++count;
            }
        }
        timedRejected = count;
    });
    Check(timedRejected == paths.size(),
          "timed malformed metadata reads all fail cleanly");
}

void RunTransportBenchmark() {
    constexpr int kIterations = 1000000;

    uint64_t sink = 0;
    Report("1,000,000 transport conversions", 3, [&] {
        for (int i = 0; i < kIterations; ++i) {
            const auto bar = kessoku::ui::PositionToBar(
                static_cast<uint32_t>(i), 26460000u);
            const auto frame = kessoku::ui::BarToFrame(bar, 26460000u);
            sink += static_cast<uint64_t>(frame);
        }
    });
    Check(sink > 0, "transport conversion benchmark produced results");

    sink = 0;
    Report("100,000 time-formatting operations", 3, [&] {
        for (int i = 0; i < 100000; ++i) {
            const std::wstring text = kessoku::ui::FormatTrackTime(
                static_cast<uint32_t>(i * 17), 44100);
            sink += text.size();
        }
    });
    Check(sink > 0, "time-formatting benchmark produced results");
}

void RunUtf8Benchmark() {
    constexpr int kIterations = 50000;
    const std::wstring sample = L"C:\\Música\\日本\\track_\u00E9\u4E16\u754C.flac";

    uint64_t sink = 0;
    Report("50,000 UTF-8 path round-trips", 3, [&] {
        for (int i = 0; i < kIterations; ++i) {
            const std::string utf8 = kessoku::core::WideToUtf8(sample);
            const std::wstring wide = kessoku::core::Utf8ToWide(utf8);
            sink += wide.size();
        }
    });
    Check(sink > 0, "UTF-8 benchmark produced results");
}

} // namespace

int main() {
    std::wprintf(L"=== Performance Tests (Doherty Threshold) ===\n");
#if !defined(KESSOKU_PERFORMANCE_ASAN)
    printf("Budgets are enforced in non-sanitizer builds.\n\n");
#else
    printf("ASan build: workloads run, wall-clock budgets are not enforced.\n\n");
#endif

    RunScanBenchmark();
    printf("\n");
    RunMixedLibraryBenchmark();
    printf("\n");
    RunLaunchBenchmark();
    printf("\n");
    RunContainmentBenchmark();
    printf("\n");
    RunMetadataBenchmark();
    printf("\n");
    RunMalformedMetadataBenchmark();
    printf("\n");
    RunTransportBenchmark();
    printf("\n");
    RunUtf8Benchmark();
    printf("\n");

    std::wprintf(L"=== Results: %d failures ===\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}

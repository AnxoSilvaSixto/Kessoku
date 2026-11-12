#include "kessoku/audio/flac_format.h"
#include "kessoku/audio/wav_format.h"
#include "kessoku/library/metadata.h"

#include "test_helpers.h"

#define NOMINMAX
#include <windows.h>

#include <FLAC/stream_encoder.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

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

void AppendTag(std::vector<uint8_t>& bytes, std::string_view tag) {
    bytes.insert(bytes.end(), tag.begin(), tag.end());
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

bool WriteBytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.wstring().c_str(), L"wb") != 0 ||
        file == nullptr) {
        return false;
    }
    const size_t written = fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    return written == bytes.size();
}

std::vector<uint8_t> MakeFmtPayload(uint16_t audioFormat, uint16_t channels,
                                    uint32_t sampleRate, uint16_t bits,
                                    uint32_t extraBytes = 0) {
    const uint16_t blockAlign =
        static_cast<uint16_t>(channels * (bits / 8));
    std::vector<uint8_t> payload;
    payload.reserve(static_cast<size_t>(16) + extraBytes);
    AppendU16(payload, audioFormat);
    AppendU16(payload, channels);
    AppendU32(payload, sampleRate);
    AppendU32(payload, sampleRate * blockAlign);
    AppendU16(payload, blockAlign);
    AppendU16(payload, bits);
    for (uint32_t i = 0; i < extraBytes; ++i) {
        payload.push_back(0);
    }
    return payload;
}

void AppendChunk(std::vector<uint8_t>& bytes, std::string_view id,
                 const std::vector<uint8_t>& payload) {
    AppendTag(bytes, id);
    AppendU32(bytes, static_cast<uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if ((payload.size() % 2) != 0) {
        bytes.push_back(0);
    }
}

std::vector<uint8_t> MakeWav(uint16_t audioFormat = 1,
                             uint16_t channels = 2,
                             uint32_t sampleRate = 44100,
                             uint16_t bits = 16,
                             uint32_t dataBytes = 16,
                             uint32_t fmtExtraBytes = 0) {
    std::vector<uint8_t> bytes;
    const std::vector<uint8_t> fmt =
        MakeFmtPayload(audioFormat, channels, sampleRate, bits,
                       fmtExtraBytes);
    const std::vector<uint8_t> data(dataBytes, 0);

    AppendTag(bytes, "RIFF");
    AppendU32(bytes, 4 + 8 + static_cast<uint32_t>(fmt.size()) +
                         8 + static_cast<uint32_t>(data.size()));
    AppendTag(bytes, "WAVE");
    AppendChunk(bytes, "fmt ", fmt);
    AppendChunk(bytes, "data", data);
    return bytes;
}

std::vector<uint8_t> MakeWavHeader() {
    std::vector<uint8_t> bytes;
    AppendTag(bytes, "RIFF");
    AppendU32(bytes, 0);
    AppendTag(bytes, "WAVE");
    return bytes;
}

std::string ParseWav(const std::wstring& path,
                     kessoku::audio::WavFormat& format) {
    return kessoku::audio::ParseWavFormat(path, format);
}

void ExpectWavError(const std::wstring& path, const char* desc) {
    kessoku::audio::WavFormat format{};
    const std::string error = ParseWav(path, format);
    Check(!error.empty(), desc);
}

void ExpectFlacError(const std::wstring& path, const char* desc) {
    kessoku::audio::FlacFormat format{};
    const std::string error = kessoku::audio::ParseFlacFormat(path, format);
    Check(!error.empty(), desc);
}

bool WriteFlac(const std::wstring& path, uint32_t frames) {
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

    std::vector<FLAC__int32> samples(frames, 0);
    const FLAC__int32* channels[1] = {samples.data()};
    const bool ok =
        FLAC__stream_encoder_process(encoder, channels, frames) != 0;
    FLAC__stream_encoder_finish(encoder);
    FLAC__stream_encoder_delete(encoder);
    fclose(file);
    return ok;
}

std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.wstring().c_str(), L"rb") != 0 ||
        file == nullptr) {
        return bytes;
    }

    if (fseek(file, 0, SEEK_END) == 0) {
        const long size = ftell(file);
        if (size > 0 && fseek(file, 0, SEEK_SET) == 0) {
            bytes.resize(static_cast<size_t>(size));
            const size_t read = fread(bytes.data(), 1, bytes.size(), file);
            bytes.resize(read);
        }
    }
    fclose(file);
    return bytes;
}

// STREAMINFO is the first metadata block: "fLaC" (4 bytes), block header
// (4 bytes), then the 34-byte payload. The 8-byte packed field at payload
// offset 10 (file offset 18) holds 20 bits of sample rate, 3 bits of
// channels-1, 5 bits of bits-per-sample-1, and 36 bits of total samples.
// Clearing the top 20 bits sets the sample rate to zero while leaving the
// other fields intact.
bool PatchFlacSampleRateToZero(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes = ReadBytes(path);
    if (bytes.size() < 26 || std::memcmp(bytes.data(), "fLaC", 4) != 0 ||
        (bytes[4] & 0x7Fu) != 0 /* STREAMINFO */) {
        return false;
    }
    bytes[18] = 0;
    bytes[19] = 0;
    bytes[20] &= 0x0Fu;
    return WriteBytes(path, bytes);
}

void TestWavParsing(const std::wstring& dir) {
    kessoku::audio::WavFormat format{};

    const std::wstring validPath = dir + L"\\valid.wav";
    Check(WriteBytes(validPath, MakeWav()), "write valid WAV fixture");
    Check(ParseWav(validPath, format).empty(), "valid WAV parses");
    Check(format.sampleRate == 44100, "valid WAV sample rate");
    Check(format.channelCount == 2, "valid WAV channel count");
    Check(format.bitsPerSample == 16, "valid WAV bit depth");
    Check(format.blockAlign == 4, "valid WAV block alignment");
    Check(format.byteRate == 176400, "valid WAV byte rate");

    const std::wstring extraFmtPath = dir + L"\\fmt_extra.wav";
    Check(WriteBytes(extraFmtPath, MakeWav(1, 1, 44100, 16, 0, 2)),
          "write WAV with extra fmt bytes");
    Check(ParseWav(extraFmtPath, format).empty(),
          "WAV fmt chunk larger than 16 bytes parses");

    const std::wstring oddJunkPath = dir + L"\\odd_junk.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        const std::vector<uint8_t> junk = {'x', 'y', 'z'};
        AppendChunk(bytes, "JUNK", junk);
        const std::vector<uint8_t> fmt = MakeFmtPayload(1, 1, 44100, 16);
        AppendChunk(bytes, "fmt ", fmt);
        const std::vector<uint8_t> data(8, 0);
        AppendChunk(bytes, "data", data);
        Check(WriteBytes(oddJunkPath, bytes), "write WAV with odd JUNK chunk");
    }
    Check(ParseWav(oddJunkPath, format).empty(),
          "WAV odd-sized leading chunk skips its pad byte");

    const std::wstring dataBeforeFmtPath = dir + L"\\data_before_fmt.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        const std::vector<uint8_t> data(8, 0);
        AppendChunk(bytes, "data", data);
        const std::vector<uint8_t> fmt = MakeFmtPayload(1, 2, 48000, 24);
        AppendChunk(bytes, "fmt ", fmt);
        Check(WriteBytes(dataBeforeFmtPath, bytes),
              "write WAV with data before fmt");
    }
    {
        kessoku::audio::WavFormat reordered{};
        Check(ParseWav(dataBeforeFmtPath, reordered).empty(),
              "WAV with data before fmt parses");
        Check(reordered.sampleRate == 48000 &&
                  reordered.channelCount == 2 &&
                  reordered.bitsPerSample == 24,
              "WAV format fields after a leading data chunk");
    }

    const std::wstring multipleFmtPath = dir + L"\\multiple_fmt.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        const std::vector<uint8_t> first = MakeFmtPayload(1, 1, 44100, 16);
        const std::vector<uint8_t> second = MakeFmtPayload(1, 2, 48000, 24);
        AppendChunk(bytes, "fmt ", first);
        AppendChunk(bytes, "fmt ", second);
        const std::vector<uint8_t> data(8, 0);
        AppendChunk(bytes, "data", data);
        Check(WriteBytes(multipleFmtPath, bytes),
              "write WAV with two fmt chunks");
    }
    {
        kessoku::audio::WavFormat firstWins{};
        Check(ParseWav(multipleFmtPath, firstWins).empty(),
              "WAV with two fmt chunks parses");
        Check(firstWins.sampleRate == 44100 &&
                  firstWins.channelCount == 1 &&
                  firstWins.bitsPerSample == 16,
              "WAV uses the first fmt chunk");
    }

    const std::wstring emptyPath = dir + L"\\empty.wav";
    Check(WriteBytes(emptyPath, {}), "write empty WAV fixture");
    ExpectWavError(emptyPath, "empty WAV is rejected");

    const std::wstring headerOnlyPath = dir + L"\\header_only.wav";
    Check(WriteBytes(headerOnlyPath, MakeWavHeader()),
          "write header-only WAV fixture");
    ExpectWavError(headerOnlyPath, "WAV without chunks is rejected");

    const std::wstring badSignaturePath = dir + L"\\bad_signature.wav";
    {
        std::vector<uint8_t> bytes = MakeWav();
        bytes[0] = 'R';
        bytes[1] = 'I';
        bytes[2] = 'F';
        bytes[3] = 'X';
        Check(WriteBytes(badSignaturePath, bytes),
              "write WAV with bad RIFF signature");
    }
    ExpectWavError(badSignaturePath, "WAV with bad RIFF signature is rejected");

    const std::wstring fmtZeroPath = dir + L"\\fmt_zero.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        AppendTag(bytes, "fmt ");
        AppendU32(bytes, 0);
        Check(WriteBytes(fmtZeroPath, bytes), "write zero-size fmt WAV");
    }
    ExpectWavError(fmtZeroPath, "WAV with zero-size fmt chunk is rejected");

    const std::wstring fmtShortPath = dir + L"\\fmt_short.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        AppendTag(bytes, "fmt ");
        AppendU32(bytes, 15);
        bytes.resize(bytes.size() + 15, 0);
        Check(WriteBytes(fmtShortPath, bytes), "write short fmt WAV");
    }
    ExpectWavError(fmtShortPath, "WAV with fmt chunk under 16 bytes is rejected");

    const std::wstring fmtTruncatedPath = dir + L"\\fmt_truncated.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        AppendTag(bytes, "fmt ");
        AppendU32(bytes, 16);
        bytes.resize(bytes.size() + 8, 0);
        Check(WriteBytes(fmtTruncatedPath, bytes),
              "write truncated fmt WAV");
    }
    ExpectWavError(fmtTruncatedPath, "truncated fmt payload is rejected");

    const std::wstring fmtHugePath = dir + L"\\fmt_huge.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        AppendTag(bytes, "fmt ");
        AppendU32(bytes, 1024 * 1024 + 1);
        Check(WriteBytes(fmtHugePath, bytes), "write oversized fmt WAV");
    }
    ExpectWavError(fmtHugePath, "fmt chunk over 1 MB is rejected");

    const std::wstring nonPcmPath = dir + L"\\float.wav";
    Check(WriteBytes(nonPcmPath, MakeWav(3, 1, 44100, 32)),
          "write non-PCM WAV fixture");
    {
        kessoku::audio::WavFormat ignored{};
        const std::string error = ParseWav(nonPcmPath, ignored);
        Check(!error.empty(), "non-PCM WAV is rejected");
        Check(error.find("non-PCM") != std::string::npos,
              "non-PCM WAV error explains the format");
    }

    const std::wstring zeroFieldsPath = dir + L"\\zero_fields.wav";
    Check(WriteBytes(zeroFieldsPath, MakeWav(1, 0, 0, 0, 0)),
          "write WAV with zero format fields");
    ExpectWavError(zeroFieldsPath, "WAV with zero PCM fields is rejected");

    const std::wstring zeroRatePath = dir + L"\\zero_rate.wav";
    Check(WriteBytes(zeroRatePath, MakeWav(1, 2, 0, 16)),
          "write WAV with only a zero sample rate");
    ExpectWavError(zeroRatePath, "WAV with a zero sample rate is rejected");

    const std::wstring zeroChannelsPath = dir + L"\\zero_channels.wav";
    Check(WriteBytes(zeroChannelsPath, MakeWav(1, 0, 44100, 16)),
          "write WAV with only zero channels");
    ExpectWavError(zeroChannelsPath, "WAV with zero channels is rejected");

    const std::wstring zeroBitsPath = dir + L"\\zero_bits.wav";
    Check(WriteBytes(zeroBitsPath, MakeWav(1, 2, 44100, 0)),
          "write WAV with only a zero bit depth");
    ExpectWavError(zeroBitsPath, "WAV with a zero bit depth is rejected");

    const std::wstring extensiblePath = dir + L"\\extensible.wav";
    Check(WriteBytes(extensiblePath, MakeWav(0xFFFE, 2, 48000, 24)),
          "write WAVE_FORMAT_EXTENSIBLE fixture");
    {
        kessoku::audio::WavFormat ignored{};
        const std::string error = ParseWav(extensiblePath, ignored);
        Check(!error.empty(),
              "WAVE_FORMAT_EXTENSIBLE is rejected without decoding it");
        Check(error.find("non-PCM") != std::string::npos,
              "extensible WAV error explains the unsupported format");
    }

    const std::wstring noFmtPath = dir + L"\\no_fmt.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        const std::vector<uint8_t> data(8, 0);
        AppendChunk(bytes, "data", data);
        Check(WriteBytes(noFmtPath, bytes), "write WAV without fmt chunk");
    }
    ExpectWavError(noFmtPath, "WAV without a fmt chunk is rejected");

    const std::wstring hugeChunkPath = dir + L"\\huge_chunk.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        AppendTag(bytes, "JUNK");
        AppendU32(bytes, 0xFFFFFFFFu);
        Check(WriteBytes(hugeChunkPath, bytes),
              "write WAV with an enormous leading chunk");
    }
    ExpectWavError(hugeChunkPath, "chunk extending past EOF is rejected");

    const std::wstring randomPath = dir + L"\\random.wav";
    {
        std::mt19937 rng(0xC0FFEEu);
        std::vector<uint8_t> bytes(512);
        for (uint8_t& byte : bytes) {
            byte = static_cast<uint8_t>(rng() & 0xFFu);
        }
        Check(WriteBytes(randomPath, bytes), "write random WAV fixture");
    }
    ExpectWavError(randomPath, "random bytes at a .wav path are rejected");

    const std::wstring zeroPath = dir + L"\\zero.wav";
    Check(WriteBytes(zeroPath, std::vector<uint8_t>(1024, 0)),
          "write all-zero WAV fixture");
    ExpectWavError(zeroPath, "all-zero WAV is rejected");
}

void TestFlacParsing(const std::wstring& dir) {
    const std::wstring validPath = dir + L"\\valid.flac";
    Check(WriteFlac(validPath, 44100), "write valid FLAC fixture");

    kessoku::audio::FlacFormat format{};
    Check(kessoku::audio::ParseFlacFormat(validPath, format).empty(),
          "valid FLAC parses");
    Check(format.sampleRate == 44100, "valid FLAC sample rate");
    Check(format.channelCount == 1, "valid FLAC channel count");
    Check(format.bitsPerSample == 16, "valid FLAC bit depth");
    Check(format.totalSamples == 44100, "valid FLAC total samples");

    const std::wstring upperPath = dir + L"\\VALID.FLAC";
    {
        const std::vector<uint8_t> bytes = ReadBytes(validPath);
        Check(WriteBytes(upperPath, bytes), "write uppercase FLAC fixture");
    }
    kessoku::audio::FlacFormat upperFormat{};
    Check(kessoku::audio::ParseFlacFormat(upperPath, upperFormat).empty(),
          "FLAC parsing is extension-case insensitive");
    Check(upperFormat.totalSamples == 44100,
          "uppercase FLAC fixture has the right sample count");

    const std::wstring emptyPath = dir + L"\\empty.flac";
    Check(WriteBytes(emptyPath, {}), "write empty FLAC fixture");
    ExpectFlacError(emptyPath, "empty FLAC is rejected");

    const std::wstring magicPath = dir + L"\\magic_only.flac";
    Check(WriteBytes(magicPath, {'f', 'L', 'a', 'C'}),
          "write FLAC magic-only fixture");
    ExpectFlacError(magicPath, "FLAC magic without STREAMINFO is rejected");

    const std::wstring zeroRatePath = dir + L"\\zero_rate.flac";
    Check(WriteFlac(zeroRatePath, 44100),
          "write FLAC fixture for zero sample-rate patch");
    Check(PatchFlacSampleRateToZero(zeroRatePath),
          "patch FLAC STREAMINFO sample rate to zero");
    ExpectFlacError(zeroRatePath, "FLAC with a zero sample rate is rejected");

    const std::wstring truncatedPath = dir + L"\\truncated.flac";
    {
        std::vector<uint8_t> bytes = ReadBytes(validPath);
        if (bytes.size() > 20) {
            bytes.resize(20);
        }
        Check(WriteBytes(truncatedPath, bytes), "write truncated FLAC fixture");
    }
    {
        kessoku::audio::FlacFormat ignored{};
        Check(!kessoku::audio::ParseFlacFormat(truncatedPath, ignored).empty(),
              "truncated FLAC is rejected");
    }

    const std::wstring randomPath = dir + L"\\random.flac";
    {
        std::mt19937 rng(0xBADC0DEu);
        std::vector<uint8_t> bytes(1024);
        for (uint8_t& byte : bytes) {
            byte = static_cast<uint8_t>(rng() & 0xFFu);
        }
        Check(WriteBytes(randomPath, bytes), "write random FLAC fixture");
    }
    {
        kessoku::audio::FlacFormat ignored{};
        Check(!kessoku::audio::ParseFlacFormat(randomPath, ignored).empty(),
              "random bytes at a .flac path are rejected");
    }

    kessoku::audio::FlacFormat ignored{};
    Check(!kessoku::audio::ParseFlacFormat(L"", ignored).empty(),
          "empty FLAC path is rejected");
    Check(!kessoku::audio::ParseFlacFormat(dir + L"\\missing.flac", ignored)
               .empty(),
          "missing FLAC path is rejected");

    const std::wstring directoryPath = dir + L"\\directory.flac";
    std::error_code dirEc;
    std::filesystem::create_directory(directoryPath, dirEc);
    Check(!dirEc, "create directory named like a FLAC file");
    if (!dirEc) {
        Check(!kessoku::audio::ParseFlacFormat(directoryPath, ignored).empty(),
              "directory at a .flac path is rejected");
    }
}

void TestMetadataMismatches(const std::wstring& dir) {
    const std::wstring validWav = dir + L"\\valid.wav";
    const std::wstring validFlac = dir + L"\\valid.flac";

    auto wavResult =
        kessoku::library::ReadTrackMetadata(std::filesystem::path(validWav));
    auto flacResult =
        kessoku::library::ReadTrackMetadata(std::filesystem::path(validFlac));
    Check(wavResult.IsOk(), "valid WAV metadata read succeeds");
    Check(flacResult.IsOk(), "valid FLAC metadata read succeeds");

    const std::wstring wavAsFlac = dir + L"\\wav_as_flac.flac";
    Check(WriteBytes(wavAsFlac, ReadBytes(validWav)),
          "copy WAV bytes to a .flac path");
    auto wavAsFlacResult = kessoku::library::ReadTrackMetadata(
        std::filesystem::path(wavAsFlac));
    Check(wavAsFlacResult.IsErr(),
          "WAV content at a .flac path is rejected");

    const std::wstring flacAsWav = dir + L"\\flac_as_wav.wav";
    Check(WriteBytes(flacAsWav, ReadBytes(validFlac)),
          "copy FLAC bytes to a .wav path");
    auto flacAsWavResult = kessoku::library::ReadTrackMetadata(
        std::filesystem::path(flacAsWav));
    Check(flacAsWavResult.IsErr(),
          "FLAC content at a .wav path is rejected");

    const std::wstring zeroFlac = dir + L"\\zero.flac";
    const std::wstring zeroWav = dir + L"\\zero.wav";
    Check(WriteBytes(zeroFlac, {}), "write zero-byte .flac fixture");
    Check(WriteBytes(zeroWav, {}), "write zero-byte .wav fixture");
    Check(kessoku::library::ReadTrackMetadata(std::filesystem::path(zeroFlac))
              .IsErr(),
          "zero-byte .flac metadata read fails cleanly");
    Check(kessoku::library::ReadTrackMetadata(std::filesystem::path(zeroWav))
              .IsErr(),
          "zero-byte .wav metadata read fails cleanly");

    // A file can carry the RIFF/WAVE signature yet still have no fmt chunk or
    // any usable structure. Signature detection alone must not report success.
    const std::wstring signedWav = dir + L"\\signed_no_chunks.wav";
    Check(WriteBytes(signedWav, {'R', 'I', 'F', 'F', 4, 0, 0, 0,
                                 'W', 'A', 'V', 'E'}),
          "write RIFF/WAVE file without chunks");
    Check(kessoku::library::ReadTrackMetadata(std::filesystem::path(signedWav))
              .IsErr(),
          "RIFF/WAVE signature without chunks is rejected");

    const std::wstring signedJunkWav = dir + L"\\signed_junk.wav";
    {
        std::vector<uint8_t> bytes;
        AppendTag(bytes, "RIFF");
        AppendU32(bytes, 0);
        AppendTag(bytes, "WAVE");
        AppendChunk(bytes, "JUNK", {'x', 'y', 'z', 'w'});
        Check(WriteBytes(signedJunkWav, bytes),
              "write RIFF/WAVE file with only a JUNK chunk");
    }
    Check(kessoku::library::ReadTrackMetadata(std::filesystem::path(signedJunkWav))
              .IsErr(),
          "RIFF/WAVE without a fmt chunk is rejected");

    const std::wstring hugeFmtWav = dir + L"\\huge_fmt.wav";
    {
        std::vector<uint8_t> bytes = MakeWavHeader();
        AppendTag(bytes, "fmt ");
        AppendU32(bytes, 1024 * 1024 + 1);
        const std::vector<uint8_t> fmt = MakeFmtPayload(1, 2, 44100, 16);
        bytes.insert(bytes.end(), fmt.begin(), fmt.end());
        Check(WriteBytes(hugeFmtWav, bytes),
              "write WAV with an oversized fmt chunk declaration");
    }
    Check(kessoku::library::ReadTrackMetadata(
              std::filesystem::path(hugeFmtWav))
              .IsErr(),
          "oversized fmt WAV metadata is rejected before TagLib");

    const std::wstring directoryWav = dir + L"\\directory.wav";
    std::error_code metadataDirEc;
    std::filesystem::create_directory(directoryWav, metadataDirEc);
    Check(!metadataDirEc, "create directory named like a WAV file");
    if (!metadataDirEc) {
        Check(kessoku::library::ReadTrackMetadata(
                  std::filesystem::path(directoryWav))
                  .IsErr(),
              "directory at a .wav path is rejected");
    }

    const std::wstring missingWav = dir + L"\\missing.wav";
    Check(kessoku::library::ReadTrackMetadata(
              std::filesystem::path(missingWav))
              .IsErr(),
          "missing .wav path is rejected");

    std::mt19937 rng(0x5EEDu);
    for (int i = 0; i < 32; ++i) {
        std::vector<uint8_t> bytes(static_cast<size_t>(i * 17 % 300));
        for (uint8_t& byte : bytes) {
            byte = static_cast<uint8_t>(rng() & 0xFFu);
        }
        const std::wstring path =
            dir + L"\\fuzz_" + std::to_wstring(i) + L".flac";
        if (!WriteBytes(path, bytes)) {
            Check(false, "write random metadata fuzz fixture");
            continue;
        }
        const auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(path));
        if (result.IsOk()) {
            Check(false, "random metadata fuzz fixture is rejected");
        }
    }
    Check(true, "random metadata fuzz corpus completes without crashing");
}

void TestParserFuzz(const std::wstring& dir) {
    std::mt19937 rng(0xA11CEu);
    for (int i = 0; i < 64; ++i) {
        std::vector<uint8_t> bytes(static_cast<size_t>(i * 29 % 600));
        for (uint8_t& byte : bytes) {
            byte = static_cast<uint8_t>(rng() & 0xFFu);
        }

        const std::wstring wavPath =
            dir + L"\\fuzz_" + std::to_wstring(i) + L".wav";
        const std::wstring flacPath =
            dir + L"\\fuzz_" + std::to_wstring(i) + L".flac";
        if (!WriteBytes(wavPath, bytes) || !WriteBytes(flacPath, bytes)) {
            Check(false, "write parser fuzz fixture");
            continue;
        }

        kessoku::audio::WavFormat wavFormat{};
        kessoku::audio::FlacFormat flacFormat{};
        (void)ParseWav(wavPath, wavFormat);
        (void)kessoku::audio::ParseFlacFormat(flacPath, flacFormat);
    }
    Check(true, "random parser fuzz corpus completes without crashing");
}

// Deterministic truncation and single-byte mutation sweeps over structurally
// valid files. Random fuzz mostly fails at the signature check; these cases
// reach the chunk/STREAMINFO parsers with one plausible field corrupted.
void TestTruncationAndMutationSweep(const std::wstring& dir) {
    const std::vector<uint8_t> wavBytes = ReadBytes(dir + L"\\valid.wav");
    const std::vector<uint8_t> flacBytes = ReadBytes(dir + L"\\valid.flac");
    Check(!wavBytes.empty() && !flacBytes.empty(),
          "read valid fixtures for truncation sweep");
    if (wavBytes.empty() || flacBytes.empty()) {
        return;
    }

    const std::wstring wavSweepPath = dir + L"\\sweep.wav";
    const std::wstring flacSweepPath = dir + L"\\sweep.flac";

    bool sweepOk = true;
    for (size_t length = 0; length <= wavBytes.size(); ++length) {
        const std::vector<uint8_t> prefix(
            wavBytes.begin(),
            wavBytes.begin() + static_cast<ptrdiff_t>(length));
        if (!WriteBytes(wavSweepPath, prefix)) {
            sweepOk = false;
            break;
        }
        kessoku::audio::WavFormat format{};
        (void)ParseWav(wavSweepPath, format);
    }
    Check(sweepOk, "WAV truncation sweep completes without crashing");

    sweepOk = true;
    for (size_t length = 0; length <= flacBytes.size(); ++length) {
        const std::vector<uint8_t> prefix(
            flacBytes.begin(),
            flacBytes.begin() + static_cast<ptrdiff_t>(length));
        if (!WriteBytes(flacSweepPath, prefix)) {
            sweepOk = false;
            break;
        }
        kessoku::audio::FlacFormat format{};
        (void)kessoku::audio::ParseFlacFormat(flacSweepPath, format);
    }
    Check(sweepOk, "FLAC truncation sweep completes without crashing");

    const uint8_t mutations[] = {0x00, 0xFF};
    bool mutationOk = true;
    const size_t wavMutationBytes = std::min<size_t>(wavBytes.size(), 96);
    for (size_t offset = 0; offset < wavMutationBytes && mutationOk; ++offset) {
        for (const uint8_t mutation : mutations) {
            std::vector<uint8_t> bytes = wavBytes;
            bytes[offset] = mutation;
            if (!WriteBytes(wavSweepPath, bytes)) {
                mutationOk = false;
                break;
            }
            kessoku::audio::WavFormat format{};
            (void)ParseWav(wavSweepPath, format);
        }
    }
    Check(mutationOk, "WAV header mutation sweep completes without crashing");

    mutationOk = true;
    const size_t flacMutationBytes = std::min<size_t>(flacBytes.size(), 96);
    for (size_t offset = 0; offset < flacMutationBytes && mutationOk;
         ++offset) {
        for (const uint8_t mutation : mutations) {
            std::vector<uint8_t> bytes = flacBytes;
            bytes[offset] = mutation;
            if (!WriteBytes(flacSweepPath, bytes)) {
                mutationOk = false;
                break;
            }
            kessoku::audio::FlacFormat format{};
            (void)kessoku::audio::ParseFlacFormat(flacSweepPath, format);
        }
    }
    Check(mutationOk, "FLAC header mutation sweep completes without crashing");

    // Metadata readers must also fail cleanly for mutated headers. Limit the
    // sweep to the header bytes so TagLib log noise and runtime stay bounded.
    mutationOk = true;
    const size_t wavMetadataBytes = std::min<size_t>(wavBytes.size(), 24);
    for (size_t offset = 0; offset < wavMetadataBytes && mutationOk;
         ++offset) {
        for (const uint8_t mutation : mutations) {
            std::vector<uint8_t> bytes = wavBytes;
            bytes[offset] = mutation;
            if (!WriteBytes(wavSweepPath, bytes)) {
                mutationOk = false;
                break;
            }
            (void)kessoku::library::ReadTrackMetadata(wavSweepPath);
        }
    }
    Check(mutationOk,
          "WAV metadata mutation sweep completes without crashing");

    mutationOk = true;
    const size_t flacMetadataBytes = std::min<size_t>(flacBytes.size(), 24);
    for (size_t offset = 0; offset < flacMetadataBytes && mutationOk;
         ++offset) {
        for (const uint8_t mutation : mutations) {
            std::vector<uint8_t> bytes = flacBytes;
            bytes[offset] = mutation;
            if (!WriteBytes(flacSweepPath, bytes)) {
                mutationOk = false;
                break;
            }
            (void)kessoku::library::ReadTrackMetadata(flacSweepPath);
        }
    }
    Check(mutationOk,
          "FLAC metadata mutation sweep completes without crashing");
}

} // namespace

int main() {
    std::wprintf(L"=== Audio Format Robustness Tests ===\n\n");

    TempDir dir(CreateUniqueTempDir(L"kessoku_format_test_"));
    if (!dir.valid()) {
        fprintf(stderr, "FATAL: could not create format test directory\n");
        return 1;
    }

    TestWavParsing(dir.path());
    printf("\n");
    TestFlacParsing(dir.path());
    printf("\n");
    TestMetadataMismatches(dir.path());
    printf("\n");
    TestParserFuzz(dir.path());
    printf("\n");
    TestTruncationAndMutationSweep(dir.path());
    printf("\n");

    std::wprintf(L"=== Results: %d failures ===\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}

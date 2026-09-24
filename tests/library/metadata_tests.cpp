#include "kessoku/library/metadata.h"

#include "kessoku/core/library_root.h"

#include "test_helpers.h"

#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/wavfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/id3v2tag.h>
#include <taglib/id3v2frame.h>
#include <taglib/infotag.h>

#include <FLAC/stream_encoder.h>

#include <cstdio>
#include <cstring>

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
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

} // namespace

#define CHECK(cond, desc) Check((cond), (desc))

// Helper: convert wstring_view to TagLib::String
TagLib::String WstrToTagLibString(std::wstring_view wstr) {
    return TagLib::String(std::wstring(wstr));
}

// ================================================================
// Test fixture: minimal valid FLAC via libFLAC stream encoder
// ================================================================

std::wstring CreateMinimalFlac(std::wstring_view dir, std::wstring_view name) {
    std::filesystem::create_directories(dir);
    std::wstring fullPath = std::wstring(dir) + L"\\" + std::wstring(name);

    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (!encoder) return L"";

    FLAC__stream_encoder_set_channels(encoder, 1);
    FLAC__stream_encoder_set_bits_per_sample(encoder, 16);
    FLAC__stream_encoder_set_sample_rate(encoder, 44100);
    FLAC__stream_encoder_set_total_samples_estimate(encoder, 4410);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, fullPath.c_str(), L"wb") != 0 || !fp) {
        FLAC__stream_encoder_delete(encoder);
        return L"";
    }

    if (FLAC__stream_encoder_init_FILE(encoder, fp,
                                        nullptr, nullptr) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(encoder);
        fclose(fp);
        std::filesystem::remove(fullPath);
        return L"";
    }

    std::vector<FLAC__int32> silence(4410, 0);
    const FLAC__int32* channels[1];
    channels[0] = silence.data();
    FLAC__stream_encoder_process(encoder, channels, static_cast<uint32_t>(silence.size()));
    FLAC__stream_encoder_finish(encoder);
    FLAC__stream_encoder_delete(encoder);
    fclose(fp);

    if (!std::filesystem::exists(fullPath)) return L"";
    return fullPath;
}

// ================================================================
// Test fixture: minimal valid WAV with empty ID3 chunk
// ================================================================

static bool WriteRawWav(std::wstring_view path, uint32_t dataBytes) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, std::wstring(path).c_str(), L"wb") != 0 || !fp) return false;

    uint32_t riffSize = 4 + 24 + 8 + dataBytes; // WAVE + fmt + ID3 + data
    uint8_t buf[256];

    // RIFF header
    memcpy(buf, "RIFF", 4);
    memcpy(buf + 4, &riffSize, 4);
    memcpy(buf + 8, "WAVE", 4);
    fwrite(buf, 12, 1, fp);

    // fmt chunk
    memcpy(buf, "fmt ", 4);
    uint32_t fmtSize = 16;
    memcpy(buf + 4, &fmtSize, 4);
    uint16_t pcm = 1;
    uint16_t ch = 1;
    uint32_t sr = 44100;
    uint32_t br = 44100 * 2;
    uint16_t ba = 2;
    uint16_t bps = 16;
    memcpy(buf + 8, &pcm, 2);
    memcpy(buf + 10, &ch, 2);
    memcpy(buf + 12, &sr, 4);
    memcpy(buf + 16, &br, 4);
    memcpy(buf + 20, &ba, 2);
    memcpy(buf + 22, &bps, 2);
    fwrite(buf, 24, 1, fp);

    // ID3 chunk (empty ID3v2 tag: exactly 10 bytes of ID3v2 header)
    memset(buf, 0, 256);
    memcpy(buf, "ID3 ", 4);
    uint32_t id3Size = 10;
    memcpy(buf + 4, &id3Size, 4);
    // ID3v2.3 header: "ID3" + version(3) + revision(0) + flags(0,0) + size(4, unsynced)
    buf[8] = 3; buf[9] = 0; buf[10] = 0; buf[11] = 0;
    // 4 zero bytes for tag content size (unsynced, not sync-safe)
    buf[12] = 0; buf[13] = 0; buf[14] = 0; buf[15] = 0;
    fwrite(buf, 8 + 10, 1, fp);

    // data chunk
    memcpy(buf, "data", 4);
    memcpy(buf + 4, &dataBytes, 4);
    fwrite(buf, 8, 1, fp);
    if (dataBytes > 0) {
        memset(buf, 0, 64);
        for (uint32_t i = 0; i < dataBytes; i += 64) {
            uint32_t toWrite = (i + 64 > dataBytes) ? (dataBytes - i) : 64;
            fwrite(buf, 1, toWrite, fp);
        }
    }

    fclose(fp);
    return std::filesystem::exists(path);
}

std::wstring CreateMinimalWav(std::wstring_view dir, std::wstring_view name) {
    std::filesystem::create_directories(dir);
    std::wstring fullPath = std::wstring(dir) + L"\\" + std::wstring(name);

    // 100ms of silent 16-bit mono PCM at 44100Hz = 4410 samples = 8820 bytes
    if (!WriteRawWav(fullPath, 8820)) return L"";
    return fullPath;
}

// Write known metadata to a FLAC file using TagLib FLAC::File.
bool StampFlacTags(std::wstring_view path,
                   std::wstring_view title, std::wstring_view artist,
                   std::wstring_view album, unsigned int trackNum) {
    TagLib::FLAC::File f(std::wstring(path).c_str(), false);
    if (!f.isValid()) return false;

    TagLib::PropertyMap props;
    if (!title.empty()) props.insert("TITLE", WstrToTagLibString(title));
    if (!artist.empty()) props.insert("ARTIST", WstrToTagLibString(artist));
    if (!album.empty()) props.insert("ALBUM", WstrToTagLibString(album));
    if (trackNum > 0) props.insert("TRACKNUMBER", TagLib::String::number(trackNum));

    TagLib::PropertyMap rejected = f.setProperties(props);
    if (!rejected.isEmpty()) return false;

    return f.save();
}

// Write known metadata to a WAV file's ID3v2 tag using RIFF::WAV::File API.
bool StampWavDirect(std::wstring_view path,
                    std::wstring_view title, std::wstring_view artist,
                    std::wstring_view album, unsigned int trackNum) {
    TagLib::RIFF::WAV::File f(std::wstring(path).c_str(), true);
    if (!f.isValid()) return false;

    TagLib::PropertyMap props;
    if (!title.empty()) props.insert("TITLE", WstrToTagLibString(title));
    if (!artist.empty()) props.insert("ARTIST", WstrToTagLibString(artist));
    if (!album.empty()) props.insert("ALBUM", WstrToTagLibString(album));
    if (trackNum > 0) props.insert("TRACKNUMBER", TagLib::String::number(trackNum));

    TagLib::PropertyMap rejected = f.setProperties(props);
    if (!rejected.isEmpty()) return false;

    return f.save();
}

// Write RIFF INFO tag to a WAV file (separate from ID3v2).
bool StampWavInfoTag(std::wstring_view path,
                     std::wstring_view title, std::wstring_view artist,
                     std::wstring_view album, unsigned int trackNum) {
    TagLib::RIFF::WAV::File f(std::wstring(path).c_str(), true);
    if (!f.isValid()) return false;

    TagLib::RIFF::Info::Tag* tag = f.InfoTag();
    if (!tag) return false;

    if (!title.empty())   tag->setTitle(WstrToTagLibString(title));
    if (!artist.empty())  tag->setArtist(WstrToTagLibString(artist));
    if (!album.empty())   tag->setAlbum(WstrToTagLibString(album));
    if (trackNum > 0)     tag->setTrack(trackNum);

    return f.save();
}

// ================================================================
// Test helpers for file hash comparison (read-only verification)
// ================================================================
std::vector<unsigned char> ReadFileBytes(std::wstring_view path) {
    std::ifstream ifs(std::wstring(path), std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
    return data;
}

int main() {
    std::wprintf(L"=== Metadata Tests ===\n\n");

    std::wstring tempParent;
    {
        wchar_t buf[MAX_PATH]{};
        DWORD len = GetTempPathW(MAX_PATH, buf);
        if (len == 0 || len > MAX_PATH) {
            fprintf(stderr, "FATAL: Could not get temp path\n");
            return 1;
        }
        tempParent = std::wstring(buf);
    }

    std::wstring root = CreateTempDir(L"\\kessoku_meta_test");
    if (root.empty()) {
        fprintf(stderr, "FATAL: Could not create test root\n");
        return 1;
    }

    // ================================================================
    // Test 1: FLAC with all four fields populated
    // ================================================================
    {
        std::wstring flacPath = CreateMinimalFlac(root, L"full_tags.flac");
        Check(!flacPath.empty(), "minimal FLAC created");

        bool stamped = StampFlacTags(flacPath,
                                      L"Test Title", L"Test Artist",
                                      L"Test Album", 7);
        CHECK(stamped, "FLAC tags stamped successfully");

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(flacPath));
        CHECK(result.IsOk(), "ReadTrackMetadata succeeded for FLAC");

        if (result.IsOk()) {
            auto meta = result.Value();
            CHECK(meta.title == L"Test Title", "FLAC title matches");
            CHECK(meta.artist == L"Test Artist", "FLAC artist matches");
            CHECK(meta.album == L"Test Album", "FLAC album matches");
            CHECK(meta.trackNumber == 7, "FLAC track number matches");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 2: WAV with all four fields in ID3v2 tag
    // ================================================================
    {
        std::wstring wavPath = CreateMinimalWav(root, L"full_tags.wav");
        Check(!wavPath.empty(), "minimal WAV created");

        bool stamped = StampWavDirect(wavPath,
                                       L"Wav Title", L"Wav Artist",
                                       L"Wav Album", 3);
        CHECK(stamped, "WAV ID3v2 tags stamped successfully");

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(wavPath));
        CHECK(result.IsOk(), "ReadTrackMetadata succeeded for WAV");

        if (result.IsOk()) {
            auto meta = result.Value();
            CHECK(meta.title == L"Wav Title", "WAV title matches");
            CHECK(meta.artist == L"Wav Artist", "WAV artist matches");
            CHECK(meta.album == L"Wav Album", "WAV album matches");
            CHECK(meta.trackNumber == 3, "WAV track number matches");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 3: WAV with different values in ID3v2 vs RIFF INFO ->
    //         ID3v2 wins, INFO is never read
    // ================================================================
    {
        std::wstring wavPath = CreateMinimalWav(root, L"dual_tags.wav");
        Check(!wavPath.empty(), "minimal WAV created for dual-tag test");

        // First stamp ID3v2
        bool id3v2Ok = StampWavDirect(wavPath,
                                       L"ID3v2 Title", L"ID3v2 Artist",
                                       L"ID3v2 Album", 5);
        CHECK(id3v2Ok, "WAV ID3v2 tags stamped");

        // Then stamp RIFF INFO with different values
        bool infoOk = StampWavInfoTag(wavPath,
                                       L"INFO Title", L"INFO Artist",
                                       L"INFO Album", 99);
        CHECK(infoOk, "WAV RIFF INFO tag stamped");

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(wavPath));
        CHECK(result.IsOk(), "ReadTrackMetadata succeeded for dual-tag WAV");

        if (result.IsOk()) {
            auto meta = result.Value();
            CHECK(meta.title == L"ID3v2 Title", "WAV title is from ID3v2, not INFO");
            CHECK(meta.artist == L"ID3v2 Artist", "WAV artist is from ID3v2, not INFO");
            CHECK(meta.album == L"ID3v2 Album", "WAV album is from ID3v2, not INFO");
            CHECK(meta.trackNumber == 5, "WAV track number is from ID3v2, not INFO");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 4: Untagged FLAC -> success with defaults
    // ================================================================
    {
        std::wstring flacPath = CreateMinimalFlac(root, L"untagged.flac");
        Check(!flacPath.empty(), "minimal untagged FLAC created");

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(flacPath));
        CHECK(result.IsOk(), "untagged FLAC -> success (not error)");

        if (result.IsOk()) {
            auto meta = result.Value();
            CHECK(meta.title.empty(), "untagged FLAC title is empty");
            CHECK(meta.artist.empty(), "untagged FLAC artist is empty");
            CHECK(meta.album.empty(), "untagged FLAC album is empty");
            CHECK(meta.trackNumber == 0, "untagged FLAC track number is 0");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 5: Untagged WAV -> success with defaults
    // ================================================================
    {
        std::wstring wavPath = CreateMinimalWav(root, L"untagged.wav");
        Check(!wavPath.empty(), "minimal untagged WAV created");

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(wavPath));
        CHECK(result.IsOk(), "untagged WAV -> success (not error)");

        if (result.IsOk()) {
            auto meta = result.Value();
            CHECK(meta.title.empty(), "untagged WAV title is empty");
            CHECK(meta.artist.empty(), "untagged WAV artist is empty");
            CHECK(meta.album.empty(), "untagged WAV album is empty");
            CHECK(meta.trackNumber == 0, "untagged WAV track number is 0");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 5b: WAV with ONLY RIFF INFO tag (no ID3v2) -> defaults
    // ================================================================
    {
        std::wstring wavPath = CreateMinimalWav(root, L"info_only.wav");
        Check(!wavPath.empty(), "minimal WAV created for INFO-only test");

        // Stamp only RIFF INFO, never write an ID3v2 tag
        bool infoOk = StampWavInfoTag(wavPath,
                                       L"INFO Title", L"INFO Artist",
                                       L"INFO Album", 99);
        CHECK(infoOk, "WAV RIFF INFO tag stamped (no ID3v2)");

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(wavPath));
        CHECK(result.IsOk(), "INFO-only WAV -> success (not error)");

        if (result.IsOk()) {
            auto meta = result.Value();
            CHECK(meta.title.empty(), "INFO-only WAV title is empty (not from INFO)");
            CHECK(meta.artist.empty(), "INFO-only WAV artist is empty (not from INFO)");
            CHECK(meta.album.empty(), "INFO-only WAV album is empty (not from INFO)");
            CHECK(meta.trackNumber == 0, "INFO-only WAV track number is 0 (not from INFO)");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 6: Corrupted/non-audio file at .flac path -> clean error
    // ================================================================
    {
        std::wstring corruptedPath = std::wstring(root) + L"\\corrupted.flac";
        CreateFileInDir(root, L"corrupted.flac");

        // Overwrite with garbage
        std::ofstream ofs(std::wstring(corruptedPath), std::ios::binary | std::ios::trunc);
        if (ofs) {
            const char* garbage = "this is not audio at all\x00\x01\x02";
            ofs.write(garbage, 30);
        }
        ofs.close();

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(corruptedPath));
        CHECK(result.IsErr(), "corrupted .flac -> error path");

        if (result.IsErr()) {
            CHECK(result.GetError().message.find("corrupted.flac") != std::string::npos,
                  "error message mentions the file path");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 7: Corrupted/non-audio file at .wav path -> clean error
    // ================================================================
    {
        std::wstring corruptedPath = std::wstring(root) + L"\\corrupted.wav";
        CreateFileInDir(root, L"corrupted.wav");

        std::ofstream ofs(std::wstring(corruptedPath), std::ios::binary | std::ios::trunc);
        if (ofs) {
            const char* garbage = "not a wav file\x00\xFF\xFE";
            ofs.write(garbage, 20);
        }
        ofs.close();

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(corruptedPath));
        CHECK(result.IsErr(), "corrupted .wav -> error path");

        if (result.IsErr()) {
            CHECK(result.GetError().message.find("corrupted.wav") != std::string::npos,
                  "error message mentions the file path");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 8: Reading does not modify the file (verify via hash)
    // ================================================================
    {
        std::wstring flacPath = CreateMinimalFlac(root, L"read_only.flac");
        Check(!flacPath.empty(), "minimal FLAC created for read-only test");

        StampFlacTags(flacPath, L"Read Only", L"Artist", L"Album", 1);

        auto beforeBytes = ReadFileBytes(flacPath);

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(flacPath));
        CHECK(result.IsOk(), "read-only FLAC read succeeded");

        auto afterBytes = ReadFileBytes(flacPath);
        CHECK(beforeBytes == afterBytes, "FLAC file not modified by read");

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 9: WAV read-only (verify via hash)
    // ================================================================
    {
        std::wstring wavPath = CreateMinimalWav(root, L"read_only.wav");
        Check(!wavPath.empty(), "minimal WAV created for read-only test");

        StampWavDirect(wavPath, L"RO WAV", L"WavArtist", L"WavAlbum", 2);

        auto beforeBytes = ReadFileBytes(wavPath);

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(wavPath));
        CHECK(result.IsOk(), "read-only WAV read succeeded");

        auto afterBytes = ReadFileBytes(wavPath);
        CHECK(beforeBytes == afterBytes, "WAV file not modified by read");

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 10: FLAC with partial tags (only title and track number)
    // ================================================================
    {
        std::wstring flacPath = CreateMinimalFlac(root, L"partial.flac");
        Check(!flacPath.empty(), "minimal FLAC created for partial tags test");

        bool stamped = StampFlacTags(flacPath,
                                      L"Partial Title", L"",
                                      L"", 42);
        CHECK(stamped, "partial FLAC tags stamped");

        auto result = kessoku::library::ReadTrackMetadata(
            std::filesystem::path(flacPath));
        CHECK(result.IsOk(), "partial FLAC read succeeded");

        if (result.IsOk()) {
            auto meta = result.Value();
            CHECK(meta.title == L"Partial Title", "partial FLAC title present");
            CHECK(meta.artist.empty(), "partial FLAC artist is empty");
            CHECK(meta.album.empty(), "partial FLAC album is empty");
            CHECK(meta.trackNumber == 42, "partial FLAC track number present");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    std::wprintf(L"=== Results: %d failures ===\n", gFailures);
    RemoveTempDir(root);
    return gFailures;
}

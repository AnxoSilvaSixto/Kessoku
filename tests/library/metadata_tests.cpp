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

#include <windows.h>

#include <cstdio>
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

// Write raw bytes to a file.
bool WriteFileBytes(std::wstring_view path, const std::vector<unsigned char>& data) {
    std::ofstream ofs(std::wstring(path), std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    return ofs.good();
}

// Create a minimal FLAC file using ffmpeg.
std::wstring CreateMinimalFlac(std::wstring_view dir, std::wstring_view name) {
    std::wstring fullPath = std::wstring(dir) + L"\\" + std::wstring(name);
    
    // Ensure directory exists
    std::filesystem::create_directories(dir);
    
    // Use ffmpeg to create a 1-second mono 16-bit 44100Hz FLAC file
    std::wstring cmd = L"cmd /c ffmpeg -y -f lavfi -i \"anullsrc=r=44100:cl=mono\" -t 1 -sample_fmt:s 16 -ar:s 44100 -c:a flac \"" + fullPath + L"\" >nul 2>&1";
    
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    
    wchar_t* cmdBuf = new wchar_t[cmd.size() + 1];
    wcscpy_s(cmdBuf, cmd.size() + 1, cmd.c_str());
    
    if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        delete[] cmdBuf;
        return L"";
    }
    
    delete[] cmdBuf;
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    // Check if file was created
    if (exitCode != 0) return L"";
    if (!std::filesystem::exists(fullPath)) return L"";
    
    return fullPath;
}

// Create a WAV file by copying a system WAV file.
std::wstring CreateWavFromSystem(std::wstring_view dir, std::wstring_view name) {
    std::wstring src = L"C:\\Windows\\Media\\Alarm01.wav";
    std::wstring dst = std::wstring(dir) + L"\\" + std::wstring(name);
    
    // Ensure directory exists
    std::filesystem::create_directories(dir);
    
    // Copy file using Windows API
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        return L"";
    }
    return dst;
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

// Write known metadata to a WAV file using direct RIFF::WAV::File API.
bool StampWavDirect(std::wstring_view path,
                    std::wstring_view title, std::wstring_view artist,
                    std::wstring_view album, unsigned int trackNum) {
    TagLib::RIFF::WAV::File f(std::wstring(path).c_str(), false);
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
    TagLib::RIFF::WAV::File f(std::wstring(path).c_str(), false);
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
        std::wstring wavPath = CreateWavFromSystem(root, L"full_tags.wav");
        Check(!wavPath.empty(), "WAV copied from system");

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
        std::wstring wavPath = CreateWavFromSystem(root, L"dual_tags.wav");
        Check(!wavPath.empty(), "WAV copied for dual-tag test");

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
        std::wstring wavPath = CreateWavFromSystem(root, L"untagged.wav");
        Check(!wavPath.empty(), "untagged WAV copied from system");

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
        std::wstring wavPath = CreateWavFromSystem(root, L"read_only.wav");
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

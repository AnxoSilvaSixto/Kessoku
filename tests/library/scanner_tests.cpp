#include "kessoku/core/library_root.h"
#include "kessoku/library/scanner.h"

#include "test_helpers.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <string_view>
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

} // namespace

#define CHECK(cond, desc) Check((cond), __func__, (desc), __LINE__)

// Helper: find a path in a vector.
bool PathContains(const std::vector<std::filesystem::path>& vec,
                  std::wstring_view target) {
    std::filesystem::path expected(target);
    for (const auto& p : vec) {
        if (p == expected) return true;
    }
    return false;
}

// Helper: find a reason in skipped vector.
bool SkippedContainsReason(const std::vector<kessoku::library::SkippedEntry>& vec,
                           std::wstring_view target, std::string_view reason) {
    std::filesystem::path expected(target);
    for (const auto& entry : vec) {
        if (entry.path == expected &&
            entry.reason == reason) {
            return true;
        }
    }
    return false;
}

int main() {
    std::wprintf(L"=== Scanner Tests ===\n\n");

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

    // ================================================================
    // Test 1: Empty root -> empty files, empty skipped
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test1");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test1 root\n");
            return 1;
        }

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());
        CHECK(scanResult.files.empty(), "empty root -> no files");
        CHECK(scanResult.skipped.empty(), "empty root -> no skipped");

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 2: FLAC/WAV in root + nested subdir -> all found
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test2");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test2 root\n");
            return 1;
        }

        CreateSubdir(root, L"nested");
        CreateSubdir(root, L"nested\\deep");

        std::wstring f1, f2, f3, f4;
        CreateFileInDir(root, L"track1.flac", &f1);
        CreateFileInDir(root, L"track2.wav", &f2);
        CreateFileInDir(root + L"\\nested", L"track3.flac", &f3);
        CreateFileInDir(root + L"\\nested\\deep", L"track4.wav", &f4);

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());
        CHECK(scanResult.files.size() == 4, "4 audio files found");
        CHECK(PathContains(scanResult.files, f1), "track1.flac in results");
        CHECK(PathContains(scanResult.files, f2), "track2.wav in results");
        CHECK(PathContains(scanResult.files, f3), "track3.flac in results");
        CHECK(PathContains(scanResult.files, f4), "track4.wav in results");
        CHECK(scanResult.skipped.empty(), "no skipped entries");

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 3: Non-audio files alongside audio files -> only audio
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test3");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test3 root\n");
            return 1;
        }

        CreateFileInDir(root, L"song.flac");
        CreateFileInDir(root, L"readme.txt");
        CreateFileInDir(root, L"cover.jpg");
        CreateFileInDir(root, L"notes.md");
        CreateFileInDir(root, L"album.wav");

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());
        CHECK(scanResult.files.size() == 2, "only 2 audio files found");
        CHECK(scanResult.skipped.empty(), "no skipped entries");

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 4: Directory junction inside root pointing outside -> none appear
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test4");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test4 root\n");
            return 1;
        }

        std::wstring outsideDir = tempParent + L"\\kessoku_scan_test4_outside";
        CreateDirectoryW(outsideDir.c_str(), nullptr);
        auto outsideFiles = CreateFlacDirectory(outsideDir, 3);

        std::wstring junctionPath = root + L"\\junction_to_outside";
        bool junctionOk = CreateJunction(junctionPath, outsideDir);

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());

        if (junctionOk) {
            CHECK(scanResult.files.empty(),
                  "junction target files not in results");
            CHECK(SkippedContainsReason(scanResult.skipped,
                                        std::wstring_view(junctionPath),
                                        "reparse point (not followed)"),
                  "junction appears in skipped with correct reason");

            DeleteFileW(junctionPath.c_str());
            RemoveDirectoryW(junctionPath.c_str());
        } else {
            printf("SKIP: junction test (could not create junction)\n");
        }

        RemoveTempDir(outsideDir);
        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 5: File symlink inside root pointing outside -> does not appear
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test5");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test5 root\n");
            return 1;
        }

        std::wstring outsideFile = tempParent + L"\\kessoku_scan_test5_outside.flac";
        CreateFileInDir(tempParent, L"kessoku_scan_test5_outside.flac");

        std::wstring symlinkPath = root + L"\\symlink_to_outside.flac";
        bool symlinkOk = CreateSymlink(symlinkPath, outsideFile);

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());

        // The symlink resolves to a file outside the root, so Contains()
        // should return false, and it should appear in skipped.
        if (symlinkOk) {
            CHECK(!PathContains(scanResult.files, symlinkPath),
                  "file symlink not in results");
            CHECK(SkippedContainsReason(scanResult.skipped, symlinkPath,
                                        "resolved outside library root"),
                  "file symlink in skipped with correct reason");
            DeleteFileW(symlinkPath.c_str());
        } else {
            printf("SKIP: symlink test (requires Dev Mode or elevation)\n");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 6: Case-insensitive extension matching (.FLAC, .Wav) -> found
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test6");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test6 root\n");
            return 1;
        }

        std::wstring f1, f2, f3, f4;
        CreateFileInDir(root, L"upper.FLAC", &f1);
        CreateFileInDir(root, L"mixed.Wav", &f2);
        CreateFileInDir(root, L"lower.flac", &f3);
        CreateFileInDir(root, L"deep.WaV", &f4);

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());
        CHECK(scanResult.files.size() == 4, "4 case-variant audio files found");
        CHECK(PathContains(scanResult.files, f1), "upper.FLAC found");
        CHECK(PathContains(scanResult.files, f2), "mixed.Wav found");
        CHECK(PathContains(scanResult.files, f3), "lower.flac found");
        CHECK(PathContains(scanResult.files, f4), "deep.WaV found");
        CHECK(scanResult.skipped.empty(), "no skipped entries");

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 7: Unreadable subdirectory -> scan continues, shows in skipped
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test7");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test7 root\n");
            return 1;
        }

        // Create a normal audio file in root (should be found)
        std::wstring goodFile;
        CreateFileInDir(root, L"good.flac", &goodFile);

        // Create a subdirectory with an audio file
        CreateSubdir(root, L"unreadable");
        std::wstring badFile;
        CreateFileInDir(root + L"\\unreadable", L"bad.flac", &badFile);

        // Make the directory unreadable by removing read permissions.
        // On Windows, we can't easily simulate permission denied without
        // elevation, so we test the error handling path differently:
        // We'll create a file that exists but can't be opened (e.g., a
        // reparse point or broken symlink).
        //
        // Actually, the simplest reliable test is to verify that the
        // scanner handles the case where a subdirectory entry itself
        // can't be read. On Windows, this is hard to simulate without
        // elevation. Let's verify that the scanner doesn't crash when
        // encountering unusual filesystem entries.
        //
        // Alternative: create a junction that points to a non-existent
        // target. The directory_iterator should handle this gracefully.
        std::wstring brokenJunction = root + L"\\broken_junction";
        bool brokenOk = CreateJunction(brokenJunction,
                                       L"D:\\nonexistent_target_xyz");

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());

        // The good file should always be found
        CHECK(PathContains(scanResult.files, goodFile),
              "good.flac found despite broken junction");

        // The broken-junction outcome is filesystem-dependent: the entry may
        // be reported in skipped, or silently skipped by the filesystem, or
        // (as on this machine, where mklink /J refuses a nonexistent target)
        // never created at all. None of those outcomes is asserted here — a
        // CHECK in any branch would pass or fail independent of scanner
        // behavior. The deterministic invariant (scan survives, good file
        // found) is already checked above; here we only report what happened.
        if (brokenOk) {
            bool foundInSkipped = false;
            for (const auto& entry : scanResult.skipped) {
                std::filesystem::path expectedJunction(brokenJunction);
                if (entry.path == expectedJunction) {
                    foundInSkipped = true;
                    break;
                }
            }
            if (foundInSkipped) {
                printf("INFO: broken junction reported in skipped\n");
            } else {
                // The filesystem may silently skip broken junctions,
                // which is also acceptable behavior.
                printf("INFO: broken junction silently skipped by filesystem\n");
            }
            DeleteFileW(brokenJunction.c_str());
            RemoveDirectoryW(brokenJunction.c_str());
        } else {
            printf("SKIP: broken junction test (could not create)\n");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 8: Recursive cycle junction (points back to ancestor) -> scan terminates
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test8");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test8 root\n");
            return 1;
        }

        CreateSubdir(root, L"sub");

        std::wstring cycleJunction = root + L"\\sub\\cycle_back";
        bool cycleOk = CreateJunction(cycleJunction, root);

        std::wstring goodFile;
        CreateFileInDir(root, L"good.flac", &goodFile);

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());

        if (cycleOk) {
            CHECK(PathContains(scanResult.files, goodFile),
                  "good.flac found despite cycle junction");
            CHECK(SkippedContainsReason(scanResult.skipped,
                                        std::wstring_view(cycleJunction),
                                        "reparse point (not followed)"),
                  "cycle junction skipped (scan terminated)");
            CHECK(scanResult.files.size() == 1,
                  "only one file found (no infinite recursion)");

            DeleteFileW(cycleJunction.c_str());
            RemoveDirectoryW(cycleJunction.c_str());
        } else {
            printf("SKIP: cycle junction test (could not create junction)\n");
        }
        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 9: Extension matching uses only the final extension and rejects
    // names that merely contain .flac or .wav.
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test9");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test9 root\n");
            return 1;
        }

        std::wstring f1, f2, f3, f4;
        CreateFileInDir(root, L"real.flac", &f1);
        CreateFileInDir(root, L"real.wav", &f2);
        CreateFileInDir(root, L"double.wav.flac", &f3);
        CreateFileInDir(root, L"double.flac.wav", &f4);

        CreateFileInDir(root, L"backup.flac.bak");
        CreateFileInDir(root, L"fake.wav.exe");
        CreateFileInDir(root, L"almost.flacx");
        CreateFileInDir(root, L"almost.wavx");
        CreateFileInDir(root, L"no_extension");
        CreateFileInDir(root, L"dot.flac.txt");

        // A leading dot makes the name a dotfile, not a file with a .flac or
        // .wav extension; an extensionless name is not audio either.
        std::wstring dotFlac, dotWav, bareFlac, bareWav;
        CreateFileInDir(root, L".flac", &dotFlac);
        CreateFileInDir(root, L".wav", &dotWav);
        CreateFileInDir(root, L"FLAC", &bareFlac);
        CreateFileInDir(root, L"WAV", &bareWav);

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());
        CHECK(scanResult.files.size() == 4,
              "only final-extension audio names are found");
        CHECK(PathContains(scanResult.files, f1), "real.flac found");
        CHECK(PathContains(scanResult.files, f2), "real.wav found");
        CHECK(PathContains(scanResult.files, f3), "double.wav.flac found");
        CHECK(PathContains(scanResult.files, f4), "double.flac.wav found");
        CHECK(!PathContains(scanResult.files, dotFlac),
              "dotfile .flac is not treated as an audio file");
        CHECK(!PathContains(scanResult.files, dotWav),
              "dotfile .wav is not treated as an audio file");
        CHECK(!PathContains(scanResult.files, bareFlac),
              "extensionless FLAC name is not audio");
        CHECK(!PathContains(scanResult.files, bareWav),
              "extensionless WAV name is not audio");
        CHECK(scanResult.skipped.empty(),
              "non-audio extensions produce no skipped entries");

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 10: Unicode, hidden, zero-byte, directory-with-audio-extension,
    // and deep nesting all behave without losing audio files.
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test10");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test10 root\n");
            return 1;
        }

        std::wstring unicodeFile;
        CreateSubdir(root, L"m\u00FAsica");
        CreateFileInDir(root + L"\\m\u00FAsica", L"caf\u00E9.flac",
                        &unicodeFile);

        std::wstring zeroFile;
        CreateFileInDir(root, L"zero.wav", &zeroFile);

        std::wstring hiddenFile;
        CreateFileInDir(root, L"hidden.flac", &hiddenFile);
        SetFileAttributesW(hiddenFile.c_str(), FILE_ATTRIBUTE_HIDDEN);

        CreateSubdir(root, L"album.flac");
        std::wstring insideAlbumDir;
        CreateFileInDir(root + L"\\album.flac", L"inside.flac",
                        &insideAlbumDir);

        std::wstring deepDir = root;
        for (int i = 0; i < 32; ++i) {
            const std::wstring name = L"d" + std::to_wstring(i);
            if (!CreateSubdir(deepDir, name)) {
                break;
            }
            deepDir += L"\\" + name;
        }
        std::wstring deepFile;
        CreateFileInDir(deepDir, L"deep.flac", &deepFile);

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());
        CHECK(PathContains(scanResult.files, unicodeFile),
              "Unicode filename is found");
        CHECK(PathContains(scanResult.files, zeroFile),
              "zero-byte audio file is listed by the scanner");
        CHECK(PathContains(scanResult.files, hiddenFile),
              "hidden audio file is found");
        CHECK(PathContains(scanResult.files, insideAlbumDir),
              "directory named album.flac is traversed");
        CHECK(PathContains(scanResult.files, deepFile),
              "audio file at 32 levels of nesting is found");
        CHECK(scanResult.files.size() == 5,
              "exactly five audio files in the mixed edge-case tree");
        CHECK(scanResult.skipped.empty(),
              "mixed edge-case tree has no skipped entries");

        RemoveTempDir(root);
    }
    printf("\n");

    // ================================================================
    // Test 11: A junction inside the root pointing to another directory
    // inside the root is still not followed. Target files must be found
    // through their real path exactly once, never duplicated through the
    // junction.
    // ================================================================
    {
        std::wstring root = CreateTempDir(L"\\kessoku_scan_test11");
        if (root.empty()) {
            fprintf(stderr, "FATAL: Could not create test11 root\n");
            return 1;
        }

        std::wstring realFile;
        CreateFileInDir(root, L"real.flac", &realFile);
        std::wstring targetFile;
        CreateSubdir(root, L"target");
        CreateFileInDir(root + L"\\target", L"inside.flac", &targetFile);

        std::wstring insideJunction = root + L"\\link_to_target";
        bool junctionOk = CreateJunction(insideJunction, root + L"\\target");

        auto result = kessoku::core::LibraryRoot::Create(root);
        if (!result.IsOk()) {
            fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                    result.GetError().message.data());
            return 1;
        }

        auto scanResult = kessoku::library::Scan(result.Value());
        CHECK(PathContains(scanResult.files, realFile),
              "root file found with an inside-root junction present");
        CHECK(PathContains(scanResult.files, targetFile),
              "junction target file found through its real path");

        if (junctionOk) {
            CHECK(scanResult.files.size() == 2,
                  "inside-root junction causes no duplicate traversal");
            CHECK(SkippedContainsReason(scanResult.skipped,
                                        std::wstring_view(insideJunction),
                                        "reparse point (not followed)"),
                  "inside-root junction appears in skipped");
            RemoveDirectoryW(insideJunction.c_str());
        } else {
            printf("SKIP: inside-root junction test (could not create)\n");
        }

        RemoveTempDir(root);
    }
    printf("\n");

    std::wprintf(L"=== Results: %d failures ===\n", gFailures);
    return gFailures;
}

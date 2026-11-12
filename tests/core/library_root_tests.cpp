#include "kessoku/core/library_root.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Create a temporary directory under %TEMP% and return its path.
// Returns empty string on failure.
std::wstring CreateTempDir(std::wstring_view prefix) {
    std::wstring tempDir;
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, buf);
    if (len == 0 || len > MAX_PATH) return {};

    tempDir = buf;
    tempDir += std::wstring(prefix);

    if (!CreateDirectoryW(tempDir.c_str(), nullptr)) {
        DWORD gle = GetLastError();
        if (gle == ERROR_ALREADY_EXISTS) {
            RemoveDirectoryW(tempDir.c_str());
            if (!CreateDirectoryW(tempDir.c_str(), nullptr)) return {};
        } else {
            return {};
        }
    }
    return tempDir;
}

// Create a file inside the given directory with the given name.
// Returns true on success.
bool CreateFileInDir(std::wstring_view dir, std::wstring_view filename,
                     std::wstring* outPath = nullptr) {
    std::wstring fullPath = std::wstring(dir) + L"\\" +
                            std::wstring(filename);
    HANDLE h = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    if (outPath) *outPath = fullPath;
    return true;
}

// Create a subdirectory inside the given directory.
bool CreateSubdir(std::wstring_view parent, std::wstring_view name) {
    std::wstring fullPath = std::wstring(parent) + L"\\" +
                            std::wstring(name);
    return CreateDirectoryW(fullPath.c_str(), nullptr) != FALSE ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

// Create a junction point at junctionPath pointing to targetPath.
// Uses mklink /J (cmd.exe built-in) — needs no elevation or Developer Mode.
bool CreateJunction(std::wstring_view junctionPath,
                    std::wstring_view targetPath) {
    std::wstring cmd = L"cmd /c mklink /J \"" +
                       std::wstring(junctionPath) + L"\" \"" +
                       std::wstring(targetPath) + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<WCHAR> cmdBuf(cmd.size() + 1);
    memcpy(cmdBuf.data(), cmd.c_str(), (cmd.size() + 1) * sizeof(WCHAR));

    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                             FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi);
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return exitCode == 0;
    }
    return false;
}

// Create a symbolic link at symlinkPath pointing to targetPath.
// Uses SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (0x2) so it works
// without Developer Mode on Windows 10+ (build 14931+).
bool CreateSymlink(std::wstring_view symlinkPath,
                   std::wstring_view targetPath) {
    std::wstring sPath(symlinkPath);
    std::wstring tPath(targetPath);
    BOOL ok = CreateSymbolicLinkW(sPath.c_str(), tPath.c_str(),
                                  SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    return ok != FALSE;
}

bool StringStartsWith(std::wstring_view haystack, std::wstring_view needle) {
    if (haystack.size() < needle.size()) return false;
    for (size_t i = 0; i < needle.size(); ++i) {
        if (haystack[i] != needle[i]) return false;
    }
    return true;
}

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

int main() {
    std::wprintf(L"=== LibraryRoot Tests ===\n\n");

    // --- Setup: create a temp library root with test structure ---
    std::wstring root = CreateTempDir(L"\\kessoku_test_root");
    if (root.empty()) {
        fprintf(stderr, "FATAL: Could not create temp root directory\n");
        return 1;
    }

    std::wstring nestedDir = root + L"\\nested";
    CreateSubdir(root, L"nested");

    std::wstring rootFile = root + L"\\test.flac";
    std::wstring nestedFile = nestedDir + L"\\deep.flac";
    CreateFileInDir(root, L"test.flac");
    CreateFileInDir(nestedDir, L"deep.flac");

    // --- Create LibraryRoot ---
    auto result = kessoku::core::LibraryRoot::Create(root);
    if (!result.IsOk()) {
        fprintf(stderr, "FATAL: LibraryRoot::Create failed: %s\n",
                result.GetError().message.data());
        return 1;
    }
    auto& libRoot = result.Value();

    // (a) A file directly inside the root -> true
    CHECK(libRoot.Contains(rootFile), "file directly inside root");

    // (b) A file in a nested subdirectory inside the root -> true
    CHECK(libRoot.Contains(nestedFile), "file in nested subdir");

    // (c) A ..-traversal path resolving outside the root -> false
    std::wstring outsideTraversal = root + L"\\..\\other_dir\\file.txt";
    CHECK(!libRoot.Contains(outsideTraversal), "..-traversal outside root");

    // (d) An absolute path on the same drive, outside the root -> false
    std::wstring tempDir = root.substr(0, root.find_last_of(L'\\'));
    std::wstring siblingDir = tempDir + L"\\kessoku_sibling_dir";
    if (CreateDirectoryW(siblingDir.c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        std::wstring siblingFile = siblingDir + L"\\file.flac";
        CreateFileInDir(siblingDir, L"file.flac");
        CHECK(!libRoot.Contains(siblingFile),
              "absolute path on same drive, outside root");
        RemoveDirectoryW(siblingDir.c_str());
    } else {
        printf("SKIP: same-drive sibling test (could not create directory)\n");
    }

    // (e) A different-drive-letter-form path that does not exist on this
    // machine -> false via the not-found early return. NOTE: this does NOT
    // reach the drive-letter prefix comparison in Contains(): only C: exists
    // here (verified), so no existing file on another drive can be produced
    // deterministically with plain Win32 calls (a subst-mapped drive resolves
    // through to the underlying C: path in GetFinalPathNameByHandleW before
    // comparison — verified empirically — and mounting a real second volume
    // needs elevation). Named honestly instead of claiming drive-specific
    // logic ran; passes for the same reason as (k).
    std::wstring otherDriveRoot;
    if (StringStartsWith(root, L"C:\\")) {
        otherDriveRoot = L"D:\\Windows\\System32\\cmd.exe";
    } else {
        otherDriveRoot = L"C:\\Windows\\System32\\cmd.exe";
    }
    CHECK(!libRoot.Contains(otherDriveRoot),
          "nonexistent path in different-drive form -> false");

    // (f) A UNC-form path with no reachable share -> false via the not-found
    // early return. NOTE: same gap as (e) — no deterministic way to produce
    // an existing UNC path with plain Win32 calls (needs a real network
    // share), so UNC-specific comparison logic is not reached. Named honestly;
    // passes for the same reason as (k), not because UNC logic ran.
    std::wstring uncPath = L"\\\\server\\share\\file.flac";
    CHECK(!libRoot.Contains(uncPath), "nonexistent UNC-form path -> false");

    // (g) A sibling whose name has the root's name as a string prefix -> false
    std::wstring rootBasename = root.substr(root.find_last_of(L'\\') + 1);
    std::wstring siblingWithPrefix = tempDir + L"\\" +
                                     std::wstring(rootBasename) + L"Other\\file.flac";
    std::wstring siblingPrefixDir = siblingWithPrefix.substr(
        0, siblingWithPrefix.find_last_of(L'\\'));
    if (CreateDirectoryW(siblingPrefixDir.c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        CreateFileInDir(siblingPrefixDir, L"file.flac");
        CHECK(!libRoot.Contains(siblingWithPrefix),
              "sibling with root name as string prefix");
        RemoveDirectoryW(siblingPrefixDir.c_str());
    }

    // (h) A directory junction inside the root pointing outside -> false
    std::wstring junctionPath = root + L"\\junction_to_outside";
    std::wstring junctionTarget = tempDir + L"\\kessoku_junction_target";
    if (CreateDirectoryW(junctionTarget.c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        CreateFileInDir(junctionTarget, L"outside.flac");

        bool junctionOk = CreateJunction(junctionPath, junctionTarget);
        if (junctionOk) {
            std::wstring viaJunction = junctionPath + L"\\outside.flac";
            CHECK(!libRoot.Contains(viaJunction),
                  "file reached through junction pointing outside");
            RemoveDirectoryW(junctionPath.c_str());
        } else {
            DWORD gle = GetLastError();
            if (gle == ERROR_PRIVILEGE_NOT_HELD) {
                printf("SKIP: junction test (requires elevation/Dev Mode)\n");
            } else {
                printf("SKIP: could not create junction\n");
            }
        }
    }

    // (i) A file symlink inside the root pointing outside -> false
    std::wstring symlinkPath = root + L"\\symlink_to_outside.flac";
    std::wstring symlinkTarget = tempDir + L"\\kessoku_symlink_target.flac";
    CreateFileInDir(tempDir, L"kessoku_symlink_target.flac");
    bool symlinkOk = CreateSymlink(symlinkPath, symlinkTarget);
    if (symlinkOk) {
        CHECK(!libRoot.Contains(symlinkPath),
              "file symlink pointing outside root");
        DeleteFileW(symlinkPath.c_str());
    } else {
        printf("SKIP: could not create symlink (requires Dev Mode or elevation)\n");
    }

    // (j) A path that IS inside the root but with different letter casing -> true
    std::wstring upperFile = rootFile;
    for (auto& ch : upperFile) {
        if (ch >= L'a' && ch <= L'z') {
            ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
    }
    CHECK(libRoot.Contains(upperFile),
          "path inside root with different casing");

    // (k) A candidate path that does not exist -> false
    CHECK(!libRoot.Contains(L"C:\\this\\path\\does\\not\\exist.flac"),
          "nonexistent candidate path");

    // (l) The root path itself -> true
    CHECK(libRoot.Contains(root), "root path itself");

    // (m) Create() with a non-existent path -> Err
    auto errResult = kessoku::core::LibraryRoot::Create(
        L"C:\\this\\path\\does\\not\\exist\\at\\all");
    CHECK(errResult.IsErr(), "Create() with non-existent path returns Err");

    // (n) A root selected at a volume root ("C:\") must contain its
    // descendants. Its canonical form ends with a separator, so the boundary
    // check has to accept that trailing separator as the boundary itself.
    wchar_t windowsDir[MAX_PATH]{};
    UINT windowsLen = GetWindowsDirectoryW(windowsDir, MAX_PATH);
    if (windowsLen > 0 && windowsLen < MAX_PATH) {
        std::wstring volumeRoot(windowsDir, 2); // e.g. "C:"
        volumeRoot += L'\\';

        auto volumeResult = kessoku::core::LibraryRoot::Create(volumeRoot);
        CHECK(volumeResult.IsOk(), "Create() with a volume root succeeds");
        if (volumeResult.IsOk()) {
            CHECK(volumeResult.Value().Contains(windowsDir),
                  "directory directly under a volume root");
        }
    } else {
        printf("SKIP: volume-root containment test "
               "(GetWindowsDirectoryW failed)\n");
    }

    // (o) A UNC network share is rejected before any filesystem access; the
    // app is local-only, and a share root cannot be scanned like a local
    // folder. This test needs no reachable share.
    auto uncResult = kessoku::core::LibraryRoot::Create(
        L"\\\\server\\share\\music");
    CHECK(uncResult.IsErr() &&
          uncResult.GetError().code == kessoku::core::ErrorCode::InvalidPath,
          "Create() with a UNC share returns InvalidPath");

    // (p) An empty path is not a usable library root.
    auto emptyResult = kessoku::core::LibraryRoot::Create(L"");
    CHECK(emptyResult.IsErr(), "Create() with an empty path returns Err");

    // (q) A file is not a library root. CreateFileW accepts files with
    // FILE_FLAG_BACKUP_SEMANTICS, so Create() must explicitly reject them.
    auto fileRootResult = kessoku::core::LibraryRoot::Create(rootFile);
    CHECK(fileRootResult.IsErr() &&
          fileRootResult.GetError().code ==
              kessoku::core::ErrorCode::InvalidPath,
          "Create() with a file path returns InvalidPath");

    // (r) A trailing separator and forward slashes canonicalize to the same
    // root and still contain its descendants.
    auto trailingRoot = kessoku::core::LibraryRoot::Create(root + L"\\");
    CHECK(trailingRoot.IsOk(), "Create() accepts a trailing separator");
    if (trailingRoot.IsOk()) {
        CHECK(trailingRoot.Value().Contains(rootFile),
              "trailing-separator root contains its files");
    }

    std::wstring forwardRoot = root;
    std::replace(forwardRoot.begin(), forwardRoot.end(), L'\\', L'/');
    auto forwardResult = kessoku::core::LibraryRoot::Create(forwardRoot);
    CHECK(forwardResult.IsOk(), "Create() accepts forward-slash paths");
    if (forwardResult.IsOk()) {
        CHECK(forwardResult.Value().Contains(rootFile),
              "forward-slash root contains its files");
    }

    // (s) A dot-dot path that resolves back inside the root is still inside;
    // a dot-dot path that escapes it is not.
    CHECK(libRoot.Contains(root + L"\\nested\\..\\test.flac"),
          "dot-dot traversal that stays inside root is accepted");
    CHECK(!libRoot.Contains(root + L"\\nested\\..\\..\\escape.flac"),
          "dot-dot traversal that escapes root is rejected");

    // (t) Extended-length syntax for a path inside the root resolves to the
    // same canonical path. Normalize first: the temp helper may leave a
    // doubled separator, which the literal \\?\ form would not collapse.
    const std::wstring canonicalRootFile =
        std::filesystem::weakly_canonical(rootFile).wstring();
    CHECK(libRoot.Contains(L"\\\\?\\" + canonicalRootFile),
          "extended-length path inside root is accepted");

    // (u) Unicode names must not confuse the case-insensitive prefix check.
    std::wstring unicodeDir = root + L"\\m\u00FAsica";
    if (CreateSubdir(root, L"m\u00FAsica")) {
        std::wstring unicodeFile;
        if (CreateFileInDir(unicodeDir, L"caf\u00E9.flac", &unicodeFile)) {
            CHECK(libRoot.Contains(unicodeFile),
                  "Unicode path inside root is accepted");
        } else {
            printf("SKIP: Unicode file test (could not create file)\n");
        }
    } else {
        printf("SKIP: Unicode directory test (could not create directory)\n");
    }

    // (v) A junction inside the root pointing to another directory inside
    // the root resolves inside the boundary and is accepted by Contains().
    // (The scanner deliberately does not traverse reparse points at all.)
    std::wstring insideTarget = root + L"\\inside_target";
    std::wstring insideJunction = root + L"\\inside_link";
    if (CreateSubdir(root, L"inside_target")) {
        CreateFileInDir(insideTarget, L"inside.flac");
        if (CreateJunction(insideJunction, insideTarget)) {
            CHECK(libRoot.Contains(insideJunction + L"\\inside.flac"),
                  "junction inside root pointing inside is accepted");
            RemoveDirectoryW(insideJunction.c_str());
        } else {
            printf("SKIP: inside-root junction test (could not create)\n");
        }
    }

    // (w) Empty and relative candidates have no resolved file inside the
    // root, so they must never be accepted by the boundary check.
    CHECK(!libRoot.Contains(L""), "empty candidate path is rejected");
    CHECK(!libRoot.Contains(L"relative.flac"),
          "relative candidate path is rejected");

    // (x) Win32 accepts forward slashes; the same file must stay inside.
    std::wstring forwardFile = rootFile;
    std::replace(forwardFile.begin(), forwardFile.end(), L'\\', L'/');
    CHECK(libRoot.Contains(forwardFile),
          "forward-slash path inside root is accepted");

    // --- Cleanup ---
    std::error_code cleanupEc;
    std::filesystem::remove_all(root, cleanupEc);
    std::filesystem::remove_all(tempDir + L"\\kessoku_junction_target",
                                cleanupEc);
    DeleteFileW((tempDir + L"\\kessoku_symlink_target.flac").c_str());

    std::wprintf(L"\n=== Results: %d failures ===\n", gFailures);
    return gFailures;
}

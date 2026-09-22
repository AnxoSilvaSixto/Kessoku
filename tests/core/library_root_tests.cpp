#include "kessoku/core/library_root.h"

#include <windows.h>

#include <cstdio>
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
    }

    // (e) An absolute path on a different drive letter -> false
    std::wstring otherDriveRoot;
    if (StringStartsWith(root, L"C:\\")) {
        otherDriveRoot = L"D:\\Windows\\System32\\cmd.exe";
    } else {
        otherDriveRoot = L"C:\\Windows\\System32\\cmd.exe";
    }
    CHECK(!libRoot.Contains(otherDriveRoot),
          "absolute path on different drive letter");

    // (f) A UNC path -> false
    std::wstring uncPath = L"\\\\server\\share\\file.flac";
    CHECK(!libRoot.Contains(uncPath), "UNC path");

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
        std::wstring targetFile = junctionTarget + L"\\outside.flac";
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

    // --- Cleanup ---
    DeleteFileW(rootFile.c_str());
    DeleteFileW(nestedFile.c_str());
    RemoveDirectoryW(nestedDir.c_str());
    RemoveDirectoryW(root.c_str());

    std::wprintf(L"\n=== Results: %d failures ===\n", gFailures);
    return gFailures;
}

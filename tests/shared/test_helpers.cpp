#include "test_helpers.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

namespace {

std::wstring GetTempParent() {
    std::wstring tempDir;
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, buf);
    if (len == 0 || len > MAX_PATH) return {};
    return std::wstring(buf);
}

} // namespace

std::wstring CreateTempDir(std::wstring_view prefix) {
    std::wstring tempParent = GetTempParent();
    if (tempParent.empty()) return {};

    std::wstring tempDir = tempParent + std::wstring(prefix);

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

bool CreateFileInDir(std::wstring_view dir, std::wstring_view filename,
                     std::wstring* outPath) {
    std::wstring fullPath = std::wstring(dir) + L"\\" +
                            std::wstring(filename);
    HANDLE h = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    if (outPath) *outPath = fullPath;
    return true;
}

bool CreateSubdir(std::wstring_view parent, std::wstring_view name) {
    std::wstring fullPath = std::wstring(parent) + L"\\" +
                            std::wstring(name);
    return CreateDirectoryW(fullPath.c_str(), nullptr) != FALSE ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

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

bool CreateSymlink(std::wstring_view symlinkPath,
                   std::wstring_view targetPath) {
    std::wstring sPath(symlinkPath);
    std::wstring tPath(targetPath);
    BOOL ok = CreateSymbolicLinkW(sPath.c_str(), tPath.c_str(),
                                  SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    return ok != FALSE;
}

std::vector<std::wstring> CreateFlacDirectory(std::wstring_view dir,
                                              int count) {
    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (int i = 0; i < count; ++i) {
        std::wstring name = L"track_" + std::to_wstring(i) + L".flac";
        std::wstring path;
        if (CreateFileInDir(dir, name, &path)) {
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

bool RemoveTempDir(std::wstring_view path) {
    // Use SHFileOperation or recursive removal.
    // For simplicity, use RemoveDirectoryW on empty dirs and DeleteFileW on files.
    // Actually, let's use a simple recursive approach via std::filesystem.
    std::error_code ec;
    std::filesystem::remove_all(std::wstring(path), ec);
    return !ec;
}

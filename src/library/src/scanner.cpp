#include "kessoku/library/scanner.h"

#include "kessoku/core/utf8.h"

#include <windows.h>

#include <array>
#include <system_error>

namespace kessoku::library {

namespace {

bool IsAudioExtension(std::wstring_view ext) {
    constexpr std::wstring_view kFlac = L".flac";
    constexpr std::wstring_view kWav = L".wav";
    if (ext.size() != kFlac.size() && ext.size() != kWav.size()) {
        return false;
    }
    if (ext.size() == kFlac.size()) {
        for (size_t i = 0; i < kFlac.size(); ++i) {
            wchar_t h = ext[i];
            wchar_t n = kFlac[i];
            if (h >= L'A' && h <= L'Z') h = static_cast<wchar_t>(h - L'A' + L'a');
            if (n >= L'A' && n <= L'Z') n = static_cast<wchar_t>(n - L'A' + L'a');
            if (h != n) return false;
        }
        return true;
    }
    for (size_t i = 0; i < kWav.size(); ++i) {
        wchar_t h = ext[i];
        wchar_t n = kWav[i];
        if (h >= L'A' && h <= L'Z') h = static_cast<wchar_t>(h - L'A' + L'a');
        if (n >= L'A' && n <= L'Z') n = static_cast<wchar_t>(n - L'A' + L'a');
        if (h != n) return false;
    }
    return true;
}

std::wstring GetErrorDescription(DWORD gle) {
    std::array<WCHAR, 512> buf{};
    DWORD result = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, gle, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf.data(), static_cast<DWORD>(buf.size()), nullptr);
    if (result == 0) {
        return L"Unknown error (" + std::to_wstring(gle) + L")";
    }
    while (result > 0 &&
           (buf[result - 1] == L'\r' || buf[result - 1] == L'\n' ||
            buf[result - 1] == L' ')) {
        --result;
    }
    std::wstring msg(buf.data(), result);
    std::wstring wideGLE = L" (error code " + std::to_wstring(gle) + L")";
    if (msg.size() + wideGLE.size() < buf.size()) {
        msg += wideGLE;
    }
    return msg;
}

// std::filesystem reports failures through std::error_code, which carries the
// Win32 error value in ec.value(). GetLastError() is unrelated at that point
// and must not be consulted.
std::string DescribeErrorCode(const std::error_code& ec) {
    return core::WideToUtf8(
        GetErrorDescription(static_cast<DWORD>(ec.value())));
}

void ScanDirectory(
    const std::filesystem::path& dir,
    const core::LibraryRoot& root,
    std::vector<std::filesystem::path>& files,
    std::vector<SkippedEntry>& skipped) {

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        skipped.push_back({dir, DescribeErrorCode(ec)});
        return;
    }

    const std::filesystem::directory_iterator end;
    while (it != end) {
        const std::filesystem::directory_entry entry = *it;

        std::error_code entryEc;
        bool isDir = entry.is_directory(entryEc);
        if (entryEc) {
            skipped.push_back({entry.path(), DescribeErrorCode(entryEc)});
        } else {
            entryEc.clear();
            bool isFile = entry.is_regular_file(entryEc);
            if (entryEc) {
                skipped.push_back({entry.path(), DescribeErrorCode(entryEc)});
            } else if (isDir) {
                DWORD attrs = GetFileAttributesW(entry.path().c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    skipped.push_back({entry.path(),
                        "reparse point (not followed)"});
                } else {
                    ScanDirectory(entry.path(), root, files, skipped);
                }
            } else if (isFile) {
                // File symlinks are reparse points too. Following one that
                // targets another file in the library would list the same
                // track twice, so skip reparse points uniformly.
                const DWORD attrs = GetFileAttributesW(entry.path().c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    skipped.push_back({entry.path(),
                        "reparse point (not followed)"});
                } else {
                    std::wstring ext = entry.path().extension().wstring();
                    if (IsAudioExtension(ext)) {
                        if (root.Contains(entry.path().wstring())) {
                            files.push_back(entry.path());
                        } else {
                            skipped.push_back({entry.path(),
                                "resolved outside library root"});
                        }
                    }
                }
            }
        }

        // Advance with the error_code overload. The range-for form calls the
        // throwing operator++, which would escape Scan() on a mid-iteration
        // I/O error; the UI runs this on a detached thread, where an
        // uncaught exception terminates the whole process.
        std::error_code incrementEc;
        it.increment(incrementEc);
        if (incrementEc) {
            skipped.push_back({dir, DescribeErrorCode(incrementEc)});
            return;
        }
    }
}

} // namespace

ScanResult Scan(const core::LibraryRoot& root) {
    ScanResult result;
    ScanDirectory(root.path(), root, result.files, result.skipped);
    return result;
}

} // namespace kessoku::library

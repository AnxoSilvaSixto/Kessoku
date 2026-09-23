#include "kessoku/library/scanner.h"

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

std::string WstringToUtf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int needed = WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed == 0) return {};
    std::string result(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                        static_cast<int>(wstr.size()),
                        result.data(), needed, nullptr, nullptr);
    return result;
}

void ScanDirectory(
    const std::filesystem::path& dir,
    const core::LibraryRoot& root,
    std::vector<std::filesystem::path>& files,
    std::vector<SkippedEntry>& skipped) {

    std::error_code ec;
    auto it = std::filesystem::directory_iterator(dir, ec);
    if (ec) {
        DWORD gle = GetLastError();
        skipped.push_back({dir, WstringToUtf8(GetErrorDescription(gle))});
        return;
    }

    for (const auto& entry : it) {
        std::error_code entryEc;
        bool isDir = entry.is_directory(entryEc);
        if (entryEc) {
            DWORD gle = GetLastError();
            skipped.push_back({entry.path(),
                WstringToUtf8(GetErrorDescription(gle))});
            continue;
        }
        entryEc.clear();
        bool isFile = entry.is_regular_file(entryEc);
        if (entryEc) {
            DWORD gle = GetLastError();
            skipped.push_back({entry.path(),
                WstringToUtf8(GetErrorDescription(gle))});
            continue;
        }

        if (isDir) {
            ScanDirectory(entry.path(), root, files, skipped);
        } else if (isFile) {
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

} // namespace

ScanResult Scan(const core::LibraryRoot& root) {
    ScanResult result;
    ScanDirectory(root.path(), root, result.files, result.skipped);
    return result;
}

} // namespace kessoku::library

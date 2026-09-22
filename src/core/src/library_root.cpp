#include "kessoku/core/library_root.h"

#include <windows.h>

#include <array>
#include <string>
#include <string_view>

namespace kessoku::core {

namespace {

constexpr DWORD kPathBufferSize = 32767;

std::wstring ResolveFinalPath(std::wstring_view path) {
    std::wstring nullTerminated(path);
    HANDLE h = CreateFileW(
        nullTerminated.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );

    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::array<WCHAR, kPathBufferSize> buf{};
    DWORD result = GetFinalPathNameByHandleW(h, buf.data(),
                                              static_cast<DWORD>(buf.size()),
                                              FILE_NAME_NORMALIZED |
                                              VOLUME_NAME_DOS);
    CloseHandle(h);

    if (result == 0 || result >= buf.size()) {
        return {};
    }

    return std::wstring(buf.data(), result);
}

bool PathHasRootBoundary(std::wstring_view resolved,
                         std::wstring_view canonicalRoot) {
    if (resolved.size() == canonicalRoot.size()) {
        return true;
    }

    if (resolved.size() <= canonicalRoot.size()) {
        return false;
    }

    return resolved[canonicalRoot.size()] == L'\\';
}

bool StringStartsWithOrdinalIAscii(std::wstring_view haystack,
                                    std::wstring_view needle) {
    if (haystack.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i < needle.size(); ++i) {
        wchar_t h = haystack[i];
        wchar_t n = needle[i];
        if (h >= L'A' && h <= L'Z') h = static_cast<wchar_t>(h - L'A' + L'a');
        if (n >= L'A' && n <= L'Z') n = static_cast<wchar_t>(n - L'A' + L'a');
        if (h != n) return false;
    }
    return true;
}

} // namespace

Result<LibraryRoot> LibraryRoot::Create(std::wstring_view selectedPath) {
    std::wstring resolved = ResolveFinalPath(selectedPath);

    if (resolved.empty()) {
        DWORD gle = GetLastError();
        if (gle == ERROR_ACCESS_DENIED || gle == ERROR_SHARING_VIOLATION) {
            return kessoku::core::Result<LibraryRoot>::Err(
                ErrorCode::AccessDenied,
                "Access denied to selected library path");
        }
        return kessoku::core::Result<LibraryRoot>::Err(
            ErrorCode::NotFound,
            "Library path not found or invalid");
    }

    // Strip the \\?\ prefix that GetFinalPathNameByHandleW returns
    // with VOLUME_NAME_DOS.
    constexpr std::wstring_view kPrefix = LR"(\\?\)";
    if (StringStartsWithOrdinalIAscii(resolved, kPrefix)) {
        resolved.erase(0, kPrefix.size());
    }

    return kessoku::core::Result<LibraryRoot>::Ok(
        LibraryRoot{std::move(resolved)});
}

bool LibraryRoot::Contains(std::wstring_view candidate) const {
    std::wstring resolved = ResolveFinalPath(candidate);

    if (resolved.empty()) {
        return false;
    }

    // Strip the \\?\ prefix.
    constexpr std::wstring_view kPrefix = LR"(\\?\)";
    if (StringStartsWithOrdinalIAscii(resolved, kPrefix)) {
        resolved.erase(0, kPrefix.size());
    }

    // resolved must be at least as long as the root for the prefix
    // comparison and boundary check below to be safe.
    if (resolved.size() < canonicalPath_.size()) {
        return false;
    }

    // Ordinal, case-insensitive prefix check (compare only root-length
    // characters of the candidate against the full canonical root).
    if (CompareStringOrdinal(
            resolved.data(), static_cast<int>(canonicalPath_.size()),
            canonicalPath_.data(), static_cast<int>(canonicalPath_.size()),
            TRUE) != CSTR_EQUAL) {
        return false;
    }

    // Boundary check: either same length or next char is backslash.
    return PathHasRootBoundary(resolved, canonicalPath_);
}

} // namespace kessoku::core

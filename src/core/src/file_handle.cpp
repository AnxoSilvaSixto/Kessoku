#include "kessoku/core/file_handle.h"

#include "kessoku/core/library_root.h"
#include "kessoku/core/utf8.h"

#define NOMINMAX
#include <windows.h>

#include <array>
#include <utility>

namespace kessoku::core {

namespace {

constexpr DWORD kPathBufferSize = 32767;

ErrorCode MapOpenError(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
            return ErrorCode::NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            return ErrorCode::AccessDenied;
        default:
            return ErrorCode::UnknownError;
    }
}

std::string DescribeOpenFailure(std::wstring_view path) {
    const std::string utf8 = WideToUtf8(path);
    if (utf8.empty()) {
        return "Could not open file";
    }
    return "Could not open file: " + utf8;
}

} // namespace

FileHandle::~FileHandle() {
    Close();
}

FileHandle::FileHandle(FileHandle&& other) noexcept
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

Result<FileHandle> FileHandle::Open(std::wstring_view path) {
    const std::wstring nullTerminated(path);
    HANDLE h = CreateFileW(
        nullTerminated.c_str(),
        GENERIC_READ,
        // Keep the handle bound to this file object but do not block
        // legitimate external writers: tag editors and tests truncate or
        // rewrite the file while a Player exists. The containment decision
        // is about the file object this handle names, not about locking it.
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        return Result<FileHandle>::Err(MapOpenError(GetLastError()),
                                       DescribeOpenFailure(path));
    }

    return Result<FileHandle>::Ok(FileHandle(h));
}

Result<FileHandle> FileHandle::OpenContained(const LibraryRoot& root,
                                             std::wstring_view path) {
    Result<FileHandle> opened = Open(path);
    if (opened.IsErr()) {
        return opened;
    }

    std::wstring finalPath;
    if (!opened.Value().FinalPath(finalPath)) {
        return Result<FileHandle>::Err(
            ErrorCode::InvalidPath,
            "Could not resolve the opened file path");
    }

    if (!root.ContainsResolvedPath(finalPath)) {
        return Result<FileHandle>::Err(
            ErrorCode::InvalidPath,
            "Path resolves outside the library root");
    }

    return opened;
}

bool FileHandle::FinalPath(std::wstring& out) const {
    if (handle_ == nullptr) {
        return false;
    }

    std::array<WCHAR, kPathBufferSize> buffer{};
    const DWORD length = GetFinalPathNameByHandleW(
        static_cast<HANDLE>(handle_), buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);

    if (length == 0 || length >= buffer.size()) {
        return false;
    }

    out.assign(buffer.data(), length);
    return true;
}

bool FileHandle::Size(uint64_t& out) const noexcept {
    if (handle_ == nullptr) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &size) ||
        size.QuadPart < 0) {
        return false;
    }

    out = static_cast<uint64_t>(size.QuadPart);
    return true;
}

void FileHandle::Close() noexcept {
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

} // namespace kessoku::core

#pragma once

#include "kessoku/core/result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace kessoku::core {

class LibraryRoot;

// Owning read handle for one file. The handle is opened once, and every
// later read uses that same handle, so a path cannot be swapped for a
// junction or symlink between a containment check and a file operation.
//
// Handles are intentionally exposed as void* so public headers do not pull in
// <windows.h>; the implementation casts to HANDLE.
class FileHandle {
public:
    FileHandle() = default;
    ~FileHandle();

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept;
    FileHandle& operator=(FileHandle&& other) noexcept;

    // Open a file for reading without a containment check. Used by callers
    // whose path policy is enforced elsewhere (tests, benchmarks).
    static Result<FileHandle> Open(std::wstring_view path);

    // Open a file for reading and require the path resolved from this exact
    // handle to be inside `root`. This is the race-free containment check:
    // the decision and the later reads are bound to the same file object.
    static Result<FileHandle> OpenContained(const LibraryRoot& root,
                                            std::wstring_view path);

    bool IsValid() const noexcept { return handle_ != nullptr; }
    void* get() const noexcept { return handle_; }

    // Resolve the final path from the open handle. Returns false when the
    // path cannot be resolved (fail closed).
    bool FinalPath(std::wstring& out) const;

    // Current file size. Returns false on failure.
    bool Size(uint64_t& out) const noexcept;

private:
    explicit FileHandle(void* handle) : handle_(handle) {}

    void Close() noexcept;

    void* handle_ = nullptr;
};

} // namespace kessoku::core

#pragma once

#include "kessoku/core/result.h"
#include <string>

namespace kessoku::core {

class LibraryRoot {
public:
    static Result<LibraryRoot> Create(std::wstring_view selectedPath);

    bool Contains(std::wstring_view candidate) const;

    // Check a path that has already been resolved from an open handle (for
    // example by GetFinalPathNameByHandleW). No filesystem operation runs
    // here, so callers can bind the containment decision to the same handle
    // they later read.
    bool ContainsResolvedPath(std::wstring_view resolvedPath) const;

    const std::wstring& path() const { return canonicalPath_; }

private:
    explicit LibraryRoot(std::wstring canonicalPath)
        : canonicalPath_(std::move(canonicalPath)) {}

    std::wstring canonicalPath_;
};

} // namespace kessoku::core

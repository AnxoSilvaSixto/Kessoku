#pragma once

#include "kessoku/core/library_root.h"
#include "kessoku/core/result.h"

#include <filesystem>
#include <string>
#include <cstdint>

namespace kessoku::library {

struct TrackMetadata {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    uint32_t trackNumber; // 0 means "not present"
};

// Read tags from a file whose containment the caller has already checked.
// The application should prefer the LibraryRoot overload below.
core::Result<TrackMetadata> ReadTrackMetadata(const std::filesystem::path& filePath);

// Read tags after re-checking that the file still resolves inside `root`
// immediately before each open. This narrows the scan-time check-then-open
// window; it does not replace binding the canonical handle to the parser.
core::Result<TrackMetadata> ReadTrackMetadata(
    const core::LibraryRoot& root,
    const std::filesystem::path& filePath);

} // namespace kessoku::library

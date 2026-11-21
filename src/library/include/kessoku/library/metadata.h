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

// Read tags after verifying that the file resolves inside `root`. The file
// is opened once; the containment decision and every TagLib read are bound to
// that same handle, so a path swapped for a junction or symlink after the
// check cannot redirect metadata parsing.
core::Result<TrackMetadata> ReadTrackMetadata(
    const core::LibraryRoot& root,
    const std::filesystem::path& filePath);

} // namespace kessoku::library

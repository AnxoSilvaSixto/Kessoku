#pragma once

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

core::Result<TrackMetadata> ReadTrackMetadata(const std::filesystem::path& filePath);

} // namespace kessoku::library

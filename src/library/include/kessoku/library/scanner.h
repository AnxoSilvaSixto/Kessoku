#pragma once

#include "kessoku/core/library_root.h"

#include <filesystem>
#include <string>
#include <vector>

namespace kessoku::library {

struct SkippedEntry {
    std::filesystem::path path;
    std::string reason;
};

struct ScanResult {
    std::vector<std::filesystem::path> files;
    std::vector<SkippedEntry> skipped;
};

ScanResult Scan(const core::LibraryRoot& root);

} // namespace kessoku::library

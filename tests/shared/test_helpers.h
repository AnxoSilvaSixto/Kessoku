#pragma once

#include <string>
#include <string_view>
#include <vector>

// Test helpers shared between library_root_tests and scanner_tests.

std::wstring CreateTempDir(std::wstring_view prefix);

bool CreateFileInDir(std::wstring_view dir, std::wstring_view filename,
                     std::wstring* outPath = nullptr);

bool CreateSubdir(std::wstring_view parent, std::wstring_view name);

bool CreateJunction(std::wstring_view junctionPath,
                    std::wstring_view targetPath);

bool CreateSymlink(std::wstring_view symlinkPath,
                   std::wstring_view targetPath);

// Create a directory full of FLAC files (returns list of created paths).
std::vector<std::wstring> CreateFlacDirectory(std::wstring_view dir,
                                              int count = 5);

// Cleanup: remove a temp directory and all contents.
bool RemoveTempDir(std::wstring_view path);

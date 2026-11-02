#pragma once

#include <string>
#include <string_view>

namespace kessoku::core {

// UTF-8 conversion helpers shared by every layer that talks to Win32 or
// libFLAC, both of which exchange UTF-8 on one side and UTF-16 on the other.
//
// Both functions return an empty string when conversion fails; an empty input
// also yields an empty result, so callers that cannot distinguish "empty" from
// "failed" should check the input first.
std::string WideToUtf8(std::wstring_view text);

std::wstring Utf8ToWide(std::string_view text);

} // namespace kessoku::core

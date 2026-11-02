#include "kessoku/core/utf8.h"

#define NOMINMAX
#include <windows.h>

#include <climits>

namespace kessoku::core {

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<size_t>(INT_MAX)) {
        return {};
    }
    const int inputLength = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), inputLength, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), inputLength, result.data(), required, nullptr,
        nullptr);
    if (written != required) {
        return {};
    }
    return result;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<size_t>(INT_MAX)) {
        return {};
    }
    const int inputLength = static_cast<int>(text.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), inputLength, nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), inputLength, result.data(), required);
    if (written != required) {
        return {};
    }
    return result;
}

} // namespace kessoku::core

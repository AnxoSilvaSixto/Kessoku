#pragma once

#include <string>
#include <string_view>

namespace kessoku::core {

enum class ErrorCode {
    Ok,
    NotFound,
    AccessDenied,
    InvalidPath,
    UnknownError
};

struct Error {
    ErrorCode code;
    std::string message;
};

} // namespace kessoku::core

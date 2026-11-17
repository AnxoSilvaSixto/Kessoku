#pragma once

#include <string>
#include <string_view>

namespace kessoku::core {

enum class ErrorCode {
    Ok,
    NotFound,
    AccessDenied,
    InvalidPath,
    DeviceNotFound,
    FormatNotSupported,
    // The device rejected a format the player itself considers valid. Kept
    // distinct from FormatNotSupported so a test can tell "this endpoint
    // cannot do it" (environmental, may skip) apart from "this file is not
    // playable" (a product result that must still assert).
    DeviceFormatRejected,
    ExclusiveModeUnavailable,
    AudioInitFailed,
    UnknownError
};

struct Error {
    ErrorCode code;
    std::string message;
};

} // namespace kessoku::core

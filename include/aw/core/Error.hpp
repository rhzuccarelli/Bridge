#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace aw::core {

/// Machine readable failure reason.
///
/// The set is intentionally small and closed.  Callers switch on the code;
/// the human readable message that travels with it is for logs and the display,
/// never for control flow.
enum class ErrorCode : std::uint16_t {
    Unknown = 0,
    NotImplemented,     ///< Placeholder scaffolding, or a backend that does not support the call.
    InvalidArgument,
    InvalidState,       ///< Correct call, wrong moment (e.g. seek before open).
    NotFound,
    AlreadyExists,
    PermissionDenied,
    IoError,
    Corrupt,            ///< Data read back does not match what was written.
    UnsupportedFormat,  ///< Well formed, but outside what this device handles (e.g. 96 kHz).
    InvalidFormat,      ///< Malformed container/header.
    OutOfMemory,
    OutOfSpace,
    DeviceNotConnected,
    DeviceBusy,
    ProtocolError,      ///< MTP/USB level failure.
    Timeout,
    Cancelled,          ///< User aborted a long running operation.
};

/// Stable, allocation free name of an error code, for logs and screens.
[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

/// A failure value.
///
/// `Error` is deliberately *not* an exception type.  Every fallible operation
/// in the portable layers returns `Result<T>` instead of throwing, because the
/// same code has to compile for a target where exceptions cost flash we do not
/// have.  Exceptions remain legal inside platform backends that already pay for
/// them (std::filesystem, libmtp wrappers) but must be converted at the module
/// boundary.
class Error {
public:
    Error() = default;

    explicit Error(ErrorCode code, std::string message = {})
        : code_{code}, message_{std::move(message)} {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// "NotFound: Track01.wav" - suitable for a log line or a status bar.
    [[nodiscard]] std::string describe() const;

private:
    ErrorCode code_{ErrorCode::Unknown};
    std::string message_;
};

/// Convenience factory so call sites read as `return makeError(ErrorCode::NotFound, path);`
[[nodiscard]] inline Error makeError(ErrorCode code, std::string message = {}) {
    return Error{code, std::move(message)};
}

/// Uniform marker for Phase 1 scaffolding.  Every stub returns this, so a
/// half-wired build fails loudly and traceably instead of silently doing nothing.
[[nodiscard]] inline Error notImplemented(std::string what) {
    return Error{ErrorCode::NotImplemented, std::move(what)};
}

}  // namespace aw::core

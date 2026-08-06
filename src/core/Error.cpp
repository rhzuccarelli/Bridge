#include "aw/core/Error.hpp"

namespace aw::core {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::NotImplemented: return "NotImplemented";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::InvalidState: return "InvalidState";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::IoError: return "IoError";
        case ErrorCode::Corrupt: return "Corrupt";
        case ErrorCode::UnsupportedFormat: return "UnsupportedFormat";
        case ErrorCode::InvalidFormat: return "InvalidFormat";
        case ErrorCode::OutOfMemory: return "OutOfMemory";
        case ErrorCode::OutOfSpace: return "OutOfSpace";
        case ErrorCode::DeviceNotConnected: return "DeviceNotConnected";
        case ErrorCode::DeviceBusy: return "DeviceBusy";
        case ErrorCode::ProtocolError: return "ProtocolError";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

std::string Error::describe() const {
    std::string out{toString(code_)};
    if (!message_.empty()) {
        out += ": ";
        out += message_;
    }
    return out;
}

}  // namespace aw::core

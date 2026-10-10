#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "UsbClient.hpp"

namespace probe {

namespace ptp {

inline constexpr std::uint16_t kGetDeviceInfo = 0x1001;
inline constexpr std::uint16_t kOpenSession = 0x1002;
inline constexpr std::uint16_t kCloseSession = 0x1003;
inline constexpr std::uint16_t kGetStorageIds = 0x1004;
inline constexpr std::uint16_t kGetStorageInfo = 0x1005;
inline constexpr std::uint16_t kGetObjectHandles = 0x1007;
inline constexpr std::uint16_t kGetObjectInfo = 0x1008;
inline constexpr std::uint16_t kGetPartialObject = 0x101B;

inline constexpr std::uint16_t kResponseOk = 0x2001;
inline constexpr std::uint16_t kSessionAlreadyOpen = 0x201E;

inline constexpr std::uint16_t kFormatAssociation = 0x3001;  ///< A folder.
inline constexpr std::uint32_t kRootParent = 0xFFFFFFFFu;

}  // namespace ptp

/// Receives a data phase as it streams in. Return false to stop collecting
/// (the transfer itself still drains; PTP has no mid-phase abort here).
using PtpDataSink = bool (*)(std::span<const std::uint8_t> chunk, void* context);

struct PtpResponse {
    std::uint16_t code{0};
    std::array<std::uint32_t, 5> params{};
    std::size_t paramCount{0};
};

/// A minimal PTP/MTP initiator over the Still Image class bulk pipes.
///
/// Only the device-to-host direction is implemented: this is a probe, and the
/// questions it answers (does the session open, can objects be listed, how
/// fast does data move) need nothing else. Uploads belong to the real backend.
class PtpSession {
public:
    PtpSession(BulkPipe& out, BulkPipe& in) : out_{out}, in_{in} {}

    esp_err_t transact(std::uint16_t operation, std::initializer_list<std::uint32_t> params,
                       PtpDataSink sink, void* context, PtpResponse& response,
                       std::uint32_t timeoutMs = 5000);

    /// Convenience: collect the whole data phase into `data`.
    esp_err_t transact(std::uint16_t operation, std::initializer_list<std::uint32_t> params,
                       std::vector<std::uint8_t>& data, PtpResponse& response,
                       std::uint32_t timeoutMs = 5000);

    esp_err_t open();
    esp_err_t close();

private:
    esp_err_t readContainer(std::size_t& received, std::uint32_t timeoutMs);

    BulkPipe& out_;
    BulkPipe& in_;
    std::uint32_t nextTransaction_{0};
};

/// Little-endian dataset reader with sticky failure, for parsing PTP datasets.
class DatasetReader {
public:
    explicit DatasetReader(std::span<const std::uint8_t> data) : data_{data} {}

    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::string string();  ///< PTP string: count byte, then UTF-16LE (ASCII kept).
    std::vector<std::uint16_t> u16Array();
    std::vector<std::uint32_t> u32Array();
    void skip(std::size_t bytes);

    [[nodiscard]] bool ok() const { return ok_; }

private:
    bool take(std::size_t bytes);

    std::span<const std::uint8_t> data_;
    std::size_t offset_{0};
    bool ok_{true};
};

}  // namespace probe

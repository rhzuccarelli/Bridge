#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "BulkPipe.hpp"

namespace aw::platform::esp32p4::usb {

namespace ptp {

inline constexpr std::uint16_t kGetDeviceInfo = 0x1001;
inline constexpr std::uint16_t kOpenSession = 0x1002;
inline constexpr std::uint16_t kCloseSession = 0x1003;
inline constexpr std::uint16_t kGetStorageIds = 0x1004;
inline constexpr std::uint16_t kGetStorageInfo = 0x1005;
inline constexpr std::uint16_t kGetObjectHandles = 0x1007;
inline constexpr std::uint16_t kGetObjectInfo = 0x1008;
inline constexpr std::uint16_t kDeleteObject = 0x100B;
inline constexpr std::uint16_t kSendObjectInfo = 0x100C;
inline constexpr std::uint16_t kSendObject = 0x100D;
inline constexpr std::uint16_t kGetPartialObject = 0x101B;

inline constexpr std::uint16_t kResponseOk = 0x2001;
inline constexpr std::uint16_t kInvalidObjectHandle = 0x2009;
inline constexpr std::uint16_t kStoreFull = 0x200C;
inline constexpr std::uint16_t kDeviceBusy = 0x2019;
inline constexpr std::uint16_t kSessionAlreadyOpen = 0x201E;

inline constexpr std::uint16_t kFormatUndefined = 0x3000;
inline constexpr std::uint16_t kFormatAssociation = 0x3001;  ///< A folder.
inline constexpr std::uint16_t kFormatWav = 0x3008;

}  // namespace ptp

/// Receives a data phase as it streams in. Return false to stop collecting
/// (the transfer itself still drains; PTP has no mid-phase abort).
using PtpDataSink = std::function<bool(std::span<const std::uint8_t> chunk)>;

/// Fills `buffer` with the next bytes of an outgoing data phase and returns
/// how many it wrote; 0 before the declared size is reached is an error.
using PtpDataSource = std::function<std::size_t(std::span<std::uint8_t> buffer)>;

struct PtpResponse {
    std::uint16_t code{0};
    std::array<std::uint32_t, 5> params{};
    std::size_t paramCount{0};
};

/// A minimal PTP/MTP initiator over the Still Image class bulk pipes.
/// Grown from the TP-7 probe; adds the host-to-device data phase.
class PtpSession {
public:
    PtpSession(BulkPipe& out, BulkPipe& in) : out_{out}, in_{in} {}

    /// Operation with an optional device-to-host data phase.
    esp_err_t transact(std::uint16_t operation, std::initializer_list<std::uint32_t> params,
                       const PtpDataSink& sink, PtpResponse& response,
                       std::uint32_t timeoutMs = 5000);

    /// Convenience: collect the whole data phase into `data`.
    esp_err_t transact(std::uint16_t operation, std::initializer_list<std::uint32_t> params,
                       std::vector<std::uint8_t>& data, PtpResponse& response,
                       std::uint32_t timeoutMs = 5000);

    /// Operation with a host-to-device data phase of exactly `bytes`.
    /// `responseTimeoutMs` covers the device committing the data afterwards.
    esp_err_t transactOut(std::uint16_t operation, std::initializer_list<std::uint32_t> params,
                          std::uint64_t bytes, const PtpDataSource& source,
                          PtpResponse& response, std::uint32_t timeoutMs = 5000,
                          std::uint32_t responseTimeoutMs = 30000);

    esp_err_t open();
    esp_err_t close();

private:
    esp_err_t sendCommand(std::uint16_t operation, std::initializer_list<std::uint32_t> params,
                          std::uint32_t timeoutMs);
    esp_err_t readContainer(std::size_t& received, std::uint32_t timeoutMs);
    esp_err_t readResponse(std::size_t received, PtpResponse& response);

    BulkPipe& out_;
    BulkPipe& in_;
    std::uint32_t nextTransaction_{0};
    std::uint32_t lastTransaction_{0};  ///< Of the last command, for its data phase.
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

/// Little-endian dataset builder, the inverse of `DatasetReader`.
class DatasetWriter {
public:
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void string(const std::string& text);  ///< Count byte (with NUL), UTF-16LE.
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

}  // namespace aw::platform::esp32p4::usb

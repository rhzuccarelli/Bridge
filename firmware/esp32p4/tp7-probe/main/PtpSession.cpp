#include "PtpSession.hpp"

#include <algorithm>

#include "esp_log.h"

namespace probe {
namespace {

constexpr const char* kTag = "ptp";
constexpr std::size_t kHeaderBytes = 12;
constexpr std::uint16_t kTypeCommand = 1;
constexpr std::uint16_t kTypeData = 2;
constexpr std::uint16_t kTypeResponse = 3;

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

bool collect(std::span<const std::uint8_t> chunk, void* context) {
    auto& data = *static_cast<std::vector<std::uint8_t>*>(context);
    data.insert(data.end(), chunk.begin(), chunk.end());
    return true;
}

}  // namespace

esp_err_t PtpSession::readContainer(std::size_t& received, std::uint32_t timeoutMs) {
    // A data phase that ended exactly on a packet boundary is followed by a
    // zero-length packet; it is not the next container, so skip it.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (const esp_err_t err = in_.read(in_.capacity(), received, timeoutMs); err != ESP_OK)
            return err;
        if (received != 0) return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t PtpSession::transact(std::uint16_t operation,
                               std::initializer_list<std::uint32_t> params, PtpDataSink sink,
                               void* context, PtpResponse& response, std::uint32_t timeoutMs) {
    if (params.size() > 5) return ESP_ERR_INVALID_ARG;
    const std::uint32_t transaction = operation == ptp::kOpenSession ? 0 : nextTransaction_++;
    if (operation == ptp::kOpenSession) nextTransaction_ = 1;

    std::array<std::uint8_t, kHeaderBytes + 5 * 4> command{};
    const std::size_t commandBytes = kHeaderBytes + params.size() * 4;
    put32(command.data(), static_cast<std::uint32_t>(commandBytes));
    put16(command.data() + 4, kTypeCommand);
    put16(command.data() + 6, operation);
    put32(command.data() + 8, transaction);
    std::size_t offset = kHeaderBytes;
    for (const std::uint32_t param : params) {
        put32(command.data() + offset, param);
        offset += 4;
    }
    if (const esp_err_t err = out_.write({command.data(), commandBytes}, true, timeoutMs);
        err != ESP_OK) {
        ESP_LOGE(kTag, "op 0x%04x: command phase failed (%s)", operation,
                 transferStatusName(out_.lastStatus()));
        return err;
    }

    std::size_t received = 0;
    if (const esp_err_t err = readContainer(received, timeoutMs); err != ESP_OK) {
        ESP_LOGE(kTag, "op 0x%04x: no data/response (%s)", operation,
                 transferStatusName(in_.lastStatus()));
        return err;
    }
    if (received < kHeaderBytes) return ESP_ERR_INVALID_RESPONSE;

    if (le16(in_.data() + 4) == kTypeData) {
        const std::uint32_t total = le32(in_.data());
        if (total < kHeaderBytes) return ESP_ERR_INVALID_RESPONSE;
        std::size_t remaining = total - received;
        bool wanted = sink == nullptr ||
                      sink({in_.data() + kHeaderBytes, received - kHeaderBytes}, context);
        while (remaining > 0) {
            if (const esp_err_t err = in_.read(remaining, received, timeoutMs); err != ESP_OK) {
                ESP_LOGE(kTag, "op 0x%04x: data phase failed with %u bytes left (%s)", operation,
                         static_cast<unsigned>(remaining), transferStatusName(in_.lastStatus()));
                return err;
            }
            if (received == 0 || received > remaining) return ESP_ERR_INVALID_RESPONSE;
            if (wanted && sink != nullptr) wanted = sink({in_.data(), received}, context);
            remaining -= received;
        }
        if (const esp_err_t err = readContainer(received, timeoutMs); err != ESP_OK) return err;
        if (received < kHeaderBytes) return ESP_ERR_INVALID_RESPONSE;
    }

    if (le16(in_.data() + 4) != kTypeResponse) return ESP_ERR_INVALID_RESPONSE;
    response.code = le16(in_.data() + 6);
    response.paramCount = std::min<std::size_t>((received - kHeaderBytes) / 4, 5);
    for (std::size_t i = 0; i < response.paramCount; ++i)
        response.params[i] = le32(in_.data() + kHeaderBytes + 4 * i);
    return ESP_OK;
}

esp_err_t PtpSession::transact(std::uint16_t operation,
                               std::initializer_list<std::uint32_t> params,
                               std::vector<std::uint8_t>& data, PtpResponse& response,
                               std::uint32_t timeoutMs) {
    data.clear();
    return transact(operation, params, &collect, &data, response, timeoutMs);
}

esp_err_t PtpSession::open() {
    PtpResponse response;
    if (const esp_err_t err = transact(ptp::kOpenSession, {1}, nullptr, nullptr, response);
        err != ESP_OK)
        return err;
    if (response.code == ptp::kResponseOk || response.code == ptp::kSessionAlreadyOpen)
        return ESP_OK;
    ESP_LOGE(kTag, "OpenSession answered 0x%04x", response.code);
    return ESP_FAIL;
}

esp_err_t PtpSession::close() {
    PtpResponse response;
    return transact(ptp::kCloseSession, {}, nullptr, nullptr, response);
}

bool DatasetReader::take(std::size_t bytes) {
    if (!ok_ || data_.size() - offset_ < bytes) {
        ok_ = false;
        return false;
    }
    return true;
}

std::uint16_t DatasetReader::u16() {
    if (!take(2)) return 0;
    const std::uint16_t value = le16(data_.data() + offset_);
    offset_ += 2;
    return value;
}

std::uint32_t DatasetReader::u32() {
    if (!take(4)) return 0;
    const std::uint32_t value = le32(data_.data() + offset_);
    offset_ += 4;
    return value;
}

std::uint64_t DatasetReader::u64() {
    const std::uint64_t low = u32();
    const std::uint64_t high = u32();
    return low | (high << 32);
}

std::string DatasetReader::string() {
    if (!take(1)) return {};
    const std::size_t chars = data_[offset_++];
    std::string text;
    for (std::size_t i = 0; i < chars; ++i) {
        const std::uint16_t unit = u16();
        if (!ok_) return {};
        if (unit == 0) continue;
        text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return text;
}

std::vector<std::uint16_t> DatasetReader::u16Array() {
    const std::uint32_t count = u32();
    std::vector<std::uint16_t> values;
    if (!take(static_cast<std::size_t>(count) * 2)) return values;
    values.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) values.push_back(u16());
    return values;
}

std::vector<std::uint32_t> DatasetReader::u32Array() {
    const std::uint32_t count = u32();
    std::vector<std::uint32_t> values;
    if (!take(static_cast<std::size_t>(count) * 4)) return values;
    values.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) values.push_back(u32());
    return values;
}

void DatasetReader::skip(std::size_t bytes) {
    if (take(bytes)) offset_ += bytes;
}

}  // namespace probe

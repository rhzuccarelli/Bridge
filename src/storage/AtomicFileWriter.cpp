#include "aw/storage/AtomicFileWriter.hpp"

namespace aw::storage {

AtomicFileWriter::AtomicFileWriter(hal::StorageInterface& storage, core::Path target)
    : storage_{storage}, target_{std::move(target)} {
    temporary_ = target_;
    temporary_ += ".tmp";
}

AtomicFileWriter::~AtomicFileWriter() {
    if (!committed_) {
        abort();
    }
}

core::Status AtomicFileWriter::open() {
    auto handle = storage_.open(temporary_, hal::OpenMode::Write);
    if (!handle) {
        return handle.error();
    }
    file_ = std::move(handle).value();
    return core::success();
}

core::Status AtomicFileWriter::write(std::span<const std::byte> data) {
    if (!file_) {
        return core::makeError(core::ErrorCode::InvalidState, "AtomicFileWriter not open");
    }
    auto written = file_->write(data);
    if (!written) {
        return written.error();
    }
    if (written.value() != data.size()) {
        return core::makeError(core::ErrorCode::OutOfSpace, target_.string());
    }
    return core::success();
}

core::Status AtomicFileWriter::commit() {
    if (!file_) {
        return core::makeError(core::ErrorCode::InvalidState, "AtomicFileWriter not open");
    }
    if (auto status = file_->flush(); !status) {
        return status;
    }
    file_.reset();  // Close before renaming: FAT does not like renaming open files.

    if (auto status = storage_.rename(temporary_, target_); !status) {
        return status;
    }
    committed_ = true;
    return core::success();
}

void AtomicFileWriter::abort() noexcept {
    file_.reset();
    if (storage_.exists(temporary_)) {
        (void)storage_.remove(temporary_);
    }
}

}  // namespace aw::storage

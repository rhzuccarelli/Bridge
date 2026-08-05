#pragma once

#include <memory>
#include <span>

#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/hal/StorageInterface.hpp"

namespace aw::storage {

/// Write-to-temp-then-rename helper.
///
/// Exists because the device has no shutdown button worth trusting: users will
/// pull power mid-save.  Writing `metadata.json` in place means one unlucky
/// moment destroys the project.  Writing a temp file, flushing it, then
/// renaming over the target makes the update atomic on both ext4 and FAT.
///
/// RAII: if `commit()` is not called, the destructor removes the temp file, so
/// an early return can never leave debris behind.
class AtomicFileWriter {
public:
    AtomicFileWriter(hal::StorageInterface& storage, core::Path target);
    ~AtomicFileWriter();

    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

    [[nodiscard]] core::Status open();
    [[nodiscard]] core::Status write(std::span<const std::byte> data);

    /// Flushes, closes and renames over the target.  After success the writer
    /// is inert.
    [[nodiscard]] core::Status commit();

    /// Discards the temp file.  Also called by the destructor.
    void abort() noexcept;

private:
    hal::StorageInterface& storage_;
    core::Path target_;
    core::Path temporary_;
    std::unique_ptr<hal::FileHandle> file_;
    bool committed_{false};
};

}  // namespace aw::storage

#include "support/MemoryStorage.hpp"

namespace awtest {
namespace {

/// A handle over a vector owned by the MemoryStorage that produced it.
class MemoryFile final : public aw::hal::FileHandle {
public:
    MemoryFile(std::vector<std::byte>& contents, bool writable)
        : contents_{contents}, writable_{writable} {}

    aw::core::Result<std::size_t> read(std::span<std::byte> destination) override {
        if (position_ >= contents_.size()) {
            return std::size_t{0};
        }
        const std::size_t available = contents_.size() - position_;
        const std::size_t count = std::min(available, destination.size());
        std::copy_n(contents_.begin() + static_cast<std::ptrdiff_t>(position_), count,
                    destination.begin());
        position_ += count;
        return count;
    }

    aw::core::Result<std::size_t> write(std::span<const std::byte> source) override {
        if (!writable_) {
            return aw::core::makeError(aw::core::ErrorCode::PermissionDenied, "read only");
        }
        if (contents_.size() < position_ + source.size()) {
            contents_.resize(position_ + source.size());
        }
        std::copy(source.begin(), source.end(),
                  contents_.begin() + static_cast<std::ptrdiff_t>(position_));
        position_ += source.size();
        return source.size();
    }

    aw::core::Result<std::uint64_t> seek(std::int64_t offset,
                                         aw::hal::SeekOrigin origin) override {
        std::int64_t base = 0;
        switch (origin) {
            case aw::hal::SeekOrigin::Begin: base = 0; break;
            case aw::hal::SeekOrigin::Current: base = static_cast<std::int64_t>(position_); break;
            case aw::hal::SeekOrigin::End: base = static_cast<std::int64_t>(contents_.size()); break;
        }
        const std::int64_t target = base + offset;
        if (target < 0) {
            return aw::core::makeError(aw::core::ErrorCode::InvalidArgument, "seek before start");
        }
        position_ = static_cast<std::size_t>(target);
        return static_cast<std::uint64_t>(position_);
    }

    aw::core::Result<std::uint64_t> tell() const override {
        return static_cast<std::uint64_t>(position_);
    }

    aw::core::Result<std::uint64_t> size() const override {
        return static_cast<std::uint64_t>(contents_.size());
    }

    aw::core::Status flush() override { return aw::core::success(); }

private:
    std::vector<std::byte>& contents_;
    std::size_t position_{0};
    bool writable_;
};

}  // namespace

aw::core::Result<std::unique_ptr<aw::hal::FileHandle>> MemoryStorage::open(
    const Path& path, aw::hal::OpenMode mode) {
    const std::string key = path.string();

    if (mode == aw::hal::OpenMode::Read) {
        const auto it = files_.find(key);
        if (it == files_.end()) {
            return aw::core::makeError(aw::core::ErrorCode::NotFound, key);
        }
        ++readOpenCounts_[key];
        return std::unique_ptr<aw::hal::FileHandle>{
            std::make_unique<MemoryFile>(it->second, false)};
    }

    if (mode == aw::hal::OpenMode::Write) {
        files_[key].clear();
    } else if (files_.find(key) == files_.end()) {
        files_[key] = {};
    }
    addParentDirectories(path);

    return std::unique_ptr<aw::hal::FileHandle>{std::make_unique<MemoryFile>(files_[key], true)};
}

}  // namespace awtest

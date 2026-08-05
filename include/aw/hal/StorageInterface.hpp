#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"

namespace aw::hal {

enum class OpenMode : std::uint8_t {
    Read,       ///< Must exist.
    Write,      ///< Created or truncated.
    ReadWrite,  ///< Created if missing, not truncated.
    Append,
};

enum class SeekOrigin : std::uint8_t { Begin, Current, End };

struct DirectoryEntry {
    std::string name;      ///< Leaf name only, never a full path.
    std::uint64_t size{0};
    bool isDirectory{false};
    std::int64_t modifiedUnixSeconds{0};
};

/// An open file.
///
/// Stream shaped rather than "read the whole thing", because the device must
/// play a 24 bit / 48 kHz multitrack project out of a few hundred kilobytes of
/// buffer.  There is no API here that can load a file into RAM, and that is on
/// purpose - it makes the memory-hungry mistake impossible to make by accident.
class FileHandle {
public:
    virtual ~FileHandle() = default;

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    /// Returns the number of bytes actually read; 0 means end of file.
    /// A short read is not an error.
    [[nodiscard]] virtual core::Result<std::size_t> read(std::span<std::byte> destination) = 0;

    [[nodiscard]] virtual core::Result<std::size_t> write(std::span<const std::byte> source) = 0;

    /// Returns the resulting absolute offset.
    [[nodiscard]] virtual core::Result<std::uint64_t> seek(std::int64_t offset,
                                                           SeekOrigin origin) = 0;

    [[nodiscard]] virtual core::Result<std::uint64_t> tell() const = 0;
    [[nodiscard]] virtual core::Result<std::uint64_t> size() const = 0;

    /// Pushes buffered data towards the medium.  Called at safe points (after
    /// writing a WAV header, after saving metadata), never per audio block.
    [[nodiscard]] virtual core::Status flush() = 0;

protected:
    FileHandle() = default;
};

/// Local persistent storage - the microSD card on the prototype.
///
/// Why not use `std::filesystem` and `std::fstream` directly?  Because the
/// ESP32-P4 talks to a FAT volume through a VFS layer with a different (and
/// smaller) API surface, and because a fake in-memory implementation of this
/// interface lets the project, WAV and import/export logic be unit tested with
/// no disk at all.  `std::filesystem::path` is still used as the *path value
/// type* (see `core::Path`); only the operations are abstracted.
class StorageInterface {
public:
    virtual ~StorageInterface() = default;

    StorageInterface(const StorageInterface&) = delete;
    StorageInterface& operator=(const StorageInterface&) = delete;

    [[nodiscard]] virtual core::Result<std::unique_ptr<FileHandle>> open(const core::Path& path,
                                                                         OpenMode mode) = 0;

    [[nodiscard]] virtual bool exists(const core::Path& path) const = 0;
    [[nodiscard]] virtual bool isDirectory(const core::Path& path) const = 0;

    [[nodiscard]] virtual core::Status createDirectories(const core::Path& path) = 0;
    [[nodiscard]] virtual core::Status remove(const core::Path& path) = 0;
    [[nodiscard]] virtual core::Status rename(const core::Path& from, const core::Path& to) = 0;

    [[nodiscard]] virtual core::Result<std::vector<DirectoryEntry>> list(
        const core::Path& directory) const = 0;

    /// Used before an import to refuse a transfer that would not fit.
    [[nodiscard]] virtual core::Result<std::uint64_t> freeSpaceBytes(
        const core::Path& path) const = 0;

    /// Root under which projects live, e.g. "/media/sd/projects".
    [[nodiscard]] virtual core::Path projectsRoot() const = 0;

protected:
    StorageInterface() = default;
};

}  // namespace aw::hal

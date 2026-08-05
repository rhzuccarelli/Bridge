#pragma once

#include <string>
#include <string_view>

#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/hal/StorageInterface.hpp"

namespace aw::storage {

/// The on-disk layout of a project, in one place.
///
///     <root>/
///       audio/          immutable source recordings, never modified after import
///       edits/          rendered/bounced material produced by destructive-ish ops
///       undo/           the serialised undo journal
///       metadata.json   the project itself: tracks, clips, mix, timeline
///
/// Every path in the system is derived here rather than assembled with string
/// concatenation at call sites.  That is what makes the layout a single
/// decision: when the ESP32 port needs 8.3-safe names on a FAT volume, this file
/// changes and nothing else does.
class ProjectPaths {
public:
    explicit ProjectPaths(core::Path projectRoot) : root_{std::move(projectRoot)} {}

    [[nodiscard]] const core::Path& root() const noexcept { return root_; }

    [[nodiscard]] core::Path audioDirectory() const { return root_ / "audio"; }
    [[nodiscard]] core::Path editsDirectory() const { return root_ / "edits"; }
    [[nodiscard]] core::Path undoDirectory() const { return root_ / "undo"; }
    [[nodiscard]] core::Path metadataFile() const { return root_ / "metadata.json"; }

    /// Where a newly imported source file lands.  `name` is sanitised.
    [[nodiscard]] core::Path audioFile(std::string_view name) const;

    /// Temporary sibling used for atomic writes: write to `.tmp`, then rename.
    [[nodiscard]] core::Path temporaryFor(const core::Path& target) const;

    /// Creates the four directories if missing.  Idempotent.
    [[nodiscard]] core::Status ensureLayout(hal::StorageInterface& storage) const;

    /// True if `root` looks like a project (has a metadata file).
    [[nodiscard]] static bool isProjectDirectory(const hal::StorageInterface& storage,
                                                 const core::Path& root);

    /// Strips characters that FAT, MTP or a 320px wide screen cannot cope with,
    /// and guarantees a non-empty result.
    [[nodiscard]] static std::string sanitiseName(std::string_view name);

private:
    core::Path root_;
};

}  // namespace aw::storage

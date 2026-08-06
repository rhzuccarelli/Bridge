#pragma once

#include <cstdint>
#include <string>

#include "aw/core/Result.hpp"
#include "aw/core/Types.hpp"
#include "aw/hal/StorageInterface.hpp"
#include "aw/project/Project.hpp"

namespace aw::project {

/// Version of the `metadata.json` schema this build writes.
///
/// Bumped whenever the on-disk shape changes.  The loader accepts anything it
/// knows how to migrate and refuses anything newer, with a clear message, rather
/// than half-reading a future format and silently losing the user's edits.
inline constexpr std::uint32_t kMetadataSchemaVersion = 1;

/// Reads and writes `metadata.json`.
///
/// Phase 3 implements this.  Separated from `Project` so that the data model has
/// no opinion about serialisation, and so a future binary format (which the
/// ESP32 may want, to avoid JSON parsing cost at boot) is a new implementation
/// here rather than a change to every struct.
///
/// Open question for Phase 3, to settle before writing the parser: hand-rolled
/// minimal JSON versus a third-party header-only library.  A dependency buys
/// robustness; hand-rolling buys a known, small flash and RAM cost on the
/// ESP32-P4 and no supply chain.  Given the schema is ours and tiny, the current
/// intent is hand-rolled - flagged here for confirmation.
class MetadataSerializer {
public:
    /// Serialises the project's timeline into `metadata.json`, atomically.
    [[nodiscard]] static core::Status save(hal::StorageInterface& storage,
                                           const Project& project);

    /// Loads a project from a directory.  Fails with InvalidFormat on a
    /// malformed file and UnsupportedFormat on a newer schema version.
    [[nodiscard]] static core::Result<Project> load(hal::StorageInterface& storage,
                                                    const core::Path& projectRoot);
};

}  // namespace aw::project

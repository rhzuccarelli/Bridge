#include "aw/project/ProjectMetadata.hpp"

// Phase 3 implements serialisation.  See the header for the open question about
// the JSON implementation.

namespace aw::project {

core::Status MetadataSerializer::save(hal::StorageInterface& /*storage*/,
                                      const Project& project) {
    return core::notImplemented("MetadataSerializer::save " + project.name() + " (Phase 3)");
}

core::Result<Project> MetadataSerializer::load(hal::StorageInterface& /*storage*/,
                                               const core::Path& projectRoot) {
    return core::notImplemented("MetadataSerializer::load " + projectRoot.string() + " (Phase 3)");
}

}  // namespace aw::project

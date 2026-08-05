#include "aw/project/Project.hpp"

namespace aw::project {

Project::Project(std::string name, core::Path root)
    : name_{std::move(name)}, paths_{std::move(root)} {
    timeline_.sampleRate = 48000;
}

}  // namespace aw::project

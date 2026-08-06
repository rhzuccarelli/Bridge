#include "aw/storage/ProjectPaths.hpp"

#include <algorithm>
#include <cctype>

namespace aw::storage {

namespace {
constexpr std::string_view kIllegalCharacters = R"(<>:"/\|?*)";
constexpr std::size_t kMaxNameLength = 64;
}  // namespace

core::Path ProjectPaths::audioFile(std::string_view name) const {
    return audioDirectory() / sanitiseName(name);
}

core::Path ProjectPaths::temporaryFor(const core::Path& target) const {
    core::Path temporary = target;
    temporary += ".tmp";
    return temporary;
}

core::Status ProjectPaths::ensureLayout(hal::StorageInterface& storage) const {
    for (const core::Path& directory :
         {root_, audioDirectory(), editsDirectory(), undoDirectory()}) {
        if (auto status = storage.createDirectories(directory); !status) {
            return status;
        }
    }
    return core::success();
}

bool ProjectPaths::isProjectDirectory(const hal::StorageInterface& storage,
                                      const core::Path& root) {
    return storage.isDirectory(root) && storage.exists(root / "metadata.json");
}

std::string ProjectPaths::sanitiseName(std::string_view name) {
    std::string out;
    out.reserve(std::min(name.size(), kMaxNameLength));

    for (const char c : name) {
        if (out.size() >= kMaxNameLength) {
            break;
        }
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || kIllegalCharacters.find(c) != std::string_view::npos) {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }

    // Trailing dots and spaces are legal on ext4 but not on FAT, and the device
    // must be able to write anything it names back onto the TP-7.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }

    return out.empty() ? std::string{"untitled"} : out;
}

}  // namespace aw::storage

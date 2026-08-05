#include "aw/storage/AtomicFileWriter.hpp"
#include "aw/storage/ProjectPaths.hpp"
#include "framework/Test.hpp"
#include "support/MemoryStorage.hpp"

#include <string_view>

using namespace aw;

namespace {

std::vector<std::byte> bytesOf(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

}  // namespace

AW_TEST(ProjectPaths, DerivesTheStandardLayout) {
    const storage::ProjectPaths paths{"/projects/Session 1"};
    AW_CHECK_EQ(paths.audioDirectory().string(), std::string{"/projects/Session 1/audio"});
    AW_CHECK_EQ(paths.editsDirectory().string(), std::string{"/projects/Session 1/edits"});
    AW_CHECK_EQ(paths.undoDirectory().string(), std::string{"/projects/Session 1/undo"});
    AW_CHECK_EQ(paths.metadataFile().string(),
                std::string{"/projects/Session 1/metadata.json"});
}

AW_TEST(ProjectPaths, EnsureLayoutCreatesEveryDirectory) {
    awtest::MemoryStorage storage;
    const storage::ProjectPaths paths{"/projects/Take"};

    AW_REQUIRE(static_cast<bool>(paths.ensureLayout(storage)));
    AW_CHECK(storage.isDirectory("/projects/Take"));
    AW_CHECK(storage.isDirectory("/projects/Take/audio"));
    AW_CHECK(storage.isDirectory("/projects/Take/edits"));
    AW_CHECK(storage.isDirectory("/projects/Take/undo"));
}

AW_TEST(ProjectPaths, SanitisesNamesForFatAndMtp) {
    AW_CHECK_EQ(storage::ProjectPaths::sanitiseName("take/one"), std::string{"take_one"});
    AW_CHECK_EQ(storage::ProjectPaths::sanitiseName("bad:name?"), std::string{"bad_name_"});
    AW_CHECK_EQ(storage::ProjectPaths::sanitiseName("trailing. "), std::string{"trailing"});
    AW_CHECK_EQ(storage::ProjectPaths::sanitiseName(""), std::string{"untitled"});
    AW_CHECK_EQ(storage::ProjectPaths::sanitiseName("  "), std::string{"untitled"});
}

AW_TEST(AtomicFileWriter, CommitReplacesTheTarget) {
    awtest::MemoryStorage storage;
    const core::Path target{"/projects/Take/metadata.json"};

    {
        storage::AtomicFileWriter writer{storage, target};
        AW_REQUIRE(static_cast<bool>(writer.open()));
        AW_REQUIRE(static_cast<bool>(writer.write(bytesOf("{\"version\":1}"))));

        // Before commit the target must not exist - a reader mid-write sees
        // the old file or nothing, never a partial one.
        AW_CHECK(!storage.exists(target));

        AW_REQUIRE(static_cast<bool>(writer.commit()));
    }

    AW_REQUIRE(storage.exists(target));
    AW_CHECK(!storage.exists("/projects/Take/metadata.json.tmp"));

    const auto* contents = storage.fileContents(target);
    AW_REQUIRE(contents != nullptr);
    AW_CHECK_EQ(contents->size(), std::size_t{13});
}

AW_TEST(AtomicFileWriter, AbandonedWriteLeavesNoDebris) {
    awtest::MemoryStorage storage;
    const core::Path target{"/projects/Take/metadata.json"};

    {
        storage::AtomicFileWriter writer{storage, target};
        AW_REQUIRE(static_cast<bool>(writer.open()));
        AW_REQUIRE(static_cast<bool>(writer.write(bytesOf("half a file"))));
        // No commit: the destructor must clean up.
    }

    AW_CHECK(!storage.exists(target));
    AW_CHECK(!storage.exists("/projects/Take/metadata.json.tmp"));
}

AW_TEST(AtomicFileWriter, WriteBeforeOpenFails) {
    awtest::MemoryStorage storage;
    storage::AtomicFileWriter writer{storage, "/projects/Take/metadata.json"};

    const auto status = writer.write(bytesOf("x"));
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(status.error().code(), core::ErrorCode::InvalidState);
}

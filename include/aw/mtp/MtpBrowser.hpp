#pragma once

#include <string>
#include <vector>

#include "aw/core/Result.hpp"
#include "aw/hal/MTPInterface.hpp"

namespace aw::mtp {

/// A row in the import browser.
struct BrowserEntry {
    hal::MtpObjectHandle handle{0};
    std::string name;
    std::uint64_t sizeBytes{0};
    bool isFolder{false};
    bool isWav{false};       ///< Name ends in .wav (case insensitive).
    bool isSelected{false};  ///< Toggled by the encoder push in the import screen.
};

/// Stateful navigation of a device's object tree.
///
/// Exists so the import screen holds *no* MTP state of its own: it renders
/// `entries()` and calls `enterFolder`/`goUp`.  That keeps the screen testable
/// with a fake browser, and keeps the "one level at a time, never recursive"
/// rule in one place - a recursive listing of a device with hundreds of takes
/// would block the UI for seconds and gain nothing, since the user only ever
/// looks at one folder.
///
/// Runs on the I/O worker thread; the UI reads a snapshot.
class MtpBrowser {
public:
    explicit MtpBrowser(hal::MTPInterface& mtp) noexcept : mtp_{mtp} {}

    MtpBrowser(const MtpBrowser&) = delete;
    MtpBrowser& operator=(const MtpBrowser&) = delete;

    /// Lists the root of `storage`.  Resets any navigation history.
    [[nodiscard]] core::Status openStorage(hal::MtpStorageId storage);

    [[nodiscard]] core::Status enterFolder(hal::MtpObjectHandle folder);
    [[nodiscard]] core::Status goUp();
    [[nodiscard]] core::Status refresh();

    [[nodiscard]] bool canGoUp() const noexcept { return !breadcrumb_.empty(); }

    /// Current folder's contents: folders first, then WAV files, both by name.
    /// Non-WAV files are filtered out - this is a WAV recorder, and showing the
    /// device's other files would only offer choices that end in an error.
    [[nodiscard]] const std::vector<BrowserEntry>& entries() const noexcept { return entries_; }

    /// "/ RECORDINGS / SESSION 3" - drawn in the import screen's title bar.
    [[nodiscard]] std::string breadcrumbText() const;

    void toggleSelection(std::size_t index) noexcept;
    void clearSelection() noexcept;
    [[nodiscard]] std::vector<BrowserEntry> selectedEntries() const;

private:
    hal::MTPInterface& mtp_;
    hal::MtpStorageId storage_{0};
    hal::MtpObjectHandle current_{hal::kMtpRootHandle};
    std::vector<hal::MtpObjectHandle> breadcrumb_;
    std::vector<std::string> breadcrumbNames_;
    std::vector<BrowserEntry> entries_;
};

}  // namespace aw::mtp

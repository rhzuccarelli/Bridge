#include "aw/mtp/MtpBrowser.hpp"

#include <algorithm>
#include <iterator>

// Phase 8 implements navigation against a real device.

namespace aw::mtp {

core::Status MtpBrowser::openStorage(hal::MtpStorageId storage) {
    storage_ = storage;
    current_ = hal::kMtpRootHandle;
    breadcrumb_.clear();
    breadcrumbNames_.clear();
    return refresh();
}

core::Status MtpBrowser::enterFolder(hal::MtpObjectHandle folder) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [folder](const BrowserEntry& e) { return e.handle == folder; });
    if (it == entries_.end() || !it->isFolder) {
        return core::makeError(core::ErrorCode::InvalidArgument, "not a folder in this listing");
    }

    breadcrumb_.push_back(current_);
    breadcrumbNames_.push_back(it->name);
    current_ = folder;
    return refresh();
}

core::Status MtpBrowser::goUp() {
    if (breadcrumb_.empty()) {
        return core::makeError(core::ErrorCode::InvalidState, "already at root");
    }
    current_ = breadcrumb_.back();
    breadcrumb_.pop_back();
    breadcrumbNames_.pop_back();
    return refresh();
}

core::Status MtpBrowser::refresh() {
    (void)mtp_;
    entries_.clear();
    return core::notImplemented("MtpBrowser::refresh (Phase 8)");
}

std::string MtpBrowser::breadcrumbText() const {
    std::string text{"/"};
    for (const std::string& name : breadcrumbNames_) {
        text += name;
        text += '/';
    }
    return text;
}

void MtpBrowser::toggleSelection(std::size_t index) noexcept {
    if (index < entries_.size() && !entries_[index].isFolder) {
        entries_[index].isSelected = !entries_[index].isSelected;
    }
}

void MtpBrowser::clearSelection() noexcept {
    for (BrowserEntry& entry : entries_) {
        entry.isSelected = false;
    }
}

std::vector<BrowserEntry> MtpBrowser::selectedEntries() const {
    std::vector<BrowserEntry> selected;
    std::copy_if(entries_.begin(), entries_.end(), std::back_inserter(selected),
                 [](const BrowserEntry& e) { return e.isSelected; });
    return selected;
}

}  // namespace aw::mtp

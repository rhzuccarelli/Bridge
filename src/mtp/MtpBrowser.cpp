#include "aw/mtp/MtpBrowser.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>

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
    auto objects = mtp_.listObjects(storage_, current_);
    if (!objects) return objects.error();

    std::vector<BrowserEntry> next;
    next.reserve(objects.value().size());
    for (const hal::MtpObjectInfo& object : objects.value()) {
        std::string extension;
        const auto dot = object.name.find_last_of('.');
        if (dot != std::string::npos) {
            extension = object.name.substr(dot);
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        const bool isWav = extension == ".wav";
        if (!object.isFolder && !isWav) continue;
        next.push_back(BrowserEntry{object.handle, object.name, object.sizeBytes,
                                    object.isFolder, isWav, false});
    }
    std::sort(next.begin(), next.end(), [](const BrowserEntry& left, const BrowserEntry& right) {
        if (left.isFolder != right.isFolder) return left.isFolder > right.isFolder;
        return left.name < right.name;
    });
    entries_ = std::move(next);
    return core::success();
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

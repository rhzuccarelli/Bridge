#include "aw/platform/rpi/LibMtpDevice.hpp"

#include <cstdlib>
#include <cstring>
#include <libmtp.h>

namespace aw::platform::rpi {
namespace {

core::Error mtpError(LIBMTP_mtpdevice_t* device, std::string operation) {
    if (device != nullptr) {
        if (LIBMTP_error_t* errors = LIBMTP_Get_Errorstack(device); errors != nullptr &&
            errors->error_text != nullptr) {
            operation += ": ";
            operation += errors->error_text;
        }
        LIBMTP_Clear_Errorstack(device);
    }
    return core::makeError(core::ErrorCode::ProtocolError, std::move(operation));
}

std::string safe(const char* text) { return text == nullptr ? std::string{} : std::string{text}; }

hal::MtpObjectInfo convertFile(const LIBMTP_file_t& file) {
    hal::MtpObjectInfo info;
    info.handle = file.item_id;
    info.parent = file.parent_id;
    info.storageId = file.storage_id;
    info.name = safe(file.filename);
    info.sizeBytes = file.filesize;
    info.isFolder = file.filetype == LIBMTP_FILETYPE_FOLDER;
    info.modifiedUnixSeconds = static_cast<std::int64_t>(file.modificationdate);
    return info;
}

struct ReadContext {
    const hal::MtpWriteSink* sink;
    const hal::MtpProgressCallback* progress;
};

std::uint16_t receiveChunk(void*, void* privateData, std::uint32_t sendLength,
                           unsigned char* data, std::uint32_t* putLength) {
    auto& context = *static_cast<ReadContext*>(privateData);
    const auto bytes = std::as_bytes(std::span{data, static_cast<std::size_t>(sendLength)});
    if (!(*context.sink)(bytes)) return LIBMTP_HANDLER_RETURN_CANCEL;
    *putLength = sendLength;
    return LIBMTP_HANDLER_RETURN_OK;
}

struct WriteContext {
    const hal::MtpReadSource* source;
    const hal::MtpProgressCallback* progress;
};

std::uint16_t provideChunk(void*, void* privateData, std::uint32_t wantLength,
                           unsigned char* data, std::uint32_t* gotLength) {
    auto& context = *static_cast<WriteContext*>(privateData);
    *gotLength = static_cast<std::uint32_t>((*context.source)(
        std::as_writable_bytes(std::span{data, static_cast<std::size_t>(wantLength)})));
    return LIBMTP_HANDLER_RETURN_OK;
}

int reportProgress(std::uint64_t done, std::uint64_t total, const void* privateData) {
    const auto* callback = static_cast<const hal::MtpProgressCallback*>(privateData);
    return (*callback)(done, total) ? 0 : 1;
}

}  // namespace

struct LibMtpDevice::Impl {
    LIBMTP_mtpdevice_t* device{nullptr};
};

LibMtpDevice::LibMtpDevice() : impl_{std::make_unique<Impl>()} { LIBMTP_Init(); }
LibMtpDevice::~LibMtpDevice() { closeSession(); }

core::Status LibMtpDevice::openSession(std::uint32_t deviceKey) {
    closeSession();
    LIBMTP_raw_device_t* devices = nullptr;
    int count = 0;
    const LIBMTP_error_number_t detected = LIBMTP_Detect_Raw_Devices(&devices, &count);
    if (detected != LIBMTP_ERROR_NONE) {
        std::free(devices);
        return core::makeError(detected == LIBMTP_ERROR_CONNECTING
                                   ? core::ErrorCode::PermissionDenied
                                   : core::ErrorCode::DeviceNotConnected,
                               "libmtp could not detect the TP-7");
    }
    const std::uint8_t bus = static_cast<std::uint8_t>(deviceKey >> 8U);
    const std::uint8_t address = static_cast<std::uint8_t>(deviceKey & 0xFFU);
    for (int index = 0; index < count; ++index) {
        if (devices[index].bus_location == bus && devices[index].devnum == address) {
            impl_->device = LIBMTP_Open_Raw_Device_Uncached(&devices[index]);
            break;
        }
    }
    std::free(devices);
    if (impl_->device == nullptr) {
        return core::makeError(core::ErrorCode::DeviceNotConnected,
                               "TP-7 disappeared or MTP access was denied");
    }
    return core::success();
}

void LibMtpDevice::closeSession() noexcept {
    if (impl_->device != nullptr) LIBMTP_Release_Device(impl_->device);
    impl_->device = nullptr;
}

bool LibMtpDevice::hasSession() const noexcept { return impl_->device != nullptr; }

core::Result<hal::MtpDeviceInfo> LibMtpDevice::deviceInfo() const {
    if (!hasSession()) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
    char* manufacturer = LIBMTP_Get_Manufacturername(impl_->device);
    char* model = LIBMTP_Get_Modelname(impl_->device);
    char* serial = LIBMTP_Get_Serialnumber(impl_->device);
    char* version = LIBMTP_Get_Deviceversion(impl_->device);
    hal::MtpDeviceInfo info{safe(manufacturer), safe(model), safe(serial), safe(version)};
    std::free(manufacturer);
    std::free(model);
    std::free(serial);
    std::free(version);
    return info;
}

core::Result<std::vector<hal::MtpStorageInfo>> LibMtpDevice::storages() const {
    if (!hasSession()) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
    if (LIBMTP_Get_Storage(impl_->device, LIBMTP_STORAGE_SORTBY_NOTSORTED) != 0)
        return mtpError(impl_->device, "list MTP storages");
    std::vector<hal::MtpStorageInfo> result;
    for (LIBMTP_devicestorage_t* storage = impl_->device->storage; storage != nullptr;
         storage = storage->next) {
        result.push_back({storage->id, safe(storage->StorageDescription), storage->MaxCapacity,
                          storage->FreeSpaceInBytes});
    }
    return result;
}

core::Result<std::vector<hal::MtpObjectInfo>> LibMtpDevice::listObjects(
    hal::MtpStorageId storage, hal::MtpObjectHandle parent) {
    if (!hasSession()) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
    LIBMTP_file_t* files = LIBMTP_Get_Files_And_Folders(impl_->device, storage, parent);
    std::vector<hal::MtpObjectInfo> result;
    while (files != nullptr) {
        LIBMTP_file_t* next = files->next;
        result.push_back(convertFile(*files));
        files->next = nullptr;
        LIBMTP_destroy_file_t(files);
        files = next;
    }
    if (LIBMTP_error_t* errors = LIBMTP_Get_Errorstack(impl_->device); errors != nullptr)
        return mtpError(impl_->device, "list MTP objects");
    return result;
}

core::Result<hal::MtpObjectInfo> LibMtpDevice::objectInfo(hal::MtpObjectHandle handle) {
    if (!hasSession()) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
    LIBMTP_file_t* file = LIBMTP_Get_Filemetadata(impl_->device, handle);
    if (file == nullptr) return mtpError(impl_->device, "read MTP object metadata");
    hal::MtpObjectInfo result = convertFile(*file);
    LIBMTP_destroy_file_t(file);
    return result;
}

core::Status LibMtpDevice::readObject(hal::MtpObjectHandle handle,
                                      const hal::MtpWriteSink& sink,
                                      const hal::MtpProgressCallback& progress) {
    if (!hasSession()) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
    ReadContext context{&sink, &progress};
    const int result = LIBMTP_Get_File_To_Handler(impl_->device, handle, receiveChunk, &context,
                                                   reportProgress, &progress);
    return result == 0 ? core::success() : mtpError(impl_->device, "download MTP object");
}

core::Result<hal::MtpObjectHandle> LibMtpDevice::writeObject(
    hal::MtpStorageId storage, hal::MtpObjectHandle parent, const std::string& name,
    std::uint64_t sizeBytes, const hal::MtpReadSource& source,
    const hal::MtpProgressCallback& progress) {
    if (!hasSession()) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
    auto existing = listObjects(storage, parent);
    if (!existing) return existing.error();
    for (const auto& object : existing.value()) {
        if (object.name == name)
            return core::makeError(core::ErrorCode::AlreadyExists, name);
    }
    LIBMTP_file_t* file = LIBMTP_new_file_t();
    file->storage_id = storage;
    file->parent_id = parent;
    file->filename = ::strdup(name.c_str());
    file->filesize = sizeBytes;
    file->filetype = LIBMTP_FILETYPE_WAV;
    WriteContext context{&source, &progress};
    const int result = LIBMTP_Send_File_From_Handler(impl_->device, provideChunk, &context, file,
                                                      reportProgress, &progress);
    const hal::MtpObjectHandle handle = file->item_id;
    LIBMTP_destroy_file_t(file);
    if (result != 0) return mtpError(impl_->device, "upload MTP object");
    return handle;
}

core::Status LibMtpDevice::deleteObject(hal::MtpObjectHandle handle) {
    if (!hasSession()) return core::makeError(core::ErrorCode::InvalidState, "no MTP session");
    return LIBMTP_Delete_Object(impl_->device, handle) == 0
               ? core::success()
               : mtpError(impl_->device, "delete MTP object");
}

}  // namespace aw::platform::rpi

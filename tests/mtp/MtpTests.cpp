#include "aw/events/EventQueue.hpp"
#include "aw/mtp/MtpManager.hpp"
#include "aw/mtp/MtpBrowser.hpp"
#include "framework/Test.hpp"

#include <vector>

using namespace aw;

namespace {

/// A USB bus we can plug things into by hand.
class FakeUsb final : public hal::USBInterface {
public:
    core::Status start() override {
        started = true;
        return core::success();
    }
    void stop() noexcept override { started = false; }
    void setHotplugListener(hal::UsbHotplugListener* value) noexcept override {
        listener = value;
    }
    core::Result<std::vector<hal::UsbDeviceInfo>> enumerate() const override { return devices; }

    void plug(const hal::UsbDeviceInfo& device) {
        devices.push_back(device);
        if (listener != nullptr) {
            listener->onUsbDeviceAttached(device);
        }
    }

    void unplug(const hal::UsbDeviceInfo& device) {
        devices.clear();
        if (listener != nullptr) {
            listener->onUsbDeviceDetached(device);
        }
    }

    hal::UsbHotplugListener* listener{nullptr};
    std::vector<hal::UsbDeviceInfo> devices;
    bool started{false};
};

/// An MTP device that accepts a session and reports one storage.
class FakeMtp final : public hal::MTPInterface {
public:
    core::Status openSession(std::uint32_t deviceKey) override {
        if (failOpen) {
            return core::makeError(core::ErrorCode::ProtocolError, "refused");
        }
        sessionOpen = true;
        lastKey = deviceKey;
        ++opens;
        return core::success();
    }
    void closeSession() noexcept override {
        if (sessionOpen) ++closes;
        sessionOpen = false;
    }
    bool hasSession() const noexcept override { return sessionOpen; }

    core::Result<hal::MtpDeviceInfo> deviceInfo() const override {
        return hal::MtpDeviceInfo{"teenage engineering", "TP-7", "SN123", "1.0"};
    }
    core::Result<std::vector<hal::MtpStorageInfo>> storages() const override {
        return std::vector<hal::MtpStorageInfo>{
            hal::MtpStorageInfo{1, "Internal", 128ULL << 30, 64ULL << 30}};
    }
    core::Result<std::vector<hal::MtpObjectInfo>> listObjects(hal::MtpStorageId,
                                                              hal::MtpObjectHandle parent) override {
        std::vector<hal::MtpObjectInfo> children;
        for (const auto& object : objects) if (object.parent == parent) children.push_back(object);
        return children;
    }
    core::Result<hal::MtpObjectInfo> objectInfo(hal::MtpObjectHandle) override {
        return core::makeError(core::ErrorCode::NotFound, "handle");
    }
    core::Status readObject(hal::MtpObjectHandle,
                            const hal::MtpWriteSink&,
                            const hal::MtpProgressCallback&) override {
        return core::success();
    }
    core::Result<hal::MtpObjectHandle> writeObject(hal::MtpStorageId,
                                                   hal::MtpObjectHandle,
                                                   const std::string&,
                                                   std::uint64_t,
                                                   const hal::MtpReadSource&,
                                                   const hal::MtpProgressCallback&) override {
        return hal::MtpObjectHandle{42};
    }
    core::Status deleteObject(hal::MtpObjectHandle) override { return core::success(); }

    bool sessionOpen{false};
    bool failOpen{false};
    int opens{0};
    int closes{0};
    std::uint32_t lastKey{0};
    std::vector<hal::MtpObjectInfo> objects;
};

hal::UsbDeviceInfo makeTp7() {
    hal::UsbDeviceInfo device;
    device.vendorId = 0x2E8A;
    device.productId = 0x0010;
    device.busNumber = 1;
    device.deviceAddress = 5;
    device.product = "TP-7";
    device.supportsMtp = true;
    return device;
}

}  // namespace

AW_TEST(MtpManager, StartsDisconnected) {
    FakeUsb usb;
    FakeMtp mtp;
    events::EventQueue queue;
    mtp::MtpManager manager{usb, mtp, queue};

    AW_CHECK_EQ(manager.state(), mtp::ConnectionState::Disconnected);
}

AW_TEST(MtpManager, DetectsADeviceAlreadyPresentAtStartup) {
    FakeUsb usb;
    FakeMtp mtp;
    events::EventQueue queue;

    usb.devices.push_back(makeTp7());

    mtp::MtpManager manager{usb, mtp, queue};
    AW_REQUIRE(static_cast<bool>(manager.start()));

    AW_CHECK_EQ(manager.state(), mtp::ConnectionState::Detected);
    // Detection alone must not open a session: plugging in never starts work.
    AW_CHECK(!mtp.sessionOpen);
}

AW_TEST(MtpManager, PostsDeviceEventsOnHotplug) {
    FakeUsb usb;
    FakeMtp mtp;
    events::EventQueue queue;
    mtp::MtpManager manager{usb, mtp, queue};
    AW_REQUIRE(static_cast<bool>(manager.start()));

    const hal::UsbDeviceInfo device = makeTp7();
    usb.plug(device);

    const auto attached = queue.poll();
    AW_REQUIRE(attached.has_value());
    const auto* event = std::get_if<events::DeviceEvent>(&*attached);
    AW_REQUIRE(event != nullptr);
    AW_CHECK(event->attached);
    AW_CHECK_EQ(event->deviceKey, device.key());

    usb.unplug(device);
    const auto detached = queue.poll();
    AW_REQUIRE(detached.has_value());
    const auto* detachedEvent = std::get_if<events::DeviceEvent>(&*detached);
    AW_REQUIRE(detachedEvent != nullptr);
    AW_CHECK(!detachedEvent->attached);
    AW_CHECK_EQ(manager.state(), mtp::ConnectionState::Disconnected);
}

AW_TEST(MtpManager, ConnectOpensASessionOnlyWhenAsked) {
    FakeUsb usb;
    FakeMtp mtp;
    events::EventQueue queue;
    mtp::MtpManager manager{usb, mtp, queue};
    AW_REQUIRE(static_cast<bool>(manager.start()));

    usb.plug(makeTp7());
    AW_CHECK(!mtp.sessionOpen);

    AW_REQUIRE(static_cast<bool>(manager.connect()));
    AW_CHECK(mtp.sessionOpen);
    AW_CHECK_EQ(manager.state(), mtp::ConnectionState::Connected);

    AW_REQUIRE(manager.deviceInfo().has_value());
    AW_CHECK_EQ(manager.deviceInfo()->model, std::string{"TP-7"});

    manager.disconnect();
    AW_CHECK(!mtp.sessionOpen);
    AW_CHECK_EQ(manager.state(), mtp::ConnectionState::Detected);
}

AW_TEST(MtpManager, ConnectWithNoDeviceFails) {
    FakeUsb usb;
    FakeMtp mtp;
    events::EventQueue queue;
    mtp::MtpManager manager{usb, mtp, queue};

    const auto status = manager.connect();
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(status.error().code(), core::ErrorCode::DeviceNotConnected);
}

AW_TEST(MtpManager, ReportsAFailedSession) {
    FakeUsb usb;
    FakeMtp mtp;
    events::EventQueue queue;
    mtp.failOpen = true;

    mtp::MtpManager manager{usb, mtp, queue};
    AW_REQUIRE(static_cast<bool>(manager.start()));
    usb.plug(makeTp7());

    const auto status = manager.connect();
    AW_REQUIRE(!status.hasValue());
    AW_CHECK_EQ(manager.state(), mtp::ConnectionState::Error);
    AW_CHECK_EQ(manager.lastError().code(), core::ErrorCode::ProtocolError);
}

AW_TEST(MtpBrowser, ShowsFoldersAndWavsOnlyAndSortsFoldersFirst) {
    FakeMtp mtp;
    mtp.objects = {
        {3, hal::kMtpRootHandle, 1, "z.WAV", 300, false, 0},
        {4, hal::kMtpRootHandle, 1, "notes.txt", 10, false, 0},
        {2, hal::kMtpRootHandle, 1, "recordings", 0, true, 0},
        {5, hal::kMtpRootHandle, 1, "a.wav", 100, false, 0},
    };
    mtp::MtpBrowser browser{mtp};
    AW_REQUIRE(browser.openStorage(1));
    AW_REQUIRE(browser.entries().size() == std::size_t{3});
    AW_CHECK(browser.entries()[0].isFolder);
    AW_CHECK_EQ(browser.entries()[0].name, std::string{"recordings"});
    AW_CHECK_EQ(browser.entries()[1].name, std::string{"a.wav"});
    AW_CHECK_EQ(browser.entries()[2].name, std::string{"z.WAV"});

    browser.toggleSelection(1);
    AW_REQUIRE(browser.selectedEntries().size() == std::size_t{1});
    AW_CHECK_EQ(browser.selectedEntries()[0].handle, hal::MtpObjectHandle{5});
}

AW_TEST(MtpBrowser, EntersAndLeavesOneFolderAtATime) {
    FakeMtp mtp;
    mtp.objects = {
        {2, hal::kMtpRootHandle, 1, "recordings", 0, true, 0},
        {3, 2, 1, "take.wav", 100, false, 0},
    };
    mtp::MtpBrowser browser{mtp};
    AW_REQUIRE(browser.openStorage(1));
    AW_REQUIRE(browser.enterFolder(2));
    AW_CHECK(browser.canGoUp());
    AW_CHECK_EQ(browser.breadcrumbText(), std::string{"/recordings/"});
    AW_REQUIRE(browser.entries().size() == std::size_t{1});
    AW_CHECK_EQ(browser.entries()[0].name, std::string{"take.wav"});
    AW_REQUIRE(browser.goUp());
    AW_CHECK(!browser.canGoUp());
}

AW_TEST(MtpManager, ReleaseAfterAFailedConnectStillClosesASession) {
    // A TP-7 leaves MTP mode only when a session closes; a failed connect
    // (out of memory on the P4) used to leave it stuck there.
    FakeUsb usb;
    FakeMtp mtp;
    events::EventQueue queue;
    mtp::MtpManager manager{usb, mtp, queue};
    AW_REQUIRE(static_cast<bool>(manager.start()));
    usb.plug(makeTp7());

    mtp.failOpen = true;
    AW_REQUIRE(!manager.connect().hasValue());
    mtp.failOpen = false;
    manager.release();
    AW_CHECK_EQ(mtp.opens, 1);
    AW_CHECK_EQ(mtp.closes, 1);
    AW_CHECK(!mtp.sessionOpen);
    AW_CHECK_EQ(manager.state(), mtp::ConnectionState::Detected);
}

AW_TEST(MtpManager, ReleaseClosesAnOpenSession) {
    FakeUsb usb;
    FakeMtp mtp;
    events::EventQueue queue;
    mtp::MtpManager manager{usb, mtp, queue};
    AW_REQUIRE(static_cast<bool>(manager.start()));
    usb.plug(makeTp7());
    AW_REQUIRE(static_cast<bool>(manager.connect()));

    manager.release();
    AW_CHECK_EQ(mtp.opens, 1);
    AW_CHECK_EQ(mtp.closes, 1);
    AW_CHECK_EQ(manager.state(), mtp::ConnectionState::Detected);
}

AW_TEST(MtpManager, ReleaseWithNoDeviceDoesNothing) {
    FakeUsb usb;
    FakeMtp mtp;
    events::EventQueue queue;
    mtp::MtpManager manager{usb, mtp, queue};
    manager.release();
    AW_CHECK_EQ(mtp.opens, 0);
}

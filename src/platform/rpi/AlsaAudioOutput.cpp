#include "aw/platform/rpi/AlsaAudioOutput.hpp"
#include "aw/platform/rpi/AlsaPcm.hpp"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace aw::platform::rpi {
namespace {

core::Error alsaError(core::ErrorCode code, std::string_view operation, int error) {
    return core::makeError(code, std::string{operation} + ": " + snd_strerror(error));
}

}  // namespace

struct AlsaAudioOutput::Impl {
    std::string deviceName;
    core::AudioFormat format{core::kNativeFormat};
    core::FrameCount periodFrames{256};
    core::FrameCount hardwareBufferFrames{512};
    core::FrameCount hardwarePeriodFrames{256};
    core::ChannelCount hardwareChannels{2};
    bool tp7MonitorMode{false};
    hal::AudioRenderCallback* callback{nullptr};
    snd_pcm_t* pcm{nullptr};
    std::vector<float> floatBuffer;
    std::vector<float> hardwareFloatBuffer;
    Tp7MonitorConverterState tp7ConverterState;
    std::vector<std::byte> packedBuffer;
    std::atomic<bool> running{false};
    std::thread thread;
};

AlsaAudioOutput::AlsaAudioOutput(std::string deviceName)
    : impl_{std::make_unique<Impl>()} {
    impl_->deviceName = std::move(deviceName);
}

AlsaAudioOutput::~AlsaAudioOutput() { close(); }

core::Status AlsaAudioOutput::open(const core::AudioFormat& format,
                                   core::FrameCount bufferFrames,
                                   hal::AudioRenderCallback& callback) {
    close();
    if (format != core::kNativeFormat) {
        return core::makeError(core::ErrorCode::UnsupportedFormat,
                               "ALSA output requires 48 kHz packed-24 stereo");
    }
    if (bufferFrames <= 0) {
        return core::makeError(core::ErrorCode::InvalidArgument, "invalid ALSA period size");
    }

    int error = snd_pcm_open(&impl_->pcm, impl_->deviceName.c_str(),
                             SND_PCM_STREAM_PLAYBACK, 0);
    if (error < 0) {
        impl_->pcm = nullptr;
        const core::ErrorCode code = error == -EBUSY ? core::ErrorCode::DeviceBusy
                                                      : core::ErrorCode::DeviceNotConnected;
        return alsaError(code, "snd_pcm_open", error);
    }

    snd_pcm_hw_params_t* hardwareRaw = nullptr;
    if ((error = snd_pcm_hw_params_malloc(&hardwareRaw)) < 0) {
        core::Error result = alsaError(core::ErrorCode::OutOfMemory,
                                      "allocate ALSA hardware parameters", error);
        close();
        return result;
    }
    const std::unique_ptr<snd_pcm_hw_params_t, decltype(&snd_pcm_hw_params_free)> hardware{
        hardwareRaw, &snd_pcm_hw_params_free};
    const auto configureBase = [&]() {
        return snd_pcm_hw_params_any(impl_->pcm, hardware.get()) >= 0 &&
               snd_pcm_hw_params_set_access(impl_->pcm, hardware.get(),
                                             SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
               snd_pcm_hw_params_set_format(impl_->pcm, hardware.get(),
                                             SND_PCM_FORMAT_S24_3LE) >= 0 &&
               snd_pcm_hw_params_set_rate_resample(impl_->pcm, hardware.get(), 0U) >= 0;
    };
    const auto configureFormat = [&](unsigned int channels, unsigned int wantedRate) {
        if (!configureBase() ||
            snd_pcm_hw_params_set_channels(impl_->pcm, hardware.get(), channels) < 0) {
            return false;
        }
        unsigned int actualRate = wantedRate;
        return snd_pcm_hw_params_set_rate_near(impl_->pcm, hardware.get(), &actualRate, nullptr) >= 0 &&
               actualRate == wantedRate;
    };

    impl_->tp7MonitorMode = !configureFormat(2U, 48000U);
    if (impl_->tp7MonitorMode && !configureFormat(6U, 96000U)) {
        core::Error result = core::makeError(
            core::ErrorCode::UnsupportedFormat,
            "ALSA device supports neither 48 kHz stereo nor TP-7 96 kHz six-channel playback");
        close();
        return result;
    }
    impl_->hardwareChannels = impl_->tp7MonitorMode ? 6 : 2;
    impl_->tp7ConverterState = {};

    snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(
        bufferFrames * (impl_->tp7MonitorMode ? 2 : 1));
    snd_pcm_uframes_t hardwareBuffer = period * 2U;
    if ((error = snd_pcm_hw_params_set_period_size_near(impl_->pcm, hardware.get(), &period, nullptr)) < 0 ||
        (error = snd_pcm_hw_params_set_buffer_size_near(impl_->pcm, hardware.get(),
                                                         &hardwareBuffer)) < 0 ||
        (error = snd_pcm_hw_params(impl_->pcm, hardware.get())) < 0) {
        core::Error result = alsaError(core::ErrorCode::UnsupportedFormat,
                                      "configure ALSA period/buffer", error);
        close();
        return result;
    }

    snd_pcm_sw_params_t* softwareRaw = nullptr;
    if ((error = snd_pcm_sw_params_malloc(&softwareRaw)) < 0) {
        core::Error result = alsaError(core::ErrorCode::OutOfMemory,
                                      "allocate ALSA software parameters", error);
        close();
        return result;
    }
    const std::unique_ptr<snd_pcm_sw_params_t, decltype(&snd_pcm_sw_params_free)> software{
        softwareRaw, &snd_pcm_sw_params_free};
    if ((error = snd_pcm_sw_params_current(impl_->pcm, software.get())) < 0 ||
        (error = snd_pcm_sw_params_set_avail_min(impl_->pcm, software.get(), period)) < 0 ||
        (error = snd_pcm_sw_params_set_start_threshold(impl_->pcm, software.get(),
                                                        hardwareBuffer)) < 0 ||
        (error = snd_pcm_sw_params(impl_->pcm, software.get())) < 0 ||
        (error = snd_pcm_prepare(impl_->pcm)) < 0) {
        core::Error result = alsaError(core::ErrorCode::IoError,
                                      "configure ALSA software parameters", error);
        close();
        return result;
    }

    impl_->format = format;
    impl_->periodFrames = static_cast<core::FrameCount>(period);
    impl_->hardwarePeriodFrames = static_cast<core::FrameCount>(period);
    if (impl_->tp7MonitorMode) {
        impl_->periodFrames /= 2;
    }
    impl_->hardwareBufferFrames = static_cast<core::FrameCount>(hardwareBuffer);
    impl_->callback = &callback;
    const std::size_t sourceSamples =
        static_cast<std::size_t>(impl_->periodFrames) * format.channels;
    const std::size_t hardwareSamples = static_cast<std::size_t>(impl_->hardwarePeriodFrames) *
                                        impl_->hardwareChannels;
    impl_->floatBuffer.assign(sourceSamples, 0.0F);
    impl_->hardwareFloatBuffer.assign(impl_->tp7MonitorMode ? hardwareSamples : 0U, 0.0F);
    impl_->packedBuffer.assign(hardwareSamples * 3U, std::byte{0});
    return core::success();
}

core::Status AlsaAudioOutput::start() {
    if (impl_->pcm == nullptr || impl_->callback == nullptr) {
        return core::makeError(core::ErrorCode::InvalidState, "ALSA output not open");
    }
    if (impl_->running.exchange(true, std::memory_order_acq_rel)) {
        return core::success();
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (const int error = snd_pcm_prepare(impl_->pcm); error < 0) {
        impl_->running.store(false, std::memory_order_release);
        return alsaError(core::ErrorCode::IoError, "snd_pcm_prepare", error);
    }

    impl_->thread = std::thread{[this] {
        sched_param scheduling{};
        scheduling.sched_priority = std::max(1, sched_get_priority_max(SCHED_FIFO) - 10);
        (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &scheduling);

        while (impl_->running.load(std::memory_order_acquire)) {
            impl_->callback->renderAudio(impl_->floatBuffer, impl_->periodFrames,
                                         impl_->format.channels);
            std::span<const float> hardwareSamples = impl_->floatBuffer;
            if (impl_->tp7MonitorMode) {
                convertStereo48ToTp7Monitor96(impl_->floatBuffer, impl_->hardwareFloatBuffer,
                                              impl_->tp7ConverterState);
                hardwareSamples = impl_->hardwareFloatBuffer;
            }
            for (std::size_t sample = 0; sample < hardwareSamples.size(); ++sample) {
                packS24_3LE(hardwareSamples[sample],
                            std::span<std::byte, 3>{impl_->packedBuffer.data() + sample * 3U, 3U});
            }

            snd_pcm_uframes_t writtenFrames = 0;
            while (writtenFrames < static_cast<snd_pcm_uframes_t>(impl_->hardwarePeriodFrames) &&
                   impl_->running.load(std::memory_order_acquire)) {
                const auto* data = impl_->packedBuffer.data() +
                                   static_cast<std::size_t>(writtenFrames) *
                                       impl_->hardwareChannels * 3U;
                const snd_pcm_sframes_t written = snd_pcm_writei(
                    impl_->pcm, data,
                    static_cast<snd_pcm_uframes_t>(impl_->hardwarePeriodFrames) - writtenFrames);
                if (written < 0) {
                    if (!impl_->running.load(std::memory_order_acquire)) {
                        break;
                    }
                    impl_->callback->onXRun();
                    if (snd_pcm_recover(impl_->pcm, static_cast<int>(written), 1) < 0) {
                        impl_->running.store(false, std::memory_order_release);
                    }
                    continue;
                }
                if (written == 0) {
                    continue;
                }
                writtenFrames += static_cast<snd_pcm_uframes_t>(written);
            }
        }
    }};
    return core::success();
}

void AlsaAudioOutput::stop() noexcept {
    const bool wasRunning = impl_->running.exchange(false, std::memory_order_acq_rel);
    if (wasRunning && impl_->pcm != nullptr) {
        (void)snd_pcm_drop(impl_->pcm);
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

void AlsaAudioOutput::close() noexcept {
    stop();
    if (impl_->pcm != nullptr) {
        (void)snd_pcm_close(impl_->pcm);
        impl_->pcm = nullptr;
    }
    impl_->callback = nullptr;
    impl_->floatBuffer.clear();
    impl_->hardwareFloatBuffer.clear();
    impl_->packedBuffer.clear();
}

bool AlsaAudioOutput::isRunning() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

core::AudioFormat AlsaAudioOutput::format() const noexcept { return impl_->format; }

core::FrameCount AlsaAudioOutput::bufferFrames() const noexcept {
    return impl_->periodFrames;
}

core::FrameCount AlsaAudioOutput::latencyFrames() const noexcept {
    return impl_->tp7MonitorMode ? impl_->hardwareBufferFrames / 2
                                 : impl_->hardwareBufferFrames;
}

}  // namespace aw::platform::rpi

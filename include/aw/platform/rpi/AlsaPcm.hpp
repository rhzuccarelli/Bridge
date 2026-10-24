#pragma once

#include "aw/platform/Tp7Monitor.hpp"

namespace aw::platform::rpi {

// The TP-7 conversion is shared with the ESP32-P4 backend.
using platform::convertStereo48ToTp7Monitor96;
using platform::kTp7MonitorChannels;
using platform::packS24_3LE;
using platform::Tp7MonitorConverterState;

}  // namespace aw::platform::rpi

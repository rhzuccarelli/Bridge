#pragma once

#include "UsbClient.hpp"

namespace probe {

/// Asks a TP-7 in its USB audio/MIDI personality to re-enumerate as MTP.
///
/// The same SysEx `AlsaTp7ModeSwitcher` sends on the Raspberry Pi; there ALSA
/// wraps it into USB-MIDI event packets, here we do it ourselves.
esp_err_t sendTp7EnterMtp(BulkPipe& midiOut);

}  // namespace probe

# TP-7 probe for the ESP32-P4

A small stand-alone ESP-IDF firmware that tests every step between "TP-7 plugged
into the P4" and "MTP session open". Run it before porting any of Bridge. It
reports the first step that fails.

It does, in order:

1. Installs the USB host stack on the **high-speed** port.
2. Prints every attached device's speed and full descriptors.
3. Summarises what it found: UAC1 or UAC2 audio, MIDI OUT, MTP.
4. If it sees a TP-7 in USB-audio mode, it sends the same SysEx the Pi build sends,
   as raw USB-MIDI packets, to switch the TP-7 into MTP mode. It does this once per
   boot.
5. When the TP-7 comes back in MTP mode, it opens a session, prints device and
   storage info, lists the root and `recordings`, and times a read of the largest
   take using `GetPartialObject`.
6. It closes the session, so the TP-7 should go back to USB audio.

The probe only reads. It never writes to or deletes anything on the TP-7.

## Build and flash

Requires ESP-IDF v5.5.x (needs `peripheral_map` and the hub driver).

```bash
. $IDF_PATH/export.sh
cd firmware/esp32p4/tp7-probe
idf.py set-target esp32p4
idf.py menuconfig      # "TP-7 probe" menu: port, VBUS GPIO, options
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

Flash and monitor through the board's **UART/debug** USB connector. The TP-7 goes
into the **high-speed USB host** connector. These are different sockets.

## If nothing attaches ("still no device")

These are the usual causes, most likely first:

1. **No 5 V on the host socket.** An ESP32-P4 board in host mode must supply VBUS.
   The TP-7 will not signal attach without it. Measure 5 V between VBUS and GND on
   the socket. Some boards gate VBUS through a load switch on a GPIO; set it in
   `menuconfig → TP-7 probe → VBUS GPIO`. Some boards power the socket only when
   the board itself is powered from a particular input. Check your board's
   schematic.
2. **Wrong socket.** The P4 has two OTG controllers. The probe defaults to the
   high-speed one (peripheral 0, internal UTMI PHY). On Espressif's
   ESP32-P4-Function-EV-Board, the USB-C HS port and the USB-A port share that
   controller and cannot be used at the same time.
3. **A hub between the P4 and the socket.** Boards with several USB-A sockets use
   an on-board hub. `CONFIG_USB_HOST_HUBS_SUPPORTED=y` is already set here.
4. **Cable.** USB-C to USB-C charge-only cables are common. Use the cable that
   worked on the Pi.

## If it attaches but enumeration fails

The TP-7 is a composite USB-audio + MIDI device, so its configuration descriptor
is large. ESP-IDF's default control-transfer buffer is 256 bytes, and with that
default **enumeration of such a device fails**. This project sets
`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=2048`. If you build your own project,
copy that line.

## What to send back

The full monitor log from boot until the line `closing MTP session`. Pay most
attention to these lines:

- `speed HIGH` or `FULL`
- `summary: audio=UAC2 ...` (decides the USB monitor-audio path; see
  `docs/esp32p4-port.md`)
- `GetPartialObject 0x101B: yes/NO`
- `Read N MiB ... = X MiB/s`

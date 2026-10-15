# TP-7 monitor-audio probe for the ESP32-P4

Step P3a of `docs/esp32p4-port.md`. A stand-alone ESP-IDF firmware that plays a
test tone through a TP-7 in USB-audio mode, over the same UAC1 playback endpoint
the Pi uses through ALSA. It shows whether the P4 can drive that endpoint before
an audio backend is written against it.

When a TP-7 attaches in USB-audio mode, the probe:

1. Finds the playback alternate setting in the active configuration (on the
   TP-7: interface 2 alt 1, isochronous OUT `0x06`, 6 channels of 24-bit in 3
   bytes, 2 × 882 bytes per 1 ms, feedback on `0x83`).
2. Claims it, sends `SET_INTERFACE` to select it on the device, then writes the
   host channel's multi-count (`HCCHAR.EC`) to 2 and its starting data PID to
   `MDATA` (see "Why the register writes").
3. Sets the endpoint's sampling frequency to 96 kHz (UAC1 `SET_CUR`).
4. Streams a sine at −18 dBFS on channels 1–2 (silence on 3–6) for 60 s,
   cycling through the packet-sizing variants under "Listening test: packet
   alignment".
5. Logs once a second: packets/s, frames/s, failed packets, and the
   feedback value with the encoding it was read as.

## Build and flash

```bash
. ~/esp/esp-idf/export.sh            # ESP-IDF v5.5.x
cd firmware/esp32p4/tp7-audio-probe
idf.py set-target esp32p4            # once
idf.py menuconfig                    # "TP-7 audio probe": tone, level, duration
idf.py -p /dev/cu.usbmodem… flash monitor
```

Wire the TP-7 exactly as for `tp7-probe` (see its README). Reset the board or
replug the TP-7 to run again.

## Configuration it depends on

- **FIFO split.** The high-speed controller's FIFO is 896 lines of 4 bytes
  (`GHWCFG3`). ESP-IDF's default split gives the periodic TX FIFO 512 bytes and
  rejects the 882-byte endpoint (`EP MPS (882) exceeds supported limit (512)`).
  The probe installs the host with RX 320, non-periodic TX 128 and periodic
  TX 448 lines. 448 lines (1792 bytes) hold one 1764-byte microframe. A split
  that adds up to more than 896 lines fails at the first port reset with
  `Invalid FIFO config`.
- **`SET_INTERFACE` is the caller's job.** `usb_host_interface_claim` only
  allocates the host-side pipes; it never tells the device. Without the request,
  the TP-7 stays on alternate 0, which has no endpoints. It drops the playback
  data and never answers on the feedback endpoint, while every OUT packet still
  reports "completed". (MTP never hit this because its interface uses
  alternate 0.)
- **Why the register writes.** The playback endpoint is high-bandwidth:
  `wMaxPacketSize` bits 12:11 ask for a second transaction in each
  microframe. ESP-IDF's DWC driver reads only the packet size and leaves
  `HCCHAR.EC` at 0, so the controller would send one 882-byte transaction per
  millisecond, half the stream. A two-transaction OUT microframe must also start
  with the `MDATA` PID (then `DATA1`). ESP-IDF sets `DATA0` once, when it
  allocates the channel. Linux's dwc2 picks the PID the same way
  (`dwc2_set_pid_isoc`). The HAL writes these registers one field at a time
  and never touches `EC` or the PID again, so values written after the claim
  (when the channel exists but is idle) stay in place.
  `menuconfig → TP-7 audio probe → Program two transactions per microframe`
  turns the writes off for comparison.

## Bench results (2026-10-14, TP-7 OS 1.1.11, Waveshare ESP32-P4-Pico)

- Claim, `SET_INTERFACE` (the TP-7 confirms alternate 1), multi-count and
  PID writes, and `SET_CUR` all work. The TP-7 reads the rate back as 96000 Hz.
- **Playback runs clean on the bus**: 1000 packets/s and 96 000 frames/s, no
  failed or skipped packets, and a clean stop and interface release.
  Isochronous OUT has no handshake, so the bus counts alone do not prove the
  TP-7 accepts the data. The listening test does.
- **Feedback works** once the alternate setting is selected. The TP-7 sends 3
  bytes in Q10.14 frames per 1 ms frame (UAC1's full-speed format, even at high
  speed), around `0x180000` = 96.000. The stream follows it.
- Before `SET_INTERFACE` was added, the tone was silent and every feedback
  packet failed. That was the TP-7 sitting on alternate 0, not a host
  controller fault.
- **Listening test, first pass: the tone is audible but has heavy
  artifacts.** That pass gated the tone to one half of each packet, which
  buzzes at 1 kHz by itself, so only its continuous 2 kHz reference counts,
  and that one was not clean either.

## Listening test: packet alignment

ESP-IDF places each packet of a transfer straight after the previous one in
the data buffer (`_buffer_fill_isoc`). A TP-7 frame is 18 bytes, so a packet
with an odd frame count (95 or 97, whenever the feedback is not exactly
96.000) leaves every later packet in that transfer off 4-byte alignment. The
suspicion is that the DWC controller's DMA does not handle that. The probe now
cycles, 8 s each, through three continuous tones:

| Tone | Packet sizes | Aligned? |
| --- | --- | --- |
| 440 Hz | exactly what the feedback asks for | no, when a count is odd |
| 660 Hz | the feedback rate in steps of 2 frames | always |
| 880 Hz | 96 frames, feedback ignored | always |

The once-a-second log line ends with `odd-sized packets`, a running count. If
440 Hz is rough while 660 Hz and 880 Hz are clean, alignment is the cause, and
the backend sizes packets in steps of 2 frames. If all three are rough, the
cause is elsewhere, most likely the second transaction of each microframe.

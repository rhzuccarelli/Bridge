# Bridge firmware for the ESP32-P4

Step P1 of `docs/esp32p4-port.md`: the whole portable application running on the
P4 with stand-in backends. It boots, runs the UI loop, and prints every changed
display frame to the serial log.

## How it is built

`components/aw` adds the repository's `src/` tree with `AW_PLATFORM=esp32p4`.
The device therefore builds the same per-layer static libraries (`aw_core` …
`aw_ui`, `aw_app`) as the Pi and the simulator, and the same layering rules
apply. ESP-IDF's compile options reach those targets because
`idf_component_register` sets them on the component directory before
`add_subdirectory` runs.

The platform assembly is `src/platform/esp32p4`, and the composition root is
`main/app_main.cpp`.

| HAL interface | P1 backend | Replaced in |
| --- | --- | --- |
| Clock | `esp_timer` + `usleep` | — |
| Display | 128×64 SSD1306 over SPI2 (`EspSpiOledBus`, 8 MHz, mode 0): SCK GPIO6, SDA GPIO5, RES GPIO4, DC GPIO3, CS GPIO14, VCC on 3.3 V. `SerialPreviewDisplay` mirrors each changed frame to the serial log | done |
| Storage | `FatFsStorage`: SD card over SDMMC (4-bit, 40 MHz, falling back to 20 MHz; CLK GPIO43, CMD GPIO44, D0–D3 GPIO39–42, powered from on-chip LDO VO4), FatFS with long file names, projects under `/sdcard/projects`. Without a card, `NoCardStorage` stands in | done |
| Encoder + push switch | `EspGpioEncoder` / `EspGpioButtons`: KY-040 with CLK on GPIO16, DT on GPIO15 and SW on GPIO17, sampled at 1 kHz with the Pi's decoder and debounce logic | done |
| Audio output | `Tp7AudioOutput` (`usb/`): the engine's 48 kHz stereo through the TP-7's USB playback endpoint (96 kHz, six channels, rate-matched to its feedback), behind `FallbackAudioOutput`, which uses `NullAudioOutput` on a wall clock while no TP-7 is in USB-audio mode | P3 (sine verified by recording) |
| USB control requests | `EspUsbControl` (`EspUsb.cpp`): vendor requests on endpoint 0 of an attached device. Home → DUET INPUTS uses it to set an Apogee Duet 2's input types | done (Duet 2 `0c60:0007` verified) |
| USB, MTP, TP-7 mode | `createUsbBackends` (`EspUsb.cpp`, `usb/`): ESP-IDF USB host on the high-speed port, a PTP/MTP initiator built from the probe (reads with GetPartialObject, writes with SendObjectInfo/SendObject), and the SysEx switch into MTP sent as USB-MIDI | done (Import verified) |

## Build, flash, watch

```bash
. ~/esp/esp-idf/export.sh            # ESP-IDF v5.5.x
cd firmware/esp32p4/bridge
idf.py set-target esp32p4            # once
idf.py -p /dev/cu.usbmodem… flash monitor
```

Expected log, about 1.8 s after reset:

```text
[INFO ] esp32p4    skeleton platform ready: serial display preview, null storage, input, audio and USB
[INFO ] app        initialised
[INFO ] main       free heap: internal 472 KiB, PSRAM 32724 KiB
[display] frame 1
+--------------------------------------------------------------------------------------------------------------------------------+
|   ▄▄  ▄                                           ▄                       ▄                    ▄▄▄    ▄▄▄▄▄     ▄▄▄   ▄▄▄▄     |
|   ██  █    ▄▄▄            ▄▄▄▄    ▄▄▄      ▄▄▄    ▄        ▄▄▄     ▄▄▄   ▄█▄                  █   ▀     █     ▄▀   ▀▄ █   █    |
|   █ █ █   █   █           █   █   █       █   █   █       █▄▄▄█   █   ▀   █                    ▀▄▄▄     █     █     █ █▄▄▄▀    |
|   █ ▀▄█   █   █           █   █   █       █   █   █       █   ▄   █   ▄   █                   ▄   █     █     ▀▄   ▄▀ █        |
|   ▀  ▀▀    ▀▀▀            █▀▀▀    ▀        ▀▀▀    █        ▀▀▀     ▀▀▀    ▀▀                   ▀▀▀      ▀       ▀▀▀   ▀        |
|                           ▀                      ▀▀                                                                            |
| ▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄ |
| █             ▄▄▄▄▄   ▄▄▄▄      ▄     ▄▄  ▄    ▄▄▄    ▄▄▄▄      ▄▄▄   ▄▄▄▄    ▄▄▄▄▄                                          █ |
| █               █     █   █    █▀▄    ██  █   █   ▀   █   █   ▄▀   ▀▄ █   █     █                                            █ |
| █    ▀▀▄▄       █     █▄▄▄▀   █▄▄█    █ █ █    ▀▄▄▄   █▄▄▄▀   █     █ █▄▄▄▀     █                                            █ |
| █     ▄▄▀       █     █  ▀█   █   █   █ ▀▄█   ▄   █   █       ▀▄   ▄▀ █  ▀█     █                                            █ |
| █    ▀          ▀     ▀   ▀  ▀    ▀   ▀  ▀▀    ▀▀▀    ▀         ▀▀▀   ▀   ▀     ▀                                            █ |
| ▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀ |
|               ▄▄▄▄▄   ▄▄▄▄      ▄       ▄▄▄   ▄   ▄    ▄▄▄                                                                     |
…
```

That is the home screen as the 128×64 OLED will show it: "No project",
"STOP", then a TRANSPORT / TRACKS / COPY CHANNEL / CHANGE DECK menu.

Frames are printed only when the picture changes, and at most every 2 s
(`Esp32P4Config::previewIntervalMillis`). With no input, the home screen prints
once.

Every console writer (Bridge's logger, ESP-IDF's `ESP_LOGx` output and the
display preview) goes through one lock in `EspConsole`, one whole line at a
time. Without it, the three writers interleave character by character on the
UART.

## Configuration notes (`sdkconfig.defaults`)

- **Board:** 32 MiB flash and 32 MiB PSRAM (Waveshare ESP32-P4-Pico), with a 4 MiB
  factory app partition. The P1 image is about 390 KiB.
- **PSRAM at 80 MHz.** The default is 20 MHz, and at that speed the boot-time
  memory test alone takes 3.8 s. 200 MHz is still marked experimental in ESP-IDF
  v5.5.1.
- **`CONFIG_FREERTOS_HZ=1000`.** At the default 100 Hz, the UI loop's 33 ms frame
  sleep rounds to 10 ms ticks.
- **Stacks:** 16 KiB for the main task (the UI loop), and 16 KiB as the pthread
  default for every `std::thread`. ESP-IDF's default is 3 KiB. Per-thread sizes
  and priorities through `esp_pthread_set_cfg` come with the real audio backend
  in P3.
- **No C++ exceptions.** The portable layers return `core::Result`. The one
  `try`/`catch` they contained (float parsing in `ProjectMetadata.cpp`) now uses
  `strtof`.

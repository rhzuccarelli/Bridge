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
| Display | `SerialPreviewDisplay`: the real `Ssd1306Display` logic (128×64), with each changed frame printed to the serial log | P2 (SSD1306 over SPI) |
| Storage | `NoCardStorage`: an empty projects root that refuses reads and writes | P2 (FatFS on SD) |
| Buttons, encoder | Never fire | P2 (GPIO) |
| Audio output | `NullAudioOutput`: drives the engine on a wall clock and discards samples | P3 (I2S) |
| USB / MTP | No devices / absent | P4 |

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

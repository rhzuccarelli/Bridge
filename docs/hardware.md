# Prototype hardware

The bench prototype, and how the software talks to each piece.

| Part | Connection | Backend |
| --- | --- | --- |
| Raspberry Pi 5 | — | — |
| **SPI OLED, 128×64 mono (SSD1306/SH1106)** | SPI + 2 GPIO | `Ssd1306Display` + `SpiOledBus` |
| KY-040 rotary encoder with push switch | GPIO | `GpioEncoder` + `GpioButtons` *(Phase 6)* |
| microSD | — | `PosixStorage` |
| USB audio interface or codec | USB / I²S | `AlsaAudioOutput` *(Phase 5)* |
| Teenage Engineering TP-7 | USB (MTP) | `LibUsbHost` + `LibMtpDevice` *(Phase 8)* |
| *(alternative)* 3.5" HDMI LCD, 480×320, XPT2046 touch | HDMI + SPI touch | `FramebufferDisplay` + `EvdevTouch` |

> **Two display options, one reason to prefer the OLED on the prototype.** The
> 3.5" HDMI LCD is a HAT that sits on header pins 1–26 — which are exactly the
> pins the encoder needs, so the two cannot coexist without a stacking header.
> The SPI OLED is wired with a handful of jumpers and leaves the rest of the
> header free, so the OLED **and** the encoder fit on a bare board. The cost is
> that it is small and monochrome (see below). Both are real backends behind the
> same `DisplayInterface`; switch with `AW_DISPLAY`.

---

## Display — SPI OLED (SSD1306 / SH1106)

A 128×64 monochrome OLED on a 7-pin SPI breakout (`GND VCC SCK SDA RES DC CS`).
It is the prototype's primary display because it frees the GPIO header for the
encoder.

### What it is, and what that costs

128×64 is **1/40th the pixels** of the 320×240 target panel, and it is one bit
per pixel — no colour. The `Ssd1306Display` backend still implements the same
`DisplayInterface` as the colour panels: the UI renders into its normal RGB565
framebuffer, and the backend thresholds each pixel to on/off and packs it into
the controller's page RAM. So nothing above the HAL changes — but the Phase 7
screens will be designed for a small monochrome canvas (transport state,
timecode, level bars, a short list), closer to a Tascam's display than the
four-lane colour layout the 320×240 panel allows.

> ⚠️ This diverges from the stated ESP32-P4 product target (320×240 colour). A
> UI laid out for 128×64 will need rework for the final panel. That is an
> accepted trade for getting the encoder and a screen working together on the
> bench today.

### Wiring

Chosen to sit clear of the encoder (GPIO 17/27/22), using hardware SPI0:

| OLED pin | Pi pin | BCM | Note |
| --- | --- | --- | --- |
| `GND` | pin 20 | — | |
| `VCC` | pin 17 | — | **3.3 V** |
| `SCK` | pin 23 | GPIO 11 | SPI0 SCLK |
| `SDA` | pin 19 | GPIO 10 | SPI0 MOSI |
| `RES` | pin 22 | GPIO 25 | reset |
| `DC`  | pin 18 | GPIO 24 | data/command |
| `CS`  | pin 24 | GPIO 8 | SPI0 CE0 — the spidev driver toggles it |

`VCC` is **3.3 V** (pin 17), not 5 V. These modules run at 3.3 V and the SPI
lines must not exceed it.

### Setup

Enable SPI, then reboot:

```bash
sudo raspi-config nonint do_spi 0     # or: add 'dtparam=spi=on' to config.txt
sudo reboot
```

Confirm `/dev/spidev0.0` exists and you are in the groups:

```bash
ls /dev/spidev0.0
groups | grep -o 'spi\|gpio'          # need both
```

### Bring-up

Before any UI exists, prove the panel with the test tool:

```bash
./build/src/platform/aw-oled-test
```

You should see a 1-pixel border, a diagonal, and a filled block in the top-left
corner. If not:

| Symptom | Fix |
| --- | --- |
| Nothing at all | Check power and wiring; confirm `spi`/`gpio` group membership. |
| Image shifted right / split | It's an SH1106, not an SSD1306: `./build/src/platform/aw-oled-test sh1106` |
| Upside down or mirrored | `AW_OLED_ROTATE=180 ./build/src/platform/aw-oled-test` |

Once the pattern is correct, note whether you needed `sh1106` / `180`, and pass
the matching env vars when running the app.

### How the backend works

- **No external library.** `SpiOledBus` talks to `/dev/spidev0.0` and the GPIO
  character device (`/dev/gpiochipN`) using only kernel uapi headers — no
  libgpiod, no wiringPi, nothing to install.
- **CS is left to spidev.** The driver asserts CE0 around each write, so the
  code only manages `DC` (per transfer) and `RES` (pulsed once at init).
- **Page-addressed flush.** The whole 1 KiB buffer is pushed one page at a time,
  which works identically on the SSD1306 and the SH1106 (the latter has no
  horizontal-addressing mode). At 8 MHz a full frame is well under a
  millisecond, so there is no damage-tracking — every `present()` pushes the lot.
- **`setBacklight` maps to contrast.** An OLED has no backlight, but the contrast
  register is the honest nearest thing.

### Config (environment variables)

| Variable | Values | Meaning |
| --- | --- | --- |
| `AW_OLED_CHIP` | `ssd1306` `sh1106` | Controller. Default `ssd1306`. |
| `AW_OLED_ROTATE` | `0` `180` | Orientation. |
| `AW_OLED_SPI` | path | Default `/dev/spidev0.0`. |

---

## Display — 3.5" HDMI LCD

This panel is **a monitor**, not an SPI display. The Pi drives it through its
normal video pipeline, so there is no controller init sequence and no panel
driver to write: the kernel hands us a memory mapping at `/dev/fb0` and we write
pixels into it. `FramebufferDisplay` supports 16 bpp (RGB565, a direct copy) and
32 bpp (converted per pixel, honouring the channel offsets the kernel reports —
different Pi firmware revisions disagree, and guessing gives you a display with
red and blue swapped).

### Logical canvas is 320×240, not 480×320

The panel is 480×320, but the UI is drawn for **320×240** and letterboxed. This is
deliberate: the shipping ESP32-P4 device has a 320×240 screen, and laying screens
out against 480×320 now would mean redoing all of them later. `AW_STRETCH=1`
fills the panel instead, if you want to see it large.

The canvas sits centred along the top, which leaves one contiguous 480×80 strip
along the bottom rather than two useless bands.

### Setup

**First, just plug it in and boot.** Most of these panels advertise their mode over
EDID and come up at 480×320 with no configuration at all. Check:

```bash
fbset -s              # expect 480x320, 16 or 32 bpp
```

If that is right, you are done — skip the rest of this section.

**If the mode is wrong, force it in `/boot/firmware/cmdline.txt`** by adding this to
the single line already there (space separated, do not add a newline):

```
video=HDMI-A-1:480x320M@60
```

> ⚠️ The legacy `hdmi_group` / `hdmi_mode` / `hdmi_cvt` settings in `config.txt` do
> **not** work on a Raspberry Pi 5. The Pi 5 is KMS-only, and those options are
> handled by the old framebuffer driver it does not have. Guides that tell you
> otherwise predate the Pi 5. Use the `cmdline.txt` `video=` parameter above.

`/dev/fb0` still exists under KMS through DRM's framebuffer emulation, which is what
`FramebufferDisplay` uses.

The user running the application needs the `video` group:

```bash
sudo usermod -aG video "$USER"     # log out and back in
```

Running from a graphical desktop session will fight the compositor for the
framebuffer. Use a plain console (`sudo systemctl set-default multi-user.target`)
or run over SSH with `AW_DISPLAY=terminal`.

---

## Touch — XPT2046

Read through the **kernel's evdev interface**, not by driving the SPI ADC
ourselves. The `ads7846`/`xpt2046` driver already does sampling, pressure
thresholding and debouncing, and its calibration is settable with the usual
userspace tools; reimplementing that would be worse in every respect.

Enable the overlay in `config.txt` (pins per your board's documentation):

```ini
dtparam=spi=on
dtoverlay=ads7846,cs=1,penirq=25,speed=50000,penirq_pull=2,xohms=150
```

Then:

```bash
ls /dev/input/event*                       # the device should appear
sudo evtest                                # confirm ABS_X / ABS_Y / BTN_TOUCH
sudo usermod -aG input "$USER"             # log out and back in
```

`EvdevTouch` finds the device by capability (EV_ABS with ABS_X/ABS_Y), so no path
is normally needed; override with `AW_TOUCH_DEVICE=/dev/input/eventN` if it picks
the wrong one. Axis ranges come from the driver's `EVIOCGABS`, so a calibrated
panel works with no extra step. If the axes are mirrored or swapped for your
mounting, set `swapAxes` / `invertX` / `invertY` in `TouchConfig`.

### Touch is a *stand-in for buttons*, not a pointer

The product has **no touchscreen**, and that constraint has to survive the
prototype. So touch never reaches the UI: a tap is hit-tested against
`VirtualPanelLayout` and emitted as an ordinary `ButtonId`, indistinguishable
from a real switch. Taps on the canvas itself are discarded.

The strip is two rows of six:

```
+--------+--------+--------+--------+--------+--------+
|  PLAY  |  STOP  |  REC   |  UNDO  |  SAVE  |  BACK  |
+--------+--------+--------+--------+--------+--------+
|  TR1   |  TR2   |  TR3   |  TR4   |  IMP   |  EXP   |
+--------+--------+--------+--------+--------+--------+
```

---

## Encoder — KY-040

The **primary navigation control**, and the one prototype part that is exactly
what the shipping device will use.

Because the display is now the jumper-wired SPI OLED rather than the pin-hungry
LCD HAT, the header is free and the encoder uses its clean original wiring near
the start of the header:

### Wiring

| KY-040 | Pi pin | BCM |
| --- | --- | --- |
| `+`   | pin 1 | — (**3.3 V**) |
| `GND` | pin 9 | — |
| `CLK` | pin 11 | 17 |
| `DT`  | pin 13 | 27 |
| `SW`  | pin 15 | 22 |

`+` goes to a real 3.3 V pin, so `GpioPinMap::encoderPowerLine` is left at `-1`
(no GPIO used for power).

> ⚠️ **`+` is 3.3 V (pin 1), not 5 V.** The Pi's GPIO is not 5 V tolerant, and a
> KY-040 powered from 5 V puts 5 V onto CLK/DT/SW and can permanently damage the
> board. Pin 1 is 3.3 V; pin 2 directly beside it is 5 V — do not confuse them.

This layout is only reachable because the header is not covered by a HAT. It does
**not** work with the 3.5" LCD HAT installed — that HAT buries pins 1–26. If you
ever go back to the HAT, either lift it on a **40-pin stacking header** (which
exposes the whole header) or move the encoder to the exposed pin-29–39 tail and
power `+` from a driven-high GPIO; the project history has that arrangement,
and `GpioPinMap::encoderPowerLine` exists to support it.

### Why powering `+` from a GPIO is a valid trick (for the HAT case)

Worth recording, since it is not obvious. If the header *is* covered and only the
tail is exposed, there is no 3.3 V pin left — and leaving `+` disconnected to
rely on the Pi's internal pull-ups **does not work on a KY-040**. Its three 10 kΩ
pull-ups share the `+` node, so with that node floating, closing the CLK contact
drags DT and SW to roughly **1.24 V** — between the logic-low (0.8 V) and
logic-high (1.6 V) thresholds. The result is an encoder that intermittently
misses and doubles steps: a nasty, flaky-looking fault. Driving a spare GPIO high
instead supplies a genuine 3.3 V at ~1 mA, well within a pin's 16 mA budget.

### Which gpiochip

The chip carrying the header moved between kernel releases: on a Pi 5 the pins sit
behind the RP1 southbridge, exposed as `gpiochip4` by older Bookworm kernels and
`gpiochip0` by newer ones. Run `gpiodetect` and use whichever reports **54 lines**;
override the default with `AW_GPIO_CHIP=gpiochipN`.

### Notes for the Phase 6 implementation

- The power line must be requested as an output and driven high **before** the
  encoder lines are read, and the request held open — releasing it drops the
  supply.
- All three signal lines still request `BIAS_PULL_UP`. It costs nothing and makes
  the wiring tolerant of a module with missing resistors, or of the power line not
  having come up yet.
- KY-040s are notoriously bouncy. Decoding is a quadrature state machine with
  debounce, coalescing the four transitions per detent into **one** reported step —
  so one physical click always moves the UI by exactly one row.
- `SW` is active-low and is reported as `core::ButtonId::EncoderPush`, i.e. it
  arrives as a *button*, not as part of the encoder interface.

---

## Mixed input sources

The prototype has a real encoder but no physical buttons yet, so button events
come from more than one source at once:

```
GPIO (KY-040 SW) ──┐
Keyboard (SSH) ────┼──> CompositeButtons ──> ButtonListener ──> events
Touch strip * ─────┘
```

`CompositeButtons` merges any number of `ButtonInterface` sources into one
stream. A source that fails to start is skipped with a warning rather than being
fatal — a missing input must not stop the others from working. When real
switches are wired, they become one more source and nothing else changes.

\* The touch strip only exists with the HDMI panel; the OLED has no touch, so on
the OLED setup buttons come from the encoder push plus the keyboard until
physical switches are added.

---

## Keyboard control (no hardware needed)

Works over SSH or on a laptop, and is the fastest way to drive the UI:

| Key | Control | Key | Control |
| --- | --- | --- | --- |
| `space` | Play | `1`–`4` | Track 1–4 |
| `s` | Stop | `Enter` | Encoder push |
| `r` | Record | `←` `→` | Encoder turn |
| `i` | Import | `↑` `↓` | Encoder turn |
| `e` | Export | `b` / `Esc` | Back |
| `u` | Undo | `q` | Quit |
| `w` | Save | | |

A terminal delivers keystrokes, not key-down/key-up, so a press is synthesised as
an immediate press+release pair. Long-press and auto-repeat are therefore **not**
testable this way — use the real encoder or the touch strip for those.

`AW_DISPLAY=terminal` additionally renders the real framebuffer into the terminal
using ANSI truecolour half-blocks, downscaled to fit. It is a preview of the
actual pixels, not a mock, so a layout bug visible there is a layout bug on the
panel.

---

## Environment variables

| Variable | Values | Meaning |
| --- | --- | --- |
| `AW_PROJECTS_ROOT` | path | Where projects live. |
| `AW_DISPLAY` | `auto` `oled` `fb` `terminal` `memory` | Display backend. |
| `AW_INPUT` | `auto` `touch` `keyboard` `none` | Input backends (comma separated). |
| `AW_OLED_CHIP` | `ssd1306` `sh1106` | OLED controller. Default `ssd1306`. |
| `AW_OLED_ROTATE` | `0` `180` | OLED orientation. |
| `AW_OLED_SPI` | path | Default `/dev/spidev0.0`. |
| `AW_FB_DEVICE` | path | Default `/dev/fb0`. |
| `AW_TOUCH_DEVICE` | path | Default: scan by capability. |
| `AW_STRETCH` | `0` `1` | Fill the HDMI panel instead of letterboxing. |

`auto` degrades gracefully: framebuffer → terminal → headless, and a missing
touchscreen simply contributes no button source. A development build that
refuses to start because a peripheral is absent is useless precisely when you
need it.

**The OLED is never chosen by `auto`.** It cannot be probed over its write-only
SPI link (`/dev/spidev0.0` exists on any SPI-enabled Pi, panel or not), so it
must be asked for explicitly with `AW_DISPLAY=oled`.

### Typical invocations

```bash
# On the bench, with the OLED and the encoder
AW_DISPLAY=oled AW_PROJECTS_ROOT=~/projects ./audio-workstation

# If the OLED is an SH1106 and mounted upside down
AW_DISPLAY=oled AW_OLED_CHIP=sh1106 AW_OLED_ROTATE=180 ./audio-workstation

# Over SSH, keyboard driven, previewing the UI in the terminal
AW_DISPLAY=terminal AW_PROJECTS_ROOT=~/projects ./audio-workstation

# CI or a headless box
AW_DISPLAY=memory AW_INPUT=none ./audio-workstation
```

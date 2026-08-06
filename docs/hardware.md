# Prototype hardware

The bench prototype, and how the software talks to each piece.

| Part | Connection | Backend |
| --- | --- | --- |
| Raspberry Pi 5 | — | — |
| 3.5" HDMI LCD, 480×320, XPT2046 touch | HDMI + SPI touch | `FramebufferDisplay` + `EvdevTouch` |
| KY-040 rotary encoder with push switch | GPIO | `GpioEncoder` + `GpioButtons` *(Phase 6)* |
| microSD | — | `PosixStorage` |
| USB audio interface or codec | USB / I²S | `AlsaAudioOutput` *(Phase 5)* |
| Teenage Engineering TP-7 | USB (MTP) | `LibUsbHost` + `LibMtpDevice` *(Phase 8)* |

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

### The constraint: the HAT owns pins 1–26

The LCD HAT's connector covers header pins 1–26, which leaves only the odd/even
tail from **pin 27 to pin 40** reachable. That tail has ground pins but — and this
is the awkward part — **no 3.3 V pin at all**. Both 3.3 V pins (1 and 17) are
underneath the HAT.

Of what remains, two ranges are off limits:

- **Pins 27 and 28** (GPIO 0/1) are reserved for HAT identification EEPROM.
- **GPIO 18–21** (pins 12, 35, 38, 40) carry I2S. Leaving them free keeps the
  option of an I2S audio codec open, which matters because the Pi 5 has no
  headphone jack.

### Wiring

| KY-040 | Pi pin | BCM | Note |
| --- | --- | --- | --- |
| `CLK` | pin 29 | 5 | |
| `DT`  | pin 31 | 6 | |
| `SW`  | pin 33 | 13 | |
| `+`   | pin 37 | 26 | Driven high in software — see below |
| `GND` | pin 39 | — | |

Everything is on the **odd-numbered row**, from pin 29 to the end of the header.
Pin 35 is skipped on purpose (GPIO 19, reserved for I2S).

### Why `+` comes from a GPIO instead of a power pin

The obvious workaround — leave `+` disconnected and rely on the Pi's internal
pull-ups — **does not work on a KY-040**, and it fails in the worst way: as an
intermittently flaky encoder rather than an obvious wiring fault.

The module's three 10 kΩ pull-ups all share the `+` node. With that node floating,
closing the CLK contact pulls it toward ground through R1, which then drags DT and
SW down through R2 and R3:

```
3.3V --50k (internal)-- DT --10k (R2)-- +node --10k (R1)-- GND   [CLK contact closed]
```

That puts DT at roughly **1.24 V** — above the 0.8 V logic-low threshold and below
the 1.6 V logic-high threshold. Indeterminate. You get phantom steps and missed
detents.

Driving a spare GPIO permanently high supplies a genuine 3.3 V instead. It is well
within spec: with every contact closed the module draws about **1 mA**
(3 × 3.3 V / 10 kΩ), against the 16 mA a Pi pin can source. The driver requests the
line as an output set high and holds the request open for as long as the encoder is
in use — see `GpioPinMap::encoderPowerLine`.

> ⚠️ Do **not** substitute a 5 V pin (2 or 4) for the missing 3.3 V. The Pi's GPIO
> is not 5 V tolerant, and a KY-040 powered from 5 V puts 5 V onto CLK/DT/SW and
> can permanently damage the board.

### If you later want physical buttons

Powering `+` from a GPIO solves the encoder, but it does not create pins. After
wiring the encoder, only **GPIO 12 (pin 32), 16 (pin 36), and 20/21** remain — far
short of the nine switches the front panel needs.

The fix at that point is mechanical, not electrical: a **40-pin stacking header**
(or a GPIO ribbon extender) raises the HAT and exposes the entire header,
including both 3.3 V pins. They cost a few pounds. Until then the touch strip and
keyboard cover the remaining controls, merged in by `CompositeButtons`.

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
come from two places at once:

```
GPIO (KY-040 SW) ──┐
                   ├──> CompositeButtons ──> ButtonListener ──> events
Touch strip ───────┤
Keyboard (SSH) ────┘
```

`CompositeButtons` merges any number of `ButtonInterface` sources into one
stream. A source that fails to start is skipped with a warning rather than being
fatal — a missing touchscreen must not stop the encoder from working. When real
switches are wired, they become one more source and nothing else changes.

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
| `AW_DISPLAY` | `auto` `fb` `terminal` `memory` | Display backend. |
| `AW_INPUT` | `auto` `touch` `keyboard` `none` | Input backends (comma separated). |
| `AW_FB_DEVICE` | path | Default `/dev/fb0`. |
| `AW_TOUCH_DEVICE` | path | Default: scan by capability. |
| `AW_STRETCH` | `0` `1` | Fill the panel instead of letterboxing. |

`auto` degrades gracefully: framebuffer → terminal → headless, and a missing
touchscreen simply contributes no button source. A development build that
refuses to start because a peripheral is absent is useless precisely when you
need it.

### Typical invocations

```bash
# On the bench, with the LCD and encoder
AW_PROJECTS_ROOT=~/projects ./audio-workstation

# Over SSH, keyboard driven, with a live preview of the real framebuffer
AW_DISPLAY=terminal AW_PROJECTS_ROOT=~/projects ./audio-workstation

# CI or a headless box
AW_DISPLAY=memory AW_INPUT=none ./audio-workstation
```

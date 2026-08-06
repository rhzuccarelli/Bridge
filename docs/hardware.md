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
what the shipping device will use. Wiring (BCM numbering):

| KY-040 | Pi pin | BCM |
| --- | --- | --- |
| `+`   | pin 1 — **3.3 V** | — |
| `GND` | pin 6 | — |
| `CLK` | pin 11 | 17 |
| `DT`  | pin 13 | 27 |
| `SW`  | pin 15 | 22 |

Which gpiochip carries the header moved between kernel releases: on a Pi 5 the
pins sit behind the RP1 southbridge, exposed as `gpiochip4` by older Bookworm
kernels and `gpiochip0` by newer ones. Run `gpiodetect` and use whichever
reports **54 lines**; override the default with `AW_GPIO_CHIP=gpiochipN`.

⚠️ **`+` goes to 3.3 V, not 5 V.** The Pi's GPIO is not 5 V tolerant, and a
KY-040 powered from 5 V will put 5 V on CLK/DT/SW.

Notes for the Phase 6 implementation:

- All three lines need pull-ups. The module's own resistors are unreliable, so
  the driver requests `BIAS_PULL_UP` from libgpiod rather than trusting the
  board.
- KY-040s are notoriously bouncy. Decoding is a quadrature state machine with
  debounce, coalescing the four transitions per detent into **one** reported
  step — so one physical click always moves the UI by exactly one row.
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

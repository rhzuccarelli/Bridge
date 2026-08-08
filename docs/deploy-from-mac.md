# Deploying from a Mac to a brand-new Raspberry Pi 5

A complete walkthrough, assuming you have never used a Raspberry Pi. Every command
is copy-paste. Each step ends with **✅ Check** — what you should see before moving on.

Total time: about 45 minutes, most of it waiting for downloads.

Throughout, commands are labelled so you always know where to type them:

- 🖥 **On your Mac** — in the Terminal app (`Cmd+Space`, type "Terminal", Enter)
- 🍓 **On the Pi** — in the SSH session you will open in Step 4

---

## Step 0 — Check you have everything

| Item | Notes |
| --- | --- |
| Raspberry Pi 5 | ✅ |
| **27 W USB-C power supply** | ⚠️ Use the official one. A phone charger causes random freezes that look like software bugs and will waste your evening. |
| microSD card, 32 GB+, class A2 | The card is the Pi's hard drive. |
| microSD reader for your Mac | Many Macs have no SD slot — check before you start. |
| **SPI OLED, 128×64 (SSD1306/SH1106)** + 7 jumper wires | The prototype's display. Small monochrome screen, wired with jumpers. |
| KY-040 rotary encoder + 5 jumper wires | Female-to-female, to go from the module to the Pi's pins. |
| Wi-Fi network name and password | Or an Ethernet cable, which is simpler. |

You do **not** need a keyboard or a monitor for the Pi. Everything is done from your
Mac over the network.

> **Why the little OLED and not the 3.5" HDMI LCD?** The HDMI LCD is a HAT that
> sits on the Pi's pin header and covers the pins the encoder needs, so the two
> can't be used together without an extra part (a stacking header). The OLED is
> wired with a few jumper wires and leaves the header free, so the screen and the
> encoder both fit. The trade is that it's small and monochrome. If you'd rather
> use the HDMI LCD, see [hardware.md](hardware.md) — but this guide uses the OLED.

---

## Step 1 — Put the operating system on the microSD card

🖥 **On your Mac**, download and install the official imager:

<https://www.raspberrypi.com/software/>

Insert the microSD card into your Mac, open **Raspberry Pi Imager**, and set the
three buttons:

1. **Choose Device** → `Raspberry Pi 5`
2. **Choose OS** → `Raspberry Pi OS (other)` → **`Raspberry Pi OS Lite (64-bit)`**

   > Pick **Lite**, not the normal desktop version. Lite has no desktop, which means
   > it boots in seconds and — importantly — nothing else is fighting our software
   > for the screen. The desktop version's compositor owns the display and our app
   > cannot draw to it.

3. **Choose Storage** → your microSD card (check the size matches!)

Then click **Next**, and when it asks *"Would you like to apply OS customisation
settings?"* click **EDIT SETTINGS**. This is the important part — it saves you
needing a keyboard and monitor.

On the **General** tab:

| Field | Enter |
| --- | --- |
| Set hostname | `audiopi` |
| Set username and password | username `pi`, and a password you will remember |
| Configure wireless LAN | your Wi-Fi name and password |
| Wireless LAN country | your country code, e.g. `GB`, `US`, `DE` |
| Set locale settings | your timezone and keyboard layout |

On the **Services** tab:

- ✅ **Enable SSH** → select **Use password authentication**

Click **SAVE**, then **YES** to apply, then **YES** to erase the card. Writing takes
about 5 minutes.

> If you use a different hostname or username, substitute it everywhere below —
> this guide uses `pi@audiopi.local`.

**✅ Check:** the Imager says "Write Successful". Eject the card.

---

## Step 2 — Assemble and power on

With the Pi **unplugged from power**:

1. Insert the microSD card into the slot on the underside of the Pi.
2. *Leave the OLED and encoder for now* — you'll wire them in Step 10, after the
   software is running and you can test them.
3. Plug in the USB-C power supply.

The Pi has no power switch; it starts when power is connected. The green LED will
flicker as it reads the card.

> For these first boots you have no screen attached yet — that's fine, everything
> is done over the network from your Mac. You'll wire the OLED in Step 10.

**First boot takes 2–3 minutes** — it resizes the filesystem and reboots itself once.
Be patient.

**✅ Check:** the green activity LED on the Pi flickers for a couple of minutes and
then mostly settles. You'll confirm it's really up in the next step, from your Mac.

---

## Step 3 — Find the Pi from your Mac

🖥 **On your Mac:**

```bash
ping -c 3 audiopi.local
```

**✅ Check:** you see replies with an IP address like `192.168.1.42`.

> **No reply?** Wait another minute and try again — the Pi may still be booting. If
> it still fails, the Pi did not join your Wi-Fi. Double-check the network name and
> password (they are case sensitive), or plug in an Ethernet cable, which always
> works.

---

## Step 4 — Log in

🖥 **On your Mac:**

```bash
ssh pi@audiopi.local
```

Type `yes` when asked about authenticity (this happens once), then your password.

**✅ Check:** your prompt changes to `pi@audiopi:~ $`. **You are now on the Pi.**
Everything marked 🍓 below is typed here.

> To leave the Pi and return to your Mac, type `exit`.

---

## Step 5 — Set up passwordless login

This is optional but makes everything afterwards one command instead of typing your
password constantly.

🖥 **On your Mac** (type `exit` first if you are still on the Pi):

```bash
# Only if you have never made an SSH key before - press Enter at every prompt:
ls ~/.ssh/id_ed25519.pub 2>/dev/null || ssh-keygen -t ed25519

# Copy it to the Pi (asks for your Pi password one last time):
ssh-copy-id pi@audiopi.local
```

**✅ Check:** `ssh pi@audiopi.local` now logs you straight in with no password.

---

## Step 6 — Get the code onto the Pi

Two ways. **Option A is simpler** — start there.

### Option A: download it on the Pi (recommended)

🍓 **On the Pi:**

```bash
sudo apt-get update && sudo apt-get install -y git
git clone --branch claude/audio-tools-hardware-workstation-yf7rbw \
    https://github.com/rhzuccarelli/Bridge.git audio-workstation
cd audio-workstation
```

> If the repository is private, GitHub will ask for a username and password —
> and password login no longer works. Either make the repo public, or use
> Option B, which needs no GitHub access from the Pi at all.

### Option B: copy from your Mac

Use this when you are editing code on your Mac and want to push changes over.

🖥 **On your Mac**, from wherever you cloned the repository:

```bash
./scripts/deploy-from-mac.sh pi@audiopi.local
```

That copies your working tree to `~/audio-workstation` on the Pi and builds it. It
skips `.git/` and any build directories, so a Mac build never contaminates the Pi.

**✅ Check:** 🍓 `ls ~/audio-workstation` shows `CMakeLists.txt`, `src`, `docs`.

---

## Step 7 — Install the build tools

🍓 **On the Pi:**

```bash
cd ~/audio-workstation
chmod +x scripts/*.sh
./scripts/setup-pi.sh
```

This takes a few minutes. It installs the compiler, CMake and Ninja; the libraries
later phases need; some diagnostic tools; and it adds you to the `video`, `input`,
`gpio` and `audio` groups.

At the end it prints a hardware check and possibly:

```
! You were added to new groups. LOG OUT AND BACK IN before running the app
```

If you see that, do exactly what it says:

```bash
exit                        # back to your Mac
ssh pi@audiopi.local        # and back in
cd ~/audio-workstation
```

> Group membership only takes effect on a new login. Skipping this is the #1
> reason the display "does not work" afterwards.

**✅ Check:** `groups` lists `video` and `input`.

---

## Step 8 — Build it

🍓 **On the Pi:**

```bash
./scripts/build-pi.sh
```

The Pi 5 compiles the whole project in well under a minute.

**✅ Check:** it ends with

```
100% tests passed, 0 tests failed out of 2
==> Built: build/src/app/audio-workstation
```

🎉 **The software is now running on your Raspberry Pi.**

---

## Step 9 — Run it in the terminal

You haven't wired the OLED yet — that's next — so run it in your SSH window, which
needs no hardware at all:

🍓 **On the Pi:**

```bash
./scripts/run-pi.sh --terminal
```

**✅ Check:** the log says `development platform ready: terminal preview ...`, and
you can drive everything from your Mac's keyboard:

| Key | | Key | |
| --- | --- | --- | --- |
| `space` | Play | `1`–`6` | Track 1–6 |
| `s` | Stop | `Enter` | Encoder push |
| `r` | Record | `←` `→` `↑` `↓` | Encoder turn |
| `i` / `e` | Import / Export | `b` or `Esc` | Back |
| `u` / `w` | Undo / Save | `q` | Quit |

The terminal preview renders the same interface as the hardware display. This is a
useful first check for the Home, Projects, Tracks, Playback and Settings screens.

Press `q` to quit. Now let's give it a real screen.

---

## Step 10 — Wire up the OLED and the encoder

**Unplug the Pi's power first.** Never move jumper wires on a powered Pi.

Because the OLED is wired with jumpers instead of sitting on the header, both it
and the encoder fit comfortably. Here's the whole picture.

### Finding pins on the header

The 40 pins are two rows of 20. **Odd** numbers (1, 3, 5…) are the row nearest the
edge of the board; **even** numbers (2, 4, 6…) are the inner row. Pin 1 is the
corner nearest the microSD/USB-C end — it usually has a square solder pad on the
underside.

### The OLED (7 wires)

| OLED pin | Pi pin | What it is |
| --- | --- | --- |
| `GND` | pin 20 | ground |
| `VCC` | pin 17 | **3.3 V** |
| `SCK` | pin 23 | SPI clock |
| `SDA` | pin 19 | SPI data |
| `RES` | pin 22 | reset |
| `DC`  | pin 18 | data/command |
| `CS`  | pin 24 | chip select |

### The encoder (5 wires)

| KY-040 pin | Pi pin | What it is |
| --- | --- | --- |
| `+`   | pin 1 | **3.3 V** |
| `GND` | pin 9 | ground |
| `CLK` | pin 11 | GPIO 17 |
| `DT`  | pin 13 | GPIO 27 |
| `SW`  | pin 15 | GPIO 22 |

> ⚠️ **Both `VCC` and `+` go to 3.3 V pins (17 and 1), never 5 V.** The Pi's GPIO
> and these modules are 3.3 V; 5 V can permanently damage the board. Pins 2 and 4
> are the 5 V pins — keep clear of them.

None of these pins overlap, so you can wire both modules and power up.

### Enable SPI (the OLED needs it)

Power back up, SSH in, and turn on the SPI bus the OLED talks over:

```bash
sudo raspi-config nonint do_spi 0
sudo reboot
```

After it reboots, SSH back in and confirm:

```bash
ls /dev/spidev0.0        # should exist
```

### Prove the OLED works

🍓 There's a dedicated bring-up tool that draws a test pattern — no UI or font
needed:

```bash
cd ~/audio-workstation
./build/src/platform/aw-oled-test
```

**✅ Check:** the OLED lights up with a **border, a diagonal line, and a small
filled square in the top-left corner**. Press `Ctrl-C` to exit.

| What you see | What it means |
| --- | --- |
| The pattern above | 🎉 Wiring, power and controller all correct. |
| Nothing at all | Check `VCC`/`GND`, re-seat the jumpers, confirm SPI is enabled and you're in the `spi`/`gpio` groups (Step 7). |
| Image shifted right or split in two | Your chip is an **SH1106**, not SSD1306. Run `./build/src/platform/aw-oled-test sh1106`. |
| Upside down or mirrored | Run `AW_OLED_ROTATE=180 ./build/src/platform/aw-oled-test`. |

**Write down** whether you needed `sh1106` and/or `180` — you'll pass the matching
options to the app.

### Prove the encoder works

The encoder's own driver is Phase 6, but you can confirm the wiring now with the
command-line tools. `+` is on a real 3.3 V pin, so there's no power trick — just
watch the lines while you turn the knob:

```bash
gpiomon --falling-edge gpiochip0 17     # CLK — turning the knob prints a line per click
```

Press `Ctrl-C`, then repeat for `27` (DT, also turning) and `22` (SW, responds to
*pressing* the knob).

**✅ Check:** all three print something. If so, Phase 6 will just work.

> **Which gpiochip?** On a Pi 5 the header is `gpiochip0` on current kernels but
> `gpiochip4` on older ones. Run `gpiodetect` and use whichever reports **54
> lines**; substitute it in the commands above and pass `AW_GPIO_CHIP=gpiochipN`
> to the app.
>
> **`gpiomon: invalid option`?** You have libgpiod v2, which changed the syntax:
> `gpiomon -e falling gpiochip0 17`.

### Run the app on the OLED

🍓 Now run for real, on the little screen:

```bash
./scripts/run-pi.sh --oled
```

Add the options you noted, if any:

```bash
AW_OLED_CHIP=sh1106 AW_OLED_ROTATE=180 ./scripts/run-pi.sh --oled
```

**✅ Check:** the log says `development platform ready: OLED 128x64 ...` and the OLED
shows the Home screen with readable text and a highlighted `PROJECTS` row.

For the TP-7 monitoring path used by this prototype, launch with its ALSA card too:

```bash
AW_ALSA_DEVICE=hw:2,0 ./scripts/run-pi.sh --oled
```

In Tracks, a short encoder-push cycles `GAIN`, `PAN`, and `POS`. Track keys `1`–`6`
select the six stereo lanes directly; there is no bank mode.

---

## Step 11 — Enable the touchscreen (only if using the HDMI LCD)

**Skip this if you're using the OLED** — it has no touchscreen, and you're done at
Step 10. This step is only for the alternative 3.5" HDMI LCD with XPT2046 touch.

The XPT2046 touch panel needs a kernel driver switched on.

🍓 **On the Pi:**

```bash
sudo nano /boot/firmware/config.txt
```

Add these two lines at the very bottom:

```ini
dtparam=spi=on
dtoverlay=ads7846,cs=1,penirq=25,speed=50000,penirq_pull=2,xohms=150
```

Save with `Ctrl+O`, `Enter`, then exit with `Ctrl+X`. Reboot:

```bash
sudo reboot
```

Wait a minute, SSH back in, and check:

```bash
ls /dev/input/event*
sudo evtest          # pick your device, touch the screen, then Ctrl-C
```

**✅ Check:** `evtest` prints events as you touch the screen.

> `penirq=25` is the common value but it varies by panel. If nothing appears, check
> your LCD's own documentation for the interrupt pin — this is the one number this
> guide cannot know for you.

Now `./scripts/run-pi.sh` will let you tap the boxes along the bottom of the screen
as buttons.

---

## Step 12 — Start automatically at boot (optional)

Makes the Pi behave like an appliance: power on, the app is running.

🍓 **On the Pi:**

```bash
sudo cp scripts/audio-workstation.service /etc/systemd/system/
sudo sed -i "s/%USER%/$USER/g; s|%DIR%|$HOME/audio-workstation|g" \
    /etc/systemd/system/audio-workstation.service
sudo systemctl daemon-reload
sudo systemctl enable --now audio-workstation
```

Watch the log:

```bash
journalctl -u audio-workstation -f      # Ctrl-C to stop watching
```

To stop it (you must do this before running the app by hand — otherwise both fight
over the screen):

```bash
sudo systemctl stop audio-workstation
```

To turn off autostart entirely:

```bash
sudo systemctl disable audio-workstation
```

---

## Your everyday workflow from now on

Once set up, editing on your Mac and testing on the Pi is one command.

🖥 **On your Mac**, from the repository folder:

```bash
./scripts/deploy-from-mac.sh --run
```

That copies your changes, rebuilds, runs the tests, and launches the app — all on the
Pi, with the output in your Mac's terminal.

Set the target once in your shell so you can drop the address:

```bash
echo 'export AW_PI=pi@audiopi.local' >> ~/.zshrc
source ~/.zshrc
```

To just deploy without running:

```bash
./scripts/deploy-from-mac.sh
```

---

## Troubleshooting

**The OLED stays black.**
Run the bring-up tool (`./build/src/platform/aw-oled-test`) — it's the fastest way
to isolate the problem. Nothing at all usually means power (`VCC` to pin 17,
`GND` to pin 20) or a loose jumper; re-seat all seven. Confirm SPI is on
(`ls /dev/spidev0.0`) and you're in the `spi` and `gpio` groups (`groups`). A
shifted or split image means it's an SH1106 — add `sh1106` to the command.

**`ssh: Could not resolve hostname audiopi.local`**
The Pi is not on the network. Wait two minutes after powering on. Check the Wi-Fi
name/password you typed in Imager (case sensitive), or use Ethernet. You can also
look for the Pi's IP in your router's device list and use that directly:
`ssh pi@192.168.1.42`.

**`Permission denied (publickey,password)`**
Wrong username or password. The username is whatever you set in Imager, not
necessarily `pi`. If you are stuck, reflash the card — it takes 5 minutes.

**The app says `development platform ready: headless` when you asked for `--oled`.**
It couldn't open the OLED. Confirm SPI is enabled (`ls /dev/spidev0.0`), that the
bring-up tool works (`aw-oled-test`), and that you're in the `spi` and `gpio`
groups (`groups`). If groups are missing, run `./scripts/setup-pi.sh` again and
**log out and back in**. Meanwhile `--terminal` always works over SSH.

**`no /dev/input/event*` or touch does nothing (HDMI LCD only).**
Step 11 not done, or `penirq` is wrong for your panel. The OLED has no touch — this
only applies to the HDMI LCD. Everything works fine without it — use `--terminal`
and the keyboard, or the encoder.

**Random freezes, reboots, or a lightning-bolt icon on screen.**
Underpowered. Use a genuine 27 W USB-C supply. Confirm with:

```bash
vcgencmd get_throttled     # 0x0 is healthy; anything else means power problems
```

**`cannot ssh to ... without a password` when deploying.**
Run Step 5 (`ssh-copy-id`).

**The build fails with `command not found: cmake`.**
Step 7 did not finish. Run `./scripts/setup-pi.sh` again and read the output.

**I want to start completely over.**
Reflash the microSD card from Step 1. Nothing here is precious.

---

## What is next

Phases 1–7 are complete. Phase 8 adds explicit TP-7 MTP browsing and transfers; its
real-device acceptance procedure is in [phase8-tp7-test.md](phase8-tp7-test.md).

See [roadmap.md](roadmap.md).

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
| **micro-HDMI → HDMI cable or adapter** | ⚠️ The Pi 5 has two *micro*-HDMI ports, not full size. Your 3.5" LCD has a full-size plug. This adapter is the single most commonly forgotten part. |
| Your 3.5" HDMI LCD | |
| KY-040 rotary encoder + 5 jumper wires | Female-to-female, to go from the module to the Pi's pins. |
| Wi-Fi network name and password | Or an Ethernet cable, which is simpler. |

You do **not** need a keyboard or a monitor for the Pi. Everything is done from your
Mac over the network.

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
2. Connect the LCD to the Pi's **micro-HDMI port nearest the USB-C power socket**
   (that one is `HDMI-A-1`, which the software expects).
3. If your LCD needs USB power, plug its USB lead into one of the Pi's USB ports.
4. *Leave the encoder for now* — Step 10.
5. Plug in the USB-C power supply.

The Pi has no power switch; it starts when power is connected. The green LED will
flicker as it reads the card.

**First boot takes 2–3 minutes** — it resizes the filesystem and reboots itself once.
Be patient.

**✅ Check:** the LCD shows text scrolling past, ending at a `audiopi login:` prompt.
If the screen stays black, see [Troubleshooting](#troubleshooting).

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

## Step 9 — Run it

🍓 **On the Pi:**

```bash
./scripts/run-pi.sh
```

**✅ Check:** the log says `development platform ready: framebuffer /dev/fb0 ...`
and the LCD goes dark with a row of outlined boxes along the bottom — the virtual
buttons. Press `q` or `Ctrl-C` to quit.

> The screens are intentionally blank: drawing them is Phase 7. What you are
> confirming now is that the display, input and audio plumbing all work.

### If you would rather see it in your SSH window

```bash
./scripts/run-pi.sh --terminal
```

This renders the *real* framebuffer as coloured blocks in your terminal, and you can
drive everything from your Mac's keyboard:

| Key | | Key | |
| --- | --- | --- | --- |
| `space` | Play | `1`–`4` | Track 1–4 |
| `s` | Stop | `Enter` | Encoder push |
| `r` | Record | `←` `→` `↑` `↓` | Encoder turn |
| `i` / `e` | Import / Export | `b` or `Esc` | Back |
| `u` / `w` | Undo / Save | `q` | Quit |

### Checking the screen resolution

🍓 If the LCD image looks wrong:

```bash
fbset -s | grep geometry
```

You want `480 320`. If it differs, see the display section of
[hardware.md](hardware.md) — the fix is one line in `/boot/firmware/cmdline.txt`.

> ⚠️ Ignore any guide telling you to set `hdmi_group` / `hdmi_mode` / `hdmi_cvt` in
> `config.txt`. Those do nothing on a Pi 5.

---

## Step 10 — Wire up the rotary encoder

**Unplug the Pi's power first.**

Holding the Pi with the USB ports facing you, the 40 pins run along the top edge.
Pin 1 is the **top-left** pin.

| KY-040 pin | Connect to Pi | Which pin is that |
| --- | --- | --- |
| `+` | **3.3 V** | pin 1 — top-left corner |
| `GND` | Ground | pin 6 — 3rd from left, bottom row |
| `CLK` | GPIO 17 | pin 11 — 6th from left, top row |
| `DT` | GPIO 27 | pin 13 — 7th from left, top row |
| `SW` | GPIO 22 | pin 15 — 8th from left, top row |

> ⚠️ **`+` goes to 3.3 V, not 5 V.** The Pi's pins are not 5 V tolerant, and a
> KY-040 powered from 5 V will feed 5 V back into GPIO 17/27/22 and can permanently
> damage the Pi. Pin 1 is 3.3 V; pin 2 (directly below it) is 5 V — do not confuse
> them.

Power back up, then 🍓:

```bash
gpiodetect
```

Note which chip reports **54 lines** — that is the 40-pin header. On a Pi 5 it may be
`gpiochip0` or `gpiochip4` depending on your kernel version. If it is not
`gpiochip0`, run the app like this:

```bash
AW_GPIO_CHIP=gpiochip4 ./scripts/run-pi.sh
```

> The encoder does not do anything yet — its driver is Phase 6. This step just gets
> it wired and confirms which chip name to use.

---

## Step 11 — Enable the touchscreen (optional)

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

**The LCD stays black.**
Check the micro-HDMI is in the port *nearest the USB-C socket*. Confirm the Pi is
actually running: `ping audiopi.local` from your Mac. If ping works but the screen is
black, the panel is not being detected — try the other micro-HDMI port and reboot.

**`ssh: Could not resolve hostname audiopi.local`**
The Pi is not on the network. Wait two minutes after powering on. Check the Wi-Fi
name/password you typed in Imager (case sensitive), or use Ethernet. You can also
look for the Pi's IP in your router's device list and use that directly:
`ssh pi@192.168.1.42`.

**`Permission denied (publickey,password)`**
Wrong username or password. The username is whatever you set in Imager, not
necessarily `pi`. If you are stuck, reflash the card — it takes 5 minutes.

**The app says `development platform ready: headless`.**
It could not open `/dev/fb0`. Almost always the groups step:

```bash
groups                # do you see 'video'?
```

If not, run `./scripts/setup-pi.sh` again, then **log out and back in**.

**`no /dev/input/event*` or touch does nothing.**
Step 11 not done, or `penirq` is wrong for your panel. Everything else works fine
without touch — use `--terminal` and the keyboard meanwhile.

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

Phase 1 is plumbing: the display, input, event and audio paths all work end to end,
but the screens are deliberately blank and no audio is produced yet.

- **Phase 2** adds WAV reading and writing.
- **Phase 5** makes the audio actually come out of the headphone socket.
- **Phase 6** brings the encoder to life.
- **Phase 7** draws the interface.

See [roadmap.md](roadmap.md).

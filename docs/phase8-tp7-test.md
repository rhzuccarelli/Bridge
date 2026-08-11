# Phase 8 — real TP-7 transfer test

This is the acceptance test for Bridge import and export. Use a short, non-critical
recording first. Import never changes the TP-7 source. Export creates a new file and
only deletes an existing same-name file after the OLED shows the overwrite warning and
you explicitly press Export again.

## Important USB-mode limitation

The TP-7 exposes MTP storage and USB audio as different operating modes, so monitoring
is briefly unavailable during a transfer. Opening Import now requests MTP mode over
USB MIDI. Finishing or leaving Import releases the MTP session, which makes the TP-7
return to USB audio automatically. Stored project audio remains 48 kHz.

## 1. Deploy and install the permission rule

On the Mac:

```bash
cd "/Users/ricardo/Documents/ChatGPT/Bridge"
./scripts/deploy-from-mac.sh pi@audiopi.local
```

On the Pi:

```bash
cd ~/audio-workstation
sudo install -m 0644 scripts/99-audio-workstation-tp7.rules \
  /etc/udev/rules.d/99-audio-workstation-tp7.rules
sudo udevadm control --reload-rules
```

Connect the TP-7 normally in USB audio/MIDI mode. Do not run `mtp-filetree`, `mtp-files`,
or another MTP program while Bridge is running; only one process can own the session.

## 2. Import a multichannel recording

Run Bridge in an interactive SSH session so the keyboard controls remain available:

```bash
ssh -t pi@audiopi.local \
  'cd ~/audio-workstation && AW_GPIO_CHIP=gpiochip0 ./scripts/run-pi.sh --oled'
```

1. Open Projects and create a uniquely named test project.
2. Press `i`. The OLED should show `STARTING MTP`, `WAITING FOR TP-7`, then the folders.
3. Open `recordings`, highlight a short WAV, and push the encoder. `X` marks it.
4. Press `i` again. The progress bar must reach the end; the TP-7 must then reconnect
   automatically as USB audio.
5. Press `w` to save the project, then `q` to exit.

Check the local copy on the Pi:

```bash
find ~/audio-projects -path '*/audio/*.wav' -printf '%p\n'
ffprobe -v error -show_entries stream=sample_rate,channels,bits_per_sample \
  -of default=noprint_wrappers=1 "$(find ~/audio-projects -path '*/audio/*.wav' | tail -1)"
```

For the known TP-7 test file, the expected result is 48,000 Hz, 24-bit and 12 channels.
Files with fewer recorded pairs legitimately report fewer channels.

## 3. Listen through the TP-7

Confirm the TP-7 returned automatically with `aplay -l`, then continue using Bridge.
If starting it manually for diagnosis, the stable card name avoids card-number changes:

```bash
AW_ALSA_DEVICE=hw:TP7,0 AW_GPIO_CHIP=gpiochip0 ./scripts/run-pi.sh --oled
```

Open the saved project and press Play. All six lanes remain synchronized; monitoring
is converted only in the ALSA backend to the TP-7's 96 kHz, six-channel USB endpoint.

## 4. Export back to the TP-7

Quit Bridge, return the TP-7 to MTP mode, reconnect it, and start Bridge again without
an ALSA device override. Open the saved test project and press `e`, then press `e`
again on the Export screen. The OLED shows rendering for the first half of progress
and sending for the second half.

The exported file appears in the TP-7 `recordings` folder as `<project name>.wav`.
If that exact name already exists, Bridge stops at `FILE EXISTS`; Back cancels without
deleting anything, while a second Export press explicitly authorizes replacement.

Phase 8 is accepted when the imported file plays after reconnecting in USB-audio mode
and the exported file appears and opens on the TP-7.

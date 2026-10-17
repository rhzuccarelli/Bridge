# TP-7 output bench test (port step P3)

Plays a 660 Hz sine at −18 dBFS from a 48 kHz stereo render callback through
the real `Tp7AudioOutput` (via `createUsbBackends()`, exactly as the bridge
firmware builds it), and checks the result objectively by recording it on the
TP-7. One cycle takes about 35 s:

| Time | Action |
| --- | --- |
| 0 s | open and start the output once a TP-7 is in USB-audio mode (`LAB run N`) |
| 6 s | CC 14 = 127 (arms recording) |
| 9 s / 14 s | MIDI Start / Stop |
| 15 s | CC 14 = 0 |
| 20 s | switch the TP-7 to MTP while streaming; the output must see it leave |
| then | send the newest dated take (≤ 4 MiB) as base64 between `LABTAKE` and `LABEND`, with a CRC-32; closing the session returns the TP-7 to audio |

The TP-7's recording input must be set to USB by hand. A `TP7 …` line each
second gives the stream's counters (packets, underruns, FIFO level, feedback
rate, empty feedback, device resets).

```bash
. ~/esp/esp-idf/export.sh
cd firmware/esp32p4/tp7-output-test
idf.py -B ~/esp/builds/tp7-output-test build
python ../tp7-audio-probe/tools/wait_safe.py /dev/cu.usbmodem… 90   # never cut an MTP transfer
idf.py -B ~/esp/builds/tp7-output-test -p /dev/cu.usbmodem… flash
mkdir -p takes && (cd ../tp7-audio-probe/tools && python collect.py /dev/cu.usbmodem… 160 ../../tp7-output-test/takes)
```

The console runs at 2 Mbit/s. `collect.py` saves `runN_variant1.wav`, checks
each CRC and prints the analysis: tone present per 1 ms block, RMS (a clean
−18 dBFS sine is −21.0 dB), and glitch samples.

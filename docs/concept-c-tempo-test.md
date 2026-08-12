# Concept C and TP-7 tempo test

## Launch

```bash
cd ~/audio-workstation
AW_INPUT=none AW_GPIO_CHIP=gpiochip0 AW_ALSA_DEVICE=hw:2,0 \
  ./scripts/run-pi.sh --oled
```

The OLED home screen is now action-first. Turn the encoder to choose an action,
release a short press to open or confirm it, and hold for one second to go back
without activating the current action.

The actions are Play/Stop, Tracks, Copy Channel, Change Deck, Import TP-7,
Export TP-7, and Projects. Gain, pan, and position modes are removed.

## Tempo round trip

`Q.WAV` contains a standard 24-byte `acid` RIFF chunk. Its final four bytes are
the little-endian float `110.0` BPM. Bridge retains all 24 bytes, not only the
displayed number.

1. Connect the TP-7 in MTP mode and import `Q.WAV`.
2. Reconnect the TP-7 in USB-audio mode if you want to monitor it.
3. Return to MTP mode and export the imported deck.
4. Copy the exported WAV back to a computer and inspect it:

```bash
python3 - exported.wav <<'PY'
import struct, sys
p = open(sys.argv[1], 'rb').read()
i = p.index(b'acid')
size = struct.unpack_from('<I', p, i + 4)[0]
tempo = struct.unpack_from('<f', p, i + 8 + 20)[0]
print('acid size:', size)
print('tempo:', tempo, 'BPM')
PY
```

Expected result:

```text
acid size: 24
tempo: 110.0 BPM
```

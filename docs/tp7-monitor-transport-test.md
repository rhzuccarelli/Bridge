# TP-7 monitoring and transport acceptance

1. Start Bridge while the TP-7 is in MTP mode. The terminal may report that the
   silent timing output is active.
2. Import a 12-channel TP-7 WAV. It becomes six synchronized stereo tracks.
3. Return the TP-7 to USB-audio mode without restarting Bridge.
4. Within two seconds the terminal should report:

   ```text
   primary output connected; monitoring is now active
   ```

5. Choose **Transport** on the OLED. Push toggles Play/Stop; turn scrubs by one
   second per detent. The elapsed position and playhead must move.
6. Listen through the TP-7. All six project tracks must be audible in the stereo
   master on TP-7 USB playback channels 1 and 2. USB channels 3–6 remain silent.
7. Hold the encoder. Home must appear immediately, without turning the encoder.

The source remains 12-channel/48 kHz in project storage and export. Only the
monitor backend produces the 96 kHz six-channel USB stream required by the TP-7.

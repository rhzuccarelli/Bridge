# Test a cross-deck channel copy

This example copies **Deck A / Track 5 / Left** into
**Deck C / Track 2 / Right**. The operation references the existing local WAV;
it does not duplicate or rewrite the audio file.

1. Open the project and make Deck C the active playback deck.
2. Open **Tracks**.
3. Hold the encoder for one second to open **COPY CHANNEL**.
4. Turn the encoder to choose a value and push it to advance through:
   - Source deck: `Deck A`
   - Source track: `Track 5`
   - Source side: `Left`
   - Destination deck: `Deck C`
   - Destination track: `Track 2`
   - Destination side: `Right`
5. At **APPLY COPY**, push the encoder.
6. Play Deck C. Track 2's left side should be unchanged; its right side should
   now follow Deck A Track 5's left channel at the same timeline positions.
7. Press **Undo** once. Track 2 and the previously active deck should return to
   their exact state before the copy.

Save only after the result is correct. Export still produces a standard TP-7
48 kHz multichannel WAV; the side-specific route is rendered into the selected
output channel.

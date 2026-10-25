# Storage bench

Times where an import's time goes, using the bridge firmware's own backends
(`FatFsStorage`, `createUsbBackends()`):

1. SD writes through `FatFsStorage` at 64, 8 and 1 KiB chunks, and raw POSIX
   writes from an aligned internal-RAM buffer (the card's ceiling);
2. SD reads at 64 and 512 KiB chunks;
3. an MTP read of the TP-7's largest recording under 200 MiB into memory
   (switching the TP-7 to MTP if it is in audio mode);
4. the import path: the same read written straight to an SD file, split into
   time waiting on MTP and time writing the card, then read back and
   compared by CRC-32;
5. the export's read path: the largest project WAV read in 2048-frame
   blocks, raw and through `WavReader`, then the 24-bit conversion alone and
   at 256, 512 and 2048 frames with its buffers in PSRAM or internal RAM.

```bash
. ~/esp/esp-idf/export.sh
cd firmware/esp32p4/storage-bench
idf.py -B ~/esp/builds/storage-bench build
idf.py -B ~/esp/builds/storage-bench -p /dev/cu.usbmodem… flash monitor   # lines start with BENCH
```

It writes and deletes `/sdcard/bench.bin` and `/sdcard/import-bench.wav`;
nothing else on the card is touched. Results are in docs/esp32p4-port.md
("Storage and import speed").

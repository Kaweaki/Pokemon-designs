# One-image release

`pokedex-gen5-full.bin` is the complete flash image for a **LilyGo T-Display S3**
(ESP32-S3, 16 MB flash, 320×170 ST7789). It contains the bootloader, partition
table, `boot_app0` and the application, so it is written at **offset `0x0`**, not
at `0x10000`.

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 pokedex-gen5-full.bin
```

On Windows the port is usually `COM5` (check Device Manager); on macOS
`/dev/cu.usbmodem*`. The board must be in download mode if it does not reset
itself: hold **BOOT**, tap **RST**, release **BOOT**, then flash.

| | |
|---|---|
| Size | 3,676,800 bytes |
| SHA-256 | `12a1ce1c31b5b209b16186ea892528b4aaa9fdbc44d16c3482327f66268e2c38` |
| Flash layout | `rainmaker_4MB` |
| PSRAM | disabled (deliberate; see the main README) |

After flashing, the board starts on entry 1 and gives every entry six seconds of
on-screen action before switching to the next world. It shows all 649 Gen 5
Pokémon, each with its own signature move fired at the opposing Ditto, on painted
EBDX battle backgrounds. Ditto is the real archive sprite and has 100,000 HP,
spent over all 649 entry-hits; after the final hit it faints and a fresh Ditto
drops in for the next pass. The image contains ESP RainMaker BLE provisioning:
use the RainMaker app with **PROV_POKEDEX** / **`pokedex123`**, then the
**Pokedex** node's Index slider and Paused toggle. Serial commands are listed in
the main README.

Verify the image before writing:

```bash
sha256sum pokedex-gen5-full.bin      # Linux
shasum -a 256 pokedex-gen5-full.bin  # macOS
```

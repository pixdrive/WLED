# PixDrive Usermod

Plays pre-recorded `.wled` binary animation files from an SD card. Designed for the [PixDrive](https://pixdrive.studio) hardware platform.

## Features

- Playback of `.wled` animation files from SD card
- RGB and RGBW LED strip support
- CRC32 integrity validation on file upload
- Per-segment playback with independent file selection
- Built-in web UI for file management (upload, delete, playback)
- Configurable FPS per animation file

## Installation (WLED 16)

The PixDrive firmware configuration lives in [`platformio_override.ini.pixdrive`](platformio_override.ini.pixdrive). Copy it to the repository root and build:

```bash
cp usermods/pixdrive/platformio_override.ini.pixdrive platformio_override.ini
pio run -e pixdrive_esp32
```

The CI release workflow uses the same file.

- `custom_usermods = sd_card pixdrive`: PixDrive needs the `sd_card` usermod; no other usermods are included.
- `WLED_USE_SD_SPI` (or `WLED_USE_SD_MMC`) selects how the SD card is connected.
- `SD_SPI_PIN_*` set the factory default SD pins (CS=33, SCK=27, MISO=26, MOSI=25). They can still be changed under *Config → Usermods → SD Card*.
- Lean build: Alexa, Hue sync, infrared, MQTT, Loxone, Adalight/serial protocols, the GIF image effect and DMX input are disabled. ESP-NOW (remote control), OTA updates and the web UI stay enabled.

## Playback

- Select the effect **PixDrive** and set the segment name to the file path, e.g. `/pixdrive/show.wled`.
- Playback always runs at the FPS stored in the file (no speed slider).
- **Brightness** (intensity slider) scales the file colours.
- **Once**: stop on the last frame instead of looping.
- Playback is time-based: frames are skipped if the strip frame rate (*Config → LED Preferences → Target FPS*) is lower than the file FPS.
- If a file cannot be opened, the segment shows dim red and the file is retried every second.
- Up to `PIXDRIVE_MAX_OPEN` (default 3) files can be open at the same time. A segment in a transition between two PixDrive presets briefly needs two.

## .wled File Format

See [wled_file_protocol.txt](wled_file_protocol.txt) for the full binary protocol specification.

**Header (20 bytes):**

| Offset | Bytes | Type       | Field       |
|--------|-------|------------|-------------|
| 0-3    | 4     | char[4]    | Magic "WLED"|
| 4      | 1     | uint8_t    | FPS         |
| 5      | 1     | uint8_t    | Flags       |
| 6-7    | 2     | uint16_t   | LED Count   |
| 8-11   | 4     | uint32_t   | Frame Count |
| 12-15  | 4     | uint32_t   | CRC32       |
| 16-19  | 4     | uint8_t[4] | Reserved    |

Frame data follows immediately after the header: `frames × leds × bpp` bytes (bpp = 3 for RGB, 4 for RGBW).

## API Endpoints

| Method | Path                | Description                                   |
|--------|---------------------|-----------------------------------------------|
| GET    | `/pixdrive`         | Web UI                                        |
| GET    | `/pixdrive/list`    | List files and SD usage as JSON               |
| POST   | `/pixdrive/upload`  | Upload a .wled file (multipart, max. 54 chars name) |
| POST   | `/pixdrive/delete`  | Delete file (`?file=/pixdrive/name.wled`)     |

Uploads are validated (header, file size, CRC32) while streaming and only replace an existing file after validation succeeded.

## Test File Generator

Use `generate_test.py` to create test animation files:

```bash
python3 generate_test.py -o rainbow.wled --leds 30 --frames 120 --fps 30
python3 generate_test.py -o rainbow_rgbw.wled --leds 30 --frames 120 --fps 30 --rgbw
```

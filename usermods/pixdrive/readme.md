# PixDrive Usermod

Plays pre-recorded `.wled` binary animation files from an SD card. Designed for the [PixDrive](https://pixdrive.studio) hardware platform.

## Features

- Playback of `.wled` animation files from SD card
- RGB and RGBW LED strip support
- CRC32 integrity validation on file upload
- Per-segment playback with independent file selection
- Built-in web UI for file management (upload, delete, playback)
- Configurable FPS per animation file

## Installation

Add the following build flags to your `platformio_override.ini`:

```ini
[env:my_pixdrive]
extends = env:esp32dev
build_flags = ${env:esp32dev.build_flags}
  -D WLED_USE_SD_SPI
  -D USERMOD_PIXDRIVE
```

### Dependencies

- Requires the **SD Card** usermod (`WLED_USE_SD_SPI` or `WLED_USE_SD_MMC`)
- SD card SPI default pins: CS=33, SCK=27, MISO=26, MOSI=25 (configurable in WLED settings)

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

| Method | Path                | Description              |
|--------|---------------------|--------------------------|
| GET    | `/pixdrive`         | Web UI                   |
| GET    | `/pixdrive/list`    | List files as JSON       |
| POST   | `/pixdrive/upload`  | Upload .wled file        |
| DELETE | `/pixdrive/delete`  | Delete file (?file=name) |
| GET    | `/pixdrive/play`    | Start playback (?file=name&seg=0) |
| GET    | `/pixdrive/stop`    | Stop playback (?seg=0)   |

## Test File Generator

Use `generate_test.py` to create test animation files:

```bash
python3 generate_test.py -o rainbow.wled --leds 30 --frames 120 --fps 30
python3 generate_test.py -o rainbow_rgbw.wled --leds 30 --frames 120 --fps 30 --rgbw
```

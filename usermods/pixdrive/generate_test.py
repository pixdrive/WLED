#!/usr/bin/env python3
"""Generate test .wled animation files (20-byte header format)."""

import struct
import zlib
import argparse
import colorsys

def generate_wled(filename, led_count=30, frame_count=120, fps=30, rgbw=False):
    bpp = 4 if rgbw else 3
    flags = 0x01 if rgbw else 0x00

    # Generate rainbow animation frames
    frames = bytearray()
    for frame in range(frame_count):
        for led in range(led_count):
            hue = ((led / led_count) + (frame / frame_count)) % 1.0
            r, g, b = colorsys.hsv_to_rgb(hue, 1.0, 1.0)
            frames.append(int(r * 255))
            frames.append(int(g * 255))
            frames.append(int(b * 255))
            if rgbw:
                frames.append(0)  # W channel

    crc = zlib.crc32(frames) & 0xFFFFFFFF

    # 20-byte header: magic(4) + fps(1) + flags(1) + leds(2) + frames(4) + crc32(4) + reserved(4)
    header = struct.pack('<4sBBHII4s',
        b'WLED',
        fps,
        flags,
        led_count,
        frame_count,
        crc,
        b'\x00\x00\x00\x00'
    )
    assert len(header) == 20

    with open(filename, 'wb') as f:
        f.write(header)
        f.write(frames)

    total_size = 20 + len(frames)
    mode = "RGBW" if rgbw else "RGB"
    print(f"Generated: {filename}")
    print(f"  {led_count} LEDs, {frame_count} frames, {fps} FPS, {mode}")
    print(f"  CRC32: 0x{crc:08X}")
    print(f"  Size: {total_size} bytes ({total_size/1024:.1f} KB)")

    # Verify
    with open(filename, 'rb') as f:
        data = f.read(20)
        magic, r_fps, r_flags, r_leds, r_frames, r_crc, _ = struct.unpack('<4sBBHII4s', data)
        assert magic == b'WLED'
        assert r_fps == fps
        assert r_leds == led_count
        assert r_frames == frame_count
        assert r_crc == crc
        frame_data = f.read()
        assert zlib.crc32(frame_data) & 0xFFFFFFFF == crc
    print("  Verification: OK")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate test .wled files')
    parser.add_argument('-o', '--output', default='test_rainbow.wled')
    parser.add_argument('-l', '--leds', type=int, default=30)
    parser.add_argument('-f', '--frames', type=int, default=120)
    parser.add_argument('--fps', type=int, default=30)
    parser.add_argument('--rgbw', action='store_true')
    args = parser.parse_args()
    generate_wled(args.output, args.leds, args.frames, args.fps, args.rgbw)

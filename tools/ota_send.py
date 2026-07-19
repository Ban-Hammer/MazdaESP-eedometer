#!/usr/bin/env python3
"""Bench sender: exercise the RP2040 ota_update module over a USB-TTL adapter,
before (or instead of) the XIAO. Mirrors the XIAO's protocol side.

Usage: ota_send.py /dev/ttyUSB0 firmware.bin
Wiring: adapter TX -> RP2040 GP1(RX), adapter RX -> RP2040 GP0(TX), GND-GND (3.3V logic).
Requires: pip install pyserial
"""
import sys, time, struct, zlib
import serial

MAGIC = b"\xA5\x5A"
ENTER, ACK, HEADER, RESULT, PING, VERSION, NAK = 1, 2, 3, 4, 5, 6, 7
RESULT_NAMES = {0: "OK", 1: "CRC_MISMATCH", 2: "FLASH_ERROR", 3: "TIMEOUT", 4: "BAD_LENGTH"}


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def frame(t, payload=b""):
    body = bytes([t]) + struct.pack("<H", len(payload)) + payload
    return MAGIC + body + struct.pack("<I", crc32(body))


def read_frame(ser, timeout=6.0):
    """Read one CRC-valid frame or return (None, None) on timeout."""
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        i = buf.find(MAGIC)
        if i < 0 or len(buf) - i < 5:
            continue
        ln = struct.unpack("<H", buf[i + 3:i + 5])[0]
        need = i + 5 + ln + 4
        if len(buf) < need:
            continue
        body = buf[i + 2:i + 5 + ln]
        crc = struct.unpack("<I", buf[i + 5 + ln:need])[0]
        if crc32(body) == crc:
            return body[0], bytes(body[3:])
        del buf[:i + 2]
    return None, None


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    port, path = sys.argv[1], sys.argv[2]
    img = open(path, "rb").read()
    ser = serial.Serial(port, 921600, timeout=1)
    time.sleep(0.2)

    ser.write(frame(PING))
    t, _ = read_frame(ser)
    print("PING ->", "ACK" if t == ACK else t)

    ser.write(frame(VERSION))
    t, p = read_frame(ser)
    if t == VERSION:
        print("VERSION ->", p.decode(errors="replace"))

    ser.write(frame(ENTER))
    t, _ = read_frame(ser)
    if t != ACK:
        print(f"FAIL: expected ACK, got {t}")
        sys.exit(2)

    ser.write(frame(HEADER, struct.pack("<II", len(img), crc32(img))))
    print(f"streaming {len(img)} bytes, crc={crc32(img):08X}")
    ser.write(img)
    ser.flush()

    t, p = read_frame(ser, timeout=40)
    code = p[0] if p else None
    print("RESULT ->", RESULT_NAMES.get(code, code))
    sys.exit(0 if code == 0 else 3)


if __name__ == "__main__":
    main()

#!/usr/bin/env python
# BK166X direct-serial prober: dump full receiver config via USRCFG_GET sweep.
#
# Frame: 42 4B <crc_msb crc_lsb> <MTYPE> <STYPE> <LEN_MSB: seq<<4> <LEN_LSB> <payload>
# CRC:   XModem (poly 0x1021, init 0x0000, no reflection) over MTYPE..payload end
#        (verified against manual example 42 4B CC 17 26 04 00 00)
#
# Usage: python bk_probe.py COM3 [baud]
import struct
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def bk_frame(mtype: int, stype: int, payload: bytes = b'', seq: int = 0) -> bytes:
    body = bytes([mtype, stype, (seq & 0x0F) << 4, len(payload) & 0xFF]) + payload
    crc = crc16_xmodem(body)
    return bytes([0x42, 0x4B, (crc >> 8) & 0xFF, crc & 0xFF]) + body


def parse_frames(buf: bytes):
    """Yield (mtype, stype, payload) for every valid BK frame in buf."""
    out = []
    i = 0
    while True:
        i = buf.find(b'BK', i)
        if i < 0 or i + 8 > len(buf):
            break
        mtype, stype, lenb, lenl = buf[i + 4], buf[i + 5], buf[i + 6], buf[i + 7]
        plen = ((lenb & 0x0F) << 8) | lenl
        end = i + 8 + plen
        if end > len(buf):
            i += 1
            continue
        body = buf[i + 4:end]
        crc = (buf[i + 2] << 8) | buf[i + 3]
        if crc16_xmodem(body) == crc:
            out.append((mtype, stype, body[4:]))
            i = end
        else:
            i += 1
    return out


KNOWN = {
    1: 'IO Type', 2: 'RF Band', 3: 'Voltage', 4: 'Baudrate', 5: 'Secondary UartID',
    6: 'Secondary Uart Baudrate', 7: 'DBG MODE', 8: 'Power Supply Mode',
    9: 'Power Manage Level', 10: 'Flash Aiding', 11: 'EXT_LNA_LDO Ctrl',
    12: 'Uart1 IO select', 13: 'I2C IO POS', 14: 'EXT_LNA_LDO Voltage',
    15: 'RTC_PERIOD_WAKEUPEN', 16: 'Working Rate', 17: 'Constellation Signal',
    18: 'Measurement Mode', 19: 'Measurement Output Rate',
    20: 'Measurement Time System', 21: 'Receiver Time System', 22: 'PPS Time System',
    23: 'Min GPS Week', 24: 'Output NMEA Message', 25: 'Output POLARIS Message',
    26: 'Output RTCM MSM Version', 27: 'Output RTCM EPH Time Interval',
    28: 'NMEA Talker ID', 29: 'Version Print', 30: 'Request AGNSS',
    31: 'Enable Long Ephemeris', 32: 'Output RTCM Message', 33: 'ASCII Command',
    34: 'NMEA LL FRAC DIGT', 35: 'NMEA OUTPUT MASK',
}


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    print(f'opened {PORT} @ {BAUD}, draining 1s...')
    time.sleep(1.0)
    ser.reset_input_buffer()

    # 1) plaintext queries for reference
    for cmd in (b'$POLCFGPTVER\r\n', b'$POLCFGPTSYS\r\n'):
        ser.write(cmd)
        time.sleep(0.3)

    # 2) binary GetVersion
    ser.write(bk_frame(0x02, 0x07))
    time.sleep(0.5)

    # 3) USRCFG_GET sweep addr 0..60
    for i, addr in enumerate(range(0, 61)):
        ser.write(bk_frame(0x26, 0x03, struct.pack('<II', 0, addr), seq=i))
        time.sleep(0.06)

    # capture everything for 6 s
    print('capturing 6s...')
    buf = b''
    t0 = time.time()
    while time.time() - t0 < 6.0:
        buf += ser.read(4096)
    ser.close()

    open('bk_rx.bin', 'wb').write(buf)
    nmea_lines = [l for l in buf.split(b'\r\n') if l.startswith(b'$')]
    print(f'rx={len(buf)}B, nmea_lines={len(nmea_lines)}')
    for l in nmea_lines:
        if not (l.startswith(b'$G') or l.startswith(b'$PO')):
            print('  text reply:', l[:60])
    frames = parse_frames(buf)
    print(f'valid BK frames: {len(frames)}')
    for mtype, stype, payload in frames:
        tag = f'mtype=0x{mtype:02X} stype=0x{stype:02X} len={len(payload)}'
        if mtype == 0x27 and stype == 0x03 and len(payload) >= 12:
            _, addr, val = struct.unpack('<III', payload[:12])
            name = KNOWN.get(addr, '?')
            print(f'  CONFIG addr={addr:3d} ({name:28s}) value={val} (0x{val:X})')
        elif mtype == 0x03 and stype == 0x07:
            print(f'  VERSION-ACK: {payload!r}')
        else:
            print(f'  {tag}: {payload[:48]!r}')


if __name__ == '__main__':
    main()

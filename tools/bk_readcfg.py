#!/usr/bin/env python
# BK166X receiver full-config reader (query-only, no SET).
#
# Protocol (recovered from NavStarTool gnss_tools.exe + PDB symbols):
#   READ  : BK frame MTYPE=0x26 STYPE=0x01, empty payload, seq nibble 0/1
#           -> ACK MTYPE=0x27 STYPE=0x01, payload = u32 version + u32 crc32
#              + 4x200B sections (leaves first, pad to section_data_length)
#   Frame : 42 4B <crc16-msb lsb> <mtype> <stype> <lenM:seq<<4> <lenL> <payload>
#           crc = XModem (poly 0x1021, init 0, no reflection) over mtype..payload
#
# Usage: python bk_readcfg.py COM3 [baud] [--json template.json]
import json
import struct
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
TPL = 'E:/Tools/NavStarTool_251208_cust/user_settings/Json_data/CommandConfig_DF_0x55002613.json'

DEFAULT_SECTIONS = ['System Parameters', 'Receiver Parameters',
                    'Navigation Parameters', 'IMU Parameters']


def crc16_xmodem(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if (c & 0x8000) else (c << 1)
        c &= 0xFFFF
    return c


def frame(mtype, stype, payload=b'', seq=0):
    body = bytes([mtype, stype, (seq & 0xF) << 4, len(payload) & 0xFF]) + payload
    return bytes([0x42, 0x4B]) + crc16_xmodem(body).to_bytes(2, 'big') + body


def parse_frames(buf):
    out, i = [], 0
    while True:
        i = buf.find(b'BK', i)
        if i < 0 or i + 8 > len(buf):
            break
        plen = ((buf[i + 6] & 0x0F) << 8) | buf[i + 7]
        end = i + 8 + plen
        if end > len(buf):
            i += 1
            continue
        if crc16_xmodem(buf[i + 4:end]) == (buf[i + 2] << 8) | buf[i + 3]:
            out.append((buf[i + 4], buf[i + 5], buf[i + 8:end]))
            i = end
        else:
            i += 1
    return out


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.write(frame(0x26, 0x01))          # READ firmware settings
    time.sleep(0.5)
    ser.write(frame(0x26, 0x01, seq=1))   # retry with next seq
    buf = b''
    t0 = time.time()
    while time.time() - t0 < 3.0:
        buf += ser.read(8192)
    ser.close()

    frames = parse_frames(buf)
    cfgblobs = [p for m, s, p in frames if m == 0x27 and s == 0x01]
    if not cfgblobs:
        print('no config reply, rx=%d bytes' % len(buf))
        return 1
    blob = cfgblobs[0]
    version, crc32 = struct.unpack('<II', blob[:8])
    cfg = blob[8:]
    print(f'version 0x{version:08X}  crc32 0x{crc32:08X}  config {len(cfg)}B')

    try:
        d = json.load(open(TPL, encoding='utf-8'))
        sections = d['configs'][0]['sections']
    except Exception:
        sections = [{'section_name': n, 'section_data_length': 200, 'leafs': []}
                    for n in DEFAULT_SECTIONS]

    base = 0
    for sec in sections:
        total = int(sec.get('section_data_length', 200))
        print(f"\n===== {sec['section_name']} (base {base}, {total}B) =====")
        off = base
        for leaf in sec.get('leafs', []):
            name = leaf.get('leaf_name', 'reserved')
            dl = int(leaf.get('leaf_data_length', 1))
            if 'reserved' in name.lower():
                off += dl
                continue
            raw = cfg[off:off + dl]
            off += dl
            lt = leaf.get('leaf_type', 'uint')
            if lt == 'enum':
                opts = [o.strip() for o in leaf.get('leaf_value_list', '').split(',')]
                texts = [t.strip() for t in leaf.get('leaf_value_text', '').split(',')]
                val = int.from_bytes(raw, 'little')
                txt = next((texts[k] for k, o in enumerate(opts)
                            if o == str(val) and k < len(texts)), 'raw=%d' % val)
                print(f'  {name:36s} = {val} ({txt})')
            elif lt == 'bitmask':
                print(f'  {name:36s} = 0x{int.from_bytes(raw, "little"):X}')
            else:
                print(f'  {name:36s} = {int.from_bytes(raw, "little")}')
        base += total
    return 0


if __name__ == '__main__':
    sys.exit(main())

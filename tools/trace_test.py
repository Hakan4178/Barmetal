#!/usr/bin/env python3
"""Minimal trace reader - bypasses curses, directly reads /proc/svm_trace"""
import struct, os, time

MAGIC = 0x5356545200000000
ENTRY_FMT = "<QQIIQQQII32Q"
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
count = 0

print(f"[*] Entry size: {ENTRY_SIZE} bytes")
print(f"[*] Opening /proc/svm_trace (blocking mode)...")

with open("/proc/svm_trace", "rb") as f:
    buf = bytearray()
    while count < 20:
        data = f.read(65536)
        if not data:
            time.sleep(0.1)
            continue
        buf.extend(data)

        while len(buf) >= ENTRY_SIZE:
            magic = struct.unpack("<Q", buf[:8])[0]
            if magic != MAGIC:
                print(f"[!] Bad magic: 0x{magic:016x}, resync...")
                del buf[0]
                continue

            rec = struct.unpack(ENTRY_FMT, buf[:ENTRY_SIZE])
            tsc = rec[1]
            ev_type = rec[2]
            lbr_count = rec[3]
            cr3 = rec[4]
            rip = rec[5]
            gpa = rec[6]
            data_size = rec[7]

            count += 1
            print(f"[{count:3d}] type={ev_type} lbr={lbr_count} RIP=0x{rip:016x} CR3=0x{cr3:016x} data_size={data_size}")

            total = ENTRY_SIZE + data_size
            del buf[:total]

print(f"\n[+] {count} records successfully parsed from ring buffer!")

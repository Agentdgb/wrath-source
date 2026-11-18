#!/usr/bin/env python3
"""
scrub_rtti.py -- Zeroes C++ type descriptor name strings in a DLL's .rdata section.
Targets strings that begin with known payload namespace prefixes (e.g. "bp::").
Run as a post-build step; modifies the DLL in-place.

Usage: python scrub_rtti.py <path_to_dll>
"""

import sys
import struct
import re

# Namespace prefixes to scrub (add more if you rename the payload namespace)
SCRUB_PREFIXES = [
    b"bp::",
    b".?A",   # MSVC RTTI mangled type names start with .?A
]

def scrub_rtti(dll_path: str) -> None:
    with open(dll_path, "r+b") as f:
        data = bytearray(f.read())

    # Locate .rdata section
    # Parse PE headers minimally
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    pe_sig   = data[e_lfanew:e_lfanew+4]
    if pe_sig != b"PE\x00\x00":
        print(f"[scrub_rtti] Not a PE file: {dll_path}")
        return

    coff_off      = e_lfanew + 4
    num_sections  = struct.unpack_from("<H", data, coff_off + 2)[0]
    opt_hdr_size  = struct.unpack_from("<H", data, coff_off + 16)[0]
    sec_table_off = coff_off + 20 + opt_hdr_size

    rdata_raw_off  = None
    rdata_raw_size = 0

    for i in range(num_sections):
        sec_off  = sec_table_off + i * 40
        name     = data[sec_off:sec_off+8].rstrip(b"\x00")
        raw_sz   = struct.unpack_from("<I", data, sec_off + 16)[0]
        raw_off  = struct.unpack_from("<I", data, sec_off + 20)[0]
        if name == b".rdata":
            rdata_raw_off  = raw_off
            rdata_raw_size = raw_sz
            break

    if rdata_raw_off is None:
        print("[scrub_rtti] .rdata section not found -- skipping")
        return

    rdata = data[rdata_raw_off: rdata_raw_off + rdata_raw_size]
    scrubbed = 0

    for prefix in SCRUB_PREFIXES:
        pos = 0
        while True:
            idx = rdata.find(prefix, pos)
            if idx == -1:
                break
            # Zero until next null byte
            end = rdata.index(b"\x00", idx) if b"\x00" in rdata[idx:] else len(rdata)
            for j in range(idx, end):
                data[rdata_raw_off + j] = 0
            scrubbed += 1
            pos = end

    with open(dll_path, "wb") as f:
        f.write(data)

    print(f"[scrub_rtti] Scrubbed {scrubbed} RTTI strings in {dll_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: scrub_rtti.py <dll_path>")
        sys.exit(1)
    scrub_rtti(sys.argv[1])

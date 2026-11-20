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

    # Locate target sections (.rdata, .data)
    # Note: .text is excluded to prevent accidental opcode corruption if mangled
    # prefix bytes happen to appear inside x86-64 instructions.
    target_sections = [b".rdata", b".data"]
    scrubbed = 0

    for i in range(num_sections):
        sec_off  = sec_table_off + i * 40
        name     = data[sec_off:sec_off+8].rstrip(b"\x00")
        raw_sz   = struct.unpack_from("<I", data, sec_off + 16)[0]
        raw_off  = struct.unpack_from("<I", data, sec_off + 20)[0]

        if name in target_sections and raw_sz > 0:
            sec_data = data[raw_off: raw_off + raw_sz]
            for prefix in SCRUB_PREFIXES:
                pos = 0
                while True:
                    idx = sec_data.find(prefix, pos)
                    if idx == -1:
                        break
                    # Zero until next null byte (O(n) via find)
                    null_idx = sec_data.find(b"\x00", idx)
                    end = null_idx if null_idx != -1 else len(sec_data)
                    for j in range(idx, end):
                        data[raw_off + j] = 0
                    scrubbed += 1
                    pos = end

    with open(dll_path, "wb") as f:
        f.write(data)

    print(f"[scrub_rtti] Scrubbed {scrubbed} RTTI/ident strings in {dll_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: scrub_rtti.py <dll_path>")
        sys.exit(1)
    scrub_rtti(sys.argv[1])

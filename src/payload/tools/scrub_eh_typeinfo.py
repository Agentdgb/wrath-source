"""
scrub_eh_typeinfo.py - Neutralize EH catch type descriptor and template signature name strings in a PE.

MSVC /EHsc emits:
  1. __CatchableType / __ThrowInfo records in .data for every type that appears
     in a catch clause (".?AUGameLoopEvent@events@dt@@").
  2. Mangled function pointer / prototype signatures in .rdata for templated
     callbacks (".P6AX...URGBA@dt@@@Z").

Both embed decorated mangled names that string scanners (detect-scanner, Echo,
Paladin) can grep.

This script scans the entire PE image for these patterns and overwrites the
tokens between prefix and suffix with 0x2A ('*') bytes, preserving length and
null terminators so internal PE offsets and structure alignment remain valid.

Usage:
    python scrub_eh_typeinfo.py <path_to_dll>
"""

import sys
import re


# (Compiled regex, prefix_length, suffix_length)
PATTERNS = [
    # EH catch type descriptors (.?AV...@@ or .?AU...@@)
    (re.compile(rb'[.][?]A[VU][^\x00]{3,}@@'), 4, 2),
    # Mangled function pointer / prototype records (.P6...@Z)
    (re.compile(rb'[.]P6[^\x00]{3,}@Z'), 3, 2),
]

TARGET_NAMESPACES = [
    b'dt', b'sync', b'events', b'modules',
    b'config', b'jvm', b'mappings', b'hook',
    b'WrathPayload', b'display_service',
]


def scrub(dll_path: str) -> None:
    with open(dll_path, 'rb') as f:
        data = bytearray(f.read())

    scrubbed = 0
    for pattern, prefix_len, suffix_len in PATTERNS:
        for m in pattern.finditer(data):
            full = m.group(0)
            if not any(ns in full for ns in TARGET_NAMESPACES):
                continue
            body_start = m.start() + prefix_len
            body_end   = m.end()   - suffix_len
            if body_end > body_start:
                data[body_start:body_end] = b'\x2a' * (body_end - body_start)
                scrubbed += 1

    print(f"[scrub_eh_typeinfo] {dll_path}")
    print(f"  descriptors neutralized : {scrubbed}")

    with open(dll_path, 'wb') as f:
        f.write(data)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: python {sys.argv[0]} <dll_path>", file=sys.stderr)
        sys.exit(1)
    scrub(sys.argv[1])

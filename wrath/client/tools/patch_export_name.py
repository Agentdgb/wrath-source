"""
patch_export_name.py — Overwrites the PE Export Directory name field in-place.

Usage:
    python patch_export_name.py <path_to_dll>

Reads the Export Directory Name RVA from the PE Optional Header Data Directory
index 0, resolves it to a file offset by walking the section table, then
overwrites the null-terminated name string with 'javaw.dll\x00' (zero-padded
to the original length). Writes the file back and prints old/new names.
"""

import sys
import struct


REPLACEMENT = b"javaw.dll\x00"


def rva_to_offset(rva: int, sections: list) -> int:
    """Convert an RVA to a raw file offset using the section table."""
    for (virt_addr, virt_size, raw_offset, raw_size) in sections:
        if virt_addr <= rva < virt_addr + virt_size:
            return raw_offset + (rva - virt_addr)
    raise ValueError(f"RVA 0x{rva:08x} not found in any section")


def patch_export_name(dll_path: str) -> None:
    with open(dll_path, "rb") as f:
        data = bytearray(f.read())

    # --- DOS header ---
    if data[0:2] != b"MZ":
        raise ValueError("Not a valid PE file (missing MZ signature)")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]

    # --- PE signature ---
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("Not a valid PE file (missing PE signature)")

    coff_offset = e_lfanew + 4

    # --- COFF File Header ---
    machine = struct.unpack_from("<H", data, coff_offset)[0]
    size_of_optional = struct.unpack_from("<H", data, coff_offset + 16)[0]
    optional_offset = coff_offset + 20

    # --- Optional Header magic (PE32 vs PE32+) ---
    magic = struct.unpack_from("<H", data, optional_offset)[0]
    if magic == 0x10B:       # PE32
        data_dir_offset = optional_offset + 96
    elif magic == 0x20B:     # PE32+
        data_dir_offset = optional_offset + 112
    else:
        raise ValueError(f"Unknown Optional Header magic: 0x{magic:04x}")

    # --- Export Directory RVA (Data Directory index 0) ---
    export_rva = struct.unpack_from("<I", data, data_dir_offset)[0]
    if export_rva == 0:
        raise ValueError("No Export Directory present in this PE")

    # --- Section table ---
    num_sections = struct.unpack_from("<H", data, coff_offset + 2)[0]
    section_table_offset = optional_offset + size_of_optional
    sections = []
    for i in range(num_sections):
        s = section_table_offset + i * 40
        virt_size   = struct.unpack_from("<I", data, s + 8)[0]
        virt_addr   = struct.unpack_from("<I", data, s + 12)[0]
        raw_size    = struct.unpack_from("<I", data, s + 16)[0]
        raw_offset  = struct.unpack_from("<I", data, s + 20)[0]
        sections.append((virt_addr, virt_size, raw_offset, raw_size))

    # --- Export Directory → Name RVA (field at offset 12 in IMAGE_EXPORT_DIRECTORY) ---
    export_offset = rva_to_offset(export_rva, sections)
    name_rva = struct.unpack_from("<I", data, export_offset + 12)[0]
    name_offset = rva_to_offset(name_rva, sections)

    # Read current null-terminated name
    end = name_offset
    while end < len(data) and data[end] != 0:
        end += 1
    original_name = bytes(data[name_offset:end]).decode("ascii", errors="replace")
    original_len = end - name_offset + 1  # include the null terminator

    # Validate replacement fits
    if len(REPLACEMENT) > original_len:
        raise ValueError(
            f"Replacement ({len(REPLACEMENT)} bytes) exceeds original field "
            f"length ({original_len} bytes); cannot patch safely"
        )

    # Build padded replacement (zero-fill to original_len)
    patched = REPLACEMENT + b"\x00" * (original_len - len(REPLACEMENT))
    data[name_offset:name_offset + original_len] = patched

    with open(dll_path, "wb") as f:
        f.write(data)

    new_name = REPLACEMENT.rstrip(b"\x00").decode("ascii")
    print(f"[patch_export_name] {dll_path}")
    print(f"  old name : {original_name}")
    print(f"  new name : {new_name}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: python {sys.argv[0]} <dll_path>", file=sys.stderr)
        sys.exit(1)
    patch_export_name(sys.argv[1])

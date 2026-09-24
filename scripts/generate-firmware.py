#!/usr/bin/env python3
"""Embed the pinned, unmodified firmware bytes; reject unreviewed replacements."""
from pathlib import Path
import hashlib

root = Path(__file__).resolve().parents[1]
data = (root / "firmware/ql2500_fw.bin").read_bytes()
expected = "28155a1830aad41dcae0272c819daa38d981b3ba76daa0b4e53ce47a9548a9ce"
if hashlib.sha256(data).hexdigest() != expected:
    raise SystemExit("Firmware SHA-256 does not match the reviewed ISP25xx image")
output = root / "generated/QL2500Firmware.hpp"
text = (
    "// Unmodified ql2500_fw.bin; see firmware/LICENCE.qla2xxx and SOURCE.txt.\n"
    "#pragma once\n#include <stdint.h>\nnamespace isp25xx {\n"
    "inline constexpr uint8_t firmwareImage[] = {\n"
    + "".join(
        ",".join(f"0x{v:02x}" for v in data[i : i + 24]) + ",\n"
        for i in range(0, len(data), 24)
    )
    + "};\n}\n"
)
output.parent.mkdir(exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

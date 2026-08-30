#!/usr/bin/env python3
"""Verify and optionally disassemble the DSZ.EXE used for ESC8 research."""

import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys


DSZ_SHA256 = "a1bc1343858c98661c5252e307dca1d0f084ead39c3d6c191c2b2842e56f7c8b"
CRC_SUFFIX = b"Copyright 1989 Omen Technology INC All Rights Reserved"
DATA_SEGMENT = 0xC9F0
CRC_SUFFIX_OFFSET = 0x1BDA
ROUTINES = (
    ("seven-bit encoder", 0x6E12, 0x6FA8,
     "b55ce91e4b7e55f05acfc03493a8982234a9010f40c74ec32449feb2f542ddc9"),
    ("header-format dispatcher", 0x6FA8, 0x70DC,
     "7f704f1a484dba856e219bcc0d930bc958a99a748e762739fa9c4331e4d598b4"),
    ("0x31 header encoder", 0x71D4, 0x72BA,
     "5340a84fbea30edaf419325bdce66815c32276b2a7e04e6fb0e4d7efdcae6c0d"),
    ("header receiver dispatcher", 0x7B8E, 0x7D5F,
     "07660d28c416364d761a21fe1697936dd725e6ad00719aaab0641bd53ae7f77e"),
    ("0x31 header decoder", 0x807A, 0x81AA,
     "614371867564405e0529ffdef5a89388ef7f9b51abfeb52c3db72aa33ac68d0c"),
    ("seven-bit decoder", 0x8642, 0x8778,
     "4e21178046c435ce04961f14b06227cae344be94d4d700846dd48fa726409921"),
    ("RLE/ESC8 data encoder", 0x8A7E, 0x8D32,
     "d18370b01a453e071aa749d4c4805c32d81493ecaa1998e684d60d8f93395fbc"),
    ("RLE/ESC8 data decoder", 0x8F9A, 0x91F8,
     "0347a2d1f557516b4459e74c4defb11040b69af1eac241b701a00cfca7d9734a"),
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dsz", type=Path, help="1997 DSZ.EXE to verify")
    parser.add_argument("--disassemble", action="store_true",
                        help="print the verified routines with ndisasm")
    args = parser.parse_args()

    executable = args.dsz.read_bytes()
    if digest(executable) != DSZ_SHA256:
        raise SystemExit("DSZ.EXE SHA-256 does not match the researched binary")
    if executable[:2] != b"MZ":
        raise SystemExit("verified file has no MZ header")
    header_size = struct.unpack_from("<H", executable, 8)[0] * 16
    if header_size != 512:
        raise SystemExit(f"unexpected MZ header size: {header_size}")
    image = executable[header_size:]

    for name, start, end, expected in ROUTINES:
        actual = digest(image[start:end])
        if actual != expected:
            raise SystemExit(f"{name} at {start:04x}:{end:04x} changed")
    suffix_start = DATA_SEGMENT + CRC_SUFFIX_OFFSET
    if image[suffix_start:suffix_start + len(CRC_SUFFIX)] != CRC_SUFFIX:
        raise SystemExit("Omen header CRC suffix was not found at DS:1bda")

    print(f"verified DSZ.EXE SHA-256 {DSZ_SHA256}")
    print(f"MZ load image begins at file offset {header_size:#x}")
    print(f"header CRC suffix verified at load-image offset {suffix_start:#x}")
    for name, start, end, _ in ROUTINES:
        print(f"CS:{start:04x}-{end - 1:04x}  {name}")

    if args.disassemble:
        ndisasm = shutil.which("ndisasm")
        if ndisasm is None:
            raise SystemExit("--disassemble requires ndisasm from NASM")
        for name, start, end, _ in ROUTINES:
            print(f"\n;;; {name}, CS:{start:04x}-{end - 1:04x}")
            subprocess.run(
                [ndisasm, "-b", "16", f"-o{start:#x}", "-"],
                input=image[start:end], check=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)

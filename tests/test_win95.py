#!/usr/bin/env python3
"""Check Open Watcom Windows 95 executable headers and imports."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / os.environ.get("WIN95_BUILD_DIR", "build/win95")).resolve()
ALLOWED_DLLS = {"KERNEL32.DLL", "USER32.DLL", "WSOCK32.DLL"}
FORBIDDEN_IMPORTS = {
    "CancelIo",
    "GetFileSizeEx",
    "GetTickCount64",
    "GetVersionExW",
    "InitializeCriticalSectionAndSpinCount",
    "SetFilePointerEx",
}


def inspect(path: Path) -> None:
    tool = shutil.which("wdump")
    if tool is None:
        raise RuntimeError("Open Watcom wdump is not in PATH")
    output = subprocess.check_output(
        [tool, "-q", "-e", str(path)], text=True, errors="replace"
    )
    required = {
        r"cpu type \(32-bit\)\s*=\s*014CH": "i386 PE target",
        r"subsystem major version number\s*=\s*0004H":
            "Windows 4.0 subsystem version",
        r"nt subsystem\s*=\s*0003H": "console subsystem",
    }
    for pattern, description in required.items():
        if re.search(pattern, output, re.IGNORECASE) is None:
            raise RuntimeError(f"{path.name} is missing {description}")

    dlls = {
        match.group(1).upper()
        for match in re.finditer(r"DLL name = <([^>]+)>", output)
    }
    if not {"KERNEL32.DLL", "WSOCK32.DLL"}.issubset(dlls):
        raise RuntimeError(f"{path.name} has incomplete imports: {sorted(dlls)}")
    unexpected = dlls - ALLOWED_DLLS
    if unexpected:
        raise RuntimeError(
            f"{path.name} imports non-Win95 DLLs: {sorted(unexpected)}"
        )
    for name in FORBIDDEN_IMPORTS:
        if re.search(rf"\s{re.escape(name)}\s*$", output, re.MULTILINE):
            raise RuntimeError(f"{path.name} imports post-Win95 API {name}")
    print(f"{path.name}: Windows 95 PE/import checks passed")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--static-only", action="store_true")
    parser.parse_args()
    for name in ("zmtx.exe", "zmrx.exe"):
        inspect(BUILD / name)


if __name__ == "__main__":
    main()

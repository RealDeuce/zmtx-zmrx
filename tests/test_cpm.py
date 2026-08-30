#!/usr/bin/env python3
"""Build-level and live-transfer checks for the CP/M frontend."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
CPM_BUILD_DIR = (
    ROOT / os.environ.get("CPM_BUILD_DIR", "build/cpm")
).resolve()
TNYLPO = os.environ.get("TNYLPO", "tnylpo")
TIMEOUT = float(os.environ.get("CPM_TEST_TIMEOUT", "30"))
PAYLOAD = bytes(range(256)) * 8


def logged_console(logfile: Path) -> str:
    if not logfile.exists():
        return ""
    text = logfile.read_text(errors="replace")
    values = re.findall(r"console output entry: e=0x([0-9a-f]{2})", text)
    return bytes(int(value, 16) for value in values).decode(errors="replace")


def logged_tail(logfile: Path, length: int = 32768) -> str:
    if not logfile.exists():
        return ""
    return logfile.read_text(errors="replace")[-length:]


def logged_bytes(logfile: Path, device: str) -> bytes:
    if not logfile.exists():
        return b""
    text = logfile.read_text(errors="replace")
    if device == "punch":
        pattern = r"punch output entry: e=0x([0-9a-f]{2})"
    else:
        pattern = r"reader input exit: a=0x([0-9a-f]{2})"
    values = re.findall(pattern, text)
    return bytes(int(value, 16) for value in values)


def run_usage(program: Path) -> None:
    command = [TNYLPO, f"./{program.name}"]
    if program.name == "zmrx.com":
        command.append("-?")
    result = subprocess.run(
        command,
        cwd=program.parent,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=TIMEOUT,
        check=False,
    )
    output = result.stdout + result.stderr
    if b"CP/M I/O uses the RDR: and PUN: devices" not in output:
        raise RuntimeError(
            f"{program.name} did not display its CP/M usage text:\n"
            + output.decode(errors="replace")
        )


def write_config(
    path: Path, drive: Path, reader: Path, punch: Path, logfile: Path
) -> None:
    path.write_text(
        f'drive a = "{drive}"\n'
        "default drive = a\n"
        "console = line\n"
        f'reader file = "{reader}"\n'
        "reader mode = raw\n"
        f'punch file = "{punch}"\n'
        "punch mode = raw\n"
        f'logfile = "{logfile}"\n'
        "loglevel = 3\n",
        encoding="ascii",
    )


def received_file(directory: Path, name: str) -> Path:
    matches = [entry for entry in directory.iterdir()
               if entry.name.casefold() == name.casefold()]
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one case-insensitive match for {name} in {directory}; "
            f"found {[entry.name for entry in matches]}"
        )
    return matches[0]


def wait_pair(
    native: subprocess.Popen, cpm: subprocess.Popen, label: str, logfile: Path
) -> tuple:
    deadline = time.monotonic() + TIMEOUT
    while time.monotonic() < deadline:
        if native.poll() is not None and cpm.poll() is not None:
            break
        time.sleep(0.02)
    else:
        native.kill()
        cpm.kill()
        native.wait()
        cpm.wait()
        native_error = native.stderr.read()
        cpm_output = cpm.stdout.read()
        cpm_error = cpm.stderr.read()
        punched = logged_bytes(logfile, "punch")
        read = logged_bytes(logfile, "reader")
        raise RuntimeError(
            f"{label} timed out\n"
            f"native stderr:\n{native_error.decode(errors='replace')}\n"
            f"CP/M stdout:\n{cpm_output.decode(errors='replace')}\n"
            f"CP/M stderr:\n{cpm_error.decode(errors='replace')}\n"
            f"logged CP/M console:\n{logged_console(logfile)}\n"
            f"last punched bytes ({len(punched)} total): {punched[-256:].hex()}\n"
            f"last reader bytes ({len(read)} total): {read[-256:].hex()}"
        )

    native_error = native.stderr.read()
    cpm_output = cpm.stdout.read()
    cpm_error = cpm.stderr.read()
    if native.returncode != 0 or cpm.returncode != 0:
        raise RuntimeError(
            f"{label} failed (native={native.returncode}, cpm={cpm.returncode})\n"
            f"native stderr:\n{native_error.decode(errors='replace')}\n"
            f"CP/M stdout:\n{cpm_output.decode(errors='replace')}\n"
            f"CP/M stderr:\n{cpm_error.decode(errors='replace')}"
        )
    return native_error, cpm_output, cpm_error


def transfer(cpm_sender: bool) -> None:
    direction = "CP/M sender" if cpm_sender else "CP/M receiver"
    with tempfile.TemporaryDirectory(prefix="zmodem-cpm-") as temporary:
        base = Path(temporary)
        cpm_drive = base / "cpm"
        native_drive = base / "native"
        cpm_drive.mkdir()
        native_drive.mkdir()
        shutil.copy2(CPM_BUILD_DIR / "zmtx.com", cpm_drive / "zmtx.com")
        shutil.copy2(CPM_BUILD_DIR / "zmrx.com", cpm_drive / "zmrx.com")

        source_drive = cpm_drive if cpm_sender else native_drive
        destination_drive = native_drive if cpm_sender else cpm_drive
        (source_drive / "payload.bin").write_bytes(PAYLOAD)

        native_to_cpm = base / "native-to-cpm"
        cpm_to_native = base / "cpm-to-native"
        os.mkfifo(native_to_cpm)
        os.mkfifo(cpm_to_native)
        config = base / "tnylpo.conf"
        logfile = base / "tnylpo.log"
        write_config(config, cpm_drive, native_to_cpm, cpm_to_native, logfile)

        keep_native_to_cpm = os.open(
            native_to_cpm, os.O_RDWR | os.O_NONBLOCK
        )
        keep_cpm_to_native = os.open(cpm_to_native, os.O_RDWR | os.O_NONBLOCK)
        native_input = open(cpm_to_native, "rb", buffering=0)
        native_output = open(native_to_cpm, "wb", buffering=0)
        try:
            if cpm_sender:
                native_command = [str(ROOT / "zmrx"), "-v"]
                cpm_command = [
                    TNYLPO,
                    "-f",
                    str(config),
                    "./zmtx.com",
                    "-v",
                    "payload.bin",
                ]
            else:
                native_command = [str(ROOT / "zmtx"), "-v", "payload.bin"]
                cpm_command = [TNYLPO, "-f", str(config), "./zmrx.com", "-v"]

            native = subprocess.Popen(
                native_command,
                cwd=native_drive,
                stdin=native_input,
                stdout=native_output,
                stderr=subprocess.PIPE,
            )
            cpm = subprocess.Popen(
                cpm_command,
                cwd=cpm_drive,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            native_input.close()
            native_output.close()
            native_error, cpm_output, cpm_error = wait_pair(
                native, cpm, direction, logfile
            )
        finally:
            native_input.close()
            native_output.close()
            os.close(keep_native_to_cpm)
            os.close(keep_cpm_to_native)

        try:
            received_path = received_file(destination_drive, "payload.bin")
        except RuntimeError as error:
            punched = logged_bytes(logfile, "punch")
            read = logged_bytes(logfile, "reader")
            raise RuntimeError(
                f"{error}\n"
                f"native stderr:\n{native_error.decode(errors='replace')}\n"
                f"CP/M stdout:\n{cpm_output.decode(errors='replace')}\n"
                f"CP/M stderr:\n{cpm_error.decode(errors='replace')}\n"
                f"logged CP/M console:\n{logged_console(logfile)}\n"
                f"tnylpo log tail:\n{logged_tail(logfile)}\n"
                f"last punched bytes ({len(punched)} total): "
                f"{punched[-256:].hex()}\n"
                f"last reader bytes ({len(read)} total): "
                f"{read[-256:].hex()}"
            ) from error
        received = received_path.read_bytes()
        if received != PAYLOAD:
            raise RuntimeError(
                f"{direction} produced {len(received)} bytes; expected "
                f"{len(PAYLOAD)} exact bytes"
            )


def main() -> None:
    if shutil.which(TNYLPO) is None:
        raise SystemExit(f"tnylpo executable not found: {TNYLPO}")

    with tempfile.TemporaryDirectory(prefix="zmodem-cpm-usage-") as temporary:
        directory = Path(temporary)
        for name in ("zmtx.com", "zmrx.com"):
            shutil.copy2(CPM_BUILD_DIR / name, directory / name)
            run_usage(directory / name)

    transfer(cpm_sender=True)
    transfer(cpm_sender=False)
    print("CP/M tnylpo transfers passed")


if __name__ == "__main__":
    main()

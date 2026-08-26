#!/usr/bin/env python3
"""Open Watcom map checks and DOSBox serial transfer tests."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
BUILD = (ROOT / os.environ.get("DOS_BUILD_DIR", "build/dos")).resolve()
TIMEOUT = float(os.environ.get("DOS_TEST_TIMEOUT", "45"))
PAYLOAD = bytes(range(256)) * 8
DGROUP_LIMIT = 0xF000


def check_maps() -> None:
    for name in ("zmtx", "zmrx"):
        path = BUILD / f"{name}.map"
        match = re.search(
            r"^DGROUP\s+[0-9a-f]+:[0-9a-f]+\s+([0-9a-f]+)$",
            path.read_text(encoding="ascii"),
            re.MULTILINE | re.IGNORECASE,
        )
        if match is None:
            raise RuntimeError(f"could not find DGROUP size in {path}")
        size = int(match.group(1), 16)
        if size > DGROUP_LIMIT:
            raise RuntimeError(
                f"{name}.exe DGROUP is {size} bytes; limit is {DGROUP_LIMIT}"
            )


def reserve_port() -> int:
    reservation = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    reservation.bind(("127.0.0.1", 0))
    port = reservation.getsockname()[1]
    reservation.close()
    return port


def connect(port: int, process: subprocess.Popen[bytes]) -> socket.socket:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("DOSBox exited before opening its serial socket")
        try:
            return socket.create_connection(("127.0.0.1", port), 0.25)
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("DOSBox did not open its serial socket")


def relay(peer: socket.socket, native: subprocess.Popen[bytes]) -> None:
    peer.setblocking(False)
    assert native.stdin is not None
    assert native.stdout is not None
    os.set_blocking(native.stdout.fileno(), False)
    selector = selectors.DefaultSelector()
    selector.register(peer, selectors.EVENT_READ, "dos")
    selector.register(native.stdout, selectors.EVENT_READ, "native")
    deadline = time.monotonic() + TIMEOUT
    while selector.get_map():
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("serial relay timed out")
        for key, _ in selector.select(min(remaining, 0.25)):
            if key.data == "dos":
                try:
                    data = peer.recv(8192)
                except BlockingIOError:
                    continue
                if not data:
                    selector.unregister(peer)
                    native.stdin.close()
                    continue
                native.stdin.write(data)
                native.stdin.flush()
            else:
                try:
                    data = os.read(native.stdout.fileno(), 8192)
                except BlockingIOError:
                    continue
                if not data:
                    selector.unregister(native.stdout)
                    try:
                        peer.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass
                    continue
                peer.sendall(data)
        if native.poll() is not None and native.stdout not in (
            key.fileobj for key in selector.get_map().values()
        ):
            break


def find_received(directory: Path) -> Path:
    matches = [path for path in directory.iterdir()
               if path.name.casefold() == "payload.bin"]
    if len(matches) != 1:
        raise RuntimeError(f"expected payload.bin in {directory}: {matches}")
    return matches[0]


def transfer(
    dosbox: str, backend: str, dos_sender: bool, x00: Path | None
) -> None:
    options = {
        "uart": ["-u", "-c1", "-r115200", "-k"],
        "bios": ["-i", "-c1", "-r2400", "-k"],
        "fossil": ["-f", "-c1", "-r9600", "-k"],
    }[backend]
    label = f"DOS {backend} {'sender' if dos_sender else 'receiver'}"
    with tempfile.TemporaryDirectory(prefix="zmodem-dos-") as temporary:
        base = Path(temporary)
        dos_drive = base / "dos"
        native_drive = base / "native"
        dos_drive.mkdir()
        native_drive.mkdir()
        shutil.copy2(BUILD / "zmtx.exe", dos_drive / "ZMTX.EXE")
        shutil.copy2(BUILD / "zmrx.exe", dos_drive / "ZMRX.EXE")
        if backend == "fossil":
            if x00 is None:
                raise RuntimeError("the FOSSIL test requires --x00")
            shutil.copy2(x00, dos_drive / "X00.EXE")

        source = dos_drive if dos_sender else native_drive
        destination = native_drive if dos_sender else dos_drive
        (source / "payload.bin").write_bytes(PAYLOAD)
        dos_program = "ZMTX.EXE" if dos_sender else "ZMRX.EXE"
        dos_arguments = [*options]
        if dos_sender:
            dos_arguments.append("PAYLOAD.BIN")
        batch = ["@echo off"]
        if backend == "fossil":
            batch.append("X00.EXE E")
        batch.extend((
            " ".join((dos_program, *dos_arguments)),
            "if errorlevel 1 goto failed",
            "echo pass>RESULT.TXT",
            "exit",
            ":failed",
            "echo fail>RESULT.TXT",
            "exit",
        ))
        (dos_drive / "TEST.BAT").write_bytes(
            ("\r\n".join(batch) + "\r\n").encode("ascii")
        )

        port = reserve_port()
        config = base / "dosbox.conf"
        config.write_text(
            "[midi]\nmididevice=none\n"
            "[serial]\n"
            f"serial1=nullmodem port:{port} transparent:1 "
            "telnet:0 usedtr:0 rxdelay:10000 txdelay:0\n",
            encoding="ascii",
        )
        log_path = base / "dosbox.log"
        environment = os.environ.copy()
        environment.setdefault("SDL_VIDEODRIVER", "dummy")
        environment.setdefault("SDL_AUDIODRIVER", "dummy")
        with log_path.open("wb") as log:
            dos = subprocess.Popen(
                [dosbox, "-noconsole", "-conf", str(config), "-c",
                 f'mount c "{dos_drive}"', "-c", "c:", "-c", "TEST.BAT"],
                stdout=log,
                stderr=subprocess.STDOUT,
                env=environment,
            )
            peer = None
            native = None
            try:
                peer = connect(port, dos)
                native_command = [
                    str(ROOT / ("zmrx" if dos_sender else "zmtx")), "-v"
                ]
                if not dos_sender:
                    native_command.append("payload.bin")
                native = subprocess.Popen(
                    native_command,
                    cwd=native_drive,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                relay(peer, native)
                native.wait(timeout=5)
                assert native.stderr is not None
                native_error = native.stderr.read()
                dos_return = dos.wait(timeout=5)
            except Exception as error:
                if native is not None and native.poll() is None:
                    native.kill()
                    native.wait()
                if dos.poll() is None:
                    dos.kill()
                    dos.wait()
                log.flush()
                details = log_path.read_bytes()[-16384:].decode(errors="replace")
                raise RuntimeError(f"{label} failed: {error}\n{details}") from error
            finally:
                if peer is not None:
                    peer.close()

        result = (dos_drive / "RESULT.TXT")
        if native.returncode != 0 or dos_return != 0 or not result.exists() or \
                result.read_text(errors="replace").strip() != "pass":
            raise RuntimeError(
                f"{label} failed (native={native.returncode}, DOSBox={dos_return})\n"
                + native_error.decode(errors="replace")
            )
        if find_received(destination).read_bytes() != PAYLOAD:
            raise RuntimeError(f"{label} did not preserve the payload")
        print(f"{label} passed", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dosbox", default=os.environ.get("DOSBOX", "dosbox"))
    parser.add_argument("--x00", type=Path)
    parser.add_argument(
        "--backend", action="append", choices=("uart", "bios", "fossil")
    )
    parser.add_argument("--maps-only", action="store_true")
    arguments = parser.parse_args()
    check_maps()
    if arguments.maps_only:
        print("DOS DGROUP budgets passed")
        return
    if shutil.which(arguments.dosbox) is None:
        raise SystemExit(f"DOSBox executable not found: {arguments.dosbox}")
    backends = arguments.backend or ["uart", "bios", "fossil"]
    for backend in backends:
        for dos_sender in (True, False):
            transfer(arguments.dosbox, backend, dos_sender, arguments.x00)


if __name__ == "__main__":
    main()

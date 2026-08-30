#!/usr/bin/env python3
"""Manual DSZ.EXE ESC8/MobyTurbo check; DSZ is not redistributed."""

import argparse
import os
from pathlib import Path
import shutil
import selectors
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import test_dos  # noqa: E402


PAYLOAD = bytes(range(256)) * 4 + b"A" * 130 + b" " * 70 + b"~" * 65
PACK7_PAYLOADS = (
    bytes((index * 73 + 19) & 0xFF for index in range(1027)),
    b"A" * 511 + b" " * 384 + b"~" * 131,
)


def relay(peer, native, from_dos, from_native, seven_bit):
    peer.setblocking(False)
    os.set_blocking(native.stdout.fileno(), False)
    selector = selectors.DefaultSelector()
    selector.register(peer, selectors.EVENT_READ, "dos")
    selector.register(native.stdout, selectors.EVENT_READ, "native")
    deadline = time.monotonic() + test_dos.TIMEOUT
    while selector.get_map():
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("serial relay timed out")
        for key, _ in selector.select(min(remaining, 0.25)):
            if key.data == "dos":
                data = peer.recv(8192)
                if not data:
                    selector.unregister(peer)
                    native.stdin.close()
                    continue
                if seven_bit:
                    data = bytes(value & 0x7F for value in data)
                from_dos.extend(data)
                native.stdin.write(data)
                native.stdin.flush()
            else:
                data = os.read(native.stdout.fileno(), 8192)
                if not data:
                    selector.unregister(native.stdout)
                    try:
                        peer.shutdown(1)
                    except OSError:
                        pass
                    continue
                if seven_bit:
                    data = bytes(value & 0x7F for value in data)
                from_native.extend(data)
                peer.sendall(data)


def transfer(dosbox, dsz, dsz_sender, option, mode, payload=PAYLOAD,
             seven_bit=False, variant="", debug_hold=0.0):
    suffix = f" {variant}" if variant else ""
    label = f"DSZ {'sender' if dsz_sender else 'receiver'} {mode}{suffix}"
    with tempfile.TemporaryDirectory(prefix="zmodem-dsz-") as temporary:
        base = Path(temporary)
        dos_drive = base / "dos"
        native_drive = base / "native"
        dos_drive.mkdir()
        native_drive.mkdir()
        shutil.copy2(dsz, dos_drive / "DSZ.EXE")
        source = dos_drive if dsz_sender else native_drive
        destination = native_drive if dsz_sender else dos_drive
        (source / "payload.bin").write_bytes(payload)

        action = f"sz {option} PAYLOAD.BIN" if dsz_sender else f"rz {option}"
        batch = (
            "@echo off\r\n"
            f"DSZ.EXE port 1 speed 9600 handshake off d {action}\r\n"
            "if errorlevel 1 goto failed\r\n"
            "echo pass>RESULT.TXT\r\n"
            "exit\r\n"
            ":failed\r\n"
            "echo fail>RESULT.TXT\r\n"
            "exit\r\n"
        )
        (dos_drive / "TEST.BAT").write_bytes(batch.encode("ascii"))

        port = test_dos.reserve_port()
        config = base / "dosbox.conf"
        config.write_text(
            "[midi]\nmididevice=none\n"
            "[serial]\n"
            f"serial1=nullmodem port:{port} transparent:1 telnet:0 "
            "usedtr:0 rxdelay:10000 txdelay:0\n",
            encoding="ascii")
        environment = os.environ.copy()
        environment.setdefault("SDL_VIDEODRIVER", "dummy")
        environment.setdefault("SDL_AUDIODRIVER", "dummy")
        dos = subprocess.Popen(
            [dosbox, "-noconsole", "-conf", str(config), "-c",
             f'mount c "{dos_drive}"', "-c", "c:", "-c", "TEST.BAT"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment)
        peer = None
        native = None
        relay_error = None
        from_dos = bytearray()
        from_native = bytearray()
        try:
            if debug_hold > 0.0:
                print(f"{label}: holding serial connection for "
                      f"{debug_hold:g} seconds")
                time.sleep(debug_hold)
            peer = test_dos.connect(port, dos)
            native_option = "-bv" if mode == "ESC8" and dsz_sender else \
                "-7v" if mode == "Pack-7" and dsz_sender else \
                "-mv" if mode == "MobyTurbo" else "-v"
            command = [str(ROOT / ("zmrx" if dsz_sender else "zmtx")),
                       native_option]
            if not dsz_sender:
                command.append("payload.bin")
            native = subprocess.Popen(
                command, cwd=native_drive, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                relay(peer, native, from_dos, from_native, seven_bit)
                native.wait(timeout=5)
                dos.wait(timeout=5)
            except Exception as error:
                relay_error = error
        finally:
            if peer is not None:
                peer.close()
            if native is not None and native.poll() is None:
                native.kill()
                native.wait()
            if dos.poll() is None:
                dos.kill()
                dos.wait()

        result = dos_drive / "RESULT.TXT"
        if relay_error is not None or native.returncode != 0 or \
                not result.exists() or \
                result.read_text(errors="replace").strip() != "pass":
            native_error = native.stderr.read().decode(errors="replace")
            dos_output = dos.stdout.read().decode(errors="replace")
            raise RuntimeError(
                f"{label} failed: {relay_error}\n{native_error}\n"
                f"last DOS bytes: {from_dos[-1024:].hex()}\n"
                f"last native bytes: {from_native[-1024:].hex()}\n"
                f"{dos_output}")
        if test_dos.find_received(destination).read_bytes() != payload:
            raise RuntimeError(f"{label} did not preserve the payload")
        print(f"{label} transfer passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dsz", type=Path, help="Omen DSZ.EXE")
    parser.add_argument("--dosbox", default=os.environ.get("DOSBOX", "dosbox"))
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--mode", choices=("all", "ESC8", "MobyTurbo",
                                            "Pack-7"), default="all",
                        help="run only one wire mode")
    parser.add_argument("--role", choices=("both", "sender", "receiver"),
                        default="both", help="limit the DSZ role")
    parser.add_argument("--fixture", type=int, choices=(1, 2),
                        help="limit Pack-7 to one fixture")
    parser.add_argument("--debug-hold", type=float, default=0.0,
                        help="delay the serial connection for a debugger")
    args = parser.parse_args()
    test_dos.TIMEOUT = args.timeout
    subprocess.run(
        [sys.executable, str(ROOT / "tools" / "verify_dsz_esc8.py"),
         str(args.dsz)], check=True)
    if shutil.which(args.dosbox) is None:
        raise SystemExit(f"DOSBox not found: {args.dosbox}")
    roles = (True, False) if args.role == "both" else \
        (args.role == "sender",)
    for option, mode, seven_bit in (
            ("-E", "ESC8", True), ("-m", "MobyTurbo", False)):
        if args.mode not in ("all", mode):
            continue
        for dsz_sender in roles:
            transfer(args.dosbox, args.dsz, dsz_sender=dsz_sender,
                     option=option, mode=mode, seven_bit=seven_bit,
                     debug_hold=args.debug_hold)
    if args.mode in ("all", "Pack-7"):
        for index, payload in enumerate(PACK7_PAYLOADS, 1):
            if args.fixture is not None and args.fixture != index:
                continue
            for dsz_sender in roles:
                transfer(args.dosbox, args.dsz, dsz_sender=dsz_sender,
                         option="-EP", mode="Pack-7", payload=payload,
                         seven_bit=True, variant=f"fixture {index}",
                         debug_hold=args.debug_hold)


if __name__ == "__main__":
    main()

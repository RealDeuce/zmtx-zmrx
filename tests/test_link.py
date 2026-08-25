#!/usr/bin/env python3

import os
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ZMTX = Path(os.environ.get("ZMTX", ROOT / "zmtx"))
ZMRX = Path(os.environ.get("ZMRX", ROOT / "zmrx"))
LSZ = os.environ.get("LSZ") or shutil.which("lsz")
LRZ = os.environ.get("LRZ") or shutil.which("lrz")
CHUNK_SIZE = 65536


def relay(source, destination, rate, corrupt_offsets, result, name):
    offsets = iter(sorted(corrupt_offsets))
    next_offset = next(offsets, None)
    wire_count = 0
    flipped = 0
    rate_start = time.monotonic()

    try:
        while True:
            data = source.recv(CHUNK_SIZE)
            if not data:
                break
            changed = bytearray(data)
            end_offset = wire_count + len(changed)
            while next_offset is not None and next_offset < end_offset:
                if next_offset >= wire_count:
                    changed[next_offset - wire_count] ^= 1
                    flipped += 1
                next_offset = next(offsets, None)

            if rate > 0:
                target = rate_start + (end_offset / rate)
                delay = target - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
            destination.sendall(changed)
            wire_count = end_offset
    except OSError:
        pass
    finally:
        result[name] = (wire_count, flipped)
        try:
            destination.shutdown(socket.SHUT_WR)
        except OSError:
            pass


class LinkTests(unittest.TestCase):
    def run_link(self, sender_command, receiver_command, source_directory,
                 destination_directory, *, corrupt_offsets=(),
                 forward_rate=0, reverse_rate=0, timeout=30):
        sender_end, sender_relay = socket.socketpair()
        receiver_end, receiver_relay = socket.socketpair()
        sender = None
        receiver = None
        result = {}

        try:
            sender = subprocess.Popen(
                [str(value) for value in sender_command],
                cwd=source_directory, stdin=sender_end, stdout=sender_end,
                stderr=subprocess.PIPE,
            )
            receiver = subprocess.Popen(
                [str(value) for value in receiver_command],
                cwd=destination_directory, stdin=receiver_end,
                stdout=receiver_end, stderr=subprocess.PIPE,
            )
        finally:
            sender_end.close()
            receiver_end.close()

        forward = threading.Thread(
            target=relay,
            args=(sender_relay, receiver_relay, forward_rate,
                  corrupt_offsets, result, "forward"),
        )
        reverse = threading.Thread(
            target=relay,
            args=(receiver_relay, sender_relay, reverse_rate, (), result,
                  "reverse"),
        )
        forward.start()
        reverse.start()

        deadline = time.monotonic() + timeout
        try:
            sender.wait(timeout=max(0.1, deadline - time.monotonic()))
            receiver.wait(timeout=max(0.1, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            sender.kill()
            receiver.kill()
            sender.wait()
            receiver.wait()
            self.fail(f"link transfer timed out after {timeout} seconds")
        finally:
            for connection in (sender_relay, receiver_relay):
                try:
                    connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                connection.close()
            forward.join(timeout=5)
            reverse.join(timeout=5)

        sender_stderr = sender.stderr.read()
        receiver_stderr = receiver.stderr.read()
        sender.stderr.close()
        receiver.stderr.close()
        self.assertEqual(
            sender.returncode, 0,
            f"sender exited {sender.returncode}: "
            f"{sender_stderr.decode(errors='replace')}",
        )
        self.assertEqual(
            receiver.returncode, 0,
            f"receiver exited {receiver.returncode}: "
            f"{receiver_stderr.decode(errors='replace')}",
        )
        return result

    def test_recovers_from_separated_wire_corruption(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source_directory = base / "source"
            destination_directory = base / "destination"
            source_directory.mkdir()
            destination_directory.mkdir()
            name = "corruption.bin"
            content = bytes(range(256)) * 8192
            (source_directory / name).write_bytes(content)

            for phase in (0, 43690, 87381):
                with self.subTest(phase=phase):
                    destination = destination_directory / name
                    if destination.exists():
                        destination.unlink()
                    corrupt_offsets = tuple(
                        65536 + phase + index * 131072
                        for index in range(12)
                    )
                    result = self.run_link(
                        (ZMTX, "-8", name), (ZMRX, "-q", "-o"),
                        source_directory, destination_directory,
                        corrupt_offsets=corrupt_offsets, timeout=60,
                    )

                    self.assertEqual(result["forward"][1],
                                     len(corrupt_offsets))
                    self.assertEqual(destination.read_bytes(), content)

    @unittest.skipUnless(LSZ and LRZ, "lrzsz is not installed")
    def test_lrzsz_interoperability(self):
        content = bytes(range(256)) * 256
        cases = (
            ("zmtx-to-lrz.bin", (ZMTX, "-8"), (LRZ, "-q", "-y")),
            ("lsz-to-zmrx.bin", (LSZ, "-q", "-8"),
             (ZMRX, "-q", "-o")),
        )

        for name, sender_prefix, receiver_command in cases:
            with self.subTest(name=name), \
                    tempfile.TemporaryDirectory() as temporary:
                base = Path(temporary)
                source_directory = base / "source"
                destination_directory = base / "destination"
                source_directory.mkdir()
                destination_directory.mkdir()
                (source_directory / name).write_bytes(content)

                self.run_link(
                    (*sender_prefix, name), receiver_command,
                    source_directory, destination_directory,
                )
                self.assertEqual(
                    (destination_directory / name).read_bytes(), content)

    def test_completes_with_asymmetric_bandwidth(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source_directory = base / "source"
            destination_directory = base / "destination"
            source_directory.mkdir()
            destination_directory.mkdir()
            name = "asymmetric.bin"
            content = bytes(range(256)) * 2048
            (source_directory / name).write_bytes(content)

            self.run_link(
                (ZMTX, "-8", name), (ZMRX, "-q", "-o"),
                source_directory, destination_directory,
                forward_rate=2 * 1024 * 1024,
                reverse_rate=16 * 1024,
                timeout=15,
            )
            self.assertEqual(
                (destination_directory / name).read_bytes(), content)


if __name__ == "__main__":
    unittest.main(verbosity=2)

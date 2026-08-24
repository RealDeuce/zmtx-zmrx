#!/usr/bin/env python3

import os
import socket
import subprocess
import tempfile
import time
import unittest
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ZMTX = Path(os.environ.get("ZMTX", ROOT / "zmtx"))
ZMRX = Path(os.environ.get("ZMRX", ROOT / "zmrx"))

ZPAD = 0x2A
ZDLE = 0x18
ZBIN = 0x41
ZHEX = 0x42
ZBIN32 = 0x43
ZVBIN = 0x61
ZVHEX = 0x62
ZVBIN32 = 0x63
XON = 0x11
ZRQINIT = 0
ZRINIT = 1
ZACK = 3
ZFILE = 4
ZSKIP = 5
ZNAK = 6
ZFIN = 8
ZRPOS = 9
ZDATA = 10
ZEOF = 11
ZFERR = 12
ZCOMPL = 15
ZF1_ZMCLOB = 4
ZF1_ZMNEW = 5
ZF1_ZMPROT = 7
ZCRCE = 0x68
ZCRCG = 0x69
ZCRCQ = 0x6A
ZCRCW = 0x6B
ESCAPE_BYTES = {ZDLE, 0x10, 0x90, XON, 0x91, 0x13, 0x93}


def crc16_update(crc, value):
    table_value = (crc >> 8) << 8
    for _ in range(8):
        table_value = ((table_value << 1) ^ 0x1021) & 0xFFFF \
            if table_value & 0x8000 else (table_value << 1) & 0xFFFF
    return table_value ^ ((crc << 8) & 0xFFFF) ^ value


def crc16(values, finish=True):
    crc = 0
    for value in values:
        crc = crc16_update(crc, value)
    if finish:
        crc = crc16_update(crc, 0)
        crc = crc16_update(crc, 0)
    return crc


def escaped(values):
    result = bytearray()
    for value in values:
        if value in ESCAPE_BYTES:
            result.extend((ZDLE, value ^ 0x40))
        else:
            result.append(value)
    return bytes(result)


def hex_header(frame_type, position=0, *, header=None, parity=False):
    if header is None:
        header = bytes((frame_type,)) + position.to_bytes(4, "little")
    check = crc16(header)
    wire = b"**\x18B" + header.hex().encode("ascii") + check.to_bytes(2, "big").hex().encode("ascii") + b"\r\n"
    if parity:
        wire = bytes(value | 0x80 for value in wire)
    return wire


def data_subpacket(data, frame_end):
    check = crc16(data + bytes((frame_end,)))
    wire = escaped(data) + bytes((ZDLE, frame_end)) + escaped(check.to_bytes(2, "big"))
    if frame_end == ZCRCW:
        wire += bytes((XON,))
    return wire


def binary32_header(frame_type, position=0):
    header = bytes((frame_type,)) + position.to_bytes(4, "little")
    check = zlib.crc32(header) & 0xFFFFFFFF
    return b"**\x18C" + escaped(header + check.to_bytes(4, "little"))


def data_subpacket32(data, frame_end):
    check = zlib.crc32(data + bytes((frame_end,))) & 0xFFFFFFFF
    wire = escaped(data) + bytes((ZDLE, frame_end))
    wire += escaped(check.to_bytes(4, "little"))
    if frame_end == ZCRCW:
        wire += bytes((XON,))
    return wire


class Peer:
    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(10)
        self.buffer = bytearray()
        self.use_crc32 = False

    def send(self, data):
        self.sock.sendall(data)

    def byte(self):
        if not self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("peer closed the protocol stream")
            self.buffer.extend(chunk)
        value = self.buffer[0]
        del self.buffer[0]
        return value

    def header(self):
        matched = 0
        marker = b"**\x18"
        while matched != len(marker):
            value = self.byte()
            if value == marker[matched]:
                matched += 1
            else:
                matched = 1 if value == marker[0] else 0
        style = self.byte()
        if style in (ZHEX, ZVHEX):
            length = 5
            if style == ZVHEX:
                length = int(bytes((self.byte(), self.byte())), 16)
            encoded = bytes(self.byte() for _ in range(2 * (length + 2)))
            raw = bytes.fromhex(encoded.decode("ascii"))
            header = raw[:length]
            if crc16(header) != int.from_bytes(raw[length:], "big"):
                raise AssertionError("bad CRC in received hex header")
            if self.byte() != 0x0D or self.byte() != 0x0A:
                raise AssertionError("bad received hex-header terminator")
        elif style in (ZBIN, ZVBIN):
            length = 5 if style == ZBIN else self._data_byte()
            raw = bytes(self._data_byte() for _ in range(length + 2))
            header = raw[:length]
            if crc16(header) != int.from_bytes(raw[length:], "big"):
                raise AssertionError("bad CRC in received binary header")
        elif style in (ZBIN32, ZVBIN32):
            length = 5 if style == ZBIN32 else self._data_byte()
            raw = bytes(self._data_byte() for _ in range(length + 4))
            header = raw[:length]
            if (zlib.crc32(header) & 0xFFFFFFFF) != \
                    int.from_bytes(raw[length:], "little"):
                raise AssertionError("bad CRC in received CRC32 header")
        else:
            raise AssertionError(f"unexpected header style {style:#x}")
        self.use_crc32 = style in (ZBIN32, ZVBIN32)
        return header[0], int.from_bytes(header[1:], "little"), header

    def _data_byte(self):
        value = self.byte()
        while value in (XON, 0x91, 0x13, 0x93):
            value = self.byte()
        if value != ZDLE:
            return value
        value = self.byte()
        while value in (XON, 0x91, 0x13, 0x93, ZDLE):
            value = self.byte()
        if value == 0x6C:
            return 0x7F
        if value == 0x6D:
            return 0xFF
        if value & 0x60 == 0x40:
            return value ^ 0x40
        raise AssertionError(f"unexpected ZDLE sequence {value:#x}")

    def data(self):
        payload = bytearray()
        while True:
            value = self.byte()
            if value in (XON, 0x91, 0x13, 0x93):
                continue
            if value != ZDLE:
                payload.append(value)
                continue
            value = self.byte()
            while value in (XON, 0x91, 0x13, 0x93, ZDLE):
                value = self.byte()
            if value in (ZCRCE, ZCRCG, ZCRCQ, ZCRCW):
                frame_end = value
                break
            if value == 0x6C:
                payload.append(0x7F)
            elif value == 0x6D:
                payload.append(0xFF)
            elif value & 0x60 == 0x40:
                payload.append(value ^ 0x40)
            else:
                raise AssertionError(f"unexpected ZDLE sequence {value:#x}")
        if self.use_crc32:
            received_crc = int.from_bytes(
                bytes(self._data_byte() for _ in range(4)), "little")
            expected_crc = zlib.crc32(bytes(payload) + bytes((frame_end,))) \
                & 0xFFFFFFFF
        else:
            received_crc = (self._data_byte() << 8) | self._data_byte()
            expected_crc = crc16(bytes(payload) + bytes((frame_end,)))
        if received_crc != expected_crc:
            raise AssertionError("bad CRC in received data subpacket")
        if frame_end == ZCRCW and self.byte() != XON:
            raise AssertionError("missing XON after ZCRCW subpacket")
        return bytes(payload), frame_end


def finish_receiver(peer, process):
    peer.send(hex_header(ZFIN))
    frame_type, _, _ = peer.header()
    if frame_type != ZFIN:
        raise AssertionError(f"expected ZFIN, got {frame_type}")
    peer.send(b"OO")
    _, stderr = process.communicate(timeout=10)
    return process.returncode, stderr


class ZmodemTests(unittest.TestCase):
    def run_self_transfer(self, contents):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source = base / "source"
            destination = base / "destination"
            source.mkdir()
            destination.mkdir()
            names = []
            for index, content in enumerate(contents):
                name = f"boundary-{index}.bin"
                (source / name).write_bytes(content)
                names.append(name)

            sender_socket, receiver_socket = socket.socketpair()
            try:
                receiver = subprocess.Popen(
                    [str(ZMRX)], cwd=destination, stdin=receiver_socket,
                    stdout=receiver_socket, stderr=subprocess.PIPE,
                )
                sender = subprocess.Popen(
                    [str(ZMTX), *names], cwd=source, stdin=sender_socket,
                    stdout=sender_socket, stderr=subprocess.PIPE,
                )
            finally:
                sender_socket.close()
                receiver_socket.close()

            sender_stderr = sender.communicate(timeout=20)[1]
            receiver_stderr = receiver.communicate(timeout=20)[1]
            self.assertEqual(sender.returncode, 0, sender_stderr.decode(errors="replace"))
            self.assertEqual(receiver.returncode, 0, receiver_stderr.decode(errors="replace"))
            for name, content in zip(names, contents):
                self.assertEqual((destination / name).read_bytes(), content)

    def start_receiver(self, cwd, *options):
        local, remote = socket.socketpair()
        process = subprocess.Popen(
            [str(ZMRX), *options], cwd=cwd, stdin=local, stdout=local,
            stderr=subprocess.PIPE,
        )
        local.close()
        peer = Peer(remote)
        frame_type, _, _ = peer.header()
        self.assertEqual(frame_type, ZRINIT)
        return process, peer

    def begin_sender(self, cwd, name, *options, flags=3, buffer_size=0,
                     zf1=0):
        local, remote = socket.socketpair()
        process = subprocess.Popen(
            [str(ZMTX), *options, name], cwd=cwd, stdin=local, stdout=local,
            stderr=subprocess.PIPE,
        )
        local.close()
        peer = Peer(remote)
        frame_type, _, _ = peer.header()
        self.assertEqual(frame_type, ZRQINIT)
        zrinit = bytes((ZRINIT, buffer_size & 0xFF,
                        (buffer_size >> 8) & 0xFF, zf1, flags))
        peer.send(hex_header(ZRINIT, header=zrinit))
        frame_type, _, _ = peer.header()
        self.assertEqual(frame_type, ZFILE)
        peer.data()
        return process, peer, zrinit

    def start_sender(self, cwd, name, *options, flags=3, buffer_size=0,
                     zf1=0):
        process, peer, zrinit = self.begin_sender(
            cwd, name, *options, flags=flags, buffer_size=buffer_size,
            zf1=zf1)
        peer.send(hex_header(ZRPOS, 0))
        return process, peer, zrinit

    def finish_sender(self, peer, process, zrinit, expected_size):
        frame_type, position, _ = peer.header()
        self.assertEqual((frame_type, position), (ZEOF, expected_size))
        peer.send(hex_header(ZRINIT, header=zrinit))
        for _ in range(10):
            frame_type, _, _ = peer.header()
            if frame_type != ZEOF:
                break
            peer.send(hex_header(ZRINIT, header=zrinit))
        self.assertEqual(frame_type, ZFIN)
        peer.send(hex_header(ZFIN))
        for _ in range(10):
            first = peer.byte()
            if first == ord("O"):
                self.assertEqual(peer.byte(), ord("O"))
                break
            peer.buffer.insert(0, first)
            frame_type, _, _ = peer.header()
            self.assertEqual(frame_type, ZFIN)
        else:
            self.fail("sender did not finish after repeated ZFIN responses")
        stderr = process.communicate(timeout=10)[1]
        self.assertEqual(process.returncode, 0, stderr.decode(errors="replace"))
        return stderr

    def test_boundary_sized_self_transfers(self):
        sizes = (0, 1, 1023, 1024, 1025, 4097)
        self.run_self_transfer([
            bytes((index * 37 + size) & 0xFF for index in range(size))
            for size in sizes
        ])

    def test_sender_reports_progress_for_empty_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "empty.bin"
            source.write_bytes(b"")
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-v")
            try:
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                payload, frame_end = peer.data()
                self.assertEqual((payload, frame_end), (b"", ZCRCE))
                stderr = self.finish_sender(peer, process, zrinit, 0)
                self.assertIn(b"sending file", stderr)
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_accepts_zedzap_subpacket(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                payload = bytes(index & 0xFF for index in range(8192))
                info = b"zedzap.bin\0" + b"8192 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))
                peer.send(hex_header(ZDATA) + data_subpacket(payload, ZCRCE))
                peer.send(hex_header(ZEOF, len(payload)))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRINIT)
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0, stderr.decode(errors="replace"))
                self.assertEqual((Path(temporary) / "zedzap.bin").read_bytes(), payload)
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_adapts_toward_zedzap_maximum(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(20000))
            source = Path(temporary) / "zedzap.bin"
            source.write_bytes(payload)
            local, remote = socket.socketpair()
            process = subprocess.Popen(
                [str(ZMTX), "-8", source.name], cwd=temporary,
                stdin=local, stdout=local, stderr=subprocess.PIPE,
            )
            local.close()
            peer = Peer(remote)
            try:
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRQINIT)
                peer.send(hex_header(ZRINIT, header=bytes((ZRINIT, 0, 0, 0, 3))))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFILE)
                peer.data()
                peer.send(hex_header(ZRPOS, 0))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))

                chunks = []
                frame_end = ZCRCG
                while frame_end == ZCRCG:
                    chunk, frame_end = peer.data()
                    chunks.append(chunk)
                self.assertEqual([len(chunk) for chunk in chunks],
                                 [1024, 2048, 4096, 8192, 4640])
                self.assertEqual(b"".join(chunks), payload)
                self.assertEqual(frame_end, ZCRCE)

                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZEOF, len(payload)))
                peer.send(hex_header(ZRINIT, header=bytes((ZRINIT, 0, 0, 0, 3))))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(hex_header(ZFIN))
                self.assertEqual(bytes(peer.byte() for _ in range(2)), b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 0, stderr.decode(errors="replace"))
            finally:
                remote.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_preserves_all_bytes_with_negotiated_escaping(self):
        for escape_control in (False, True):
            with self.subTest(escape_control=escape_control), \
                    tempfile.TemporaryDirectory() as temporary:
                byte_cases = b"".join(
                    bytes((ord("@"), value, ord("A"), value))
                    for value in range(256)
                )
                payload = byte_cases * 20 + b"sentinel"
                source = Path(temporary) / "escaping.bin"
                source.write_bytes(payload)
                flags = 3 | (0x40 if escape_control else 0)
                process, peer, zrinit = self.start_sender(
                    temporary, source.name, "-8", flags=flags)
                try:
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZDATA, 0))
                    received = bytearray()
                    frame_end = ZCRCG
                    while frame_end == ZCRCG:
                        chunk, frame_end = peer.data()
                        received.extend(chunk)
                    self.assertEqual(
                        (bytes(received), frame_end), (payload, ZCRCE))
                    self.finish_sender(peer, process, zrinit, len(payload))
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_sender_debug_capability_reporting(self):
        management_options = (
            ("-p", b"protecting destination"),
            ("-o", b"overwriting destination"),
            ("-n", b"overwriting destination if newer"),
        )
        for flags in (0, 0xE7):
            for management, message in management_options:
                with self.subTest(flags=flags, management=management), \
                    tempfile.TemporaryDirectory() as temporary:
                    payload = b"debug capabilities"
                    source = Path(temporary) / "debug.bin"
                    source.write_bytes(payload)
                    process, peer, zrinit = self.start_sender(
                        temporary, source.name, "-d", management,
                        flags=flags, zf1=1 if flags else 0)
                    try:
                        frame_type, position, _ = peer.header()
                        self.assertEqual((frame_type, position), (ZDATA, 0))
                        chunk, frame_end = peer.data()
                        expected_end = ZCRCE if flags & 0x02 else ZCRCW
                        self.assertEqual((chunk, frame_end),
                                         (payload, expected_end))
                        if frame_end == ZCRCW:
                            peer.send(hex_header(ZACK, len(payload)))
                        stderr = self.finish_sender(
                            peer, process, zrinit, len(payload))
                        self.assertIn(message, stderr)
                        self.assertIn(b"receiver buffer size", stderr)
                        if flags:
                            self.assertIn(
                                b"receiver can use variable headers", stderr)
                    finally:
                        peer.sock.close()
                        if process.poll() is None:
                            process.kill()
                            process.wait()

    def test_receiver_requests_non_streaming_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            local, remote = socket.socketpair()
            process = subprocess.Popen(
                [str(ZMRX), "-s"], cwd=temporary, stdin=local, stdout=local,
                stderr=subprocess.PIPE,
            )
            local.close()
            peer = Peer(remote)
            try:
                frame_type, _, header = peer.header()
                self.assertEqual(frame_type, ZRINIT)
                self.assertEqual(int.from_bytes(header[1:3], "little"), 8192)
                self.assertEqual(header[4] & 0x02, 0)
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0, stderr.decode(errors="replace"))
            finally:
                remote.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_handles_session_control_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                peer.send(hex_header(ZRQINIT))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRINIT)

                peer.send(hex_header(ZACK))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZCOMPL)

                for frame_type in (ZRQINIT, ZEOF):
                    peer.send(hex_header(frame_type))
                    response_type, _, _ = peer.header()
                    self.assertEqual(response_type, ZRINIT)

                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_debug_and_pathname_options(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary, "-dvj")
            try:
                cases = (
                    (b"directory/junked.bin\0" +
                     b"6 0 0 0 1 0\0", "junked.bin", b"abc"),
                    (b"plain.bin\0\0", "plain.bin", b""),
                )
                for info, expected_name, payload in cases:
                    peer.send(hex_header(ZFILE) +
                              data_subpacket(info, ZCRCW))
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZRPOS, 0))
                    if payload:
                        time.sleep(1.1)
                    peer.send(hex_header(ZDATA) +
                              data_subpacket(payload, ZCRCE))
                    peer.send(hex_header(ZEOF, len(payload)))
                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZRINIT)
                    self.assertEqual(
                        (Path(temporary) / expected_name).read_bytes(), payload)

                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
                self.assertIn(b"contact established", stderr)
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_quiet_option_overrides_debug(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary, "-dvq")
            try:
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
                self.assertEqual(stderr, b"")
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_protect_management(self):
        cases = (("-p", 0), (None, ZF1_ZMPROT))
        for option, management in cases:
            with self.subTest(option=option, management=management), \
                    tempfile.TemporaryDirectory() as temporary:
                target = Path(temporary) / "protected.bin"
                target.write_bytes(b"original")
                options = () if option is None else (option,)
                process, peer = self.start_receiver(temporary, *options)
                try:
                    header = bytes((ZFILE, 0, 0, management, 0))
                    info = b"protected.bin\0" + b"3 0 0 0 1 0\0"
                    peer.send(hex_header(ZFILE, header=header) +
                              data_subpacket(info, ZCRCW))
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZSKIP, 0))
                    self.assertEqual(target.read_bytes(), b"original")
                    returncode, stderr = finish_receiver(peer, process)
                    self.assertEqual(returncode, 0,
                                     stderr.decode(errors="replace"))
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_receiver_clobber_management(self):
        cases = (("-o", 0), (None, ZF1_ZMCLOB))
        for option, management in cases:
            with self.subTest(option=option, management=management), \
                    tempfile.TemporaryDirectory() as temporary:
                target = Path(temporary) / "clobber.bin"
                target.write_bytes(b"original")
                options = () if option is None else (option,)
                process, peer = self.start_receiver(temporary, *options)
                try:
                    header = bytes((ZFILE, 0, 0, management, 0))
                    info = b"clobber.bin\0" + b"3 0 0 0 1 0\0"
                    peer.send(hex_header(ZFILE, header=header) +
                              data_subpacket(info, ZCRCW))
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZRPOS, 0))
                    peer.send(hex_header(ZDATA) +
                              data_subpacket(b"new", ZCRCE))
                    peer.send(hex_header(ZEOF, 3))
                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZRINIT)
                    self.assertEqual(target.read_bytes(), b"new")
                    returncode, stderr = finish_receiver(peer, process)
                    self.assertEqual(returncode, 0,
                                     stderr.decode(errors="replace"))
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_receiver_newer_management(self):
        cases = (
            ("-n", 0, b"3 1747 0 0 1 0\0", False),
            ("-n", 0, b"3\0", False),
            (None, ZF1_ZMNEW, b"3 1751 0 0 1 0\0", True),
        )
        for option, management, metadata, transferred in cases:
            with self.subTest(option=option, transferred=transferred), \
                    tempfile.TemporaryDirectory() as temporary:
                target = Path(temporary) / "newer.bin"
                target.write_bytes(b"old")
                os.utime(target, (1000, 1000))
                options = () if option is None else (option,)
                process, peer = self.start_receiver(temporary, *options)
                try:
                    header = bytes((ZFILE, 0, 0, management, 0))
                    info = b"newer.bin\0" + metadata
                    peer.send(hex_header(ZFILE, header=header) +
                              data_subpacket(info, ZCRCW))
                    frame_type, position, _ = peer.header()
                    if transferred:
                        self.assertEqual((frame_type, position), (ZRPOS, 0))
                        peer.send(hex_header(ZDATA) +
                                  data_subpacket(b"new", ZCRCE))
                        peer.send(hex_header(ZEOF, 3))
                        frame_type, _, _ = peer.header()
                        self.assertEqual(frame_type, ZRINIT)
                        self.assertEqual(target.read_bytes(), b"new")
                    else:
                        self.assertEqual((frame_type, position), (ZSKIP, 0))
                        self.assertEqual(target.read_bytes(), b"old")
                    returncode, stderr = finish_receiver(peer, process)
                    self.assertEqual(returncode, 0,
                                     stderr.decode(errors="replace"))
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_receiver_recovery_boundaries(self):
        cases = (
            ("unknown-size.bin", 3, b"-1 1750\0", 3, b"x", b"oldx"),
            ("short-remote.bin", 3, b"2 1750\0", 0, b"n", b"n"),
            ("large-local.bin", 0x100000001,
             b"4294967297 1750\0", 0, b"n", b"n"),
        )
        for name, local_size, metadata, position, payload, expected in cases:
            with self.subTest(name=name), \
                    tempfile.TemporaryDirectory() as temporary:
                target = Path(temporary) / name
                try:
                    with target.open("wb") as output:
                        if local_size == 3:
                            output.write(b"old")
                        else:
                            output.truncate(local_size)
                except OSError as error:
                    self.skipTest(f"sparse files are unavailable: {error}")
                os.utime(target, (1000, 1000))
                process, peer = self.start_receiver(temporary)
                try:
                    info = name.encode("ascii") + b"\0" + metadata
                    peer.send(hex_header(ZFILE) +
                              data_subpacket(info, ZCRCW))
                    frame_type, received_position, _ = peer.header()
                    self.assertEqual((frame_type, received_position),
                                     (ZRPOS, position))
                    peer.send(hex_header(ZDATA, position) +
                              data_subpacket(payload, ZCRCE))
                    peer.send(hex_header(ZEOF, position + len(payload)))
                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZRINIT)
                    self.assertEqual(target.read_bytes(), expected)
                    returncode, stderr = finish_receiver(peer, process)
                    self.assertEqual(returncode, 0,
                                     stderr.decode(errors="replace"))
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_receiver_retries_initial_handshake_after_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRINIT)
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_retries_invitation_after_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"invite-timeout.bin\0" + b"0 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))
                peer.send(hex_header(ZDATA) + data_subpacket(b"", ZCRCE) +
                          hex_header(ZEOF))
                for _ in range(2):
                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZRINIT)
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_reports_broken_invitation_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            peer.send(hex_header(ZEOF))
            peer.sock.close()
            stderr = process.communicate(timeout=10)[1]
            self.assertEqual(process.returncode, 4,
                             stderr.decode(errors="replace"))

    def test_receiver_accepts_garbage_before_over_and_out(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                peer.send(hex_header(ZFIN))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(b"garbageOfillerO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_reports_over_and_out_timeouts(self):
        for response in (b"", b"O"):
            with self.subTest(response=response), \
                    tempfile.TemporaryDirectory() as temporary:
                process, peer = self.start_receiver(temporary)
                try:
                    peer.send(hex_header(ZFIN))
                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZFIN)
                    peer.send(response)
                    stderr = process.communicate(timeout=10)[1]
                    self.assertEqual(process.returncode, 4,
                                     stderr.decode(errors="replace"))
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_sender_waits_for_each_non_streaming_ack(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(2500))
            source = Path(temporary) / "nonstream.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-s")
            try:
                offset = 0
                for expected_length in (1024, 1024, 452):
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZDATA, offset))
                    chunk, frame_end = peer.data()
                    self.assertEqual((len(chunk), frame_end),
                                     (expected_length, ZCRCW))
                    self.assertEqual(chunk, payload[offset:offset + expected_length])
                    offset += expected_length

                    peer.sock.settimeout(0.05)
                    with self.assertRaises(socket.timeout):
                        peer.byte()
                    peer.sock.settimeout(10)
                    peer.send(hex_header(ZACK, offset))

                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_automatically_honors_missing_overlap_capability(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = b"automatic non-streaming"
            source = Path(temporary) / "automatic.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, flags=1)
            try:
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                chunk, frame_end = peer.data()
                self.assertEqual((chunk, frame_end), (payload, ZCRCW))
                peer.send(hex_header(ZACK, len(payload)))
                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_honors_finite_receiver_buffer(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(4000))
            source = Path(temporary) / "bounded.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-4", buffer_size=1500)
            try:
                offset = 0
                expected = (
                    (1024, ZCRCG), (476, ZCRCW),
                    (1500, ZCRCW), (1000, ZCRCE),
                )
                for expected_length, expected_end in expected:
                    if offset in (0, 1500, 3000):
                        frame_type, position, _ = peer.header()
                        self.assertEqual((frame_type, position), (ZDATA, offset))
                    chunk, frame_end = peer.data()
                    self.assertEqual((len(chunk), frame_end),
                                     (expected_length, expected_end))
                    self.assertEqual(chunk, payload[offset:offset + expected_length])
                    offset += expected_length
                    if frame_end == ZCRCW:
                        peer.send(hex_header(ZACK, offset))

                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_enforces_fixed_window(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(6500))
            source = Path(temporary) / "window.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-w4K")
            try:
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                offset = 0
                for _ in range(4):
                    chunk, frame_end = peer.data()
                    self.assertEqual((len(chunk), frame_end), (1024, ZCRCQ))
                    self.assertEqual(chunk, payload[offset:offset + 1024])
                    offset += 1024

                peer.sock.settimeout(0.05)
                with self.assertRaises(socket.timeout):
                    peer.byte()
                peer.sock.settimeout(10)
                peer.send(hex_header(ZACK, 2048))

                for _ in range(2):
                    chunk, frame_end = peer.data()
                    self.assertEqual((len(chunk), frame_end), (1024, ZCRCQ))
                    self.assertEqual(chunk, payload[offset:offset + 1024])
                    offset += 1024

                peer.send(hex_header(ZACK, 1024))
                peer.sock.settimeout(0.05)
                with self.assertRaises(socket.timeout):
                    peer.byte()
                peer.sock.settimeout(10)
                peer.send(hex_header(ZACK, offset))

                chunk, frame_end = peer.data()
                self.assertEqual((chunk, frame_end), (payload[offset:], ZCRCE))
                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_window_falls_back_without_full_duplex(self):
        for verbose in (False, True):
            with self.subTest(verbose=verbose), \
                    tempfile.TemporaryDirectory() as temporary:
                payload = b"not full duplex"
                source = Path(temporary) / "fallback.bin"
                source.write_bytes(payload)
                options = ("-v", "-w4K") if verbose else ("-w4K",)
                process, peer, zrinit = self.start_sender(
                    temporary, source.name, *options, flags=2)
                try:
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZDATA, 0))
                    chunk, frame_end = peer.data()
                    self.assertEqual((chunk, frame_end), (payload, ZCRCW))
                    peer.send(hex_header(ZACK, len(payload)))
                    stderr = self.finish_sender(
                        peer, process, zrinit, len(payload))
                    if verbose:
                        self.assertIn(
                            b"receiver is not full duplex", stderr)
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_window_restarts_after_future_acknowledgement(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(1500))
            source = Path(temporary) / "future-ack.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-d", "-w4K")
            try:
                peer.send(hex_header(ZACK, 5000))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                chunk, frame_end = peer.data()
                self.assertEqual((chunk, frame_end), (payload[:1024], ZCRCQ))

                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                received = bytearray()
                frame_end = ZCRCG
                while frame_end == ZCRCG:
                    chunk, frame_end = peer.data()
                    received.extend(chunk)
                self.assertEqual((bytes(received), frame_end), (payload, ZCRCE))
                stderr = self.finish_sender(
                    peer, process, zrinit, len(payload))
                self.assertIn(b"invalid acknowledgement position", stderr)
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_window_rejects_future_acknowledgement_while_blocked(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(5000))
            source = Path(temporary) / "blocked-future-ack.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-w4500")
            try:
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                lengths = (1024, 1024, 1024, 1024, 404)
                for expected_length in lengths:
                    chunk, _ = peer.data()
                    self.assertEqual(len(chunk), expected_length)

                peer.send(hex_header(ZACK, 5001))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))

                received = bytearray()
                while True:
                    chunk, frame_end = peer.data()
                    received.extend(chunk)
                    if frame_end == ZCRCQ:
                        peer.send(hex_header(ZACK, len(received)))
                    if frame_end == ZCRCE:
                        break
                self.assertEqual(bytes(received), payload)
                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_window_restarts_after_reposition_request(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(5000))
            source = Path(temporary) / "window-reposition.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-d", "-w4K")
            try:
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                offset = 0
                for _ in range(4):
                    chunk, frame_end = peer.data()
                    self.assertEqual((len(chunk), frame_end),
                                     (1024, ZCRCQ))
                    self.assertEqual(chunk, payload[offset:offset + 1024])
                    offset += len(chunk)

                peer.send(hex_header(ZRPOS, 0))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))

                peer.send(hex_header(ZRPOS, 0))
                chunk, frame_end = peer.data()
                self.assertEqual((chunk, frame_end),
                                 (payload[:512], ZCRCG))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))

                received = bytearray()
                while True:
                    chunk, frame_end = peer.data()
                    received.extend(chunk)
                    if frame_end == ZCRCQ:
                        peer.send(hex_header(ZACK, len(received)))
                    if frame_end == ZCRCE:
                        break
                self.assertEqual(bytes(received), payload)
                stderr = self.finish_sender(
                    peer, process, zrinit, len(payload))
                self.assertIn(b"reducing data subpacket size", stderr)
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_rejects_invalid_window_options(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "unused.bin"
            source.write_bytes(b"unused")
            for options in (("-w",), ("-w-1",), ("-w0",),
                            ("-wgarbage",), ("-w4G",), ("-w4KB",),
                            ("-w3K",), ("-w4194304K",),
                            ("-w999999999999999999999999",),
                            ("-8", "-w16K"), ("-s", "-w4K")):
                with self.subTest(options=options):
                    result = subprocess.run(
                        [str(ZMTX), *options, source.name], cwd=temporary,
                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE, timeout=10,
                    )
                    self.assertEqual(result.returncode, 1)

    def test_sender_accepts_common_device_and_window_arguments(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "unused.bin"
            source.write_bytes(b"unused")
            for options in (("-w1M",), ("-l/dev/null",)):
                with self.subTest(options=options):
                    result = subprocess.run(
                        [str(ZMTX), *options, source.name], cwd=temporary,
                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE, timeout=10,
                    )
                    self.assertEqual(result.returncode, 3,
                                     result.stderr.decode(errors="replace"))

    def test_command_line_rejects_invalid_arguments(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "unused.bin"
            source.write_bytes(b"unused")
            cases = (
                (ZMTX, ("-x", source.name), 1),
                (ZMTX, ("-n", "-o", source.name), 1),
                (ZMTX, (), 1),
                (ZMTX, ("-l/path/that/does/not/exist", source.name), 2),
                (ZMRX, ("-x",), 1),
                (ZMRX, ("-n", "-o"), 1),
                (ZMRX, ("unexpected",), 1),
                (ZMRX, ("-l",), 2),
                (ZMRX, ("-l/dev/null",), 3),
            )
            for program, arguments, returncode in cases:
                with self.subTest(program=program.name, arguments=arguments):
                    result = subprocess.run(
                        [str(program), *arguments], cwd=temporary,
                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE, timeout=10,
                    )
                    self.assertEqual(result.returncode, returncode)

    @unittest.skipUnless(Path("/dev/full").exists(), "/dev/full is unavailable")
    def test_programs_report_initial_output_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "unused.bin"
            source.write_bytes(b"unused")
            cases = ((ZMTX, (source.name,)), (ZMRX, ()))
            for program, arguments in cases:
                with self.subTest(program=program.name), \
                        open("/dev/full", "wb", buffering=0) as full:
                    result = subprocess.run(
                        [str(program), *arguments], cwd=temporary,
                        stdin=subprocess.DEVNULL, stdout=full,
                        stderr=subprocess.PIPE, timeout=10,
                    )
                    self.assertEqual(result.returncode, 3,
                                     result.stderr.decode(errors="replace"))

    def test_receiver_acknowledges_committed_position_and_ignores_length_estimate(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"estimate.bin\0" + b"4294967296 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))

                peer.send(hex_header(ZEOF, 9))
                payload = b"protocol-data"
                peer.send(hex_header(ZDATA, 0) + data_subpacket(payload, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZACK, len(payload)))

                peer.send(hex_header(ZEOF, len(payload)))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRINIT)
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0, stderr.decode(errors="replace"))
                self.assertEqual((Path(temporary) / "estimate.bin").read_bytes(), payload)
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_resynchronizes_file_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"resync.bin\0" + b"6 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))

                peer.send(hex_header(ZFILE) +
                          data_subpacket(b"duplicate", ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))

                peer.send(hex_header(ZDATA, 1))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))

                corrupt = bytearray(data_subpacket(b"bad", ZCRCE))
                corrupt[-1] ^= 1
                peer.send(hex_header(ZDATA) + corrupt)
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))

                peer.send(hex_header(ZDATA) +
                          data_subpacket(b"abc", ZCRCQ) +
                          data_subpacket(b"def", ZCRCE) +
                          hex_header(ZEOF, 6))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZACK, 3))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRINIT)
                self.assertEqual(
                    (Path(temporary) / "resync.bin").read_bytes(), b"abcdef")
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_recovers_after_data_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                payload = b"after timeout"
                info = b"timeout.bin\0" + b"13 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))

                peer.send(binary32_header(ZDATA))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))

                peer.send(binary32_header(ZDATA) +
                          data_subpacket32(payload, ZCRCE) +
                          hex_header(ZEOF, len(payload)))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRINIT)
                self.assertEqual(
                    (Path(temporary) / "timeout.bin").read_bytes(), payload)
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_stops_after_repeated_bad_positions(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"bad-position.bin\0" + b"1 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))
                for attempt in range(10):
                    peer.send(hex_header(ZDATA, attempt + 1))
                    frame_type, position, _ = peer.header()
                    if attempt != 9:
                        self.assertEqual((frame_type, position), (ZRPOS, 0))
                    else:
                        self.assertEqual((frame_type, position), (ZFERR, 0))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_stops_after_repeated_corrupt_packets(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"bad-packets.bin\0" + b"1 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))
                for attempt in range(10):
                    corrupt = bytearray(data_subpacket(b"bad", ZCRCE))
                    corrupt[-1] ^= 1
                    peer.send(hex_header(ZDATA) + corrupt)
                    frame_type, position, _ = peer.header()
                    if attempt != 9:
                        self.assertEqual((frame_type, position), (ZRPOS, 0))
                    else:
                        self.assertEqual((frame_type, position), (ZFERR, 0))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_reports_file_open_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary, "-v")
            try:
                info = b"missing/file.bin\0" + b"1 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZFERR, 0))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4,
                                 stderr.decode(errors="replace"))
                self.assertIn(b"can't open file", stderr)
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_handles_remote_file_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"remote-error.bin\0" + b"1 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))
                peer.send(hex_header(ZFERR))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_rejects_malformed_and_oversized_file_info(self):
        for info in (b"\0", b"abc\0", b"\0\0", b"unterminated",
                     b"a" * 1025):
            with self.subTest(length=len(info)), tempfile.TemporaryDirectory() as temporary:
                process, peer = self.start_receiver(temporary)
                try:
                    peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZNAK, 0))
                    returncode, stderr = finish_receiver(peer, process)
                    self.assertEqual(returncode, 0, stderr.decode(errors="replace"))
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_receiver_rejects_empty_junked_pathname(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary, "-j")
            try:
                peer.send(hex_header(ZFILE) +
                          data_subpacket(b"/\0\0", ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZSKIP, 0))
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_tolerates_invalid_file_metadata(self):
        metadata_cases = (
            b"   0 -1\0",
            b"-1 0\0",
            b"x 0\0",
            b"999999999999999999999999999999999999999 0\0",
            b"0 x\0",
            b"0 999999999999999999999999999999999999999\0",
            b"0 1777777777777777777777\0",
            b"0 777777777777777777777777777777777777777\0",
            b"0 1x\0",
            b"0    1\0",
        )
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                for index, metadata in enumerate(metadata_cases):
                    name = f"metadata-{index}.bin"
                    info = name.encode("ascii") + b"\0" + metadata
                    peer.send(hex_header(ZFILE) +
                              data_subpacket(info, ZCRCW))
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZRPOS, 0))
                    peer.send(hex_header(ZDATA) +
                              data_subpacket(b"", ZCRCE))
                    peer.send(hex_header(ZEOF))
                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZRINIT)
                    self.assertEqual((Path(temporary) / name).read_bytes(), b"")
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_rejects_wrong_file_info_terminator(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"wrong-end.bin\0" + b"0 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCE))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZNAK, 0))
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_consumes_oversized_crc32_subpacket(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"a" * 8193
                peer.send(binary32_header(ZFILE) +
                          data_subpacket32(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZNAK, 0))
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_consumes_oversized_crc16_subpacket(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"a" * 8193
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZNAK, 0))
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_skips_a_pathname_that_does_not_fit(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"n" * 128 + b"\0" + b"0 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZSKIP, 0))
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0, stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_accepts_maximum_length_pathname(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                name = "n" * 127
                info = name.encode("ascii") + b"\0" + b"0 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))
                peer.send(hex_header(ZDATA) + data_subpacket(b"", ZCRCE))
                peer.send(hex_header(ZEOF))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRINIT)
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0,
                                 stderr.decode(errors="replace"))
                self.assertEqual((Path(temporary) / name).read_bytes(), b"")
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_rejects_position_overflow_independent_of_size_t(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "offset.bin"
            try:
                with target.open("wb") as output:
                    output.truncate(0xFFFFFFFE)
            except OSError as error:
                self.skipTest(f"large sparse files are unavailable: {error}")
            os.utime(target, (1000, 1000))

            process, peer = self.start_receiver(temporary)
            try:
                info = b"offset.bin\0" + b"4294967295 1750 0 0 1 0\0"
                peer.send(hex_header(ZFILE) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0xFFFFFFFE))
                peer.send(hex_header(ZDATA, position) + data_subpacket(b"xx", ZCRCE))
                frame_type, error_position, _ = peer.header()
                self.assertEqual((frame_type, error_position), (ZFERR, position))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4, stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_receiver_accepts_parity_marked_hex_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"parity.bin\0" + b"0 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE, parity=True) + data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))
                peer.send(hex_header(ZDATA, parity=True) + data_subpacket(b"", ZCRCE))
                peer.send(hex_header(ZEOF, parity=True))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRINIT)
                returncode, stderr = finish_receiver(peer, process)
                self.assertEqual(returncode, 0, stderr.decode(errors="replace"))
                self.assertEqual((Path(temporary) / "parity.bin").read_bytes(), b"")
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    @unittest.skipUnless(Path("/dev/full").exists(), "/dev/full is unavailable")
    def test_receiver_reports_immediate_write_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                payload = bytes(index & 0xFF for index in range(8192))
                info = b"/dev/full\0" + b"8192 0 0 0 1 0\0"
                peer.send(hex_header(
                    ZFILE, header=bytes((ZFILE, 1, 0, 0, 0))) +
                    data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))
                peer.send(hex_header(ZDATA) +
                          data_subpacket(payload, ZCRCE))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZFERR, 0))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    @unittest.skipUnless(Path("/dev/full").exists(), "/dev/full is unavailable")
    def test_receiver_reports_delayed_write_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer = self.start_receiver(temporary)
            try:
                info = b"/dev/full\0" + b"3 0 0 0 1 0\0"
                peer.send(hex_header(ZFILE, header=bytes((ZFILE, 1, 0, 0, 0))) +
                          data_subpacket(info, ZCRCW))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZRPOS, 0))
                peer.send(hex_header(ZDATA) + data_subpacket(b"bad", ZCRCE))
                peer.send(hex_header(ZEOF, 3))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZFERR, 3))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4, stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_rejects_out_of_range_resume_position(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "small.bin"
            source.write_bytes(b"small")
            local, remote = socket.socketpair()
            process = subprocess.Popen(
                [str(ZMTX), source.name], cwd=temporary, stdin=local, stdout=local,
                stderr=subprocess.PIPE,
            )
            local.close()
            peer = Peer(remote)
            try:
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRQINIT)
                peer.send(hex_header(ZRINIT, header=bytes((ZRINIT, 0, 0, 0, 3))))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFILE)
                peer.data()
                peer.send(hex_header(ZRPOS, 6))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZFERR, 6))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(hex_header(ZFIN))
                self.assertEqual(bytes(peer.byte() for _ in range(2)), b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4, stderr.decode(errors="replace"))
            finally:
                remote.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_reports_unavailable_files(self):
        for kind in ("missing", "oversized"):
            with self.subTest(kind=kind), \
                    tempfile.TemporaryDirectory() as temporary:
                name = "missing.bin"
                if kind == "oversized":
                    name = "oversized.bin"
                    try:
                        with (Path(temporary) / name).open("wb") as output:
                            output.truncate(0x100000000)
                    except OSError as error:
                        self.skipTest(
                            f"large sparse files are unavailable: {error}")
                local, remote = socket.socketpair()
                process = subprocess.Popen(
                    [str(ZMTX), "-v", name], cwd=temporary, stdin=local,
                    stdout=local, stderr=subprocess.PIPE,
                )
                local.close()
                peer = Peer(remote)
                try:
                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZRQINIT)
                    peer.send(hex_header(
                        ZRINIT, header=bytes((ZRINIT, 0, 0, 0, 3))))
                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZFIN)
                    peer.send(hex_header(ZFIN))
                    self.assertEqual(
                        bytes(peer.byte() for _ in range(2)), b"OO")
                    stderr = process.communicate(timeout=10)[1]
                    self.assertEqual(process.returncode, 4,
                                     stderr.decode(errors="replace"))
                    expected = b"too large" if kind == "oversized" \
                        else b"can't open"
                    self.assertIn(expected, stderr)
                finally:
                    remote.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_sender_reports_payload_read_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            process, peer, _ = self.begin_sender(temporary, ".")
            try:
                peer.send(hex_header(ZRPOS))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZFERR, 0))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(hex_header(ZFIN))
                self.assertEqual(bytes(peer.byte() for _ in range(2)), b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_handles_pathname_and_pre_epoch_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "directory"
            directory.mkdir()
            source = directory / "old.bin"
            source.write_bytes(b"old timestamp")
            try:
                os.utime(source, (-1, -1))
            except (OSError, OverflowError) as error:
                self.skipTest(f"pre-epoch timestamps are unavailable: {error}")
            process, peer, zrinit = self.start_sender(
                temporary, "directory/old.bin")
            try:
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                payload, frame_end = peer.data()
                self.assertEqual((payload, frame_end),
                                 (b"old timestamp", ZCRCE))
                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_handles_stale_init_and_resume_after_eof(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = b"resume-after-eof"
            source = Path(temporary) / "resume.bin"
            source.write_bytes(payload)
            local, remote = socket.socketpair()
            process = subprocess.Popen(
                [str(ZMTX), source.name], cwd=temporary, stdin=local, stdout=local,
                stderr=subprocess.PIPE,
            )
            local.close()
            peer = Peer(remote)
            try:
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRQINIT)
                zrinit = bytes((ZRINIT, 0, 0, 0, 3))
                peer.send(hex_header(ZRINIT, header=zrinit))

                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFILE)
                peer.data()
                peer.send(hex_header(ZRINIT, header=zrinit))
                peer.send(hex_header(ZRPOS, 0))

                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                received, frame_end = peer.data()
                self.assertEqual((received, frame_end), (payload, ZCRCE))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZEOF, len(payload)))

                resume_position = 3
                peer.send(hex_header(ZRPOS, resume_position))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, resume_position))
                received, frame_end = peer.data()
                self.assertEqual((received, frame_end),
                                 (payload[resume_position:], ZCRCE))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZEOF, len(payload)))

                peer.send(hex_header(ZRINIT, header=zrinit))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(hex_header(ZFIN))
                self.assertEqual(bytes(peer.byte() for _ in range(2)), b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 0, stderr.decode(errors="replace"))
            finally:
                remote.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_accepts_ack_before_initial_position(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = b"initial acknowledgement"
            source = Path(temporary) / "initial-ack.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.begin_sender(
                temporary, source.name)
            try:
                peer.send(hex_header(ZACK))
                peer.send(hex_header(ZRPOS))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                chunk, frame_end = peer.data()
                self.assertEqual((chunk, frame_end), (payload, ZCRCE))
                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_honors_initial_skip(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "skip.bin"
            source.write_bytes(b"skip")
            process, peer, _ = self.begin_sender(
                temporary, source.name, "-v")
            try:
                peer.send(hex_header(ZSKIP))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(hex_header(ZFIN))
                self.assertEqual(bytes(peer.byte() for _ in range(2)), b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_honors_skip_during_file_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(1500))
            source = Path(temporary) / "stream-skip.bin"
            source.write_bytes(payload)
            process, peer, _ = self.begin_sender(temporary, source.name)
            try:
                peer.send(hex_header(ZRPOS, 0) + hex_header(ZSKIP))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                chunk, frame_end = peer.data()
                self.assertEqual((chunk, frame_end),
                                 (payload[:1024], ZCRCG))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(hex_header(ZFIN))
                self.assertEqual(bytes(peer.byte() for _ in range(2)), b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 0,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_nonstreaming_sender_honors_reposition(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(20000))
            source = Path(temporary) / "nonstream-reposition.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-s", "-8")
            try:
                offset = 0
                for expected_length in (1024, 2048, 4096, 8192):
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position),
                                     (ZDATA, offset))
                    chunk, frame_end = peer.data()
                    self.assertEqual((chunk, frame_end),
                                     (payload[offset:offset + expected_length],
                                      ZCRCW))
                    offset += expected_length
                    response = ZRPOS if expected_length == 8192 else ZACK
                    response_position = 0 if response == ZRPOS else offset
                    peer.send(hex_header(response, response_position))

                offset = 0
                for expected_length in (4096, 4096, 4096, 4096, 3616):
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position),
                                     (ZDATA, offset))
                    chunk, frame_end = peer.data()
                    self.assertEqual((chunk, frame_end),
                                     (payload[offset:offset + expected_length],
                                      ZCRCW))
                    offset += expected_length
                    peer.send(hex_header(ZACK, offset))

                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_exhausts_file_header_retries(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "retry.bin"
            source.write_bytes(b"retry")
            process, peer, _ = self.begin_sender(temporary, source.name)
            try:
                for attempt in range(10):
                    if attempt != 0:
                        frame_type, _, _ = peer.header()
                        self.assertEqual(frame_type, ZFILE)
                        peer.data()
                    peer.send(hex_header(ZNAK))
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFIN)
                peer.send(hex_header(ZFIN))
                self.assertEqual(bytes(peer.byte() for _ in range(2)), b"OO")
                stderr = process.communicate(timeout=10)[1]
                self.assertEqual(process.returncode, 4,
                                 stderr.decode(errors="replace"))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_retries_after_polled_nak(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = bytes(index & 0xFF for index in range(1500))
            source = Path(temporary) / "polled-nak.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-w4K")
            try:
                peer.send(hex_header(ZNAK))
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                first, frame_end = peer.data()
                self.assertEqual((first, frame_end),
                                 (payload[:1024], ZCRCQ))

                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                received = bytearray()
                frame_end = ZCRCG
                while frame_end == ZCRCG:
                    chunk, frame_end = peer.data()
                    received.extend(chunk)
                self.assertEqual((bytes(received), frame_end),
                                 (payload, ZCRCE))
                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_handles_eof_ack_and_skip(self):
        for response in (ZACK, ZSKIP):
            with self.subTest(response=response), \
                    tempfile.TemporaryDirectory() as temporary:
                payload = b"finish response"
                source = Path(temporary) / "finish.bin"
                source.write_bytes(payload)
                process, peer, zrinit = self.start_sender(
                    temporary, source.name)
                try:
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZDATA, 0))
                    chunk, frame_end = peer.data()
                    self.assertEqual((chunk, frame_end), (payload, ZCRCE))
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position),
                                     (ZEOF, len(payload)))
                    peer.send(hex_header(response))
                    if response == ZACK:
                        frame_type, position, _ = peer.header()
                        self.assertEqual((frame_type, position),
                                         (ZEOF, len(payload)))
                        peer.send(hex_header(ZRINIT, header=zrinit))
                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZFIN)
                    peer.send(hex_header(ZFIN))
                    self.assertEqual(
                        bytes(peer.byte() for _ in range(2)), b"OO")
                    stderr = process.communicate(timeout=10)[1]
                    self.assertEqual(process.returncode, 0,
                                     stderr.decode(errors="replace"))
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_sender_handles_failed_eof_completion(self):
        for response in (ZFERR, ZACK):
            with self.subTest(response=response), \
                    tempfile.TemporaryDirectory() as temporary:
                payload = b"failed finish response"
                source = Path(temporary) / "failed-finish.bin"
                source.write_bytes(payload)
                process, peer, _ = self.start_sender(
                    temporary, source.name)
                try:
                    frame_type, position, _ = peer.header()
                    self.assertEqual((frame_type, position), (ZDATA, 0))
                    chunk, frame_end = peer.data()
                    self.assertEqual((chunk, frame_end), (payload, ZCRCE))

                    attempts = 10 if response == ZACK else 1
                    for _ in range(attempts):
                        frame_type, position, _ = peer.header()
                        self.assertEqual((frame_type, position),
                                         (ZEOF, len(payload)))
                        peer.send(hex_header(response))

                    frame_type, _, _ = peer.header()
                    self.assertEqual(frame_type, ZFIN)
                    peer.send(hex_header(ZFIN))
                    self.assertEqual(
                        bytes(peer.byte() for _ in range(2)), b"OO")
                    stderr = process.communicate(timeout=10)[1]
                    self.assertEqual(process.returncode, 4,
                                     stderr.decode(errors="replace"))
                finally:
                    peer.sock.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def test_sender_ignores_stale_nonstreaming_ack(self):
        with tempfile.TemporaryDirectory() as temporary:
            payload = b"stale block acknowledgement"
            source = Path(temporary) / "stale-block.bin"
            source.write_bytes(payload)
            process, peer, zrinit = self.start_sender(
                temporary, source.name, "-s")
            try:
                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                chunk, frame_end = peer.data()
                self.assertEqual((chunk, frame_end), (payload, ZCRCW))
                peer.send(hex_header(ZACK, 0))
                peer.send(hex_header(ZACK, len(payload)))
                self.finish_sender(peer, process, zrinit, len(payload))
            finally:
                peer.sock.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_retries_after_repeated_stale_init(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "stale.bin"
            source.write_bytes(b"stale initialization")
            local, remote = socket.socketpair()
            process = subprocess.Popen(
                [str(ZMTX), source.name], cwd=temporary, stdin=local,
                stdout=local, stderr=subprocess.PIPE,
            )
            local.close()
            peer = Peer(remote)
            try:
                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZRQINIT)
                zrinit = hex_header(
                    ZRINIT, header=bytes((ZRINIT, 0, 0, 0, 3)))
                peer.send(zrinit)

                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFILE)
                peer.data()
                peer.send(zrinit + zrinit)

                frame_type, _, _ = peer.header()
                self.assertEqual(frame_type, ZFILE)
                peer.data()
                peer.send(hex_header(ZRPOS, 0))

                frame_type, position, _ = peer.header()
                self.assertEqual((frame_type, position), (ZDATA, 0))
                payload, frame_end = peer.data()
                self.assertEqual((payload, frame_end),
                                 (b"stale initialization", ZCRCE))
                self.finish_sender(peer, process,
                                   bytes((ZRINIT, 0, 0, 0, 3)),
                                   len(payload))
            finally:
                remote.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_sender_reports_broken_protocol_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "broken.bin"
            source.write_bytes(b"broken output")
            local, remote = socket.socketpair()
            process = subprocess.Popen(
                [str(ZMTX), source.name], cwd=temporary, stdin=local, stdout=local,
                stderr=subprocess.PIPE,
            )
            local.close()
            peer = Peer(remote)
            frame_type, _, _ = peer.header()
            self.assertEqual(frame_type, ZRQINIT)
            peer.send(hex_header(ZRINIT, header=bytes((ZRINIT, 0, 0, 0, 3))))
            remote.close()
            stderr = process.communicate(timeout=10)[1]
            self.assertEqual(process.returncode, 4, stderr.decode(errors="replace"))


if __name__ == "__main__":
    unittest.main(verbosity=2)

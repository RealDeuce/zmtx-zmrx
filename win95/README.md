# Windows 95 port

This is a secondary Open Watcom port for 32-bit Windows 95. POSIX remains the
only first-class platform.

After loading the Open Watcom environment, build with its `wmake` rather than
the system make:

```bat
wmake -f makefile.win95
```

The build produces 386-compatible `build/win95/zmtx.exe` and
`build/win95/zmrx.exe` console programs. It retains the normal 8 KiB transfer
buffers and slicing-by-8 CRC implementation. The executables use Open
Watcom's static C runtime and Winsock 1.1 from `WSOCK32.DLL`.

## Passed transports

The launcher must pass exactly one already-open communications object as an
attached unsigned decimal value:

- `-cHANDLE` uses a borrowed bidirectional Win32 COM handle.
- `-tSOCKET` uses a borrowed, connected Winsock socket.
- `-i` optionally sends an outbound `0xff` byte as the standard ZMODEM
  `ZDLE ZRUB1` sequence. It does not parse, negotiate, or otherwise implement
  Telnet.

The launcher must make the object inheritable and keep the numeric value valid
in the child process. zmtx/zmrx never close passed handles or sockets. A COM
port's baud rate, framing, and flow control remain unchanged; only its read
timeouts are temporarily changed and then restored. A passed socket must
already be connected, and any incoming Telnet negotiation must be handled
elsewhere.

Windows 95 filesystem behavior is supplied by Open Watcom's Win32 C runtime.
Its signed 32-bit `off_t` limits individual files to approximately 2 GiB even
though the ZMODEM wire format can represent larger positions.

## Tests

`wmake -f makefile.win95 check` verifies the PE32 machine, console subsystem
4.0, static runtime DLL set, and absence of selected post-Windows-95 APIs.
`check-runtime` also runs COM API tests and a real loopback Winsock test:

```bat
wmake -f makefile.win95 check-runtime
```

When cross-building on a system with Wine, supply it as the runner:

```sh
wmake -f makefile.win95 check-runtime WIN95_RUNNER=wine PYTHON=python3
```

CI exercises the build and transport tests on a current 32-bit-compatible
Windows environment. Actual Windows 95 serial drivers and hardware still
require a manual smoke test.

# Omen ESC8 and RLE disassembly evidence

The public ZMODEM text names `ESC8`, `ZBINR32`, and `ZTRLE`, but does not
fully specify Omen's seven-bit wire format. The implementation in this tree is
therefore based on the 1997 Omen Technology `DSZ.EXE`, treated as normative,
and not on behavioral guessing.

The researched executable is the `DSZ.EXE` distributed in `dszexe.zip` at
<https://www.bbsing.com/bbsprotocols/>. Neither archive nor executable is
redistributed here.

| File | SHA-256 |
| --- | --- |
| `dszexe.zip` | `168eaead460b36f5ed50247f03cb0641ad331fdd7800e59880b0ee83adfdae1c` |
| `DSZ.EXE` | `a1bc1343858c98661c5252e307dca1d0f084ead39c3d6c191c2b2842e56f7c8b` |

Run `python3 tools/verify_dsz_esc8.py DSZ.EXE --disassemble` to verify the
exact binary and reproduce the relevant 16-bit x86 disassembly. The verifier
checks the complete executable, MZ load offset, each cited routine, and the
CRC string independently before invoking `ndisasm`.

After building the native programs, an optional two-direction DOSBox check is
available as `python3 tools/test_dsz_esc8.py DSZ.EXE`. It verifies the binary,
runs DSZ as both sender and receiver with `-E`, and checks an all-byte and
RLE-heavy payload. DSZ is deliberately absent from the repository and CI.

## Recovered format

The MZ header occupies `0x200` bytes. Addresses below are offsets in the load
image and therefore match the displayed `CS` offsets.

| Address | Function established by data flow and callers |
| --- | --- |
| `CS:6e12-6fa7` | Seven-bit encoder |
| `CS:6fa8-70db` | Header-format dispatcher; mode 4 selects byte `0x31` |
| `CS:71d4-72b9` | `0x31` header encoder |
| `CS:7b8e-7d5e` | Header receiver dispatch; byte `0x31` selects mode 4 |
| `CS:807a-81a9` | `0x31` header and CRC decoder |
| `CS:8642-8777` | Seven-bit quoted-byte decoder |
| `CS:8a7e-8d31` | RLE, CRC32, and seven-bit data encoder |
| `CS:8f9a-91f7` | Seven-bit, CRC32, and RLE data decoder |

An `0x31` header begins with `ZPAD ZDLE 0x31`. The next raw seven-bit byte is
`0x22 + parameter_count`; a normal four-parameter header therefore uses
`0x26`. Header type and parameters are quoted, followed by little-endian
CRC32. In addition to the header bytes, that CRC covers the literal string:

```
Copyright 1989 Omen Technology INC All Rights Reserved
```

The encoder at `CS:6e12` emits `SO` (`0x0e`) followed by the low seven bits for
an ordinary high-bit byte. Values that collide with framing have dedicated
`ZDLE` sequences: `l`=`0x7f`, `m`=`0xff`, `n`=`0x0e`, `o`=`0x8e`,
`p`=`0x90`, `q`=`0x91`, `r`=`0x93`, `s`=`0x80`, and `t`=`0x98`.
Ordinary escaped controls retain the standard `ZDLE` plus XOR-`0x40` form.
With `ESCCTL`, a high-bit control may consequently occupy three wire bytes:
`SO ZDLE (low7 ^ 0x40)`.

Mode 4 always applies RLE before seven-bit quoting. CRC32 covers the RLE token
stream, then the subpacket terminator; it does not cover the expanded file
bytes. `ZRESC` (`0x7e`) introduces RLE:

- `ZRESC 0x40` represents one literal `ZRESC`.
- `ZRESC (run + 0x1d)` represents a run of 3 through 34 spaces.
- `ZRESC (run + 0x40) value` represents 2 through 63 copies of `value`.
- A two-byte run of an ordinary low-seven-bit value is left literal.

Longer runs are divided at 63 bytes. The receiver routines perform the exact
inverse and check CRC32 over tokens before accepting the expanded output.

## Independent cross-check

Qodem's current
[`source/zmodem.c`](https://codeberg.org/AutumnMeowMeow/qodem/src/branch/main/source/zmodem.c)
implements the older `ZDLE` plus XOR-`0x40` interpretation of `ESC8`. It has no
`0x31`, SO quoting, or Omen RLE framing. This is useful evidence for retaining
that interpretation as a receive-only legacy mode, but it cannot define the
normative Omen format. The two forms are safely distinguished by their header
indicator.

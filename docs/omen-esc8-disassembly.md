# Omen ESC8, Pack-7, RLE, and MobyTurbo disassembly evidence

The public ZMODEM text names `ESC8`, `ZBINR32`, and `ZTRLE`, but does not
fully specify Omen's seven-bit wire format. The implementation in this tree is
therefore based on the 1997 Omen Technology `DSZ.EXE`, treated as normative,
and not on behavioral guessing.

The same executable defines Pack-7 (`0x32`) and the private MobyTurbo (`0x33`)
transparent mode. No whole-frame format was inferred from black-box output.

The researched executable is the `DSZ.EXE` distributed in `dszexe.zip` at
<https://www.bbsing.com/bbsprotocols/>. Neither archive nor executable is
redistributed here.

| File | SHA-256 |
| --- | --- |
| `dszexe.zip` | `168eaead460b36f5ed50247f03cb0641ad331fdd7800e59880b0ee83adfdae1c` |
| `DSZ.EXE` | `a1bc1343858c98661c5252e307dca1d0f084ead39c3d6c191c2b2842e56f7c8b` |

Run `python3 tools/verify_dsz_esc8.py DSZ.EXE --disassemble` to verify the
exact binary and reproduce the relevant 16-bit x86 disassembly. The verifier
checks the complete executable, MZ load offset, the ESC8 and Pack-7 routine
ranges, and the CRC string independently before invoking `ndisasm`; its output
also includes every MobyTurbo range cited below.

After building the native programs, an optional two-direction DOSBox check is
available as `python3 tools/test_dsz_esc8.py DSZ.EXE`. It verifies the binary,
runs DSZ as both sender and receiver with `-E`, `-EP`, and `-m`. The ESC8 and
Pack-7 cases pass through a relay which actually clears bit 7. DSZ is
deliberately absent from the repository and CI.

## Recovered format

The MZ header occupies `0x200` bytes. Addresses below are offsets in the load
image and therefore match the displayed `CS` offsets.

| Address | Function established by data flow and callers |
| --- | --- |
| `CS:6e12-6fa7` | Seven-bit encoder |
| `CS:6fa8-70db` | Header-format dispatcher; mode 4 selects byte `0x31` |
| `CS:71d4-72b9` | Shared `0x31`/`0x32` header encoder |
| `CS:7b8e-7d5e` | Header receiver dispatch; byte `0x31` selects mode 4 |
| `CS:807a-81a9` | Shared `0x31`/`0x32` header and CRC decoder |
| `CS:8642-8777` | Seven-bit quoted-byte decoder |
| `CS:8a7e-8d31` | RLE, CRC32, and seven-bit data encoder |
| `CS:8f9a-91f7` | Seven-bit, CRC32, and RLE data decoder |

Pack-7 adds these authenticated paths:

| Address | Function established by data flow and callers |
| --- | --- |
| `CS:0a56-0a5e` | `-P` enables Pack-7 preference |
| `CS:5450-547e` | Extended `ZRPOS` bits select Pack-7, MobyTurbo, or RLE |
| `CS:6681-6695` | Receiver requests Pack-7 with parameter-six bit `0x02` |
| `CS:72ba-739b` | Pack-7 payload CRC32 and subpacket encoder |
| `CS:739c-741d` | One-to-four-byte base-88 group encoder |
| `CS:81aa-831d` | Pack-7 subpacket, terminator, and CRC32 decoder |
| `CS:831e-83a1` | Base-88 group decoder |

MobyTurbo is mode 3 in the same dispatcher. Its additional paths are:

| Address | Function established by data flow and callers |
| --- | --- |
| `CS:0a44`, `CS:0b2c` | `-M` veto and `-m` request option state |
| `CS:5121-5160` | Set `ZFILE.ZF3` bit `0x04` and transmit `23 c1 d4 93 11` probe |
| `CS:5450-547e` | Sender selects mode 3 from extended `ZRPOS` parameter six bit 0 |
| `CS:6696-66cc` | Receiver emits the seven-parameter `ZRPOS` request |
| `CS:6fac-71d3` | Header dispatcher and `0x33` salted-CRC32 encoder |
| `CS:761b-7646` | Transparent data encoder: quote `ZDLE`, pass other bytes |
| `CS:76d5-7803` | MobyTurbo data receive and CRC32 path |
| `CS:7cc2-8055` | Variable `0x33` header receiver and salted CRC32 check |
| `CS:850e-8641` | Mode-3 quoted-byte decoder, retaining raw flow-control bytes |

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

## Pack-7 wire format

Mode 5 uses the same header layout, seven-bit quoting, copyright suffix, and
little-endian salted CRC32 as mode 4, but sends indicator `0x32`. The header
length byte is likewise `0x22 + parameter_count`.

The encoder treats each group of one through four payload bytes as one
big-endian unsigned integer. It repeatedly divides by 88, adds `0x22` to each
remainder, and emits exactly one more digit than the input byte count in
most-significant-first order. Thus every wire digit is in `0x22` through
`0x79`; four input bytes always produce five digits. There is no RLE or SO
quoting in Pack-7 data.

After the final full or partial group, the sender emits `0x21` and then the
raw subpacket terminator, without `ZDLE`. CRC32 covers the original decoded
payload bytes followed by that terminator. The complemented CRC's four
little-endian bytes are sent as one final five-digit Pack-7 group. `ZCRCW`
retains its trailing `XON`.

The receiver consumes five digits for every full group. A preceding group of
two, three, or four digits followed by `0x21` yields one, two, or three final
bytes; `0x21` by itself ends a group-aligned or empty packet. A terminator
before `0x21`, a non-terminator byte after it, or a CRC group other than five
digits is malformed.

DSZ is permissive on corrupt input: its group routine counts up to five
decoded symbols, accepts `0x20` through `0xac` around the nominal alphabet,
and allows its 32-bit accumulator to wrap. The implementation in this tree
intentionally accepts only canonical `0x22`-through-`0x79` digits, rejects
one-digit partial groups and numeric overflow, and still matches DSZ for every
valid frame.

The authenticated disassembly establishes the group widths, accumulator order,
raw `0x21` partial-group termination, CRC byte order, and extended-`ZRPOS` mode
transition. For an optional live cross-check, `tools/test_dsz_esc8.py` accepts
`--mode Pack-7`, `--role`, `--fixture`, and `--debug-hold`; the hold leaves DSZ
waiting on its serial connection so heavy-debugger breakpoints can be set at
`CS:739c`, `CS:831e`, or their callers before the relay starts. The ordinary
interoperability run cross-checks the recovered paths with DSZ `-EP` in both
roles.

## MobyTurbo negotiation and wire format

DSZ calls internal mode 3 “MobyTurbo.” A sender emits the five raw bytes
`23 c1 d4 93 11` immediately before `ZFILE` and uses `ZFILE.ZF3` bit `0x04`
to offer the mode. The receiver records success only when all five values,
including high-bit flow-control lookalikes, arrive unchanged. It requests the
mode with a seven-parameter variable `ZRPOS`; parameters four and five are
zero, and parameter six bit 0 is the MobyTurbo request. (`0x02` and `0x04` in
that parameter select Pack-7 and RLE.) The sender enters mode 3 only after
receiving this request.

Mode 3 headers begin `ZPAD ZDLE 0x33`. The following byte is the ordinary,
unoffset parameter count. CRC32 is little-endian and covers the header bytes
plus the same Omen copyright string used by `0x31`; the count itself is not
covered. Data is neither RLE-compressed nor seven-bit encoded. It is CRC32
protected and every value is literal except `ZDLE`, which is emitted as
`ZDLE ZDLEE`. Conversely, the mode-3 decoder returns raw XON/XOFF values as
data instead of applying the standard receiver's flow-control discard rule.

## Independent cross-check

Qodem's current
[`source/zmodem.c`](https://codeberg.org/AutumnMeowMeow/qodem/src/branch/main/source/zmodem.c)
implements the older `ZDLE` plus XOR-`0x40` interpretation of `ESC8`. It has no
`0x31`, SO quoting, or Omen RLE framing. This is useful evidence for retaining
that interpretation as a receive-only legacy mode, but it cannot define the
normative Omen format. The two forms are safely distinguished by their header
indicator.

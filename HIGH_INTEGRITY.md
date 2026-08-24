# High-integrity C review register

This branch implements an initial restrictive, independently described C
coding profile and stronger verification.  It makes no claim of compliance
with any external standard.  A later reviewer may map these records to
licensed guidance.

Each open item records a design choice, construct, or dependency that still
needs profile acceptance or further work.  Closing an item requires either
removing it or documenting its necessity, risks, and verification controls.

| ID | Area | Review question | Status and controls |
|----|------|-----------------|---------------------|
| HI-001 | POSIX transport | Are `select`, `read`, `write`, terminal control, and `errno` acceptable at the platform boundary? | Implemented boundary, pending profile acceptance; isolated in `zmdm_posix.c`, with adapter success and reported-failure tests. |
| HI-002 | Broken pipes | Can the programs report output failure without changing process-wide `SIGPIPE` handling? | Implemented boundary, pending profile acceptance; POSIX provides no per-descriptor suppression, so the programs install process-wide `SIG_IGN` with `sigaction`.  The behavioral suite verifies that broken protocol output is reported. |
| HI-003 | File and diagnostic I/O | Which hosted stream operations can be replaced by checked descriptor operations? | Open; protocol transport and sender payload I/O use checked descriptors. Receiver payload output retains a checked hosted stream because its accepted-byte position at a delayed write failure is externally observable on the wire. Diagnostics also remain hosted streams. |
| HI-004 | Platform types | Are all supported representations of `off_t`, `time_t`, `size_t`, and terminal types covered by checked conversions? | Open; keep platform values outside wire-format arithmetic. |
| HI-005 | Protocol constants | Should externally specified integer constants remain macros, or be represented by typed constants? | Open; validate every value and conversion at its use site. |
| HI-006 | Generated lookup data | Should derived CRC slicing tables be immutable generated data rather than initialized mutable state? | Resolved; the slicing tables are immutable generated data, their generator is tracked, and CRC equivalence tests cover aligned and unaligned lengths. |
| HI-007 | Application state | Must sender and receiver process state also be instance-owned? | Open; protocol state is instance-owned and reentrant, while command-line application state remains file-local for this single-session implementation. |
| HI-008 | Best-effort cleanup | Which cleanup and metadata operations must affect the process result? | Open; close/flush failures on the active received file affect transfer status, while terminal restoration, emergency cleanup, input close, and timestamp-setting remain best effort. |

## Initial reassessment

The initial reassessment gate was reached on 2026-08-24.  The complete quality
target passed with compatibility and installation tests, strict compiler
diagnostics, Clang and GCC static analysis, clang-tidy, address and undefined
behaviour sanitizers, a 10,000-run fuzz smoke test, and structural coverage.
No unexplained findings remained.

At reassessment, production coverage was 91.36% line coverage, 90.38% branch
coverage, and 100% modified condition/decision coverage for applicable
decisions.  These results support the initial coding-profile and test work;
they do not demonstrate complete requirements coverage or constitute a safety
qualification argument.

The branch stops at this boundary for reassessment.  The open register entries
remain future design and acceptance work.  Qualification-process, safety-case,
and SEooC artifacts are deliberately outside the current scope.

The automated structural-coverage gate currently requires at least 65% line
coverage and 90% branch coverage across production sources as regression
floors.  It separately requires 100% modified condition/decision coverage for
applicable decisions.  Coverage percentages alone are not evidence of complete
requirements coverage, and any infeasible or tool-unsupported decision needs
an explicit review record rather than a reduced target.

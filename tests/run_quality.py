#!/usr/bin/env python3

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMMON = [
    "-std=c99",
    "-D_POSIX_C_SOURCE=200112L",
    "-D_FILE_OFFSET_BITS=64",
    "-I",
    str(ROOT),
]
PRODUCTION = ["zmtx.c", "zmrx.c", "zmdm.c", "zmdm_posix.c", "crctab.c"]
MINIMUM_LINE_COVERAGE = 65.0
MINIMUM_BRANCH_COVERAGE = 50.0
REQUIRED_MCDC_COVERAGE = 100.0


def tool(environment, default):
    value = os.environ.get(environment, default)
    resolved = shutil.which(value)
    if resolved is None:
        raise SystemExit(f"required tool not found: {value}")
    return resolved


def llvm_toolchain():
    overrides = (
        os.environ.get("CLANG"),
        os.environ.get("LLVM_PROFDATA"),
        os.environ.get("LLVM_COV"),
    )
    if any(value is not None for value in overrides):
        return (
            tool("CLANG", "clang"),
            tool("LLVM_PROFDATA", "llvm-profdata"),
            tool("LLVM_COV", "llvm-cov"),
        )

    for suffix in ("22", "21", "20", "19", "18", "17", "16", "15"):
        for separator in ("", "-"):
            names = (f"clang{separator}{suffix}",
                     f"llvm-profdata{separator}{suffix}",
                     f"llvm-cov{separator}{suffix}")
            resolved = tuple(shutil.which(name) for name in names)
            if all(path is not None for path in resolved):
                return resolved
    resolved = tuple(shutil.which(name) for name in
                     ("clang", "llvm-profdata", "llvm-cov"))
    if all(path is not None for path in resolved):
        return resolved
    raise SystemExit("required matching LLVM toolchain not found")


def gcc_compiler():
    if os.environ.get("GCC") is not None:
        return tool("GCC", "gcc")
    for suffix in ("15", "14", "13", "12"):
        for separator in ("", "-"):
            resolved = shutil.which(f"gcc{separator}{suffix}")
            if resolved is not None:
                return resolved
    return tool("GCC", "gcc")


def run(command, *, cwd=ROOT, env=None, quiet=False):
    if not quiet:
        print("+", " ".join(str(item) for item in command), flush=True)
    subprocess.run([str(item) for item in command], cwd=cwd, env=env,
                   check=True)


def compile_program(compiler, output, sources, flags):
    run([compiler, *COMMON, *flags, *[ROOT / source for source in sources],
         "-o", output])


def static_analysis(directory):
    clang, _, _ = llvm_toolchain()
    gcc = gcc_compiler()
    diagnostics = ["-Wall", "-Wextra", "-Werror", "-pedantic-errors"]
    generated_crc = directory / "crctab_slicing.h"

    run([sys.executable, ROOT / "tools/generate_crc32_slicing.py",
         generated_crc])
    if generated_crc.read_bytes() != (ROOT / "crctab_slicing.h").read_bytes():
        raise SystemExit("crctab_slicing.h is not current")
    for source in PRODUCTION:
        output = directory / (Path(source).stem + ".clang.o")
        run([clang, *COMMON, *diagnostics, "--analyze", ROOT / source,
             "-o", output])
    for source in PRODUCTION:
        output = directory / (Path(source).stem + ".gcc.o")
        run([gcc, *COMMON, *diagnostics, "-fanalyzer", "-c", ROOT / source,
             "-o", output])

    clang_tidy = os.environ.get("CLANG_TIDY")
    tidy_candidates = ([clang_tidy] if clang_tidy else [
        "clang-tidy22", "clang-tidy21", "clang-tidy20", "clang-tidy19",
        "clang-tidy17", "clang-tidy15", "clang-tidy",
    ])
    tidy_path = next((shutil.which(candidate) for candidate in tidy_candidates
                      if shutil.which(candidate) is not None), None)
    if tidy_path is not None:
        checks = (
            "clang-analyzer-*,bugprone-*,cert-*,portability-*,"
            "-bugprone-easily-swappable-parameters"
        )
        run([tidy_path, *[ROOT / source for source in PRODUCTION],
             f"--checks={checks}", "--warnings-as-errors=*", "--", *COMMON])
    else:
        print("note: optional clang-tidy was not found", flush=True)


def build_instrumented(directory, flags):
    clang, _, _ = llvm_toolchain()
    diagnostics = ["-Wall", "-Wextra", "-Werror", "-pedantic-errors"]
    common_sources = ["zmdm.c", "zmdm_posix.c", "crctab.c"]
    compile_program(clang, directory / "zmtx",
                    ["zmtx.c", *common_sources], [*diagnostics, *flags])
    compile_program(clang, directory / "zmrx",
                    ["zmrx.c", *common_sources], [*diagnostics, *flags])
    compile_program(clang, directory / "test_crc",
                    ["tests/test_crc.c", "crctab.c"],
                    [*diagnostics, *flags])
    compile_program(clang, directory / "test_zmdm",
                    ["tests/test_zmdm.c", "zmdm.c", "crctab.c"],
                    [*diagnostics, *flags])
    compile_program(clang, directory / "test_posix_io",
                    ["tests/test_posix_io.c", "zmdm_posix.c"],
                    [*diagnostics, *flags])


def run_tests(directory, environment):
    run([directory / "test_crc"], env=environment)
    run([directory / "test_zmdm"], env=environment)
    run([directory / "test_posix_io"], env=environment)
    test_environment = environment.copy()
    test_environment["ZMTX"] = str(directory / "zmtx")
    test_environment["ZMRX"] = str(directory / "zmrx")
    run([sys.executable, ROOT / "tests/test_zmodem.py"], env=test_environment)


def sanitizers(directory):
    flags = [
        "-O1", "-g", "-fno-omit-frame-pointer",
        "-fsanitize=address,undefined",
    ]
    build_instrumented(directory, flags)
    environment = os.environ.copy()
    asan_options = ["strict_string_checks=1"]
    if sys.platform.startswith("linux"):
        asan_options.append("detect_leaks=1")
    environment["ASAN_OPTIONS"] = ":".join(asan_options)
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    run_tests(directory, environment)


def fuzz(directory):
    clang, _, _ = llvm_toolchain()
    executable = directory / "fuzz_zmdm"
    corpus = directory / "fuzz-corpus"

    corpus.mkdir()
    shutil.copy(ROOT / "tests/fuzz_corpus/plain-text", corpus)
    compile_program(
        clang,
        executable,
        ["tests/fuzz_zmdm.c", "zmdm.c", "crctab.c"],
        ["-O1", "-g", "-fno-omit-frame-pointer", "-Wall", "-Wextra",
         "-Werror", "-pedantic-errors", "-fsanitize=fuzzer,address,undefined"],
    )
    run([executable, corpus, ROOT / "tests/fuzz_corpus", "-runs=10000",
         "-max_len=16384", "-timeout=10", "-verbosity=0"])


def coverage(directory):
    flags = ["-O0", "-g", "-fprofile-instr-generate", "-fcoverage-mapping",
             "-fcoverage-mcdc"]
    build_instrumented(directory, flags)
    environment = os.environ.copy()
    environment["LLVM_PROFILE_FILE"] = str(directory / "%p.profraw")
    run_tests(directory, environment)

    _, llvm_profdata, llvm_cov = llvm_toolchain()
    profile = directory / "coverage.profdata"
    raw_profiles = sorted(directory.glob("*.profraw"))
    run([llvm_profdata, "merge", "-sparse", *raw_profiles, "-o", profile])
    objects = [
        directory / "zmtx",
        "-object", directory / "zmrx",
        "-object", directory / "test_crc",
        "-object", directory / "test_zmdm",
        "-object", directory / "test_posix_io",
    ]
    run([llvm_cov, "report", *objects, f"-instr-profile={profile}",
         "-ignore-filename-regex=tests/", "--show-mcdc-summary"])
    result = subprocess.run(
        [llvm_cov, "export", *map(str, objects), f"-instr-profile={profile}",
         "-summary-only", "-ignore-filename-regex=tests/"],
        cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE,
    )
    totals = json.loads(result.stdout)["data"][0]["totals"]
    lines = totals["lines"]["percent"]
    branches = totals["branches"]["percent"]
    mcdc = totals.get("mcdc")
    if mcdc is None:
        raise SystemExit("selected LLVM toolchain did not report MC/DC coverage")
    mcdc_percent = mcdc["percent"]
    print(f"production coverage: {lines:.2f}% lines, "
          f"{branches:.2f}% branches, {mcdc_percent:.2f}% MC/DC", flush=True)
    if lines < MINIMUM_LINE_COVERAGE:
        raise SystemExit(
            f"line coverage {lines:.2f}% is below "
            f"{MINIMUM_LINE_COVERAGE:.2f}%"
        )
    if branches < MINIMUM_BRANCH_COVERAGE:
        raise SystemExit(
            f"branch coverage {branches:.2f}% is below "
            f"{MINIMUM_BRANCH_COVERAGE:.2f}%"
        )
    if mcdc_percent < REQUIRED_MCDC_COVERAGE:
        raise SystemExit(
            f"MC/DC coverage {mcdc_percent:.2f}% is below "
            f"{REQUIRED_MCDC_COVERAGE:.2f}%"
        )


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in {
        "static", "sanitize", "fuzz", "coverage"
    }:
        raise SystemExit(
            "usage: run_quality.py {static|sanitize|fuzz|coverage}"
        )
    with tempfile.TemporaryDirectory(prefix="zmtx-quality-") as temporary:
        directory = Path(temporary)
        action = sys.argv[1]
        if action == "static":
            static_analysis(directory)
        elif action == "sanitize":
            sanitizers(directory)
        elif action == "fuzz":
            fuzz(directory)
        else:
            coverage(directory)


if __name__ == "__main__":
    main()

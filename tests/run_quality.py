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
    "-I",
    str(ROOT / "posix"),
    "-I",
    str(ROOT),
]
PRODUCTION = [
    "zmtx.c", "zmrx.c", "zmdm.c", "posix/zmodem_plat.c", "crctab.c",
]
ROOT_PRODUCTION = ["zmtx.c", "zmrx.c", "zmdm.c", "crctab.c"]
MINIMUM_LINE_COVERAGE = 65.0
MINIMUM_BRANCH_COVERAGE = 90.0
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
    if sys.platform == "darwin":
        brew = shutil.which("brew")
        if brew is not None:
            installed = subprocess.run(
                [brew, "list", "--formula"], check=False, text=True,
                stdout=subprocess.PIPE,
            )
            formulae = set(installed.stdout.split())
            candidates = [f"llvm@{suffix}" for suffix in
                          ("22", "21", "20", "19", "18")]
            candidates.append("llvm")
            for formula in candidates:
                if formula not in formulae:
                    continue
                prefix = subprocess.run(
                    [brew, "--prefix", formula], check=True, text=True,
                    stdout=subprocess.PIPE,
                ).stdout.strip()
                directory = Path(prefix) / "bin"
                resolved = tuple(str(directory / name) for name in
                                 ("clang", "llvm-profdata", "llvm-cov"))
                if all(Path(path).is_file() for path in resolved):
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


def run(command, *, cwd=ROOT, env=None, quiet=False, timeout=None):
    if not quiet:
        print("+", " ".join(str(item) for item in command), flush=True)
    subprocess.run([str(item) for item in command], cwd=cwd, env=env,
                   check=True, timeout=timeout)


def compile_program(compiler, output, sources, flags):
    run([compiler, *COMMON, *flags, *[ROOT / source for source in sources],
         "-o", output])


def static_analysis(directory):
    clang, _, _ = llvm_toolchain()
    gcc = gcc_compiler()
    diagnostics = ["-Wall", "-Wextra", "-Werror", "-pedantic-errors"]
    generated_crc = directory / "crctab_slicing.h"
    no_uint64 = directory / "no_uint64.h"

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

    forbidden_headers = (
        "<fcntl.h>", "<signal.h>", "<sys/select.h>", "<sys/stat.h>",
        "<sys/types.h>", "<termios.h>", "<unistd.h>", "<utime.h>",
    )
    for source in ROOT.rglob("*.c"):
        includes = [line for line in source.read_text(encoding="ascii").splitlines()
                    if line.startswith("#include")]
        if not includes or includes[0] != '#include "plat.h"':
            raise SystemExit(f"{source.relative_to(ROOT)} does not include "
                             '"plat.h" first')

    for source in ROOT_PRODUCTION:
        contents = (ROOT / source).read_text(encoding="ascii")
        for header in forbidden_headers:
            if header in contents:
                raise SystemExit(f"root source {source} includes {header}")

    c99_platform = ROOT / "tests/c99-platform"
    c99_common = [
        "-std=c99", "-I", c99_platform, "-I", ROOT, *diagnostics,
    ]
    for source in [*ROOT_PRODUCTION, "tests/c99-platform/zmodem_plat.c"]:
        output = directory / (Path(source).stem + ".c99-platform.o")
        run([clang, *c99_common, "-c", ROOT / source, "-o", output])

    no_uint64.write_text(
        "#include <stdint.h>\n#undef UINT64_MAX\n#undef UINT64_C\n",
        encoding="ascii",
    )
    fallback_test = directory / "test_zmdm_uint32_spans"
    compile_program(
        clang,
        fallback_test,
        ["tests/test_zmdm.c", "zmdm.c", "crctab.c"],
        [*diagnostics, "-include", no_uint64],
    )
    run([fallback_test], timeout=60)

    clang_tidy = os.environ.get("CLANG_TIDY")
    tidy_candidates = ([clang_tidy] if clang_tidy else [
        str(Path(clang).with_name("clang-tidy")),
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


def build_instrumented(directory, flags, compiler=None):
    if compiler is None:
        compiler, _, _ = llvm_toolchain()
    diagnostics = ["-Wall", "-Wextra", "-Werror", "-pedantic-errors"]
    common_sources = ["zmdm.c", "posix/zmodem_plat.c", "crctab.c"]
    compile_program(compiler, directory / "zmtx",
                    ["zmtx.c", *common_sources], [*diagnostics, *flags])
    compile_program(compiler, directory / "zmrx",
                    ["zmrx.c", *common_sources], [*diagnostics, *flags])
    compile_program(compiler, directory / "test_crc",
                    ["tests/test_crc.c", "crctab.c"],
                    [*diagnostics, *flags])
    compile_program(compiler, directory / "test_zmdm",
                    ["tests/test_zmdm.c", "zmdm.c", "crctab.c"],
                    [*diagnostics, *flags])
    compile_program(compiler, directory / "test_zmtx",
                    ["tests/test_zmtx.c", *common_sources],
                    [*diagnostics, *flags])
    compile_program(compiler, directory / "test_zmrx",
                    ["tests/test_zmrx.c", *common_sources],
                    [*diagnostics, *flags])
    compile_program(compiler, directory / "test_posix_io",
                    ["tests/test_posix_io.c", "posix/zmodem_plat.c"],
                    [*diagnostics, *flags])
    compile_program(compiler, directory / "test_posix_cleanup",
                    ["tests/test_posix_cleanup.c"],
                    [*diagnostics, *flags])


def run_tests(directory, environment):
    run([directory / "test_crc"], env=environment, timeout=60)
    run([directory / "test_zmdm"], env=environment, timeout=60)
    run([directory / "test_zmtx"], env=environment, timeout=60)
    run([directory / "test_zmrx"], env=environment, timeout=60)
    run([directory / "test_posix_io"], env=environment, timeout=60)
    run([directory / "test_posix_cleanup"], env=environment, timeout=60)
    test_environment = environment.copy()
    test_environment["ZMTX"] = str(directory / "zmtx")
    test_environment["ZMRX"] = str(directory / "zmrx")
    run([sys.executable, ROOT / "tests/test_zmodem.py"],
        env=test_environment, timeout=300)


def runtime_instrumentation_compiler():
    if sys.platform == "darwin" and os.environ.get("CLANG") is None:
        return tool("CLANG", "clang")
    clang, _, _ = llvm_toolchain()
    return clang


def sanitizers(directory):
    flags = [
        "-O1", "-g", "-fno-omit-frame-pointer",
        "-fsanitize=address,undefined",
    ]
    build_instrumented(directory, flags, runtime_instrumentation_compiler())
    environment = os.environ.copy()
    asan_options = ["strict_string_checks=1"]
    if sys.platform.startswith("linux"):
        asan_options.append("detect_leaks=1")
    environment["ASAN_OPTIONS"] = ":".join(asan_options)
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    run_tests(directory, environment)


def reduced_memory(directory):
    build_instrumented(directory, ["-O2", "-DREDUCED_MEMORY"])
    environment = os.environ.copy()
    environment["ZMODEM_REDUCED_MEMORY"] = "1"
    run_tests(directory, environment)


def fuzz(directory):
    # Current Apple toolchains omit libclang_rt.fuzzer_osx.a.
    clang, _, _ = llvm_toolchain()
    instrumentation = ("fuzzer,undefined" if sys.platform == "darwin" else
                       "fuzzer,address,undefined")
    executable = directory / "fuzz_zmdm"
    corpus = directory / "fuzz-corpus"

    corpus.mkdir()
    shutil.copy(ROOT / "tests/fuzz_corpus/plain-text", corpus)
    compile_program(
        clang,
        executable,
        ["tests/fuzz_zmdm.c", "zmdm.c", "crctab.c"],
        ["-O1", "-g", "-fno-omit-frame-pointer", "-Wall", "-Wextra",
         "-Werror", "-pedantic-errors", f"-fsanitize={instrumentation}"],
    )
    run([executable, corpus, ROOT / "tests/fuzz_corpus", "-runs=10000",
         "-max_len=16384", "-timeout=10", "-verbosity=0"], timeout=120)


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
        "-object", directory / "test_zmtx",
        "-object", directory / "test_zmrx",
        "-object", directory / "test_posix_io",
        "-object", directory / "test_posix_cleanup",
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
        "static", "sanitize", "fuzz", "reduced", "coverage"
    }:
        raise SystemExit(
            "usage: run_quality.py {static|sanitize|fuzz|reduced|coverage}"
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
        elif action == "reduced":
            reduced_memory(directory)
        else:
            coverage(directory)


if __name__ == "__main__":
    main()

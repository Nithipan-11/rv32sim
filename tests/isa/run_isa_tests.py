#!/usr/bin/env python3
"""Build the official riscv-tests (rv32ui, rv32um, rv32mi; physical-memory `p` env)
and run every one on rv32sim. A test passes when it writes 1 to `tohost`.

    python3 tests/isa/run_isa_tests.py --sim build/rv32sim --riscv-tests third_party/riscv-tests
"""
import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
from pathlib import Path

SUITES = {
    "rv32ui": "rv32im_zicsr_zifencei",
    "rv32um": "rv32im_zicsr_zifencei",
    "rv32mi": "rv32im_zicsr_zifencei",
}
# Tests for optional features this hart deliberately does not implement.
SKIP = {
    "rv32mi": {
        "breakpoint",  # Sdtrig debug triggers (tselect/tdata*)
        "pmpaddr",     # physical memory protection (pmpcfg/pmpaddr CSRs)
    },
}

def find_cc():
    for cc in ("riscv64-unknown-elf-gcc", "riscv32-unknown-elf-gcc", "riscv64-linux-gnu-gcc"):
        if shutil.which(cc):
            return cc
    sys.exit("no RISC-V GCC found (install gcc-riscv64-unknown-elf)")


def build(cc, rt, suite, march, name, out):
    src = rt / "isa" / suite / f"{name}.S"
    elf = out / f"{suite}-p-{name}"
    cmd = [cc, f"-march={march}", "-mabi=ilp32", "-static", "-mcmodel=medany", "-fvisibility=hidden",
           "-nostdlib", "-nostartfiles", f"-I{rt/'env'/'p'}", f"-I{rt/'isa'/'macros'/'scalar'}",
           f"-T{rt/'env'/'p'/'link.ld'}", str(src), "-o", str(elf)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr.strip().splitlines()[-1] if r.stderr else "build failed"
    return elf, None


def run(sim, elf):
    r = subprocess.run([sim, "--max-insns", "2000000", str(elf)], capture_output=True, text=True, timeout=30)
    return r.returncode == 0, (r.stderr.strip().splitlines() or ["?"])[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default="build/rv32sim")
    ap.add_argument("--riscv-tests", default="third_party/riscv-tests")
    ap.add_argument("--out", default="build/isa")
    ap.add_argument("--include-skipped", action="store_true", help="also run tests outside the supported subset")
    args = ap.parse_args()

    rt = Path(args.riscv_tests).resolve()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    cc = find_cc()

    jobs = []
    for suite, march in SUITES.items():
        frag = (rt / "isa" / suite / "Makefrag").read_text()
        # Test names are listed between `<suite>_sc_tests = \` and the next blank line.
        block = frag[frag.index(f"{suite}_sc_tests"):].split("\n\n")[0]
        names = block.replace("\\", " ").split()[2:]
        for n in names:
            skipped = n in SKIP.get(suite, set())
            if skipped and not args.include_skipped:
                continue
            jobs.append((suite, march, n, skipped))

    results = []
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as ex:
        def one(job):
            suite, march, n, skipped = job
            elf, err = build(cc, rt, suite, march, n, out)
            if elf is None:
                return suite, n, False, f"build error: {err}", skipped
            ok, msg = run(args.sim, elf)
            return suite, n, ok, msg, skipped
        results = list(ex.map(one, jobs))

    by_suite = {}
    for suite, n, ok, msg, skipped in sorted(results):
        by_suite.setdefault(suite, []).append((n, ok))
        mark = "PASS" if ok else "FAIL"
        print(f"{mark}  {suite}-p-{n}" + ("" if ok else f"   ({msg})"))
    print()
    total_ok = sum(1 for r in results if r[2])
    for suite, rows in by_suite.items():
        print(f"{suite}: {sum(ok for _, ok in rows)}/{len(rows)} passed")
    print(f"total: {total_ok}/{len(results)} passed")
    return 0 if total_ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())

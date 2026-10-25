#!/usr/bin/env python3
"""Run the bare-metal benchmarks, check their output against values computed by a
native (x86/ARM) build of the same C sources, and print a Markdown table.

    make -C bench && python3 bench/run_bench.py --sim build/rv32sim
"""
import argparse
import re
import statistics
import subprocess
import sys

# Expected console output, verified against a native gcc -O2 build of each program.
EXPECTED = {
    "sieve": "primes below 2000000: 148933",
    "matmul": "matmul checksum: 0x0d81ed37",
    "qsort": "sorted 200000 values, min 0x00010041 max 0xffffebc9",
    "crc32": "crc32: 0x158987c5",
}


def run(sim, elf, extra):
    r = subprocess.run([sim, "--stats", *extra, elf], capture_output=True, text=True, timeout=600)
    insns = int(re.search(r"instructions: (\d+)", r.stderr).group(1))
    secs = float(re.search(r"time: ([\d.]+)", r.stderr).group(1))
    return r.returncode, r.stdout.strip(), insns, secs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default="build/rv32sim")
    ap.add_argument("--dir", default="build/bench")
    ap.add_argument("--repeat", type=int, default=3, help="runs per configuration (median reported)")
    args = ap.parse_args()

    print("| Program | Instructions | MIPS (decode cache) | MIPS (no cache) | Speedup | Output check |")
    print("|---|---:|---:|---:|---:|---|")
    ok_all = True
    for name, expect in EXPECTED.items():
        elf = f"{args.dir}/{name}.elf"
        rows = {}
        for label, extra in (("cache", []), ("nocache", ["--no-decode-cache"])):
            mips, out, insns, rc = [], "", 0, 0
            for _ in range(args.repeat):
                rc, out, insns, secs = run(args.sim, elf, extra)
                mips.append(insns / secs / 1e6)
            rows[label] = (statistics.median(mips), out, insns, rc)
        mc, out, insns, rc = rows["cache"]
        mn = rows["nocache"][0]
        ok = rc == 0 and out == expect and rows["nocache"][1] == expect
        ok_all &= ok
        print(f"| {name} | {insns:,} | {mc:.0f} | {mn:.0f} | {mc / mn:.2f}x | {'pass' if ok else 'FAIL: ' + out} |")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())

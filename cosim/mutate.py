#!/usr/bin/env python3
"""Mutation testing for the co-simulation fuzzer itself.

Plants one known bug at a time into a copy of the RTL and checks that the
fuzzer reports a mismatch. A fuzzer that passes buggy hardware is worthless,
so every mutant must be "killed".

    python3 cosim/mutate.py --rtl third_party/simple-riscv-cpu --sim build/rv32sim
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

MUTANTS = [
    ("alu.v", "($signed(a) < $signed(b))", "(a < b)", "slt compares unsigned"),
    ("alu.v", "$signed(a) >>> b[4:0]", "a >> b[4:0]", "sra shifts logically"),
    ("alu.v", "a << b[4:0]", "a << b[3:0]", "sll ignores shamt bit 4"),
    ("alu.v", "4'b0100: result = a ^ b;", "4'b0100: result = a | b;", "xor computes or"),
    ("cpu.v", "branch_ne & ~alu_zero", "branch_ne & alu_zero", "bne inverted"),
    ("cpu.v", "instr[11:8], 1'b0}", "instr[11:8], 1'b1}", "branch offset bit 0 set"),
    ("cpu.v", "{{20{instr[31]}}, instr[31:20]}", "{20'd0, instr[31:20]}", "I-immediate zero-extended"),
    ("cpu.v", "instr[31:25], instr[11:7]}", "instr[31:25], 5'd0}", "S-immediate drops low bits"),
    # x0 must be protected on both ports to be observable: the read port alone also
    # forces x0 to zero, so making only the write port unprotected is an equivalent mutant.
    ("regfile.v", [("(write_addr != 5'd0)", "1'b1"),
                   ("(read_addr_1 == 5'd0) ? 32'd0 : r[read_addr_1]", "r[read_addr_1]")],
     None, "x0 writable and readable"),
    ("control.v", "3'b111: alu_ctrl = 4'b0010;     // andi", "3'b111: alu_ctrl = 4'b0011;     // andi", "andi computes or"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rtl", default="third_party/simple-riscv-cpu")
    ap.add_argument("--sim", default="build/rv32sim")
    ap.add_argument("--seeds", type=int, default=200)
    args = ap.parse_args()

    fuzz = Path(__file__).with_name("fuzz.py")
    survived = 0
    for fname, old, new, label in MUTANTS:
        work = Path(tempfile.mkdtemp(prefix="mutant-"))
        for f in Path(args.rtl).glob("*.v"):
            shutil.copy(f, work)
        src = (work / fname).read_text()
        edits = old if isinstance(old, list) else [(old, new)]
        if any(o not in src for o, _ in edits):
            print(f"SKIP    {label:<32} (pattern not found in {fname})")
            continue
        for o, n in edits:
            src = src.replace(o, n, 1)
        (work / fname).write_text(src)
        r = subprocess.run([sys.executable, str(fuzz), "--rtl", str(work), "--sim", args.sim,
                            "--seeds", str(args.seeds)], capture_output=True, text=True)
        summary = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()[-200:]
        killed = r.returncode != 0
        survived += not killed
        print(f"{'KILLED' if killed else 'SURVIVED'}  {label:<32} {summary}")
    print(f"\n{len(MUTANTS) - survived}/{len(MUTANTS)} mutants detected")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())

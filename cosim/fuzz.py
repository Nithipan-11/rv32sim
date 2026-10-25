#!/usr/bin/env python3
"""Differential fuzzing: rv32sim vs the simple-riscv-cpu Verilog RTL.

Generates random programs, runs each on both the RTL (Icarus Verilog) and
rv32sim, and compares the commit logs instruction by instruction. The first
divergence is reported with both commit lines and a disassembly.

    python3 cosim/fuzz.py --rtl third_party/simple-riscv-cpu --sim build/rv32sim --seeds 500
    python3 cosim/fuzz.py ... --probe-unsupported   # RV32I ops the RTL does not claim

Programs use only the instructions the RTL's README lists, unless
--probe-unsupported is given, which instead checks each remaining RV32I
instruction once to see whether the RTL executes it correctly, traps, or
silently computes something else.
"""
import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

# ----------------------------------------------------------------------------- encoders
def r_type(f7, rs2, rs1, f3, rd, op=0x33):
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op

def i_type(imm, rs1, f3, rd, op=0x13):
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op

def s_type(imm, rs2, rs1, f3=2):
    return (((imm >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 0x1F) << 7) | 0x23

def b_type(imm, rs2, rs1, f3):
    return (((imm >> 12) & 1) << 31) | (((imm >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) | \
           (f3 << 12) | (((imm >> 1) & 0xF) << 8) | (((imm >> 11) & 1) << 7) | 0x63

R_OPS = {"add": (0, 0), "sub": (0x20, 0), "sll": (0, 1), "slt": (0, 2), "xor": (0, 4),
         "srl": (0, 5), "sra": (0x20, 5), "or": (0, 6), "and": (0, 7)}
I_OPS = {"addi": 0, "slti": 2, "xori": 4, "ori": 6, "andi": 7}

# RV32I instructions outside the RTL's documented subset (probe mode).
UNSUPPORTED = {
    "sltu": lambda: r_type(0, 2, 1, 3, 3),
    "sltiu": lambda: i_type(-1, 1, 3, 3),
    "slli": lambda: r_type(0, 4, 1, 1, 3, 0x13),
    "srli": lambda: r_type(0, 4, 1, 5, 3, 0x13),
    "srai": lambda: r_type(0x20, 4, 1, 5, 3, 0x13),
    "lb": lambda: i_type(0x400, 0, 0, 3, 0x03),
    "lh": lambda: i_type(0x400, 0, 1, 3, 0x03),
    "lbu": lambda: i_type(0x400, 0, 4, 3, 0x03),
    "lhu": lambda: i_type(0x400, 0, 5, 3, 0x03),
    "sb": lambda: s_type(0x404, 1, 0, 0),
    "sh": lambda: s_type(0x404, 1, 0, 1),
    "blt": lambda: b_type(8, 2, 1, 4),
    "bge": lambda: b_type(8, 2, 1, 5),
    "bltu": lambda: b_type(8, 2, 1, 6),
    "bgeu": lambda: b_type(8, 2, 1, 7),
    "lui": lambda: (0x12345 << 12) | (3 << 7) | 0x37,
    "auipc": lambda: (0x00001 << 12) | (3 << 7) | 0x17,
    "jal": lambda: (((8 >> 1) & 0x3FF) << 21) | (3 << 7) | 0x6F,
    "jalr": lambda: i_type(0x18, 0, 0, 3, 0x67),  # jumps to the next instruction
}

SELF_LOOP = b_type(0, 0, 0, 0)  # beq x0, x0, 0
DATA_BASE = 0x400               # data window [0x400, 0x800), above the code
REGS = list(range(1, 9))        # a small register set forces dependencies


def random_program(rng, n):
    prog = []
    for r in REGS:  # seed registers with mixed signs/magnitudes
        prog.append(i_type(rng.randint(-2048, 2047), 0, 0, r))
    # pre-fill a few data words so loads see non-zero values
    for k in range(4):
        prog.append(s_type(DATA_BASE + 4 * k, rng.choice(REGS), 0))
    body = n - len(prog) - 1
    for idx in range(body):
        kind = rng.choices(["r", "i", "lw", "sw", "br"], weights=[40, 30, 10, 10, 10])[0]
        rd, rs1, rs2 = rng.choice(REGS + [0]), rng.choice(REGS + [0]), rng.choice(REGS + [0])
        if kind == "r":
            f7, f3 = R_OPS[rng.choice(list(R_OPS))]
            prog.append(r_type(f7, rs2, rs1, f3, rd))
        elif kind == "i":
            prog.append(i_type(rng.randint(-2048, 2047), rs1, I_OPS[rng.choice(list(I_OPS))], rd))
        elif kind == "lw":
            prog.append(i_type(DATA_BASE + 4 * rng.randrange(64), 0, 2, rd, 0x03))
        elif kind == "sw":
            prog.append(s_type(DATA_BASE + 4 * rng.randrange(64), rs2, 0))
        else:  # forward-only branches guarantee termination
            remaining = body - idx  # instructions left including the final self-loop
            off = 4 * rng.randint(1, max(1, min(remaining, 8)))
            prog.append(b_type(off, rs2, rs1, rng.choice([0, 1])))
    prog.append(SELF_LOOP)
    assert len(prog) * 4 <= DATA_BASE
    return prog


def write_hex(prog, path):
    path.write_text("".join(f"{w:08x}\n" for w in prog))


def run_rtl(vvp, hexfile):
    r = subprocess.run(["vvp", "-n", str(vvp), f"+hex={hexfile}"], capture_output=True, text=True, timeout=60)
    return [l for l in r.stdout.splitlines() if l.startswith("core") or l == "TIMEOUT"]


def run_iss(sim, hexfile):
    r = subprocess.run([sim, "--hex", str(hexfile), "--base", "0", "--harvard", "--trace", "--max-insns", "10000"],
                       capture_output=True, text=True, timeout=60)
    return [l.rstrip() for l in r.stderr.splitlines() if l.startswith("core")]


def normalize(line):
    return " ".join(line.split())


def compare(rtl, iss):
    for i, (a, b) in enumerate(zip(rtl, iss)):
        if normalize(a) != normalize(b):
            return i, a, b
    if len(rtl) != len(iss):
        i = min(len(rtl), len(iss))
        return i, rtl[i] if i < len(rtl) else "<end>", iss[i] if i < len(iss) else "<end>"
    return None


def build_rtl(rtl_dir, out):
    rtl_dir = Path(rtl_dir)
    srcs = [rtl_dir / f for f in ("cpu.v", "control.v", "alu.v", "regfile.v", "imem.v", "dmem.v")]
    vvp = out / "tb_cosim.vvp"
    subprocess.run(["iverilog", "-g2012", "-o", str(vvp), str(Path(__file__).with_name("tb_cosim.v")),
                    *map(str, srcs)], check=True)
    return vvp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rtl", default="third_party/simple-riscv-cpu")
    ap.add_argument("--sim", default="build/rv32sim")
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--length", type=int, default=80, help="instructions per program")
    ap.add_argument("--first-seed", type=int, default=1)
    ap.add_argument("--probe-unsupported", action="store_true")
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="cosim-"))
    vvp = build_rtl(args.rtl, tmp)
    hexfile = tmp / "prog.hex"

    if args.probe_unsupported:
        # Set up x1=-5, x2=3, x4=7 and a data word, run the instruction, then stop.
        prelude = [i_type(-5, 0, 0, 1), i_type(3, 0, 0, 2), i_type(7, 0, 0, 4),
                   i_type(0x123, 0, 0, 5), s_type(0x400, 5, 0)]
        print(f"{'instruction':<8} result")
        wrong = 0
        for name, make in UNSUPPORTED.items():
            prog = prelude + [make(), i_type(0, 0, 0, 0), i_type(0, 0, 0, 0), SELF_LOOP]
            write_hex(prog, hexfile)
            diff = compare(run_rtl(vvp, hexfile), run_iss(args.sim, hexfile))
            if diff is None:
                print(f"{name:<8} matches the ISA")
            else:
                wrong += 1
                _, a, b = diff
                print(f"{name:<8} WRONG   rtl: {normalize(a)}\n{'':<16}spec: {normalize(b)}")
        print(f"\n{wrong}/{len(UNSUPPORTED)} unsupported instructions execute incorrectly instead of trapping")
        return 0

    failures = 0
    total_insns = 0
    for seed in range(args.first_seed, args.first_seed + args.seeds):
        prog = random_program(random.Random(seed), args.length)
        write_hex(prog, hexfile)
        rtl, iss = run_rtl(vvp, hexfile), run_iss(args.sim, hexfile)
        total_insns += len(iss)
        diff = compare(rtl, iss)
        if diff:
            failures += 1
            i, a, b = diff
            keep = tmp / f"fail_seed{seed}.hex"
            write_hex(prog, keep)
            print(f"seed {seed}: mismatch at commit {i}\n  rtl: {normalize(a)}\n  iss: {normalize(b)}\n  program: {keep}")
    print(f"{args.seeds - failures}/{args.seeds} programs matched "
          f"({total_insns} instructions compared commit-by-commit)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

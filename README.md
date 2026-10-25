# rv32sim

A RISC-V instruction-set simulator in C++20 for **RV32IM + Zicsr**, machine mode, with precise traps.

It passes the official [riscv-tests](https://github.com/riscv-software-src/riscv-tests) suite and runs bare-metal C programs at **120 to 160 million instructions per second**. It also serves as a golden reference model for my Verilog CPU, [simple-riscv-cpu](https://github.com/Nithipan-11/simple-riscv-cpu): the two run in lockstep and are compared instruction by instruction.

| Check | Result |
|---|---|
| Official riscv-tests (rv32ui, rv32um, rv32mi; `p` environment) | **64 / 64 pass** |
| Unit tests (GoogleTest) | **31 / 31 pass** |
| Differential fuzzing vs. the Verilog RTL | **2,000 / 2,000** random programs, 141,084 instructions, identical commit logs |
| Mutation testing of the fuzzer (bugs planted in the RTL) | **10 / 10** mutants detected |
| Benchmarks, output checked against a native build | **4 / 4** correct |

## What it found in my CPU

The fuzzer also has a probe mode. It runs each RV32I instruction that simple-riscv-cpu does not claim to support. A correct core either executes the instruction properly or traps. My CPU does neither for **15 of the 19**: they silently produce wrong results. Some examples from `cosim/fuzz.py --probe-unsupported`:

```
lui   WRONG  rtl:  core 0: 3 0x00000014 (0x123451b7)                       # no register write
             spec: core 0: 3 0x00000014 (0x123451b7) x3 0x12345000
sb    WRONG  rtl:  core 0: 3 0x00000014 (0x40100223) mem 0x00000404 0xfffffffb  # writes a whole word
             spec: core 0: 3 0x00000014 (0x40100223) mem 0x00000404 0xfb
blt   WRONG  rtl:  core 0: 3 0x00000018 (0x00000013)                       # branch was not taken
             spec: core 0: 3 0x0000001c (0x00000013)
```

The wrong ones are `sltu sltiu slli srli srai lb lbu sb sh blt bgeu lui auipc jal jalr`. `lh`, `lhu`, `bge` and `bltu` only match because of the operands the probe happens to use. The fix is for the control unit to flag unknown opcode/funct3 combinations as illegal. This is exactly the kind of bug a reference model is built to catch.

## Performance

These numbers are from a 2-core Intel Xeon @ 2.10 GHz with g++ 13.3 at `-O2`. Each figure is the median of 3 runs.

| Program | Instructions | MIPS (decode cache) | MIPS (no cache) | Speedup |
|---|---:|---:|---:|---:|
| sieve (primes < 2,000,000) | 125,518,721 | 123 | 72 | 1.71x |
| matmul (96×96 integer, ×5) | 31,838,899 | 147 | 85 | 1.74x |
| qsort (200,000 values) | 38,920,576 | 137 | 64 | 2.14x |
| crc32 (bitwise, 1 MiB) | 70,254,810 | 158 | 73 | 2.17x |

Three changes account for most of this speed:

1. **Decoded-instruction cache.** Each 4 KiB page stores the decoded form of every word that has been fetched. The bits are decoded once, not on every execution. A store to a word clears that word's valid bit, so self-modifying code stays correct and does not need a global flush.
2. **Flat page table.** Every memory access is one array index into 2^20 page pointers, not a hash-map lookup. This roughly doubled speed over the first version, which ran at 63 to 83 MIPS.
3. **Trace-free fast path.** `exec<bool Trace>` is a template, so the untraced build of the interpreter never constructs commit records.

## Build and run

You need CMake 3.20+ and a C++20 compiler. GoogleTest is optional: the build uses the system package if present and otherwise fetches it.

```bash
git clone --recursive https://github.com/Nithipan-11/rv32sim
cd rv32sim
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

The ISA tests, benchmarks and co-simulation also need the RISC-V GCC cross-compiler and Icarus Verilog. On Ubuntu:

```bash
sudo apt install gcc-riscv64-unknown-elf iverilog

python3 tests/isa/run_isa_tests.py                  # official riscv-tests -> 64/64
make -C bench && python3 bench/run_bench.py          # benchmark table above
python3 cosim/fuzz.py --seeds 500                    # rv32sim vs. Verilog, lockstep
python3 cosim/fuzz.py --probe-unsupported            # the 15/19 finding
python3 cosim/mutate.py --seeds 100                  # does the fuzzer catch planted bugs?
```

### Command line

```
rv32sim [options] <program>
  --hex                 program is a $readmemh hex file (one word per line)
  --base ADDR           load/start address for --hex (default 0)
  --max-insns N         stop after N instructions
  --trace               print a Spike-style commit log to stderr
  --stats               print instruction count, time and MIPS to stderr
  --no-decode-cache     decode every instruction on every fetch
  --dump-regs           print the register file on exit
  --stop-on-self-loop   stop when a jump/branch targets itself
  --harvard             separate instruction and data memories
```

ELF programs are loaded from their program headers. If the program defines a `tohost` symbol, as the riscv-tests do, the simulator watches it for pass/fail. Otherwise the simulator handles `ecall` as a host system call: `write` (64) and `exit` (93).

The simulator can also run my CPU's own sample program straight from its hex file:

```
$ rv32sim --hex --harvard --trace --dump-regs third_party/simple-riscv-cpu/program.hex
core   0: 3 0x00000000 (0x00500093) x1  0x00000005
core   0: 3 0x00000004 (0x00700113) x2  0x00000007
core   0: 3 0x00000008 (0x002081b3) x3  0x0000000c
core   0: 3 0x0000000c (0x00302023) mem 0x00000000 0x0000000c
core   0: 3 0x00000010 (0x00000203) x4  0x0000000c
core   0: 3 0x00000014 (0x00108463)
core   0: 3 0x0000001c (0x02a00313) x6  0x0000002a
core   0: 3 0x00000020 (0x00000000) trap
...
```

The program has no end-of-program loop, so it runs into zeroed memory. Zeroed memory is an illegal instruction, and in hex mode the simulator stops there. `--harvard` is needed because the CPU has separate instruction and data memories: the `sw` to address 0 writes data memory and must not overwrite the first instruction.

## How the co-simulation works

```
random program ──► hex file ──┬──► Icarus Verilog + tb_cosim.v ──► commit log ─┐
                              │                                                 ├─► compare line by line
                              └──► rv32sim --hex --harvard --trace ─► commit log ┘
```

- `cosim/tb_cosim.v` wraps the RTL. On every rising clock edge it prints one line in the same format as `rv32sim --trace`: pc, instruction, the register written and its value, or the memory address written and its value.
- `cosim/fuzz.py` generates random programs from the instructions the CPU supports. The programs use eight registers, so instructions depend on each other. Loads and stores go to a shared data window, and branches only jump forward, so every program terminates. The script reports the first line where the two logs differ, with both lines and a disassembly.
- `cosim/mutate.py` tests the tester. It plants a known bug in a copy of the RTL (signed compare made unsigned, `bne` inverted, immediate zero-extended, x0 writable, …) and checks that the fuzzer catches it. All 10 are caught.

## Layout

```
include/rv32sim/   decoder.hpp  memory.hpp  cpu.hpp  elf_loader.hpp
src/               decoder.cpp  cpu.cpp  elf_loader.cpp  main.cpp   (~1,050 lines)
tests/unit/        GoogleTest: decoder, execution, memory/decode cache
tests/isa/         builds and runs the official riscv-tests
bench/             bare-metal runtime (crt0, linker script, tiny libc) + 4 C benchmarks
cosim/             lockstep testbench, differential fuzzer, mutation tester
third_party/       riscv-tests, simple-riscv-cpu (git submodules)
docs/DESIGN.md     how every part works, and why
```

## Scope

Implemented: RV32I, the M extension, Zicsr, and `fence.i`. Machine mode only, with synchronous exceptions: illegal instruction, misaligned fetch, `ecall`, `ebreak`. Supported CSRs: `mstatus misa mie mip mtvec mscratch mepc mcause mtval`, the `cycle`/`instret` counters, and the ID registers.

Not implemented: compressed instructions (C), atomics (A), floating point, S/U modes and virtual memory, interrupts, PMP, and the debug trigger module. Two riscv-tests need hardware that is out of scope, so they are skipped: `rv32mi-breakpoint` (debug triggers) and `rv32mi-pmpaddr` (PMP).

## License

MIT

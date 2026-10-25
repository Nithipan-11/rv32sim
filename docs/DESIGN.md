# How rv32sim works

This guide walks through the simulator in the order an instruction flows through it. Each section ends with the questions an interviewer is likely to ask and the answers you should be able to give from memory.

---

## 1. The big picture

An instruction-set simulator (ISS) is a program that behaves like a CPU. It has an array of 32 registers, a program counter, and a memory. In a loop it:

1. **fetches** the 32-bit word at `pc`,
2. **decodes** it into "which operation, which registers, which immediate",
3. **executes** that operation, updating registers, memory, and `pc`.

That's the whole idea. Everything else is getting the corner cases exactly right (traps, CSRs, sign extension) and making the loop fast.

```
          ┌──────────────┐  fetch(pc)   ┌───────────────────────────┐
  pc ───► │   Memory     │ ───────────► │ decode cache hit?         │
          │ (paged)      │              │  yes → reuse Inst         │
          └──────────────┘              │  no  → decode(word), save │
                 ▲                      └─────────────┬─────────────┘
                 │ loads/stores                       ▼
          ┌──────┴───────────────────────────────────────────────┐
          │ Cpu::exec(): switch (inst.op) { ... }                 │
          │  writes x[rd], memory, pc  — or trap() to mtvec       │
          └───────────────────────────────────────────────────────┘
```

| File | Role |
|---|---|
| `decoder.hpp/.cpp` | Raw 32-bit word → `Inst {op, rd, rs1, rs2, imm, csr}`; disassembler for traces |
| `memory.hpp` | Sparse paged memory + per-page decoded-instruction cache |
| `cpu.hpp/.cpp` | Registers, CSRs, the execute loop, traps, host syscalls |
| `elf_loader.cpp` | Loads ELF program headers, symbol table; also raw binaries and `$readmemh` hex |
| `main.cpp` | Command-line driver |

---

## 2. Decoding (`src/decoder.cpp`)

A RISC-V instruction is always 32 bits here (no compressed "C" extension). The low 7 bits are the **opcode**, which picks the format. `funct3` (bits 14–12) and `funct7` (bits 31–25) pick the exact operation within it.

| Format | Used by | Immediate bits |
|---|---|---|
| R | `add`, `sub`, `mul`, … | none |
| I | `addi`, loads, `jalr`, CSR ops | `[31:20]` |
| S | stores | `[31:25]` + `[11:7]` |
| B | branches | scrambled: `[31] [7] [30:25] [11:8]`, bit 0 is always 0 |
| U | `lui`, `auipc` | `[31:12]`, low 12 bits zero |
| J | `jal` | scrambled: `[31] [19:12] [20] [30:21]`, bit 0 is always 0 |

**Why are B and J immediates scrambled?** So that the sign bit is always bit 31 and most immediate bits sit in the same positions across formats. Hardware can then sign-extend and route bits with fewer multiplexers. Software (us) pays for it with bit shuffling: see `imm_b` and `imm_j`.

**Sign extension** uses a branch-free trick:

```cpp
constexpr int32_t sext(uint32_t v, int width) {
    const uint32_t m = 1u << (width - 1);   // the sign bit of a width-bit number
    return static_cast<int32_t>((v ^ m) - m);
}
```

If the sign bit is 0, XOR sets it and subtracting removes it again, giving the same value. If the sign bit is 1, XOR clears it and subtracting `m` makes the result negative.

For loads and branches, `funct3` indexes a small `constexpr` table of operations. The unused slots hold `Op::Illegal`, so a bad encoding becomes an illegal-instruction trap instead of silently doing something. That is the exact bug the fuzzer found in the Verilog CPU (section 8).

**Interview questions**
- *Why decode into a struct instead of decoding inside the execute switch?* So decoding can be cached (section 4) and tested on its own (`tests/unit/test_decoder.cpp`).
- *Why is the branch offset's bit 0 not stored?* Instructions are at least 2-byte aligned (because of the C extension), so it is always 0. Leaving it out doubles the branch range.

---

## 3. Executing (`src/cpu.cpp`, `Cpu::exec`)

`exec()` is one big `switch (in.op)`. A few things worth knowing:

- **x0 is always zero.** Writes go through `if (writes_rd && in.rd != 0)`, so an instruction that targets x0 has no effect.
- **Signed vs. unsigned** comes down to C++ casts. `blt` compares `static_cast<int32_t>(rs1) < static_cast<int32_t>(rs2)`, while `bltu` compares the raw `uint32_t` values. `sra` shifts an `int32_t`, which keeps the sign bit. `srl` shifts a `uint32_t`, which fills with zeros.
- **Only the low 5 bits of a shift amount are used** (`rs2 & 31`). Shifting a 32-bit value by 32 or more is undefined behavior in C++, so this mask is required for correctness, not just for matching the spec.
- **Division never traps in RISC-V.** The spec defines the edge cases, and the simulator returns them:

  | Case | `div` | `divu` | `rem` | `remu` |
  |---|---|---|---|---|
  | x / 0 | −1 | 0xFFFFFFFF | x | x |
  | INT_MIN / −1 (overflow) | INT_MIN | — | 0 | — |

  In C++, `INT_MIN / -1` is undefined behavior and crashes on x86, so it has to be special-cased before dividing.
- **`mulh`, `mulhsu`, `mulhu`** return the upper 32 bits of a 64-bit product. The operands are widened to `int64_t` or `uint64_t` (with the right signedness for each operand), multiplied, and shifted right by 32.

### The commit record

When tracing, every instruction fills a `Commit` record: pc, raw instruction, which register it wrote and the value, which address it stored to and the value. `--trace` prints it in the same format as Spike (the official RISC-V reference simulator):

```
core   0: 3 0x8000003c (0x00001f17) x30 0x8000103c
core   0: 3 0x80000040 (0xfc3f2223) mem 0x80001000 0x00000001
```

That one-line-per-instruction format is what makes co-simulation possible (section 7).

**Interview questions**
- *How do you handle `jalr`?* Target = `(rs1 + imm) & ~1`. The link value `pc + 4` is computed *before* jumping, because `rd` may be the same register as `rs1`.
- *What happens on `INT_MIN / -1`?* The result is INT_MIN with remainder 0, and nothing traps. Natively it would crash the host with SIGFPE, so it needs a special case.

---

## 4. Memory and the decode cache (`include/rv32sim/memory.hpp`)

### Paged, sparse memory
A 32-bit address space is 4 GiB, which is too much to allocate. Memory is split into **4 KiB pages** that are created the first time they are touched. The riscv-tests link at `0x80000000` and use only a few pages, so they cost a few KiB.

**Page lookup** uses a flat table of 2^20 page pointers (one per possible page, 8 MiB total):

```cpp
Page& page(uint32_t a) {
    auto& slot = table_[a >> 12];   // page number = top 20 bits
    if (!slot) slot = std::make_unique<Page>();
    return *slot;
}
```

The first version used a hash map from page number to page, so every load, store and fetch paid for a hash and a bucket walk. Replacing that with one array index, together with the trace-free fast path in section 8, roughly **doubled throughput** (sieve: 63 → ~125 MIPS). It's a classic space/time trade: 8 MiB of mostly-null pointers buys O(1) lookup with no hashing.

### Decoded-instruction cache
Decoding is pure bit-twiddling, but a loop body executes millions of times, so decoding it every time is wasted work. Each page therefore also stores:

```cpp
std::array<Inst, 1024> decoded;   // one decoded slot per 4-byte word
std::bitset<1024>      valid;     // is that slot up to date?
```

- `fetch(pc)`: if `valid[slot]` is set, return the cached `Inst`. Otherwise decode, store it, and set the bit.
- **Every store clears the valid bit of the word it touches** (`store8` does `p.valid.reset(o >> 2)`). This is what keeps **self-modifying code** correct: code that writes new instructions gets them re-decoded on the next fetch. `test_memory.cpp` has a test for exactly this.
- `fence.i` (the instruction programs use to say "I've changed my code") flushes every page's valid bits.

Result: **1.7× to 2.2× faster** across the benchmarks (`--no-decode-cache` turns it off for comparison).

### Harvard mode
Real RISC-V systems, and the riscv-tests, use one memory for code and data (von Neumann). The Verilog CPU has separate `imem` and `dmem` (Harvard). `Cpu(Memory& imem, Memory& dmem)` fetches from one and loads and stores to the other. Without this, the sample program's `sw x3, 0(x0)` would overwrite its own first instruction in the simulator but not in the hardware.

**Interview questions**
- *Why invalidate per word and not per page?* Programs often keep data next to code in the same page. Invalidating the whole page on every store would throw away the cache constantly.
- *Why not a hash map?* Hashing costs more than one memory access, and every instruction does at least one lookup (the fetch).
- *Memory cost of the page table?* 2^20 × 8 bytes = 8 MiB of pointers, allocated once. Pages themselves (4 KiB of bytes plus 1024 decoded slots) are only created when used.

---

## 5. Traps and CSRs

A **trap** is the CPU's way of saying "I can't continue normally". Causes include an illegal instruction, a jump to a misaligned address, `ecall` and `ebreak`. Hardware then:

1. saves the faulting pc in **`mepc`**,
2. records why in **`mcause`** (2 = illegal instruction, 11 = ecall from M-mode, …),
3. puts extra information in **`mtval`** (the bad instruction bits, or the bad address),
4. saves the interrupt-enable bit `MIE` into `MPIE` in `mstatus`, then clears `MIE`,
5. jumps to the handler address in **`mtvec`**.

The handler finishes with `mret`, which jumps back to `mepc` and restores `MIE` from `MPIE`. All of this is in `Cpu::trap()` and the `Mret` case.

These rules are **precise**: the faulting instruction has no effect (no register write, no `instret` increment), and every earlier instruction has fully completed. In `exec()` that is the `if (!trapped_) { ... }` block around write-back.

**CSRs** (control and status registers) are read and written with the `csrrw/csrrs/csrrc` family. Rules that matter:
- Reading a CSR that doesn't exist is an **illegal instruction**. `read_csr` returns `std::nullopt`, and the caller traps.
- CSRs whose address has top bits `11` are read-only. Writing one is illegal.
- `csrrs`/`csrrc` with `rs1 = x0` **don't write at all**. That makes them safe for reading read-only CSRs.

### Bugs the official tests caught (good stories for interviews)
The first run of riscv-tests failed a few `rv32mi` tests. Each failure taught a spec detail:

| Test | What I had wrong | Fix |
|---|---|---|
| `ma_fetch` | I trapped when *fetching* from a misaligned pc | Without the C extension, the **jump itself** traps, and `mepc` points at the jump. Done by the `jump_to` lambda. |
| `instret_overflow` | Writing `minstret` was immediately overwritten by that instruction's own `+1` | A write to a counter CSR suppresses that instruction's increment (`suppress_instret_`). `mcycle` became a separate counter for the same reason. |
| `breakpoint` | Unknown CSRs read as 0 | Unknown CSRs must be illegal, which a general fix covers. The test still needs debug-trigger hardware (Sdtrig), which is out of scope, so it is skipped. |
| `pmpaddr` | — | Needs PMP hardware, which is out of scope, so it is skipped. |

---

## 6. Running programs

### riscv-tests (`tests/isa/run_isa_tests.py`)
The official tests are assembly files built against the `p` environment: physical memory, machine mode only. Each test writes to a memory word called **`tohost`**:
- `1` means **pass**.
- `(test_number << 1) | 1` means **fail** at that test case.

The ELF loader finds the `tohost` symbol, and the store path watches that address. The runner reads the test list from each suite's Makefrag, compiles every test with `riscv64-unknown-elf-gcc -march=rv32im_zicsr_zifencei -mabi=ilp32`, and runs them in parallel: **64/64 pass**.

### Bare-metal C (`bench/`)
There's no operating system, so the repo provides a tiny runtime:
- `crt0.S`: sets the stack pointer, zeroes `.bss` (C requires globals to start at zero), calls `main`, then calls `exit`.
- `link.ld`: places code at `0x80000000` and reserves a 1 MiB stack.
- `rt.c`: `print`, `memset`, `memcpy`. These are needed because `-nostdlib` means no libc, and gcc emits calls to `memset`/`memcpy` on its own.
- **Host syscalls**: when a program has no `tohost`, `ecall` is handled by the simulator itself, Linux-style. `a7` holds the syscall number: 64 = `write(fd, buf, len)`, 93 = `exit(code)`.

Each benchmark's output is compared against the same C file compiled natively for x86. That way the benchmark also checks correctness, not just speed.

---

## 7. Co-simulation with the Verilog CPU (`cosim/`)

**Goal:** prove the hardware does what the ISA says, one instruction at a time.

1. `tb_cosim.v` instantiates the CPU and loads a hex file with `$readmemh`. **On every rising clock edge** it prints a commit line in the same format as `rv32sim --trace`.
   - *Why the rising edge?* In a single-cycle CPU, by the rising edge the combinational logic has settled for the current instruction. The register and memory writes happen *at* that edge (non-blocking assignments), so the values sampled there are the ones about to be committed.
   - My first version sampled on the falling edge and missed instruction 0.
2. `fuzz.py` generates a random program, runs it on both Icarus Verilog and `rv32sim --harvard`, and compares the logs line by line. The first difference is the bug.
3. Generator design choices:
   - **Only 8 registers** (x1–x8): with 31, most instructions would not depend on each other. With 8, results feed into later instructions, so a wrong value propagates and shows up.
   - **Seed every register** with a random value first, and pre-fill some data words, so loads don't just read zero.
   - **Forward-only branches**: a program can never loop forever, so every run ends. It ends at a `beq x0, x0, 0` self-loop, which both sides detect.
   - **Deterministic seeds**: `random.Random(seed)`, so any failure can be reproduced.

### Testing the tester: mutation testing (`mutate.py`)
A fuzzer that never reports anything is either finding no bugs or broken. To tell those apart, `mutate.py` copies the RTL, plants one realistic bug, and checks that the fuzzer catches it:

- signed compare made unsigned
- `sra` made logical
- `bne` inverted
- I-immediate zero-extended
- S-immediate drops its low bits
- x0 made writable
- …

**10/10 are caught.** One subtle case: making only x0's *write* port unguarded survived. The read port also forces x0 to 0, so that change has no visible effect. In mutation-testing terms it is an *equivalent mutant*. The fix was to mutate both guards together.

### Probe mode
`--probe-unsupported` runs each RV32I instruction the CPU doesn't list as supported and checks whether it executes correctly, traps, or computes something wrong. **15/19 compute something wrong.** The cause: the control unit treats unknown encodings as whichever supported instruction they resemble, instead of flagging them illegal. The fix is a default "illegal" case in `control.v` and an exception path.

---

## 8. Performance: what was measured and changed

| Version | sieve | matmul | qsort | crc32 |
|---|---:|---:|---:|---:|
| First working (hash-map pages, commit record every step) | 63 | 69 | 72 | 83 MIPS |
| Flat page table + trace-free `exec<false>` + decode cache | 123 | 147 | 137 | 158 MIPS |
| Same, decode cache **off** | 72 | 85 | 64 | 73 MIPS |

The changes, in order of impact:
1. **Flat page table** (section 4): removed hashing from every memory access.
2. **Decode cache** (section 4): removed decoding from every instruction.
3. **`template <bool Trace> void exec()`**: `if constexpr (Trace)` removes all commit-record code from the fast path *at compile time*, with no runtime cost when tracing is off. `run()` picks `exec<false>` when there is no tracer.

Ideas not done yet, and good to mention if asked "what next?":
- **Threaded dispatch**: store a function pointer or label address in each decoded instruction instead of `switch`.
- **Basic-block caching**: execute a whole straight-line run without returning to the main loop.
- **A JIT**: translate hot blocks to x86, as QEMU does.

---

## 9. Testing strategy in one paragraph

Four independent layers check correctness:

- **Unit tests** check decoder fields and individual instruction semantics, including corner cases.
- **The official riscv-tests** are the spec authority, written by the people who wrote the spec.
- **The benchmarks** run real compiled C and compare its output with a native build.
- **Co-simulation** compares against independent hardware, and mutation testing proves that comparison has teeth.

CI (`.github/workflows/ci.yml`) runs all of them on every push.

---

## 10. Quick-fire interview answers

- **Why C++?** Control over memory layout (the page struct, the decode cache), zero-cost abstractions like `if constexpr`, and it's the language real simulators (Spike, gem5) are written in.
- **What's precise about your traps?** The faulting instruction has no architectural effect, every older instruction has fully completed, and `mepc` points exactly at the faulting instruction.
- **How do you know it's correct?** 64/64 official tests, checked benchmark outputs, and 141K instructions matched against independent hardware.
- **Hardest bug?** `ma_fetch`: I assumed a misaligned pc traps on fetch. Without the C extension, the spec says the *jump* traps, so `mepc` must point at the jump instruction, not its target.
- **What did you learn about your own CPU?** It silently executes 15 instructions it doesn't support. A reference model plus a fuzzer found that in seconds. Directed tests had missed it because they only used supported instructions.
- **Spike vs. yours?** Spike supports far more extensions (RV64, vector, virtual memory). Mine is deliberately small: RV32IM, M-mode only. It uses the same commit-log format, so the two are interchangeable for co-simulation.

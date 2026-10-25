// A single RV32IM hart running in machine mode, with Zicsr and precise traps.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "rv32sim/decoder.hpp"
#include "rv32sim/memory.hpp"

namespace rv32sim {

namespace csr {
constexpr uint16_t mstatus = 0x300, misa = 0x301, medeleg = 0x302, mideleg = 0x303, mie = 0x304,
                   mtvec = 0x305, mscratch = 0x340, mepc = 0x341, mcause = 0x342, mtval = 0x343,
                   mip = 0x344, mcycle = 0xB00, minstret = 0xB02, mcycleh = 0xB80, minstreth = 0xB82,
                   cycle = 0xC00, time = 0xC01, instret = 0xC02, cycleh = 0xC80, timeh = 0xC81,
                   instreth = 0xC82, mvendorid = 0xF11, marchid = 0xF12, mimpid = 0xF13, mhartid = 0xF14;
}

enum class Cause : uint32_t {
    InstrMisaligned = 0, InstrAccessFault = 1, IllegalInstruction = 2, Breakpoint = 3,
    LoadMisaligned = 4, LoadAccessFault = 5, StoreMisaligned = 6, StoreAccessFault = 7,
    EcallM = 11,
};

// What the hart committed in one step — the unit of comparison for lockstep
// co-simulation against RTL and for Spike-style commit logs.
struct Commit {
    uint32_t pc = 0;
    uint32_t insn = 0;
    bool reg_write = false;
    uint8_t rd = 0;
    uint32_t rd_value = 0;
    bool mem_write = false;
    uint32_t mem_addr = 0;
    uint32_t mem_value = 0;
    uint8_t mem_size = 0;
    bool trapped = false;
};

enum class StopReason { None, Exit, ToHost, InstLimit, SelfLoop, Ebreak, Trap };

class Cpu {
public:
    // Von Neumann: one memory for code and data (what riscv-tests and real
    // RISC-V systems use).
    explicit Cpu(Memory& mem) : imem_(mem), mem_(mem) {}
    // Harvard: separate instruction and data memories, matching simple
    // single-cycle cores such as simple-riscv-cpu (imem.v / dmem.v).
    Cpu(Memory& imem, Memory& dmem) : imem_(imem), mem_(dmem) {}

    void reset(uint32_t pc);
    // Execute one instruction (or take one trap). Returns what was committed.
    Commit step();
    // Run until a stop condition. Returns the reason.
    StopReason run(uint64_t max_instructions);

    uint32_t pc() const { return pc_; }
    uint32_t reg(unsigned i) const { return x_[i]; }
    void set_reg(unsigned i, uint32_t v) { if (i) x_[i] = v; }
    uint64_t instret() const { return instret_; }
    uint64_t cycles() const { return mcycle_; }
    // Returns nullopt for CSRs this hart does not implement (access raises illegal-instruction).
    std::optional<uint32_t> read_csr(uint16_t addr) const;

    // Host-call mode: ecall is serviced by the simulator (exit/write) instead of
    // trapping to mtvec. Used for bare-metal benchmarks; riscv-tests use traps.
    bool host_syscalls = false;
    // Address that the riscv-tests environment writes its result to (if any).
    std::optional<uint32_t> tohost;
    // Stop when a branch/jump targets itself (the idiom bare programs end with).
    bool stop_on_self_loop = false;
    // Stop at the first trap instead of entering mtvec (for hex images that have
    // no trap handler and simply end by running into zeroed memory).
    bool stop_on_trap = false;

    int exit_code() const { return exit_code_; }
    StopReason stop_reason() const { return stop_; }
    std::function<void(const Commit&)> on_commit;  // optional tracer
    std::string console;                            // bytes written via the write() host call

private:
    template <bool Trace>
    void exec();
    void trap(Cause cause, uint32_t tval);
    bool csr_access(uint16_t addr, uint32_t write_value, bool do_write, uint32_t& old_value);
    void host_call();

    Memory& imem_;
    Memory& mem_;
    std::array<uint32_t, 32> x_{};
    uint32_t pc_ = 0;
    uint64_t instret_ = 0;

    uint32_t mstatus_ = 0, mie_ = 0, mtvec_ = 0, mscratch_ = 0, mepc_ = 0, mcause_ = 0, mtval_ = 0, mip_ = 0;
    uint64_t mcycle_ = 0;
    // A CSR write to a counter replaces that instruction's own increment.
    bool suppress_instret_ = false, suppress_cycle_ = false;
    bool trapped_ = false;

    StopReason stop_ = StopReason::None;
    int exit_code_ = 0;
    Commit pending_;
};

}  // namespace rv32sim

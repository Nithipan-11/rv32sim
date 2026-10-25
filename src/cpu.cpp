#include "rv32sim/cpu.hpp"

#include <cstdio>

namespace rv32sim {
namespace {

constexpr uint32_t MSTATUS_MIE = 1u << 3;
constexpr uint32_t MSTATUS_MPIE = 1u << 7;
constexpr uint32_t MSTATUS_MPP = 3u << 11;
// RV32 + I + M
constexpr uint32_t MISA = (1u << 30) | (1u << ('I' - 'A')) | (1u << ('M' - 'A'));

}  // namespace

void Cpu::reset(uint32_t pc) {
    x_.fill(0);
    pc_ = pc;
    instret_ = 0;
    mstatus_ = MSTATUS_MPP;  // machine mode only, so MPP reads as M
    mie_ = mtvec_ = mscratch_ = mepc_ = mcause_ = mtval_ = mip_ = 0;
    mcycle_ = 0;
    stop_ = StopReason::None;
    exit_code_ = 0;
    console.clear();
}

void Cpu::trap(Cause cause, uint32_t tval) {
    mepc_ = pc_;
    mcause_ = static_cast<uint32_t>(cause);
    mtval_ = tval;
    // Save MIE into MPIE and disable interrupts; we are always in M-mode.
    mstatus_ = (mstatus_ & ~MSTATUS_MPIE) | ((mstatus_ & MSTATUS_MIE) ? MSTATUS_MPIE : 0);
    mstatus_ &= ~MSTATUS_MIE;
    mstatus_ |= MSTATUS_MPP;
    trapped_ = true;
    pending_.trapped = true;
    if (stop_on_trap) {
        stop_ = StopReason::Trap;  // leave pc on the faulting instruction
        return;
    }
    pc_ = mtvec_ & ~3u;  // synchronous exceptions always use the base address
}

std::optional<uint32_t> Cpu::read_csr(uint16_t addr) const {
    switch (addr) {
        case csr::mstatus: return mstatus_;
        case csr::misa: return MISA;
        case csr::mie: return mie_;
        case csr::mtvec: return mtvec_;
        case csr::mscratch: return mscratch_;
        case csr::mepc: return mepc_;
        case csr::mcause: return mcause_;
        case csr::mtval: return mtval_;
        case csr::mip: return mip_;
        // One instruction per cycle, so cycle and time advance with instret.
        case csr::mcycle: case csr::cycle: case csr::time: return static_cast<uint32_t>(mcycle_);
        case csr::mcycleh: case csr::cycleh: case csr::timeh: return static_cast<uint32_t>(mcycle_ >> 32);
        case csr::minstret: case csr::instret: return static_cast<uint32_t>(instret_);
        case csr::minstreth: case csr::instreth: return static_cast<uint32_t>(instret_ >> 32);
        case csr::mvendorid: case csr::marchid: case csr::mimpid: case csr::mhartid: return 0u;
        // Everything else (satp, pmp*, medeleg, triggers, ...) is not implemented:
        // this is an M-mode-only hart without S/U modes, PMP or debug triggers.
        default: return std::nullopt;
    }
}

// Returns false if the access is illegal (caller raises an illegal-instruction trap).
bool Cpu::csr_access(uint16_t addr, uint32_t value, bool do_write, uint32_t& old) {
    const auto current = read_csr(addr);
    if (!current) return false;
    const bool read_only = (addr >> 10) == 3;
    if (do_write && read_only) return false;
    old = *current;
    if (!do_write) return true;
    switch (addr) {
        case csr::mstatus: mstatus_ = (value & (MSTATUS_MIE | MSTATUS_MPIE)) | MSTATUS_MPP; break;
        case csr::misa: break;  // WARL: writes ignored
        case csr::mie: mie_ = value; break;
        case csr::mtvec: mtvec_ = value & ~2u; break;  // modes 0 (direct) and 1 (vectored)
        case csr::mscratch: mscratch_ = value; break;
        case csr::mepc: mepc_ = value & ~3u; break;
        case csr::mcause: mcause_ = value; break;
        case csr::mtval: mtval_ = value; break;
        case csr::mip: mip_ = value; break;
        case csr::minstret:
            instret_ = (instret_ & 0xFFFFFFFF00000000ull) | value;
            suppress_instret_ = true;
            break;
        case csr::minstreth:
            instret_ = (instret_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
            suppress_instret_ = true;
            break;
        case csr::mcycle:
            mcycle_ = (mcycle_ & 0xFFFFFFFF00000000ull) | value;
            suppress_cycle_ = true;
            break;
        case csr::mcycleh:
            mcycle_ = (mcycle_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
            suppress_cycle_ = true;
            break;
        default: break;
    }
    return true;
}

void Cpu::host_call() {
    const uint32_t n = x_[17];  // a7: Linux-style syscall numbers
    switch (n) {
        case 93:  // exit(code)
            exit_code_ = static_cast<int32_t>(x_[10]);
            stop_ = StopReason::Exit;
            break;
        case 64: {  // write(fd, buf, len)
            const uint32_t buf = x_[11], len = x_[12];
            for (uint32_t i = 0; i < len; ++i) console.push_back(static_cast<char>(mem_.load8(buf + i)));
            x_[10] = len;
            break;
        }
        default:
            x_[10] = static_cast<uint32_t>(-38);  // -ENOSYS
    }
}

// The interpreter body. Trace=false is the hot path used by run() when no tracer
// is attached: it skips building Commit records entirely.
template <bool Trace>
void Cpu::exec() {
    trapped_ = false;
    if constexpr (Trace) {
        pending_ = Commit{};
        pending_.pc = pc_;
    }

    if (pc_ & 3u) {  // only reachable via mepc/mtvec writes; jumps check their targets
        trap(Cause::InstrMisaligned, pc_);
        return;
    }

    const Inst& in = imem_.fetch(pc_);
    if constexpr (Trace) pending_.insn = in.raw;
    const uint32_t rs1 = x_[in.rs1];
    const uint32_t rs2 = x_[in.rs2];
    const uint32_t imm = static_cast<uint32_t>(in.imm);
    uint32_t next = pc_ + 4;
    uint32_t rd_val = 0;
    bool writes_rd = true;

    // Without the C extension a jump/branch to a non-4-byte-aligned target raises
    // instruction-address-misaligned on the jump itself (mepc = jump, mtval = target).
    auto jump_to = [&](uint32_t target) {
        if (target & 3u) {
            trap(Cause::InstrMisaligned, target);
            return;
        }
        next = target;
    };
    auto branch = [&](bool taken) {
        writes_rd = false;
        if (taken) jump_to(pc_ + imm);
    };
    auto store = [&](uint32_t addr, uint32_t v, uint8_t size) {
        writes_rd = false;
        switch (size) {
            case 1: mem_.store8(addr, static_cast<uint8_t>(v)); v &= 0xFF; break;
            case 2: mem_.store16(addr, static_cast<uint16_t>(v)); v &= 0xFFFF; break;
            default: mem_.store32(addr, v); break;
        }
        if constexpr (Trace) {
            pending_.mem_write = true;
            pending_.mem_addr = addr;
            pending_.mem_value = v;
            pending_.mem_size = size;
        }
        if (tohost && addr == *tohost && v != 0) stop_ = StopReason::ToHost;
    };
    auto illegal = [&]() { trap(Cause::IllegalInstruction, in.raw); };

    switch (in.op) {
        case Op::Lui: rd_val = imm; break;
        case Op::Auipc: rd_val = pc_ + imm; break;
        case Op::Jal:
            rd_val = pc_ + 4;
            jump_to(pc_ + imm);
            break;
        case Op::Jalr:
            rd_val = pc_ + 4;
            jump_to((rs1 + imm) & ~1u);
            break;
        case Op::Beq: branch(rs1 == rs2); break;
        case Op::Bne: branch(rs1 != rs2); break;
        case Op::Blt: branch(static_cast<int32_t>(rs1) < static_cast<int32_t>(rs2)); break;
        case Op::Bge: branch(static_cast<int32_t>(rs1) >= static_cast<int32_t>(rs2)); break;
        case Op::Bltu: branch(rs1 < rs2); break;
        case Op::Bgeu: branch(rs1 >= rs2); break;

        // Misaligned loads/stores are handled in "hardware" (allowed by the spec).
        case Op::Lb: rd_val = static_cast<uint32_t>(static_cast<int8_t>(mem_.load8(rs1 + imm))); break;
        case Op::Lh: rd_val = static_cast<uint32_t>(static_cast<int16_t>(mem_.load16(rs1 + imm))); break;
        case Op::Lw: rd_val = mem_.load32(rs1 + imm); break;
        case Op::Lbu: rd_val = mem_.load8(rs1 + imm); break;
        case Op::Lhu: rd_val = mem_.load16(rs1 + imm); break;
        case Op::Sb: store(rs1 + imm, rs2, 1); break;
        case Op::Sh: store(rs1 + imm, rs2, 2); break;
        case Op::Sw: store(rs1 + imm, rs2, 4); break;

        case Op::Addi: rd_val = rs1 + imm; break;
        case Op::Slti: rd_val = static_cast<int32_t>(rs1) < static_cast<int32_t>(imm); break;
        case Op::Sltiu: rd_val = rs1 < imm; break;
        case Op::Xori: rd_val = rs1 ^ imm; break;
        case Op::Ori: rd_val = rs1 | imm; break;
        case Op::Andi: rd_val = rs1 & imm; break;
        case Op::Slli: rd_val = rs1 << (imm & 31); break;
        case Op::Srli: rd_val = rs1 >> (imm & 31); break;
        case Op::Srai: rd_val = static_cast<uint32_t>(static_cast<int32_t>(rs1) >> (imm & 31)); break;

        case Op::Add: rd_val = rs1 + rs2; break;
        case Op::Sub: rd_val = rs1 - rs2; break;
        case Op::Sll: rd_val = rs1 << (rs2 & 31); break;
        case Op::Slt: rd_val = static_cast<int32_t>(rs1) < static_cast<int32_t>(rs2); break;
        case Op::Sltu: rd_val = rs1 < rs2; break;
        case Op::Xor: rd_val = rs1 ^ rs2; break;
        case Op::Srl: rd_val = rs1 >> (rs2 & 31); break;
        case Op::Sra: rd_val = static_cast<uint32_t>(static_cast<int32_t>(rs1) >> (rs2 & 31)); break;
        case Op::Or: rd_val = rs1 | rs2; break;
        case Op::And: rd_val = rs1 & rs2; break;

        case Op::Mul: rd_val = rs1 * rs2; break;
        case Op::Mulh:
            rd_val = static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(rs1)) *
                                            static_cast<int64_t>(static_cast<int32_t>(rs2))) >> 32);
            break;
        case Op::Mulhsu:
            rd_val = static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(rs1)) *
                                            static_cast<int64_t>(static_cast<uint64_t>(rs2))) >> 32);
            break;
        case Op::Mulhu:
            rd_val = static_cast<uint32_t>((static_cast<uint64_t>(rs1) * static_cast<uint64_t>(rs2)) >> 32);
            break;
        // Division edge cases are defined by the spec (no traps): x/0 = -1, x%0 = x,
        // and INT_MIN / -1 overflows to INT_MIN with remainder 0.
        case Op::Div:
            if (rs2 == 0) rd_val = 0xFFFFFFFFu;
            else if (rs1 == 0x80000000u && rs2 == 0xFFFFFFFFu) rd_val = rs1;
            else rd_val = static_cast<uint32_t>(static_cast<int32_t>(rs1) / static_cast<int32_t>(rs2));
            break;
        case Op::Divu: rd_val = rs2 == 0 ? 0xFFFFFFFFu : rs1 / rs2; break;
        case Op::Rem:
            if (rs2 == 0) rd_val = rs1;
            else if (rs1 == 0x80000000u && rs2 == 0xFFFFFFFFu) rd_val = 0;
            else rd_val = static_cast<uint32_t>(static_cast<int32_t>(rs1) % static_cast<int32_t>(rs2));
            break;
        case Op::Remu: rd_val = rs2 == 0 ? rs1 : rs1 % rs2; break;

        case Op::Fence: case Op::Wfi: writes_rd = false; break;
        case Op::FenceI: writes_rd = false; imem_.flush_decode_cache(); break;
        case Op::Ecall:
            writes_rd = false;
            if (host_syscalls) host_call();
            else { trap(Cause::EcallM, 0); }
            break;
        case Op::Ebreak:
            writes_rd = false;
            if (host_syscalls) stop_ = StopReason::Ebreak;
            else trap(Cause::Breakpoint, pc_);
            break;
        case Op::Mret:
            writes_rd = false;
            next = mepc_;
            mstatus_ = (mstatus_ & ~MSTATUS_MIE) | ((mstatus_ & MSTATUS_MPIE) ? MSTATUS_MIE : 0);
            mstatus_ |= MSTATUS_MPIE;
            break;

        case Op::Csrrw: case Op::Csrrs: case Op::Csrrc:
        case Op::Csrrwi: case Op::Csrrsi: case Op::Csrrci: {
            const bool is_imm = in.op >= Op::Csrrwi;
            const uint32_t src = is_imm ? imm : rs1;
            const Op base = is_imm ? static_cast<Op>(static_cast<int>(in.op) - 3) : in.op;
            // csrrs/csrrc with x0 / zero immediate must not write (no side effects on read-only CSRs).
            const bool do_write = base == Op::Csrrw || in.rs1 != 0;
            uint32_t old = 0;
            const uint32_t current = read_csr(in.csr).value_or(0);
            uint32_t nv = src;
            if (base == Op::Csrrs) nv = current | src;
            else if (base == Op::Csrrc) nv = current & ~src;
            if (!csr_access(in.csr, nv, do_write, old)) { illegal(); break; }
            rd_val = old;
            break;
        }

        case Op::Illegal:
        default:
            illegal();
            break;
    }

    if (!trapped_) {
        if (writes_rd && in.rd != 0) {
            x_[in.rd] = rd_val;
            if constexpr (Trace) {
                pending_.reg_write = true;
                pending_.rd = in.rd;
                pending_.rd_value = rd_val;
            }
        }
        if (stop_on_self_loop && next == pc_) stop_ = StopReason::SelfLoop;
        pc_ = next;
        if (!suppress_instret_) ++instret_;
    }
    if (!suppress_cycle_) ++mcycle_;
    suppress_instret_ = suppress_cycle_ = false;
}

Commit Cpu::step() {
    exec<true>();
    if (on_commit) on_commit(pending_);
    return pending_;
}

StopReason Cpu::run(uint64_t max_instructions) {
    uint64_t steps = 0;
    if (on_commit) {
        while (stop_ == StopReason::None && steps++ < max_instructions) step();
    } else {
        while (stop_ == StopReason::None && steps++ < max_instructions) exec<false>();
    }
    if (stop_ == StopReason::None) stop_ = StopReason::InstLimit;
    return stop_;
}

}  // namespace rv32sim

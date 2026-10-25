// Instruction decoder: turns a 32-bit RV32IM/Zicsr instruction word into a
// flat struct so the execute loop never has to re-extract bit fields.
#pragma once

#include <cstdint>
#include <string>

namespace rv32sim {

enum class Op : uint8_t {
    Illegal,
    // RV32I
    Lui, Auipc, Jal, Jalr,
    Beq, Bne, Blt, Bge, Bltu, Bgeu,
    Lb, Lh, Lw, Lbu, Lhu,
    Sb, Sh, Sw,
    Addi, Slti, Sltiu, Xori, Ori, Andi, Slli, Srli, Srai,
    Add, Sub, Sll, Slt, Sltu, Xor, Srl, Sra, Or, And,
    Fence, FenceI, Ecall, Ebreak, Mret, Wfi,
    // Zicsr
    Csrrw, Csrrs, Csrrc, Csrrwi, Csrrsi, Csrrci,
    // RV32M
    Mul, Mulh, Mulhsu, Mulhu, Div, Divu, Rem, Remu,
    Count_
};

struct Inst {
    Op op = Op::Illegal;
    uint8_t rd = 0, rs1 = 0, rs2 = 0;
    int32_t imm = 0;     // sign-extended immediate (or zimm / shamt)
    uint16_t csr = 0;    // CSR address for Zicsr ops
    uint32_t raw = 0;    // original encoding, for traces and mtval
};

Inst decode(uint32_t word);

// Human-readable mnemonic, e.g. "addi" — used by the disassembler and traces.
const char* mnemonic(Op op);
std::string disassemble(const Inst& in, uint32_t pc);

}  // namespace rv32sim

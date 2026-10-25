#include "rv32sim/decoder.hpp"

#include <array>
#include <cstdio>

namespace rv32sim {
namespace {

constexpr uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1u); }

constexpr int32_t sext(uint32_t v, int width) {
    const uint32_t m = 1u << (width - 1);
    return static_cast<int32_t>((v ^ m) - m);
}

int32_t imm_i(uint32_t w) { return sext(bits(w, 31, 20), 12); }
int32_t imm_s(uint32_t w) { return sext((bits(w, 31, 25) << 5) | bits(w, 11, 7), 12); }
int32_t imm_b(uint32_t w) {
    return sext((bits(w, 31, 31) << 12) | (bits(w, 7, 7) << 11) | (bits(w, 30, 25) << 5) | (bits(w, 11, 8) << 1), 13);
}
int32_t imm_u(uint32_t w) { return static_cast<int32_t>(w & 0xFFFFF000u); }
int32_t imm_j(uint32_t w) {
    return sext((bits(w, 31, 31) << 20) | (bits(w, 19, 12) << 12) | (bits(w, 20, 20) << 11) | (bits(w, 30, 21) << 1), 21);
}

}  // namespace

Inst decode(uint32_t w) {
    Inst in;
    in.raw = w;
    in.rd = static_cast<uint8_t>(bits(w, 11, 7));
    in.rs1 = static_cast<uint8_t>(bits(w, 19, 15));
    in.rs2 = static_cast<uint8_t>(bits(w, 24, 20));
    const uint32_t opcode = bits(w, 6, 0);
    const uint32_t f3 = bits(w, 14, 12);
    const uint32_t f7 = bits(w, 31, 25);

    if ((w & 3u) != 3u) return in;  // compressed encodings are not supported

    switch (opcode) {
        case 0x37: in.op = Op::Lui; in.imm = imm_u(w); break;
        case 0x17: in.op = Op::Auipc; in.imm = imm_u(w); break;
        case 0x6F: in.op = Op::Jal; in.imm = imm_j(w); break;
        case 0x67:
            if (f3 == 0) { in.op = Op::Jalr; in.imm = imm_i(w); }
            break;
        case 0x63: {
            static constexpr std::array<Op, 8> ops{Op::Beq, Op::Bne, Op::Illegal, Op::Illegal,
                                                   Op::Blt, Op::Bge, Op::Bltu, Op::Bgeu};
            in.op = ops[f3];
            in.imm = imm_b(w);
            break;
        }
        case 0x03: {
            static constexpr std::array<Op, 8> ops{Op::Lb, Op::Lh, Op::Lw, Op::Illegal,
                                                   Op::Lbu, Op::Lhu, Op::Illegal, Op::Illegal};
            in.op = ops[f3];
            in.imm = imm_i(w);
            break;
        }
        case 0x23: {
            static constexpr std::array<Op, 8> ops{Op::Sb, Op::Sh, Op::Sw, Op::Illegal,
                                                   Op::Illegal, Op::Illegal, Op::Illegal, Op::Illegal};
            in.op = ops[f3];
            in.imm = imm_s(w);
            break;
        }
        case 0x13:
            in.imm = imm_i(w);
            switch (f3) {
                case 0: in.op = Op::Addi; break;
                case 2: in.op = Op::Slti; break;
                case 3: in.op = Op::Sltiu; break;
                case 4: in.op = Op::Xori; break;
                case 6: in.op = Op::Ori; break;
                case 7: in.op = Op::Andi; break;
                case 1:
                    if (f7 == 0x00) { in.op = Op::Slli; in.imm = static_cast<int32_t>(in.rs2); }
                    break;
                case 5:
                    if (f7 == 0x00) { in.op = Op::Srli; in.imm = static_cast<int32_t>(in.rs2); }
                    else if (f7 == 0x20) { in.op = Op::Srai; in.imm = static_cast<int32_t>(in.rs2); }
                    break;
            }
            break;
        case 0x33:
            if (f7 == 0x00) {
                static constexpr std::array<Op, 8> ops{Op::Add, Op::Sll, Op::Slt, Op::Sltu,
                                                       Op::Xor, Op::Srl, Op::Or, Op::And};
                in.op = ops[f3];
            } else if (f7 == 0x20) {
                if (f3 == 0) in.op = Op::Sub;
                else if (f3 == 5) in.op = Op::Sra;
            } else if (f7 == 0x01) {
                static constexpr std::array<Op, 8> ops{Op::Mul, Op::Mulh, Op::Mulhsu, Op::Mulhu,
                                                       Op::Div, Op::Divu, Op::Rem, Op::Remu};
                in.op = ops[f3];
            }
            break;
        case 0x0F:
            if (f3 == 0) in.op = Op::Fence;
            else if (f3 == 1) in.op = Op::FenceI;
            break;
        case 0x73:
            in.csr = static_cast<uint16_t>(bits(w, 31, 20));
            switch (f3) {
                case 0:
                    if (w == 0x00000073) in.op = Op::Ecall;
                    else if (w == 0x00100073) in.op = Op::Ebreak;
                    else if (w == 0x30200073) in.op = Op::Mret;
                    else if (w == 0x10500073) in.op = Op::Wfi;
                    break;
                case 1: in.op = Op::Csrrw; break;
                case 2: in.op = Op::Csrrs; break;
                case 3: in.op = Op::Csrrc; break;
                case 5: in.op = Op::Csrrwi; in.imm = in.rs1; break;
                case 6: in.op = Op::Csrrsi; in.imm = in.rs1; break;
                case 7: in.op = Op::Csrrci; in.imm = in.rs1; break;
            }
            break;
        default: break;
    }
    return in;
}

const char* mnemonic(Op op) {
    static constexpr const char* names[] = {
        "illegal", "lui", "auipc", "jal", "jalr", "beq", "bne", "blt", "bge", "bltu", "bgeu",
        "lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw",
        "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
        "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
        "fence", "fence.i", "ecall", "ebreak", "mret", "wfi",
        "csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci",
        "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Op::Count_));
    return names[static_cast<size_t>(op)];
}

std::string disassemble(const Inst& in, uint32_t pc) {
    char buf[96];
    const char* m = mnemonic(in.op);
    switch (in.op) {
        case Op::Lui: case Op::Auipc:
            std::snprintf(buf, sizeof buf, "%s x%u, 0x%x", m, in.rd, static_cast<uint32_t>(in.imm) >> 12);
            break;
        case Op::Jal:
            std::snprintf(buf, sizeof buf, "%s x%u, 0x%x", m, in.rd, pc + static_cast<uint32_t>(in.imm));
            break;
        case Op::Beq: case Op::Bne: case Op::Blt: case Op::Bge: case Op::Bltu: case Op::Bgeu:
            std::snprintf(buf, sizeof buf, "%s x%u, x%u, 0x%x", m, in.rs1, in.rs2, pc + static_cast<uint32_t>(in.imm));
            break;
        case Op::Lb: case Op::Lh: case Op::Lw: case Op::Lbu: case Op::Lhu: case Op::Jalr:
            std::snprintf(buf, sizeof buf, "%s x%u, %d(x%u)", m, in.rd, in.imm, in.rs1);
            break;
        case Op::Sb: case Op::Sh: case Op::Sw:
            std::snprintf(buf, sizeof buf, "%s x%u, %d(x%u)", m, in.rs2, in.imm, in.rs1);
            break;
        case Op::Addi: case Op::Slti: case Op::Sltiu: case Op::Xori: case Op::Ori: case Op::Andi:
        case Op::Slli: case Op::Srli: case Op::Srai:
            std::snprintf(buf, sizeof buf, "%s x%u, x%u, %d", m, in.rd, in.rs1, in.imm);
            break;
        case Op::Csrrw: case Op::Csrrs: case Op::Csrrc:
            std::snprintf(buf, sizeof buf, "%s x%u, 0x%03x, x%u", m, in.rd, in.csr, in.rs1);
            break;
        case Op::Csrrwi: case Op::Csrrsi: case Op::Csrrci:
            std::snprintf(buf, sizeof buf, "%s x%u, 0x%03x, %d", m, in.rd, in.csr, in.imm);
            break;
        case Op::Fence: case Op::FenceI: case Op::Ecall: case Op::Ebreak: case Op::Mret: case Op::Wfi:
            std::snprintf(buf, sizeof buf, "%s", m);
            break;
        case Op::Illegal:
            std::snprintf(buf, sizeof buf, "illegal 0x%08x", in.raw);
            break;
        default:
            std::snprintf(buf, sizeof buf, "%s x%u, x%u, x%u", m, in.rd, in.rs1, in.rs2);
    }
    return buf;
}

}  // namespace rv32sim

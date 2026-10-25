// Tiny instruction encoder for building test programs in C++.
#pragma once

#include <cstdint>
#include <vector>

#include "rv32sim/memory.hpp"

namespace enc {

inline uint32_t r(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
inline uint32_t i(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
    return (static_cast<uint32_t>(imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
inline uint32_t s(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((u & 0x1F) << 7) | 0x23;
}
inline uint32_t b(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           (((u >> 1) & 0xF) << 8) | (((u >> 11) & 1) << 7) | 0x63;
}
inline uint32_t u(uint32_t imm20, uint32_t rd, uint32_t op) { return (imm20 << 12) | (rd << 7) | op; }
inline uint32_t j(int32_t imm, uint32_t rd) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 20) & 1) << 31) | (((u >> 1) & 0x3FF) << 21) | (((u >> 11) & 1) << 20) |
           (((u >> 12) & 0xFF) << 12) | (rd << 7) | 0x6F;
}

inline uint32_t addi(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 0, rd, 0x13); }
inline uint32_t add(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(0, rs2, rs1, 0, rd, 0x33); }
inline uint32_t sub(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(0x20, rs2, rs1, 0, rd, 0x33); }
inline uint32_t mul(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(1, rs2, rs1, 0, rd, 0x33); }
inline uint32_t mulh(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(1, rs2, rs1, 1, rd, 0x33); }
inline uint32_t mulhu(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(1, rs2, rs1, 3, rd, 0x33); }
inline uint32_t div(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(1, rs2, rs1, 4, rd, 0x33); }
inline uint32_t divu(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(1, rs2, rs1, 5, rd, 0x33); }
inline uint32_t rem(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(1, rs2, rs1, 6, rd, 0x33); }
inline uint32_t sra(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(0x20, rs2, rs1, 5, rd, 0x33); }
inline uint32_t srai(uint32_t rd, uint32_t rs1, uint32_t sh) { return r(0x20, sh, rs1, 5, rd, 0x13); }
inline uint32_t slt(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(0, rs2, rs1, 2, rd, 0x33); }
inline uint32_t sltu(uint32_t rd, uint32_t rs1, uint32_t rs2) { return r(0, rs2, rs1, 3, rd, 0x33); }
inline uint32_t lui(uint32_t rd, uint32_t imm20) { return u(imm20, rd, 0x37); }
inline uint32_t auipc(uint32_t rd, uint32_t imm20) { return u(imm20, rd, 0x17); }
inline uint32_t jal(uint32_t rd, int32_t off) { return j(off, rd); }
inline uint32_t jalr(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 0, rd, 0x67); }
inline uint32_t beq(uint32_t rs1, uint32_t rs2, int32_t off) { return b(off, rs2, rs1, 0); }
inline uint32_t bne(uint32_t rs1, uint32_t rs2, int32_t off) { return b(off, rs2, rs1, 1); }
inline uint32_t blt(uint32_t rs1, uint32_t rs2, int32_t off) { return b(off, rs2, rs1, 4); }
inline uint32_t bltu(uint32_t rs1, uint32_t rs2, int32_t off) { return b(off, rs2, rs1, 6); }
inline uint32_t lw(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 2, rd, 0x03); }
inline uint32_t lb(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 0, rd, 0x03); }
inline uint32_t lbu(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 4, rd, 0x03); }
inline uint32_t lh(uint32_t rd, uint32_t rs1, int32_t imm) { return i(imm, rs1, 1, rd, 0x03); }
inline uint32_t sw(uint32_t rs2, uint32_t rs1, int32_t imm) { return s(imm, rs2, rs1, 2); }
inline uint32_t sb(uint32_t rs2, uint32_t rs1, int32_t imm) { return s(imm, rs2, rs1, 0); }
inline uint32_t csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) { return i(static_cast<int32_t>(csr), rs1, 1, rd, 0x73); }
inline uint32_t csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) { return i(static_cast<int32_t>(csr), rs1, 2, rd, 0x73); }
inline constexpr uint32_t ecall = 0x00000073, ebreak = 0x00100073, mret = 0x30200073, fence_i = 0x0000100F;

inline void place(rv32sim::Memory& m, uint32_t base, const std::vector<uint32_t>& words) {
    for (size_t k = 0; k < words.size(); ++k) m.store32(base + 4 * static_cast<uint32_t>(k), words[k]);
}

}  // namespace enc

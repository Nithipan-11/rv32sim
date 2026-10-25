#include <gtest/gtest.h>

#include "encode.hpp"
#include "rv32sim/decoder.hpp"

using namespace rv32sim;

TEST(Decoder, ITypeSignExtendsImmediate) {
    const Inst in = decode(enc::addi(5, 6, -1));
    EXPECT_EQ(in.op, Op::Addi);
    EXPECT_EQ(in.rd, 5);
    EXPECT_EQ(in.rs1, 6);
    EXPECT_EQ(in.imm, -1);
}

TEST(Decoder, BranchImmediateRoundTripsAcrossRange) {
    for (int32_t off = -4096; off < 4096; off += 2) {
        const Inst in = decode(enc::beq(1, 2, off));
        ASSERT_EQ(in.op, Op::Beq);
        ASSERT_EQ(in.imm, off) << "offset " << off;
    }
}

TEST(Decoder, JalImmediateRoundTripsAtExtremes) {
    for (int32_t off : {-(1 << 20), -2, 2, 2048, (1 << 20) - 2}) {
        const Inst in = decode(enc::jal(1, off));
        ASSERT_EQ(in.op, Op::Jal);
        EXPECT_EQ(in.imm, off);
    }
}

TEST(Decoder, StoreImmediateSplitsAcrossFields) {
    for (int32_t off : {-2048, -1, 0, 31, 32, 2047}) {
        const Inst in = decode(enc::sw(3, 4, off));
        ASSERT_EQ(in.op, Op::Sw);
        EXPECT_EQ(in.imm, off);
        EXPECT_EQ(in.rs2, 3);
        EXPECT_EQ(in.rs1, 4);
    }
}

TEST(Decoder, DistinguishesShiftsAndMExtension) {
    EXPECT_EQ(decode(enc::srai(1, 2, 7)).op, Op::Srai);
    EXPECT_EQ(decode(enc::srai(1, 2, 7)).imm, 7);
    EXPECT_EQ(decode(enc::sra(1, 2, 3)).op, Op::Sra);
    EXPECT_EQ(decode(enc::sub(1, 2, 3)).op, Op::Sub);
    EXPECT_EQ(decode(enc::mul(1, 2, 3)).op, Op::Mul);
    EXPECT_EQ(decode(enc::divu(1, 2, 3)).op, Op::Divu);
}

TEST(Decoder, SystemInstructions) {
    EXPECT_EQ(decode(enc::ecall).op, Op::Ecall);
    EXPECT_EQ(decode(enc::ebreak).op, Op::Ebreak);
    EXPECT_EQ(decode(enc::mret).op, Op::Mret);
    EXPECT_EQ(decode(enc::fence_i).op, Op::FenceI);
    const Inst c = decode(enc::csrrw(1, 0x305, 2));
    EXPECT_EQ(c.op, Op::Csrrw);
    EXPECT_EQ(c.csr, 0x305);
}

TEST(Decoder, RejectsReservedEncodings) {
    EXPECT_EQ(decode(0x00000000).op, Op::Illegal);   // all zeros is defined as illegal
    EXPECT_EQ(decode(0xFFFFFFFF).op, Op::Illegal);
    EXPECT_EQ(decode(0x0000).op, Op::Illegal);       // compressed (low bits != 11)
    EXPECT_EQ(decode(enc::r(0x10, 1, 2, 0, 3, 0x33)).op, Op::Illegal);  // bad funct7
}

TEST(Decoder, Disassembles) {
    EXPECT_EQ(disassemble(decode(enc::addi(1, 0, 5)), 0), "addi x1, x0, 5");
    EXPECT_EQ(disassemble(decode(enc::sw(3, 0, 0)), 0), "sw x3, 0(x0)");
    EXPECT_EQ(disassemble(decode(enc::beq(1, 1, 8)), 0x14), "beq x1, x1, 0x1c");
}

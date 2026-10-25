#include <gtest/gtest.h>

#include <climits>

#include "encode.hpp"
#include "rv32sim/cpu.hpp"

using namespace rv32sim;
using namespace enc;

namespace {

struct Machine {
    Memory mem;
    Cpu cpu{mem};
    explicit Machine(const std::vector<uint32_t>& prog, uint32_t base = 0) {
        place(mem, base, prog);
        cpu.reset(base);
        cpu.stop_on_self_loop = true;
    }
    void run(uint64_t n = 10'000) { cpu.run(n); }
};

uint32_t li_hi(int32_t v) { return static_cast<uint32_t>((v + 0x800) >> 12) & 0xFFFFF; }

}  // namespace

TEST(Execute, ReadmeSampleProgramMatchesExpectedState) {
    // The sample program from the simple-riscv-cpu README.
    Machine m({addi(1, 0, 5), addi(2, 0, 7), add(3, 1, 2), sw(3, 0, 256), lw(4, 0, 256),
               beq(1, 1, 8), addi(5, 0, 99), addi(6, 0, 42), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.stop_reason(), StopReason::SelfLoop);
    EXPECT_EQ(m.cpu.reg(3), 12u);
    EXPECT_EQ(m.cpu.reg(4), 12u);
    EXPECT_EQ(m.cpu.reg(5), 0u);  // skipped by the branch
    EXPECT_EQ(m.cpu.reg(6), 42u);
    EXPECT_EQ(m.mem.load32(256), 12u);
}

TEST(Execute, X0IsHardwiredToZero) {
    Machine m({addi(0, 0, 123), add(1, 0, 0), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(0), 0u);
    EXPECT_EQ(m.cpu.reg(1), 0u);
}

TEST(Execute, LuiAddiBuildsArbitraryConstants) {
    const int32_t v = static_cast<int32_t>(0xDEADBEEF);
    Machine m({lui(1, li_hi(v)), addi(1, 1, static_cast<int32_t>(static_cast<uint32_t>(v) << 20) >> 20), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(1), 0xDEADBEEFu);
}

TEST(Execute, SignedVsUnsignedComparisons) {
    Machine m({addi(1, 0, -1), addi(2, 0, 1), slt(3, 1, 2), sltu(4, 1, 2), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(3), 1u);  // -1 < 1 signed
    EXPECT_EQ(m.cpu.reg(4), 0u);  // 0xFFFFFFFF > 1 unsigned
}

TEST(Execute, ArithmeticShiftKeepsSign) {
    Machine m({addi(1, 0, -16), srai(2, 1, 2), addi(3, 0, 4), sra(4, 1, 3), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(static_cast<int32_t>(m.cpu.reg(2)), -4);
    EXPECT_EQ(static_cast<int32_t>(m.cpu.reg(4)), -1);
}

TEST(Execute, JalAndJalrLinkAndReturn) {
    // 0: jal x1, +12 -> 12 ; 4: addi x5,x0,7 ; 8: self-loop ; 12: jalr x0, 0(x1)
    Machine m({jal(1, 12), addi(5, 0, 7), beq(0, 0, 0), jalr(0, 1, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(1), 4u);
    EXPECT_EQ(m.cpu.reg(5), 7u);
}

TEST(Execute, ByteAndHalfLoadsSignExtend) {
    Machine m({addi(1, 0, 0x80), sb(1, 0, 512), lb(2, 0, 512), lbu(3, 0, 512), lh(4, 0, 512), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(2), 0xFFFFFF80u);
    EXPECT_EQ(m.cpu.reg(3), 0x80u);
    EXPECT_EQ(m.cpu.reg(4), 0x80u);
}

TEST(Execute, MisalignedWordAccessWorks) {
    Machine m({lui(1, 0x12345), addi(1, 1, 0x678), sw(1, 0, 513), lw(2, 0, 513), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(2), 0x12345678u);
}

TEST(Execute, MultiplyHighHalves) {
    Machine m({addi(1, 0, -2), addi(2, 0, 3), mulh(3, 1, 2), mulhu(4, 1, 2), mul(5, 1, 2), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(3), 0xFFFFFFFFu);  // -6 >> 32
    EXPECT_EQ(m.cpu.reg(4), 2u);           // 0xFFFFFFFE * 3 >> 32
    EXPECT_EQ(static_cast<int32_t>(m.cpu.reg(5)), -6);
}

TEST(Execute, DivisionEdgeCasesFollowTheSpec) {
    // x1 = INT_MIN, x2 = -1, x3 = 0, x4 = 7
    Machine m({lui(1, 0x80000), addi(2, 0, -1), addi(4, 0, 7),
               div(5, 1, 2), rem(6, 1, 2), div(7, 4, 3), divu(8, 4, 3), rem(9, 4, 3), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(5), 0x80000000u);  // overflow returns the dividend
    EXPECT_EQ(m.cpu.reg(6), 0u);
    EXPECT_EQ(m.cpu.reg(7), 0xFFFFFFFFu);  // divide by zero = -1
    EXPECT_EQ(m.cpu.reg(8), 0xFFFFFFFFu);
    EXPECT_EQ(m.cpu.reg(9), 7u);           // remainder by zero = dividend
}

TEST(Execute, IllegalInstructionTrapsToMtvecAndMretReturns) {
    // 0: set mtvec = 0x100 ; 8: illegal ; 12: self-loop
    // handler at 0x100: mepc += 4 ; mret
    Machine m({addi(1, 0, 0x100), csrrw(0, csr::mtvec, 1), 0x00000000, beq(0, 0, 0)});
    place(m.mem, 0x100, {csrrs(2, csr::mepc, 0), addi(2, 2, 4), csrrw(0, csr::mepc, 2),
                         csrrs(3, csr::mcause, 0), csrrs(4, csr::mtval, 0), mret});
    m.run();
    EXPECT_EQ(m.cpu.stop_reason(), StopReason::SelfLoop);
    EXPECT_EQ(m.cpu.pc(), 12u);
    EXPECT_EQ(m.cpu.reg(3), static_cast<uint32_t>(Cause::IllegalInstruction));
    EXPECT_EQ(m.cpu.reg(4), 0u);  // mtval holds the faulting encoding
}

TEST(Execute, EcallTrapsWithMachineCause) {
    Machine m({addi(1, 0, 0x40), csrrw(0, csr::mtvec, 1), ecall});
    place(m.mem, 0x40, {csrrs(2, csr::mcause, 0), csrrs(3, csr::mepc, 0), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(2), 11u);
    EXPECT_EQ(m.cpu.reg(3), 8u);
}

TEST(Execute, WritingReadOnlyCsrIsIllegal) {
    Machine m({addi(1, 0, 0x40), csrrw(0, csr::mtvec, 1), csrrw(0, csr::mhartid, 1)});
    place(m.mem, 0x40, {csrrs(2, csr::mcause, 0), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.reg(2), 2u);
}

TEST(Execute, InstretCountsRetiredInstructionsOnly) {
    Machine m({addi(1, 0, 1), addi(1, 1, 1), addi(1, 1, 1), beq(0, 0, 0)});
    m.run();
    EXPECT_EQ(m.cpu.instret(), 4u);  // the self-loop branch retires once before stopping
}

TEST(Execute, HostSyscallsWriteAndExit) {
    // a0=1 (stdout), a1=buf, a2=2, a7=64 write ; then a0=3, a7=93 exit
    Machine m({addi(10, 0, 1), addi(11, 0, 0x200), addi(12, 0, 2), addi(17, 0, 64), ecall,
               addi(10, 0, 3), addi(17, 0, 93), ecall});
    m.mem.store8(0x200, 'h');
    m.mem.store8(0x201, 'i');
    m.cpu.host_syscalls = true;
    m.run();
    EXPECT_EQ(m.cpu.stop_reason(), StopReason::Exit);
    EXPECT_EQ(m.cpu.exit_code(), 3);
    EXPECT_EQ(m.cpu.console, "hi");
}

TEST(Execute, CommitRecordsRegisterAndMemoryEffects) {
    Machine m({addi(1, 0, 9), sw(1, 0, 64), beq(0, 0, 0)});
    std::vector<Commit> log;
    m.cpu.on_commit = [&](const Commit& c) { log.push_back(c); };
    m.run();
    ASSERT_EQ(log.size(), 3u);
    EXPECT_TRUE(log[0].reg_write);
    EXPECT_EQ(log[0].rd, 1);
    EXPECT_EQ(log[0].rd_value, 9u);
    EXPECT_TRUE(log[1].mem_write);
    EXPECT_EQ(log[1].mem_addr, 64u);
    EXPECT_FALSE(log[2].reg_write);
}

TEST(Execute, HarvardModeKeepsDataOutOfInstructionMemory) {
    // simple-riscv-cpu has separate imem/dmem: `sw` to address 0 must not
    // overwrite the instruction at address 0.
    Memory imem, dmem;
    place(imem, 0, {addi(1, 0, 7), sw(1, 0, 0), lw(2, 0, 0), beq(0, 0, 0)});
    Cpu cpu(imem, dmem);
    cpu.reset(0);
    cpu.stop_on_self_loop = true;
    cpu.run(100);
    EXPECT_EQ(cpu.stop_reason(), StopReason::SelfLoop);
    EXPECT_EQ(cpu.reg(2), 7u);
    EXPECT_EQ(dmem.load32(0), 7u);
    EXPECT_EQ(imem.load32(0), addi(1, 0, 7));
}

TEST(Execute, StopOnTrapLeavesPcOnFaultingInstruction) {
    Machine m({addi(1, 0, 1), 0x00000000u});
    m.cpu.stop_on_trap = true;
    m.run();
    EXPECT_EQ(m.cpu.stop_reason(), StopReason::Trap);
    EXPECT_EQ(m.cpu.pc(), 4u);
    EXPECT_EQ(m.cpu.read_csr(csr::mcause), static_cast<uint32_t>(Cause::IllegalInstruction));
}

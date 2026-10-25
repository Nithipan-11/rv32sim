#include <gtest/gtest.h>

#include "encode.hpp"
#include "rv32sim/cpu.hpp"
#include "rv32sim/memory.hpp"

using namespace rv32sim;

TEST(Memory, LittleEndianAndUntouchedMemoryReadsZero) {
    Memory m;
    m.store32(0x80000000, 0x11223344);
    EXPECT_EQ(m.load8(0x80000000), 0x44);
    EXPECT_EQ(m.load16(0x80000002), 0x1122);
    EXPECT_EQ(m.load32(0x12345678), 0u);
}

TEST(Memory, AccessesStraddlingPagesWork) {
    Memory m;
    m.store32(0xFFE, 0xA1B2C3D4);
    EXPECT_EQ(m.load32(0xFFE), 0xA1B2C3D4u);
    EXPECT_EQ(m.pages_allocated(), 2u);
}

TEST(Memory, DecodeCacheIsReusedAndInvalidatedByStores) {
    Memory m;
    m.store32(0x100, enc::addi(1, 0, 1));
    EXPECT_EQ(m.fetch(0x100).imm, 1);
    EXPECT_EQ(m.fetch(0x100).imm, 1);
    EXPECT_EQ(m.decodes(), 1u);
    EXPECT_EQ(m.cache_hits(), 1u);
    m.store32(0x100, enc::addi(1, 0, 2));  // overwrite the instruction
    EXPECT_EQ(m.fetch(0x100).imm, 2);
    EXPECT_EQ(m.decodes(), 2u);
}

TEST(Memory, SelfModifyingCodeExecutesTheNewInstruction) {
    // Program patches the instruction at 0x10 from "addi x5,x0,1" to "addi x5,x0,2"
    // after it has already run once, then jumps back to it.
    Memory mem;
    Cpu cpu(mem);
    const uint32_t patched = enc::addi(5, 0, 2);
    enc::place(mem, 0, {
        enc::lui(6, (patched + 0x800) >> 12),                                   // 0x00
        enc::addi(6, 6, static_cast<int32_t>(patched << 20) >> 20),             // 0x04
        enc::addi(7, 0, 0),                                                     // 0x08 pass counter
        enc::fence_i,                                                           // 0x0C
        enc::addi(5, 0, 1),                                                     // 0x10 patched target
        enc::bne(7, 0, 16),                                                     // 0x14 second pass -> 0x24
        enc::sw(6, 0, 0x10),                                                    // 0x18 patch 0x10
        enc::addi(7, 0, 1),                                                     // 0x1C
        enc::jal(0, -16),                                                       // 0x20 -> 0x10
        enc::beq(0, 0, 0),                                                      // 0x24
    });
    cpu.reset(0);
    cpu.stop_on_self_loop = true;
    cpu.run(100);
    EXPECT_EQ(cpu.reg(5), 2u);
}

TEST(Memory, ResultsAreIdenticalWithAndWithoutDecodeCache) {
    for (bool cache : {true, false}) {
        Memory mem;
        mem.set_decode_cache(cache);
        Cpu cpu(mem);
        // sum 1..100 in a loop
        enc::place(mem, 0, {enc::addi(1, 0, 100), enc::addi(2, 0, 0), enc::add(2, 2, 1), enc::addi(1, 1, -1),
                            enc::bne(1, 0, -8), enc::beq(0, 0, 0)});
        cpu.reset(0);
        cpu.stop_on_self_loop = true;
        cpu.run(10'000);
        EXPECT_EQ(cpu.reg(2), 5050u) << "cache=" << cache;
        EXPECT_EQ(cpu.instret(), 303u);
    }
}

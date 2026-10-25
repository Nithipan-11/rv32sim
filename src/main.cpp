// rv32sim command-line front end.
//
//   rv32sim prog.elf                       run a riscv-tests or bare-metal ELF
//   rv32sim --hex program.hex --base 0     run a $readmemh hex image
//   rv32sim --trace prog.elf               Spike-style commit log on stderr
//   rv32sim --stats prog.elf               instructions retired and MIPS
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "rv32sim/cpu.hpp"
#include "rv32sim/elf_loader.hpp"

using namespace rv32sim;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: rv32sim [options] <program>\n"
                 "  --hex                 program is a $readmemh hex file (one word per line)\n"
                 "  --base ADDR           load/start address for --hex (default 0)\n"
                 "  --max-insns N         stop after N instructions (default 1e10)\n"
                 "  --trace               print a commit log to stderr\n"
                 "  --stats               print instruction count and speed to stderr\n"
                 "  --no-decode-cache     decode every instruction on every fetch\n"
                 "  --dump-regs           print the register file on exit\n"
                 "  --stop-on-self-loop   stop when a jump/branch targets itself\n"
                 "  --harvard             separate instruction and data memories (for --hex images\n"
                 "                        from Harvard RTL such as simple-riscv-cpu)\n");
}

uint32_t parse_u32(const char* s) { return static_cast<uint32_t>(std::stoul(s, nullptr, 0)); }

}  // namespace

int main(int argc, char** argv) {
    bool hex = false, trace = false, stats = false, dump = false, self_loop = false, cache = true, harvard = false;
    uint32_t base = 0;
    uint64_t max_insns = 10'000'000'000ull;
    const char* path = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&]() -> const char* {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--hex") hex = true;
        else if (a == "--base") base = parse_u32(need());
        else if (a == "--max-insns") max_insns = std::stoull(need());
        else if (a == "--trace") trace = true;
        else if (a == "--stats") stats = true;
        else if (a == "--dump-regs") dump = true;
        else if (a == "--stop-on-self-loop") self_loop = true;
        else if (a == "--no-decode-cache") cache = false;
        else if (a == "--harvard") harvard = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
        else path = argv[i];
    }
    if (!path) { usage(); return 2; }

    Memory mem, data_mem;
    mem.set_decode_cache(cache);
    Cpu cpu = harvard ? Cpu(mem, data_mem) : Cpu(mem);
    uint32_t entry = base;

    try {
        if (hex) {
            load_hex_words(path, mem, base);
            // Hex images from the Verilog flow have no trap handler: they end with a
            // self-loop, or by running into zeroed memory (an illegal instruction).
            self_loop = true;
            cpu.stop_on_trap = true;
        } else {
            const auto prog = load_elf(path, mem);
            entry = prog.entry;
            cpu.tohost = prog.symbol("tohost");
            // riscv-tests report through tohost; everything else uses host calls.
            cpu.host_syscalls = !cpu.tohost.has_value();
        }
    } catch (const LoadError& e) {
        std::fprintf(stderr, "rv32sim: %s\n", e.what());
        return 2;
    }

    cpu.reset(entry);
    cpu.stop_on_self_loop = self_loop;
    if (trace) {
        cpu.on_commit = [](const Commit& c) {
            if (c.trapped) {
                std::fprintf(stderr, "core   0: 3 0x%08x (0x%08x) trap\n", c.pc, c.insn);
                return;
            }
            std::fprintf(stderr, "core   0: 3 0x%08x (0x%08x)", c.pc, c.insn);
            if (c.reg_write) std::fprintf(stderr, " x%-2u 0x%08x", c.rd, c.rd_value);
            if (c.mem_write) std::fprintf(stderr, " mem 0x%08x 0x%0*x", c.mem_addr, c.mem_size * 2, c.mem_value);
            std::fputc('\n', stderr);
        };
    }

    const auto t0 = std::chrono::steady_clock::now();
    const StopReason why = cpu.run(max_insns);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (!cpu.console.empty()) std::fwrite(cpu.console.data(), 1, cpu.console.size(), stdout);

    if (dump) {
        for (unsigned r = 0; r < 32; ++r) std::printf("x%-2u = 0x%08x%s", r, cpu.reg(r), (r % 4 == 3) ? "\n" : "   ");
        std::printf("pc  = 0x%08x\n", cpu.pc());
    }
    if (stats) {
        std::fprintf(stderr, "instructions: %" PRIu64 "\ntime: %.3f s\nMIPS: %.1f\ndecodes: %" PRIu64 "\n",
                     cpu.instret(), secs, static_cast<double>(cpu.instret()) / secs / 1e6, mem.decodes());
    }

    int rc = 0;
    switch (why) {
        case StopReason::ToHost: {
            const uint32_t v = mem.load32(*cpu.tohost);
            if (v == 1) { std::fprintf(stderr, "PASS\n"); rc = 0; }
            else { std::fprintf(stderr, "FAIL (test %u)\n", v >> 1); rc = 1; }
            break;
        }
        case StopReason::Exit: rc = cpu.exit_code(); break;
        case StopReason::SelfLoop: case StopReason::Ebreak: rc = 0; break;
        case StopReason::Trap:
            if (hex) { rc = 0; break; }  // end of a hex image
            std::fprintf(stderr, "rv32sim: trap at pc 0x%08x\n", cpu.pc());
            rc = 4;
            break;
        case StopReason::InstLimit:
            std::fprintf(stderr, "rv32sim: instruction limit reached at pc 0x%08x\n", cpu.pc());
            rc = 3;
            break;
        default: break;
    }
    return rc;
}

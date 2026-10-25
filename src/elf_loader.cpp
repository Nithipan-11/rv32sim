#include "rv32sim/elf_loader.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace rv32sim {
namespace {

#pragma pack(push, 1)
struct Ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
struct Shdr {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
};
struct Sym {
    uint32_t name, value, size;
    uint8_t info, other;
    uint16_t shndx;
};
#pragma pack(pop)

constexpr uint16_t EM_RISCV = 243;
constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t SHT_SYMTAB = 2;

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw LoadError("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

template <typename T>
T at(const std::vector<uint8_t>& d, size_t off) {
    if (off + sizeof(T) > d.size()) throw LoadError("truncated ELF");
    T v;
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

}  // namespace

LoadedProgram load_elf(const std::string& path, Memory& mem) {
    const auto d = read_file(path);
    if (d.size() < sizeof(Ehdr) || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0) throw LoadError(path + ": not an ELF file");
    const auto eh = at<Ehdr>(d, 0);
    if (eh.ident[4] != 1) throw LoadError(path + ": not ELF32 (this simulator is RV32)");
    if (eh.ident[5] != 1) throw LoadError(path + ": not little-endian");
    if (eh.machine != EM_RISCV) throw LoadError(path + ": not a RISC-V binary");

    LoadedProgram prog;
    prog.entry = eh.entry;

    for (unsigned i = 0; i < eh.phnum; ++i) {
        const auto ph = at<Phdr>(d, eh.phoff + i * eh.phentsize);
        if (ph.type != PT_LOAD) continue;
        if (static_cast<size_t>(ph.offset) + ph.filesz > d.size()) throw LoadError("segment outside file");
        for (uint32_t b = 0; b < ph.filesz; ++b) mem.store8(ph.paddr + b, d[ph.offset + b]);
        for (uint32_t b = ph.filesz; b < ph.memsz; ++b) mem.store8(ph.paddr + b, 0);  // .bss
    }

    for (unsigned i = 0; i < eh.shnum; ++i) {
        const auto sh = at<Shdr>(d, eh.shoff + i * eh.shentsize);
        if (sh.type != SHT_SYMTAB) continue;
        const auto strtab = at<Shdr>(d, eh.shoff + sh.link * eh.shentsize);
        for (uint32_t off = 0; off + sizeof(Sym) <= sh.size; off += sizeof(Sym)) {
            const auto s = at<Sym>(d, sh.offset + off);
            if (!s.name) continue;
            const char* name = reinterpret_cast<const char*>(d.data() + strtab.offset + s.name);
            prog.symbols.emplace(name, s.value);
        }
    }
    return prog;
}

uint32_t load_binary(const std::string& path, Memory& mem, uint32_t base) {
    const auto d = read_file(path);
    for (size_t i = 0; i < d.size(); ++i) mem.store8(base + static_cast<uint32_t>(i), d[i]);
    return static_cast<uint32_t>(d.size());
}

uint32_t load_hex_words(const std::string& path, Memory& mem, uint32_t base) {
    std::ifstream f(path);
    if (!f) throw LoadError("cannot open " + path);
    std::string line;
    uint32_t addr = base, words = 0;
    while (std::getline(f, line)) {
        if (auto c = line.find("//"); c != std::string::npos) line.erase(c);
        std::istringstream ss(line);
        std::string tok;
        while (ss >> tok) {
            if (tok[0] == '@') {  // $readmemh address directive (word address)
                addr = base + 4 * static_cast<uint32_t>(std::stoul(tok.substr(1), nullptr, 16));
                continue;
            }
            mem.store32(addr, static_cast<uint32_t>(std::stoul(tok, nullptr, 16)));
            addr += 4;
            ++words;
        }
    }
    return words;
}

}  // namespace rv32sim

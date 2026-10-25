// Minimal ELF32 little-endian RISC-V loader: copies PT_LOAD segments into
// memory and exposes the entry point and symbol table (for `tohost`).
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "rv32sim/memory.hpp"

namespace rv32sim {

struct LoadedProgram {
    uint32_t entry = 0;
    std::unordered_map<std::string, uint32_t> symbols;
    std::optional<uint32_t> symbol(const std::string& name) const {
        auto it = symbols.find(name);
        if (it == symbols.end()) return std::nullopt;
        return it->second;
    }
};

struct LoadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

LoadedProgram load_elf(const std::string& path, Memory& mem);

// Load a raw binary or a $readmemh-style hex file (one 32-bit word per line) at `base`.
uint32_t load_binary(const std::string& path, Memory& mem, uint32_t base);
uint32_t load_hex_words(const std::string& path, Memory& mem, uint32_t base);

}  // namespace rv32sim

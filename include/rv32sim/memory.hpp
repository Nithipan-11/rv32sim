// Sparse, paged, little-endian physical memory.
//
// Pages are allocated on first touch, so a program linked at 0x80000000 costs
// only the pages it uses. Each page also carries a decoded-instruction cache:
// the first fetch from an address decodes it, later fetches reuse the result,
// and any store into the page invalidates the affected slot (this keeps
// self-modifying code and fence.i correct without a global flush).
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <vector>

#include "rv32sim/decoder.hpp"

namespace rv32sim {

class Memory {
public:
    static constexpr uint32_t kPageBits = 12;
    static constexpr uint32_t kPageSize = 1u << kPageBits;
    static constexpr uint32_t kWordsPerPage = kPageSize / 4;

    struct Page {
        std::array<uint8_t, kPageSize> bytes{};
        std::array<Inst, kWordsPerPage> decoded{};
        std::bitset<kWordsPerPage> valid;
    };

    uint8_t load8(uint32_t a) { return page(a).bytes[a & (kPageSize - 1)]; }
    uint16_t load16(uint32_t a) { return static_cast<uint16_t>(load8(a) | (load8(a + 1) << 8)); }
    uint32_t load32(uint32_t a) {
        if ((a & 3u) == 0) {  // aligned fast path: a single page lookup
            const auto& b = page(a).bytes;
            const uint32_t o = a & (kPageSize - 1);
            return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
        }
        return load16(a) | (static_cast<uint32_t>(load16(a + 2)) << 16);
    }

    void store8(uint32_t a, uint8_t v) {
        Page& p = page(a);
        const uint32_t o = a & (kPageSize - 1);
        p.bytes[o] = v;
        p.valid.reset(o >> 2);
    }
    void store16(uint32_t a, uint16_t v) {
        store8(a, static_cast<uint8_t>(v & 0xFF));
        store8(a + 1, static_cast<uint8_t>(v >> 8));
    }
    void store32(uint32_t a, uint32_t v) {
        store16(a, static_cast<uint16_t>(v & 0xFFFF));
        store16(a + 2, static_cast<uint16_t>(v >> 16));
    }

    // Fetch a decoded instruction. `pc` must be 4-byte aligned.
    const Inst& fetch(uint32_t pc) {
        Page& p = page(pc);
        const uint32_t slot = (pc & (kPageSize - 1)) >> 2;
        if (decode_cache_ && p.valid.test(slot)) {
            ++cache_hits_;
            return p.decoded[slot];
        }
        p.decoded[slot] = decode(load32(pc));
        p.valid.set(slot);
        ++decodes_;
        return p.decoded[slot];
    }

    void flush_decode_cache() {
        for (Page* p : allocated_) p->valid.reset();
    }

    void set_decode_cache(bool on) { decode_cache_ = on; }
    uint64_t decodes() const { return decodes_; }
    uint64_t cache_hits() const { return cache_hits_; }
    size_t pages_allocated() const { return allocated_.size(); }

private:
    // Flat one-level page table: 2^20 pointers (8 MiB) indexed directly by the
    // page number, so every access is one array load instead of a hash lookup.
    Page& page(uint32_t a) {
        auto& slot = table_[a >> kPageBits];
        if (!slot) [[unlikely]] {
            slot = std::make_unique<Page>();
            allocated_.push_back(slot.get());
        }
        return *slot;
    }

    std::vector<std::unique_ptr<Page>> table_ = std::vector<std::unique_ptr<Page>>(1u << (32 - kPageBits));
    std::vector<Page*> allocated_;
    bool decode_cache_ = true;
    uint64_t decodes_ = 0;
    uint64_t cache_hits_ = 0;
};

}  // namespace rv32sim

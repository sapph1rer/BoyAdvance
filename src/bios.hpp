#pragma once
#include <string>
#include <vector>
#include "types.hpp"

class BIOS {
public:
    bool load(const std::string& path);
    // Loads BIOS bytes already resident in memory (e.g. an embedded array
    // compiled into the binary). Returns false unless size is exactly 16KB.
    bool loadFromMemory(const u8* data, size_t size);

    u8 read8(u32 addr) const;
    u16 read16(u32 addr) const;
    u32 read32(u32 addr) const;

    bool loaded() const { return !data_.empty(); }

private:
    std::vector<u8> data_;
};

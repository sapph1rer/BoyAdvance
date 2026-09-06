#pragma once
#include <string>
#include <vector>
#include "types.hpp"

class Cartridge {
public:
    bool loadROM(const std::string& path);

    u8 read8(u32 addr) const;
    u16 read16(u32 addr) const;
    u32 read32(u32 addr) const;

    const std::vector<u8>& data() const { return rom; }

private:
    std::vector<u8> rom;
};

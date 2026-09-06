#include "cartridge.hpp"
#include <fstream>
#include <iterator>

bool Cartridge::loadROM(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !rom.empty();
}

u8 Cartridge::read8(u32 addr) const {
    if (addr < 0x08000000) return 0xFF;
    u32 off = addr - 0x08000000;
    if (off >= rom.size()) return 0xFF;
    return rom[off];
}

u16 Cartridge::read16(u32 addr) const {
    return static_cast<u16>(read8(addr) | (read8(addr + 1) << 8));
}

u32 Cartridge::read32(u32 addr) const {
    return static_cast<u32>(read8(addr) |
        (read8(addr + 1) << 8) |
        (read8(addr + 2) << 16) |
        (read8(addr + 3) << 24));
}

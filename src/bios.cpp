#include "bios.hpp"
#include <fstream>
#include <iterator>

bool BIOS::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return data_.size() == 16 * 1024;
}

bool BIOS::loadFromMemory(const u8* data, size_t size) {
    if (size != 16 * 1024) return false;
    data_.assign(data, data + size);
    return true;
}

u8 BIOS::read8(u32 addr) const {
    if (addr >= data_.size()) return 0;
    return data_[addr];
}

u16 BIOS::read16(u32 addr) const {
    return static_cast<u16>(read8(addr) | (read8(addr + 1) << 8));
}

u32 BIOS::read32(u32 addr) const {
    return static_cast<u32>(read8(addr) |
        (read8(addr + 1) << 8) |
        (read8(addr + 2) << 16) |
        (read8(addr + 3) << 24));
}

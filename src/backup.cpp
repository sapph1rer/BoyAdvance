#include "backup.hpp"
#include "state_io.hpp"
#include <fstream>
#include <algorithm>
#include <filesystem>

static BackupDevice::Type inferBackupTypeFromSaveSize(size_t bytes) {
    switch (bytes) {
        case 512: return BackupDevice::Type::EEPROM512;
        case 8 * 1024: return BackupDevice::Type::EEPROM8K;
        case 32 * 1024: return BackupDevice::Type::SRAM;
        case 64 * 1024: return BackupDevice::Type::FLASH64K;
        case 128 * 1024: return BackupDevice::Type::FLASH128K;
        default: return BackupDevice::Type::None;
    }
}

bool BackupDevice::isEEPROM() const {
    return backupType == Type::EEPROM512 || backupType == Type::EEPROM8K;
}

bool BackupDevice::init(Type t) {
    backupType = t;
    flashMode = FlashMode::Ready;
    flashBank = 0;
    dirty = false;
    eepromReset();

    switch (backupType) {
        case Type::None: data.clear(); return true;
        case Type::SRAM: data.assign(32 * 1024, 0xFF); return true;
        case Type::FLASH64K: data.assign(64 * 1024, 0xFF); flashManufacturer = 0x32; flashDevice = 0x1B; return true;
        case Type::FLASH128K: data.assign(128 * 1024, 0xFF); flashManufacturer = 0x62; flashDevice = 0x13; return true;
        case Type::EEPROM512: data.assign(512, 0xFF); return true;
        case Type::EEPROM8K: data.assign(8 * 1024, 0xFF); return true;
    }
    return false;
}

bool BackupDevice::loadFromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<u8> loaded((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    if (backupType == Type::None) {
        Type inferred = inferBackupTypeFromSaveSize(loaded.size());
        if (inferred == Type::None) return false;
        if (!init(inferred)) return false;
    }

    if (data.empty()) return false;
    if (isEEPROM()) {
        if (loaded.size() >= 8 * 1024) {
            backupType = Type::EEPROM8K;
            data.assign(8 * 1024, 0xFF);
            eepromAddrBits = 10;
        } else {
            backupType = Type::EEPROM512;
            data.assign(512, 0xFF);
            eepromAddrBits = 6;
        }
    }
    std::fill(data.begin(), data.end(), 0xFF);
    size_t count = std::min(data.size(), loaded.size());
    std::copy_n(loaded.begin(), static_cast<std::ptrdiff_t>(count), data.begin());
    dirty = false;
    eepromReset();
    return true;
}

bool BackupDevice::saveToFile(const std::string& path, bool force) {
    if (backupType == Type::None || data.empty()) return true;
    if (!force && !dirty) return true;

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path target(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) return false;
    }
    fs::path temp = target;
    temp += ".tmp";
    fs::path backup = target;
    backup += ".bak";

    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f) return false;
    f.close();

    if (fs::exists(backup, ec)) fs::remove(backup, ec);
    if (fs::exists(target, ec)) fs::rename(target, backup, ec);

    if (fs::exists(target, ec)) fs::remove(target, ec);
    fs::rename(temp, target, ec);
    if (ec) {
        // Best effort fallback: copy temp content into target directly.
        std::ofstream direct(target, std::ios::binary | std::ios::trunc);
        if (!direct) return false;
        direct.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!direct) return false;
        fs::remove(temp, ec);
    }

    dirty = false;
    return true;
}

bool BackupDevice::saveState(std::ostream& os) const {
    using namespace stateio;
    u32 type = static_cast<u32>(backupType);
    u32 mode = static_cast<u32>(flashMode);
    if (!writePod(os, type)) return false;
    if (!writeVector(os, data)) return false;
    if (!writeBool(os, dirty)) return false;
    if (!writePod(os, mode)) return false;
    if (!writePod(os, flashManufacturer)) return false;
    if (!writePod(os, flashDevice)) return false;
    if (!writePod(os, flashBank)) return false;
    if (!writePod(os, eepromAddrBits)) return false;
    if (!writeVector(os, eepromCmdBits)) return false;
    if (!writeVector(os, eepromReadBits)) return false;
    if (!writePod(os, eepromReadPos)) return false;
    return true;
}

bool BackupDevice::loadState(std::istream& is) {
    using namespace stateio;
    u32 type = 0;
    u32 mode = 0;
    if (!readPod(is, type)) return false;
    if (!readVector(is, data)) return false;
    if (!readBool(is, dirty)) return false;
    if (!readPod(is, mode)) return false;
    if (!readPod(is, flashManufacturer)) return false;
    if (!readPod(is, flashDevice)) return false;
    if (!readPod(is, flashBank)) return false;
    if (!readPod(is, eepromAddrBits)) return false;
    if (!readVector(is, eepromCmdBits)) return false;
    if (!readVector(is, eepromReadBits)) return false;
    if (!readPod(is, eepromReadPos)) return false;

    if (type > static_cast<u32>(Type::EEPROM8K)) return false;
    if (mode > static_cast<u32>(FlashMode::EraseCmd)) return false;
    backupType = static_cast<Type>(type);
    flashMode = static_cast<FlashMode>(mode);
    return true;
}

std::string BackupDevice::typeName() const {
    switch (backupType) {
        case Type::None: return "None";
        case Type::SRAM: return "SRAM";
        case Type::FLASH64K: return "FLASH64K";
        case Type::FLASH128K: return "FLASH128K";
        case Type::EEPROM512: return "EEPROM512";
        case Type::EEPROM8K: return "EEPROM8K";
    }
    return "Unknown";
}

size_t BackupDevice::flashOffset(u32 addr) const {
    size_t base = static_cast<size_t>(addr & 0xFFFF);
    if (backupType == Type::FLASH128K) base += static_cast<size_t>(flashBank) * 64 * 1024;
    return base % data.size();
}

u8 BackupDevice::flashRead(u32 addr) const {
    if (flashMode == FlashMode::IdMode) {
        u32 a = addr & 0xFFFF;
        if (a == 0x0000) return flashManufacturer;
        if (a == 0x0001) return flashDevice;
    }
    return data[flashOffset(addr)];
}

void BackupDevice::flashWrite(u32 addr, u8 value) {
    u32 a = addr & 0xFFFF;
    switch (flashMode) {
        case FlashMode::Ready:
            if (a == 0x5555 && value == 0xAA) { flashMode = FlashMode::CmdAA; return; }
            break;
        case FlashMode::CmdAA:
            if (a == 0x2AAA && value == 0x55) { flashMode = FlashMode::Cmd55; return; }
            flashMode = FlashMode::Ready; break;
        case FlashMode::Cmd55:
            if (a == 0x5555 && value == 0x90) { flashMode = FlashMode::IdMode; return; }
            if (a == 0x5555 && value == 0xA0) { flashMode = FlashMode::Program; return; }
            if (a == 0x5555 && value == 0x80) { flashMode = FlashMode::EraseAA; return; }
            if (a == 0x5555 && value == 0xB0) { flashMode = FlashMode::BankSwitch; return; }
            if (a == 0x5555 && value == 0xF0) { flashMode = FlashMode::Ready; return; }
            flashMode = FlashMode::Ready; return;
        case FlashMode::Program:
        {
            size_t off = flashOffset(addr);
            u8 programmed = static_cast<u8>(data[off] & value);
            if (data[off] != programmed) {
                data[off] = programmed;
                dirty = true;
            }
            flashMode = FlashMode::Ready;
            return;
        }
        case FlashMode::BankSwitch:
            if (backupType == Type::FLASH128K && a == 0x0000) flashBank = value & 1;
            flashMode = FlashMode::Ready; return;
        case FlashMode::IdMode:
            if (value == 0xF0) flashMode = FlashMode::Ready; return;
        case FlashMode::EraseAA:
            if (a == 0x5555 && value == 0xAA) { flashMode = FlashMode::Erase55; return; }
            flashMode = FlashMode::Ready; return;
        case FlashMode::Erase55:
            if (a == 0x2AAA && value == 0x55) { flashMode = FlashMode::EraseCmd; return; }
            flashMode = FlashMode::Ready; return;
        case FlashMode::EraseCmd:
            if (a == 0x5555 && value == 0x10) {
                if (std::any_of(data.begin(), data.end(), [](u8 b) { return b != 0xFF; })) dirty = true;
                std::fill(data.begin(), data.end(), 0xFF);
            } else if (value == 0x30) {
                size_t sectorBase = flashOffset(addr) & ~static_cast<size_t>(0x0FFF);
                size_t end = std::min(sectorBase + 0x1000, data.size());
                if (std::any_of(data.begin() + static_cast<std::ptrdiff_t>(sectorBase),
                                data.begin() + static_cast<std::ptrdiff_t>(end),
                                [](u8 b) { return b != 0xFF; })) {
                    dirty = true;
                }
                std::fill(data.begin() + sectorBase, data.begin() + end, 0xFF);
            }
            flashMode = FlashMode::Ready; return;
    }
    flashMode = FlashMode::Ready;
}

void BackupDevice::eepromReset() {
    if (backupType == Type::EEPROM512) eepromAddrBits = 6;
    else if (backupType == Type::EEPROM8K) eepromAddrBits = 10;
    else eepromAddrBits = 0;
    eepromCmdBits.clear();
    eepromReadBits.clear();
    eepromReadPos = 0;
}

void BackupDevice::eepromEnsureSizeForAddrBits(int addrBits) {
    if (addrBits == 10 && data.size() != 8 * 1024) {
        std::vector<u8> resized(8 * 1024, 0xFF);
        size_t count = std::min(resized.size(), data.size());
        std::copy_n(data.begin(), static_cast<std::ptrdiff_t>(count), resized.begin());
        data.swap(resized);
        backupType = Type::EEPROM8K;
    } else if (addrBits == 6 && data.size() != 512) {
        std::vector<u8> resized(512, 0xFF);
        size_t count = std::min(resized.size(), data.size());
        std::copy_n(data.begin(), static_cast<std::ptrdiff_t>(count), resized.begin());
        data.swap(resized);
        backupType = Type::EEPROM512;
    }
    if (addrBits == 6 || addrBits == 10) eepromAddrBits = addrBits;
}

void BackupDevice::eepromStartReadStream(u32 blockAddr) {
    if (data.empty()) return;
    const size_t blockCount = data.size() / 8;
    if (blockCount == 0) return;
    size_t block = static_cast<size_t>(blockAddr) % blockCount;
    size_t base = block * 8;

    eepromReadBits.clear();
    eepromReadBits.reserve(68);
    for (int i = 0; i < 4; ++i) eepromReadBits.push_back(0);
    for (int b = 0; b < 8; ++b) {
        u8 v = data[base + static_cast<size_t>(b)];
        for (int bit = 7; bit >= 0; --bit) {
            eepromReadBits.push_back(static_cast<u8>((v >> bit) & 1));
        }
    }
    eepromReadPos = 0;
}

void BackupDevice::eepromWriteBlock(u32 blockAddr, const std::array<u8, 8>& bytes) {
    if (data.empty()) return;
    const size_t blockCount = data.size() / 8;
    if (blockCount == 0) return;
    size_t block = static_cast<size_t>(blockAddr) % blockCount;
    size_t base = block * 8;
    for (int i = 0; i < 8; ++i) {
        u8 old = data[base + static_cast<size_t>(i)];
        u8 next = bytes[static_cast<size_t>(i)];
        if (old != next) {
            data[base + static_cast<size_t>(i)] = next;
            dirty = true;
        }
    }
}

void BackupDevice::eepromTryExecuteCommand(bool finalizePending) {
    if (!isEEPROM()) return;
    if (eepromCmdBits.size() < 2) return;

    const int op = (static_cast<int>(eepromCmdBits[0]) << 1) | static_cast<int>(eepromCmdBits[1]);
    auto parseAddr = [&](int addrBits) -> u32 {
        u32 addr = 0;
        for (int i = 0; i < addrBits; ++i) {
            addr = static_cast<u32>((addr << 1) | (eepromCmdBits[static_cast<size_t>(2 + i)] & 1));
        }
        return addr;
    };

    // Read command: 2 opcode bits + addr bits + 1 stop bit.
    // For protocol disambiguation, execute short read only when command stream is finalized
    // (caller starts reading data line), while long read is accepted as soon as complete.
    if (op == 0b11) {
        if (eepromCmdBits.size() == 13) {
            if (eepromCmdBits[12] != 0) {
                eepromCmdBits.clear();
                return;
            }
            eepromEnsureSizeForAddrBits(10);
            u32 addr = parseAddr(10);
            eepromStartReadStream(addr);
            eepromCmdBits.clear();
            return;
        }
        if (finalizePending && eepromCmdBits.size() == 9) {
            if (eepromCmdBits[8] != 0) {
                eepromCmdBits.clear();
                return;
            }
            eepromEnsureSizeForAddrBits(6);
            u32 addr = parseAddr(6);
            eepromStartReadStream(addr);
            eepromCmdBits.clear();
            return;
        }
        if (eepromCmdBits.size() > 20) {
            eepromCmdBits.clear();
        }
        return;
    }

    // Write command: 2 opcode bits + addr bits + 64 data bits + 1 stop bit.
    if (op == 0b10) {
        auto parseWrite = [&](int addrBits, size_t stopIdx) {
            if (eepromCmdBits[stopIdx] != 0) {
                eepromCmdBits.clear();
                return;
            }
            eepromEnsureSizeForAddrBits(addrBits);
            u32 addr = parseAddr(addrBits);
            size_t dataStart = static_cast<size_t>(2 + addrBits);
            std::array<u8, 8> bytes{};
            for (int i = 0; i < 8; ++i) {
                u8 v = 0;
                for (int bit = 0; bit < 8; ++bit) {
                    v = static_cast<u8>((v << 1) | (eepromCmdBits[dataStart + static_cast<size_t>(i * 8 + bit)] & 1));
                }
                bytes[static_cast<size_t>(i)] = v;
            }
            eepromWriteBlock(addr, bytes);
            eepromCmdBits.clear();
        };

        if (eepromCmdBits.size() == 77) {
            parseWrite(10, 76);
            return;
        }
        if (finalizePending && eepromCmdBits.size() == 73) {
            parseWrite(6, 72);
            return;
        }
        if (eepromCmdBits.size() > 80) {
            eepromCmdBits.clear();
        }
        return;
    }

    // Unknown command stream; reset parser if it gets too long.
    if (eepromCmdBits.size() > 80) eepromCmdBits.clear();
}

u8 BackupDevice::readEEPROMBit() {
    if (!isEEPROM()) return 0xFF;
    if (eepromReadBits.empty() && !eepromCmdBits.empty()) {
        eepromTryExecuteCommand(true);
    }
    if (eepromReadPos < eepromReadBits.size()) {
        return static_cast<u8>(eepromReadBits[eepromReadPos++]);
    }
    eepromReadBits.clear();
    eepromReadPos = 0;
    return 1;
}

void BackupDevice::writeEEPROMBit(u8 bit) {
    if (!isEEPROM()) return;
    bit &= 1;
    if (!eepromReadBits.empty() && eepromReadPos < eepromReadBits.size()) {
        // New command cancels current read stream.
        eepromReadBits.clear();
        eepromReadPos = 0;
    }
    // EEPROM command streams always begin with 1; ignore idle 0s between commands.
    if (eepromCmdBits.empty() && bit == 0) return;
    if (eepromCmdBits.size() >= 96) eepromCmdBits.clear();
    eepromCmdBits.push_back(bit);
    eepromTryExecuteCommand(false);
}

u8 BackupDevice::read8(u32 addr) {
    switch (backupType) {
        case Type::None: return 0xFF;
        case Type::SRAM: return data[addr & 0x7FFF];
        case Type::FLASH64K:
        case Type::FLASH128K: return flashRead(addr);
        case Type::EEPROM512:
        case Type::EEPROM8K: return readEEPROMBit();
    }
    return 0xFF;
}

void BackupDevice::write8(u32 addr, u8 value) {
    switch (backupType) {
        case Type::None: return;
        case Type::SRAM: {
            size_t off = addr & 0x7FFF;
            if (data[off] != value) {
                data[off] = value;
                dirty = true;
            }
            return;
        }
        case Type::FLASH64K:
        case Type::FLASH128K: flashWrite(addr, value); return;
        case Type::EEPROM512:
        case Type::EEPROM8K: writeEEPROMBit(value); return;
    }
}

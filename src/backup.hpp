#pragma once
#include <string>
#include <vector>
#include <array>
#include <iosfwd>
#include "types.hpp"

class BackupDevice {
public:
    enum class Type {
        None,
        SRAM,
        FLASH64K,
        FLASH128K,
        EEPROM512,
        EEPROM8K
    };

    bool init(Type t);
    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path, bool force = false);
    bool saveState(std::ostream& os) const;
    bool loadState(std::istream& is);

    u8 read8(u32 addr);
    void write8(u32 addr, u8 value);
    u8 readEEPROMBit();
    void writeEEPROMBit(u8 bit);
    bool isEEPROM() const;

    Type type() const { return backupType; }
    std::string typeName() const;
    bool isDirty() const { return dirty; }
    size_t sizeBytes() const { return data.size(); }

private:
    Type backupType = Type::None;
    std::vector<u8> data;
    bool dirty = false;

    enum class FlashMode {
        Ready, CmdAA, Cmd55, Program, BankSwitch, IdMode, EraseAA, Erase55, EraseCmd
    };

    FlashMode flashMode = FlashMode::Ready;
    u8 flashManufacturer = 0xC2;
    u8 flashDevice = 0x1C;
    u8 flashBank = 0;

    int eepromAddrBits = 0; // 0=unknown, 6=512B, 10=8KB
    std::vector<u8> eepromCmdBits;
    std::vector<u8> eepromReadBits;
    size_t eepromReadPos = 0;

    size_t flashOffset(u32 addr) const;
    void flashWrite(u32 addr, u8 value);
    u8 flashRead(u32 addr) const;
    void eepromReset();
    void eepromEnsureSizeForAddrBits(int addrBits);
    void eepromTryExecuteCommand(bool finalizePending);
    void eepromStartReadStream(u32 blockAddr);
    void eepromWriteBlock(u32 blockAddr, const std::array<u8, 8>& bytes);
};

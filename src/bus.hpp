#pragma once
#include <array>
#include <iosfwd>
#include <string>
#include "types.hpp"
#include "bios.hpp"
#include "cartridge.hpp"
#include "interrupts.hpp"
#include "ppu.hpp"
#include "timers.hpp"
#include "keypad.hpp"
#include "dma.hpp"
#include "backup.hpp"
#include "apu.hpp"

class Bus {
public:
    Bus();

    bool loadBIOS(const std::string& path);
    bool loadROM(const std::string& path);
    bool loadSaveFile(const std::string& path);
    bool saveSaveFile(const std::string& path, bool force = false);

    u8  read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);

    void write8(u32 addr, u8 value);
    void write16(u32 addr, u16 value);
    void write32(u32 addr, u32 value);

    void tick(int cycles);
    bool saveState(std::ostream& os) const;
    bool loadState(std::istream& is);
    void setCpuExecAddress(u32 addr) { cpuExecInBios = addr <= 0x00003FFF; }
    void latchFetchedBiosOpcode32(u32 opcode) { biosReadLatch = opcode; }
    void latchPrefetch32(u32 opcode) { openBusLatch = opcode; }
    void resetRuntimeState();
    void invalidateSequentialTiming();

    int accessTime32(u32 addr, bool sequential = false) const;
    int accessTime16(u32 addr, bool sequential = false) const;

    BIOS bios;
    Cartridge cart;
    InterruptController irq;
    PPU ppu;
    Timers timers;
    Keypad keypad;
    DMAController dma;
    APU apu;
    BackupDevice backup;

    std::array<u8, 256 * 1024> ewram{};
    std::array<u8, 32 * 1024>  iwram{};
    std::array<u8, 1024>       io{};
    std::array<u8, 1024>       palette{};
    std::array<u8, 96 * 1024>  vram{};
    std::array<u8, 1024>       oam{};

    u16 waitcnt = 0;
    u8 haltcnt = 0;
    bool haltMode = false;
    bool intrWaitActive = false;
    u16 intrWaitMask = 0;
    int lastAccessCycles = 0;

private:
    bool cpuExecInBios = false;
    u32 biosReadLatch = 0;
    u32 openBusLatch = 0;
    bool lastVBlank = false;
    bool lastHBlank = false;
    bool timingLastValid = false;
    u32 timingLastAddr = 0;
    u8 timingLastRegion = 0xFF;
    u8 timingLastSize = 0;

    u8 timingRegion(u32 addr) const;
    bool isSequentialAccess(u32 addr, u8 sizeBytes) const;
    void noteAccess(u32 addr, u8 sizeBytes);
    u8 read8Raw(u32 addr);
    void write8Raw(u32 addr, u8 value);

    u16 readIO16(u32 addr);
    u32 readIO32(u32 addr);
    void writeIO16(u32 addr, u16 value);
    void writeIO32(u32 addr, u32 value);
};

#include "bus.hpp"
#include "state_io.hpp"
#include "util.hpp"
#include "debug_trace.hpp"
#include "emu_log.hpp"
#include <algorithm>
#include <sstream>

static inline u8 latchByte(u32 latch, u32 addr) {
    return static_cast<u8>((ror32(latch, static_cast<int>((addr & 3) * 8))) & 0xFF);
}

static inline u16 latchHalf(u32 latch, u32 addr) {
    return static_cast<u16>(ror32(latch, static_cast<int>((addr & 3) * 8)) & 0xFFFF);
}

static inline size_t ewramOff(u32 addr) {
    return static_cast<size_t>((addr - 0x02000000) & 0x3FFFF);
}

static inline size_t iwramOff(u32 addr) {
    return static_cast<size_t>((addr - 0x03000000) & 0x7FFF);
}

static inline size_t paletteOff(u32 addr) {
    return static_cast<size_t>((addr - 0x05000000) & 0x3FF);
}

static inline size_t oamOff(u32 addr) {
    return static_cast<size_t>((addr - 0x07000000) & 0x3FF);
}

static inline size_t vramOff(u32 addr) {
    u32 off = (addr - 0x06000000) & 0x1FFFF;
    if (off >= 0x18000) off -= 0x8000;
    return static_cast<size_t>(off);
}

static inline s32 signExtend28(u32 v) {
    v &= 0x0FFFFFFF;
    if (v & 0x08000000) v |= 0xF0000000;
    return static_cast<s32>(v);
}

static inline bool isEepromCartAddr(u32 addr) {
    return addr >= 0x0D000000 && addr <= 0x0DFFFFFF;
}

Bus::Bus() : ppu(irq), timers(irq), dma(irq) {}

bool Bus::loadBIOS(const std::string& path) { return bios.load(path); }
bool Bus::loadROM(const std::string& path) { return cart.loadROM(path); }
bool Bus::loadSaveFile(const std::string& path) { return backup.loadFromFile(path); }
bool Bus::saveSaveFile(const std::string& path, bool force) { return backup.saveToFile(path, force); }

void Bus::resetRuntimeState() {
    irq.IE = 0;
    irq.IF = 0;
    irq.IME = 0;
    waitcnt = 0;
    haltcnt = 0;
    haltMode = false;
    intrWaitActive = false;
    intrWaitMask = 0;
    lastAccessCycles = 0;
    cpuExecInBios = false;
    biosReadLatch = 0;
    openBusLatch = 0;
    lastVBlank = false;
    lastHBlank = false;
    timingLastValid = false;
    timingLastAddr = 0;
    timingLastRegion = 0xFF;
    timingLastSize = 0;
    io.fill(0);
    ppu.reset();
    timers.reset();
    dma.reset();
    apu.reset();
}

u8 Bus::timingRegion(u32 addr) const {
    if (addr >= 0x08000000 && addr <= 0x09FFFFFF) return 0;
    if (addr >= 0x0A000000 && addr <= 0x0BFFFFFF) return 1;
    if (addr >= 0x0C000000 && addr <= 0x0DFFFFFF) return 2;
    if (addr >= 0x0E000000 && addr <= 0x0FFFFFFF) return 3;
    return 0xFF;
}

bool Bus::isSequentialAccess(u32 addr, u8 sizeBytes) const {
    u8 region = timingRegion(addr);
    if (!timingLastValid || region == 0xFF || region != timingLastRegion) return false;
    return addr == timingLastAddr + timingLastSize;
}

void Bus::noteAccess(u32 addr, u8 sizeBytes) {
    u8 region = timingRegion(addr);
    if (region == 0xFF) {
        timingLastValid = false;
        timingLastAddr = 0;
        timingLastRegion = 0xFF;
        timingLastSize = 0;
        return;
    }
    timingLastValid = true;
    timingLastAddr = addr;
    timingLastRegion = region;
    timingLastSize = sizeBytes;
}

void Bus::invalidateSequentialTiming() {
    timingLastValid = false;
    timingLastAddr = 0;
    timingLastRegion = 0xFF;
    timingLastSize = 0;
}

int Bus::accessTime16(u32 addr, bool sequential) const {
    if (addr >= 0x02000000 && addr <= 0x02FFFFFF) return 3; // EWRAM
    if (addr >= 0x03000000 && addr <= 0x03FFFFFF) return 1; // IWRAM
    if (addr >= 0x04000000 && addr <= 0x040003FF) return 1; // IO
    if (addr >= 0x05000000 && addr <= 0x05FFFFFF) return 1; // Palette
    if (addr >= 0x06000000 && addr <= 0x06FFFFFF) return 1; // VRAM
    if (addr >= 0x07000000 && addr <= 0x07FFFFFF) return 1; // OAM

    if (addr >= 0x08000000 && addr <= 0x0DFFFFFF) {
        int ws = (addr >> 25) & 0x3;
        if (ws > 2) ws = 2;
        static constexpr int nTable[4] = { 4, 3, 2, 8 };
        static constexpr int sTable[3][2] = {
            { 2, 1 }, // WS0
            { 4, 1 }, // WS1
            { 8, 1 }  // WS2
        };
        int nSelShift = 2 + ws * 3;
        int nSel = (waitcnt >> nSelShift) & 0x3;
        int sSelShift = 4 + ws * 3;
        int sSel = (waitcnt >> sSelShift) & 0x1;
        return sequential ? sTable[ws][sSel] : nTable[nSel];
    }

    if (addr >= 0x0E000000 && addr <= 0x0FFFFFFF) {
        static constexpr int sramWait[4] = { 4, 3, 2, 8 };
        return sramWait[waitcnt & 0x3];
    }

    return 1;
}

int Bus::accessTime32(u32 addr, bool sequential) const {
    int first = accessTime16(addr, sequential);
    int second = accessTime16(addr + 2, true);
    return first + second;
}

void Bus::tick(int cycles) {
    int remaining = cycles < 1 ? 1 : cycles;
    while (remaining > 0) {
        int toPpuEvent = ppu.cyclesUntilNextModeEvent();
        if (toPpuEvent < 1) toPpuEvent = 1;
        int slice = std::min(remaining, toPpuEvent);

        timers.tick(slice);
        for (int i = 0; i < 4; ++i) {
            if (timers.overflowCount[i] > 0) {
                apu.onTimerOverflow(*this, i, timers.overflowCount[i]);
            }
        }
        // DMA uses this slice budget before any new LCD-triggered DMA starts at the end
        // of the same slice, which keeps HBlank/VBlank start timing deterministic.
        dma.tick(*this, slice);
        ppu.tick(slice, vram, palette, oam);
        int vblankEvents = ppu.consumeVBlankEvents();
        int hblankEvents = ppu.consumeHBlankEvents();
        for (int i = 0; i < vblankEvents; ++i) dma.triggerVBlank();
        for (int i = 0; i < hblankEvents; ++i) dma.triggerHBlank();
        apu.tick(slice);
        remaining -= slice;
    }
    lastVBlank = (ppu.dispstat & (1 << 0)) != 0;
    lastHBlank = (ppu.dispstat & (1 << 1)) != 0;
    if (haltMode) {
        if (intrWaitActive) {
            u16 masked = static_cast<u16>(irq.IF & intrWaitMask);
            if (masked != 0) {
                // Do NOT clear IF here. Real hardware wakes the CPU from its
                // low-power halt state and dispatches the pending interrupt
                // normally (BIOS vector at 0x18 -> the game's own registered
                // handler), and it's that real handler which acks the flag by
                // writing to IF itself. Clearing IF ourselves used to skip the
                // game's interrupt handler entirely -- which many games (this
                // ROM's NES core included) rely on to do real per-frame work,
                // causing the game to freeze/visually glitch even though our
                // "wait" logic considered itself satisfied.
                intrWaitActive = false;
                intrWaitMask = 0;
                haltMode = false;
            }
        } else if (irq.wakePending()) {
            haltMode = false;
        }
    }
}

bool Bus::saveState(std::ostream& os) const {
    using namespace stateio;
    if (!writePod(os, irq.IE)) return false;
    if (!writePod(os, irq.IF)) return false;
    if (!writePod(os, irq.IME)) return false;
    if (!writePod(os, waitcnt)) return false;
    if (!writePod(os, haltcnt)) return false;
    if (!writeBool(os, haltMode)) return false;
    if (!writeBool(os, intrWaitActive)) return false;
    if (!writePod(os, intrWaitMask)) return false;
    if (!writePod(os, lastAccessCycles)) return false;
    if (!writeBool(os, cpuExecInBios)) return false;
    if (!writePod(os, biosReadLatch)) return false;
    if (!writePod(os, openBusLatch)) return false;
    if (!writeBool(os, lastVBlank)) return false;
    if (!writeBool(os, lastHBlank)) return false;
    if (!writeBool(os, timingLastValid)) return false;
    if (!writePod(os, timingLastAddr)) return false;
    if (!writePod(os, timingLastRegion)) return false;
    if (!writePod(os, timingLastSize)) return false;

    if (!writeArray(os, ewram)) return false;
    if (!writeArray(os, iwram)) return false;
    if (!writeArray(os, io)) return false;
    if (!writeArray(os, palette)) return false;
    if (!writeArray(os, vram)) return false;
    if (!writeArray(os, oam)) return false;

    for (int i = 0; i < 4; ++i) {
        if (!writePod(os, timers.t[i].reload)) return false;
        if (!writePod(os, timers.t[i].counter)) return false;
        if (!writePod(os, timers.t[i].control)) return false;
        if (!writePod(os, timers.t[i].prescalerCounter)) return false;
        if (!writePod(os, timers.overflowCount[i])) return false;
    }

    for (int i = 0; i < 4; ++i) {
        const DMAChannel& c = dma.ch[i];
        if (!writePod(os, c.src)) return false;
        if (!writePod(os, c.dst)) return false;
        if (!writePod(os, c.count)) return false;
        if (!writePod(os, c.control)) return false;
        if (!writePod(os, c.internalSrc)) return false;
        if (!writePod(os, c.internalDst)) return false;
        if (!writePod(os, c.internalCount)) return false;
        if (!writePod(os, c.startupDelay)) return false;
        if (!writeBool(os, c.active)) return false;
        if (!writeBool(os, c.latched)) return false;
    }

    if (!ppu.saveState(os)) return false;
    if (!apu.saveState(os)) return false;
    if (!backup.saveState(os)) return false;
    return true;
}

bool Bus::loadState(std::istream& is) {
    using namespace stateio;
    if (!readPod(is, irq.IE)) return false;
    if (!readPod(is, irq.IF)) return false;
    if (!readPod(is, irq.IME)) return false;
    if (!readPod(is, waitcnt)) return false;
    if (!readPod(is, haltcnt)) return false;
    if (!readBool(is, haltMode)) return false;
    if (!readBool(is, intrWaitActive)) return false;
    if (!readPod(is, intrWaitMask)) return false;
    if (!readPod(is, lastAccessCycles)) return false;
    if (!readBool(is, cpuExecInBios)) return false;
    if (!readPod(is, biosReadLatch)) return false;
    if (!readPod(is, openBusLatch)) return false;
    if (!readBool(is, lastVBlank)) return false;
    if (!readBool(is, lastHBlank)) return false;
    if (!readBool(is, timingLastValid)) return false;
    if (!readPod(is, timingLastAddr)) return false;
    if (!readPod(is, timingLastRegion)) return false;
    if (!readPod(is, timingLastSize)) return false;

    if (!readArray(is, ewram)) return false;
    if (!readArray(is, iwram)) return false;
    if (!readArray(is, io)) return false;
    if (!readArray(is, palette)) return false;
    if (!readArray(is, vram)) return false;
    if (!readArray(is, oam)) return false;

    for (int i = 0; i < 4; ++i) {
        if (!readPod(is, timers.t[i].reload)) return false;
        if (!readPod(is, timers.t[i].counter)) return false;
        if (!readPod(is, timers.t[i].control)) return false;
        if (!readPod(is, timers.t[i].prescalerCounter)) return false;
        if (!readPod(is, timers.overflowCount[i])) return false;
    }

    for (int i = 0; i < 4; ++i) {
        DMAChannel& c = dma.ch[i];
        if (!readPod(is, c.src)) return false;
        if (!readPod(is, c.dst)) return false;
        if (!readPod(is, c.count)) return false;
        if (!readPod(is, c.control)) return false;
        if (!readPod(is, c.internalSrc)) return false;
        if (!readPod(is, c.internalDst)) return false;
        if (!readPod(is, c.internalCount)) return false;
        if (!readPod(is, c.startupDelay)) return false;
        if (!readBool(is, c.active)) return false;
        if (!readBool(is, c.latched)) return false;
    }

    if (!ppu.loadState(is)) return false;
    if (!apu.loadState(is)) return false;
    if (!backup.loadState(is)) return false;
    return true;
}

u8 Bus::read8Raw(u32 addr) {
    if (addr <= 0x00003FFF) {
        if (cpuExecInBios && bios.loaded()) return bios.read8(addr);
        return latchByte(biosReadLatch, addr);
    }
    if (addr >= 0x02000000 && addr <= 0x02FFFFFF) return ewram[ewramOff(addr)];
    if (addr >= 0x03000000 && addr <= 0x03FFFFFF) return iwram[iwramOff(addr)];
    if (addr >= 0x04000000 && addr <= 0x040003FF) {
        u16 v = readIO16(addr & ~1u);
        return (addr & 1) ? (v >> 8) : (v & 0xFF);
    }
    if (addr >= 0x05000000 && addr <= 0x05FFFFFF) return palette[paletteOff(addr)];
    if (addr >= 0x06000000 && addr <= 0x06FFFFFF) return vram[vramOff(addr)];
    if (addr >= 0x07000000 && addr <= 0x07FFFFFF) return oam[oamOff(addr)];
    if (addr >= 0x08000000 && addr <= 0x0DFFFFFF) {
        if (isEepromCartAddr(addr) && backup.isEEPROM()) {
            return backup.readEEPROMBit();
        }
        return cart.read8(addr);
    }
    if (addr >= 0x0E000000 && addr <= 0x0FFFFFFF) return backup.read8(addr & 0xFFFF);
    return latchByte(openBusLatch, addr);
}

u8 Bus::read8(u32 addr) {
    bool sequential = isSequentialAccess(addr, 1);
    lastAccessCycles += accessTime16(addr, sequential);
    noteAccess(addr, 1);
    u8 value = read8Raw(addr);
    openBusLatch = static_cast<u32>(value) * 0x01010101u;
    return value;
}

u16 Bus::read16(u32 addr) {
    bool sequential = isSequentialAccess(addr, 2);
    lastAccessCycles += accessTime16(addr, sequential);
    noteAccess(addr, 2);
    if (addr >= 0x04000000 && addr <= 0x040003FF) {
        u16 v = readIO16(addr);
        openBusLatch = static_cast<u32>(v) | (static_cast<u32>(v) << 16);
        return v;
    }
    if (isEepromCartAddr(addr) && backup.isEEPROM()) {
        u16 v = static_cast<u16>(backup.readEEPROMBit() & 1u);
        openBusLatch = static_cast<u32>(v) | (static_cast<u32>(v) << 16);
        return v;
    }
    if (addr >= 0x0E000000 && addr <= 0x0FFFFFFF) {
        // Backup area is an 8-bit device on GBA bus; wider reads mirror byte lanes.
        u8 v = backup.read8(addr & 0xFFFF);
        u16 w = static_cast<u16>(v | (static_cast<u16>(v) << 8));
        openBusLatch = static_cast<u32>(w) | (static_cast<u32>(w) << 16);
        return w;
    }
    if (addr <= 0x00003FFF) {
        if (cpuExecInBios && bios.loaded()) return bios.read16(addr);
        u16 v = latchHalf(biosReadLatch, addr);
        openBusLatch = static_cast<u32>(v) | (static_cast<u32>(v) << 16);
        return v;
    }
    u16 v = static_cast<u16>(read8Raw(addr) | (read8Raw(addr + 1) << 8));
    openBusLatch = static_cast<u32>(v) | (static_cast<u32>(v) << 16);
    return v;
}

u32 Bus::read32(u32 addr) {
    bool sequential = isSequentialAccess(addr, 4);
    lastAccessCycles += accessTime32(addr, sequential);
    noteAccess(addr, 4);
    if (addr >= 0x04000000 && addr <= 0x040003FF) {
        u32 v = readIO32(addr);
        openBusLatch = v;
        return v;
    }
    if (isEepromCartAddr(addr) && backup.isEEPROM()) {
        u32 v = static_cast<u32>(backup.readEEPROMBit() & 1u);
        openBusLatch = v;
        return v;
    }
    if (addr >= 0x0E000000 && addr <= 0x0FFFFFFF) {
        // Backup area is an 8-bit device on GBA bus; wider reads mirror byte lanes.
        u8 v = backup.read8(addr & 0xFFFF);
        u32 w = static_cast<u32>(v) * 0x01010101u;
        openBusLatch = w;
        return w;
    }
    if (addr <= 0x00003FFF) {
        if (cpuExecInBios && bios.loaded()) return bios.read32(addr);
        u32 v = ror32(biosReadLatch, static_cast<int>((addr & 3) * 8));
        openBusLatch = v;
        return v;
    }
    u32 v = static_cast<u32>(read8Raw(addr) |
        (read8Raw(addr + 1) << 8) |
        (read8Raw(addr + 2) << 16) |
        (read8Raw(addr + 3) << 24));
    openBusLatch = v;
    return v;
}

void Bus::write8Raw(u32 addr, u8 value) {
    if (addr >= 0x02000000 && addr <= 0x02FFFFFF) { ewram[ewramOff(addr)] = value; return; }
    if (addr >= 0x03000000 && addr <= 0x03FFFFFF) { iwram[iwramOff(addr)] = value; return; }
    if (addr == 0x04000300) {
        io[0x300] = value;
        return;
    }
    if (addr == 0x04000301) {
        haltcnt = value;
        io[0x301] = value;
        if ((value & 0x80) == 0) {
            haltMode = true;
            intrWaitActive = false;
            intrWaitMask = 0;
        }
        return;
    }
    if (addr >= 0x04000000 && addr <= 0x040003FF) {
        u32 base = addr & ~1u;
        u16 cur = readIO16(base);
        if (addr & 1) cur = static_cast<u16>((cur & 0x00FF) | (value << 8));
        else cur = static_cast<u16>((cur & 0xFF00) | value);
        writeIO16(base, cur);
        return;
    }
    if (addr >= 0x05000000 && addr <= 0x05FFFFFF) {
        size_t off = paletteOff(addr) & ~static_cast<size_t>(1);
        palette[off] = value;
        palette[off + 1] = value;
        return;
    }
    if (addr >= 0x06000000 && addr <= 0x06FFFFFF) {
        size_t off = vramOff(addr);
        size_t half = off & ~static_cast<size_t>(1);
        vram[half] = value;
        vram[half + 1] = value;
        return;
    }
    if (addr >= 0x07000000 && addr <= 0x07FFFFFF) {
        // 8-bit writes to OAM are ignored on GBA.
        return;
    }
    if (isEepromCartAddr(addr) && backup.isEEPROM()) {
        backup.writeEEPROMBit(value & 1);
        return;
    }
    if (addr >= 0x0E000000 && addr <= 0x0FFFFFFF) { backup.write8(addr & 0xFFFF, value); return; }
}

void Bus::write8(u32 addr, u8 value) {
    bool sequential = isSequentialAccess(addr, 1);
    lastAccessCycles += accessTime16(addr, sequential);
    noteAccess(addr, 1);
    openBusLatch = static_cast<u32>(value) * 0x01010101u;
    write8Raw(addr, value);
}

void Bus::write16(u32 addr, u16 value) {
    bool sequential = isSequentialAccess(addr, 2);
    lastAccessCycles += accessTime16(addr, sequential);
    noteAccess(addr, 2);
    openBusLatch = static_cast<u32>(value) | (static_cast<u32>(value) << 16);
    if (addr >= 0x04000000 && addr <= 0x040003FF) { writeIO16(addr, value); return; }
    if (isEepromCartAddr(addr) && backup.isEEPROM()) {
        backup.writeEEPROMBit(static_cast<u8>(value & 1));
        return;
    }
    if (addr >= 0x02000000 && addr <= 0x02FFFFFF) {
        size_t off = ewramOff(addr) & ~static_cast<size_t>(1);
        ewram[off] = static_cast<u8>(value & 0xFF);
        ewram[off + 1] = static_cast<u8>(value >> 8);
        return;
    }
    if (addr >= 0x03000000 && addr <= 0x03FFFFFF) {
        size_t off = iwramOff(addr) & ~static_cast<size_t>(1);
        iwram[off] = static_cast<u8>(value & 0xFF);
        iwram[off + 1] = static_cast<u8>(value >> 8);
        return;
    }
    if (addr >= 0x05000000 && addr <= 0x05FFFFFF) {
        size_t off = paletteOff(addr) & ~static_cast<size_t>(1);
        palette[off] = static_cast<u8>(value & 0xFF);
        palette[off + 1] = static_cast<u8>(value >> 8);
        return;
    }
    if (addr >= 0x06000000 && addr <= 0x06FFFFFF) {
        size_t off = vramOff(addr) & ~static_cast<size_t>(1);
        vram[off] = static_cast<u8>(value & 0xFF);
        vram[off + 1] = static_cast<u8>(value >> 8);
        return;
    }
    if (addr >= 0x07000000 && addr <= 0x07FFFFFF) {
        size_t off = oamOff(addr) & ~static_cast<size_t>(1);
        oam[off] = static_cast<u8>(value & 0xFF);
        oam[off + 1] = static_cast<u8>(value >> 8);
        return;
    }
    if (addr >= 0x0E000000 && addr <= 0x0FFFFFFF) {
        // Backup area is 8-bit bus; only low byte is observed.
        backup.write8(addr & 0xFFFF, static_cast<u8>(value & 0xFF));
        return;
    }
}

void Bus::write32(u32 addr, u32 value) {
    bool sequential = isSequentialAccess(addr, 4);
    lastAccessCycles += accessTime32(addr, sequential);
    noteAccess(addr, 4);
    openBusLatch = value;
    if (addr >= 0x04000000 && addr <= 0x040003FF) { writeIO32(addr, value); return; }
    if (isEepromCartAddr(addr) && backup.isEEPROM()) {
        backup.writeEEPROMBit(static_cast<u8>(value & 1));
        return;
    }
    if (addr >= 0x02000000 && addr <= 0x02FFFFFF) {
        size_t off = ewramOff(addr) & ~static_cast<size_t>(3);
        ewram[off] = static_cast<u8>(value & 0xFF);
        ewram[off + 1] = static_cast<u8>((value >> 8) & 0xFF);
        ewram[off + 2] = static_cast<u8>((value >> 16) & 0xFF);
        ewram[off + 3] = static_cast<u8>((value >> 24) & 0xFF);
        return;
    }
    if (addr >= 0x03000000 && addr <= 0x03FFFFFF) {
        size_t off = iwramOff(addr) & ~static_cast<size_t>(3);
        iwram[off] = static_cast<u8>(value & 0xFF);
        iwram[off + 1] = static_cast<u8>((value >> 8) & 0xFF);
        iwram[off + 2] = static_cast<u8>((value >> 16) & 0xFF);
        iwram[off + 3] = static_cast<u8>((value >> 24) & 0xFF);
        return;
    }
    if (addr >= 0x05000000 && addr <= 0x05FFFFFF) {
        size_t off = paletteOff(addr) & ~static_cast<size_t>(3);
        palette[off] = static_cast<u8>(value & 0xFF);
        palette[off + 1] = static_cast<u8>((value >> 8) & 0xFF);
        palette[off + 2] = static_cast<u8>((value >> 16) & 0xFF);
        palette[off + 3] = static_cast<u8>((value >> 24) & 0xFF);
        return;
    }
    if (addr >= 0x06000000 && addr <= 0x06FFFFFF) {
        size_t off = vramOff(addr) & ~static_cast<size_t>(3);
        vram[off] = static_cast<u8>(value & 0xFF);
        vram[off + 1] = static_cast<u8>((value >> 8) & 0xFF);
        vram[off + 2] = static_cast<u8>((value >> 16) & 0xFF);
        vram[off + 3] = static_cast<u8>((value >> 24) & 0xFF);
        return;
    }
    if (addr >= 0x07000000 && addr <= 0x07FFFFFF) {
        size_t off = oamOff(addr) & ~static_cast<size_t>(3);
        oam[off] = static_cast<u8>(value & 0xFF);
        oam[off + 1] = static_cast<u8>((value >> 8) & 0xFF);
        oam[off + 2] = static_cast<u8>((value >> 16) & 0xFF);
        oam[off + 3] = static_cast<u8>((value >> 24) & 0xFF);
        return;
    }
    if (addr >= 0x0E000000 && addr <= 0x0FFFFFFF) {
        // Backup area is 8-bit bus; only low byte is observed.
        backup.write8(addr & 0xFFFF, static_cast<u8>(value & 0xFF));
        return;
    }
}

u16 Bus::readIO16(u32 addr) {
    u32 ioAddr = addr & 0x3FF;
    if (addr >= 0x04000090 && addr <= 0x0400009E && (addr & 1u) == 0) {
        return apu.readIO16(addr);
    }
    switch (addr) {
        case 0x04000000: return ppu.dispcnt;
        case 0x04000004: return ppu.dispstat;
        case 0x04000006: return ppu.vcount;
        case 0x04000008: return ppu.bgcnt[0];
        case 0x0400000A: return ppu.bgcnt[1];
        case 0x0400000C: return ppu.bgcnt[2];
        case 0x0400000E: return ppu.bgcnt[3];
        case 0x04000010: return ppu.bghofs[0];
        case 0x04000012: return ppu.bgvofs[0];
        case 0x04000014: return ppu.bghofs[1];
        case 0x04000016: return ppu.bgvofs[1];
        case 0x04000018: return ppu.bghofs[2];
        case 0x0400001A: return ppu.bgvofs[2];
        case 0x0400001C: return ppu.bghofs[3];
        case 0x0400001E: return ppu.bgvofs[3];
        case 0x04000020: return ppu.bgpa[0];
        case 0x04000022: return ppu.bgpb[0];
        case 0x04000024: return ppu.bgpc[0];
        case 0x04000026: return ppu.bgpd[0];
        case 0x04000028: return static_cast<u16>(static_cast<u32>(ppu.bgx[0]) & 0xFFFF);
        case 0x0400002A: return static_cast<u16>((static_cast<u32>(ppu.bgx[0]) >> 16) & 0xFFFF);
        case 0x0400002C: return static_cast<u16>(static_cast<u32>(ppu.bgy[0]) & 0xFFFF);
        case 0x0400002E: return static_cast<u16>((static_cast<u32>(ppu.bgy[0]) >> 16) & 0xFFFF);
        case 0x04000030: return ppu.bgpa[1];
        case 0x04000032: return ppu.bgpb[1];
        case 0x04000034: return ppu.bgpc[1];
        case 0x04000036: return ppu.bgpd[1];
        case 0x04000038: return static_cast<u16>(static_cast<u32>(ppu.bgx[1]) & 0xFFFF);
        case 0x0400003A: return static_cast<u16>((static_cast<u32>(ppu.bgx[1]) >> 16) & 0xFFFF);
        case 0x0400003C: return static_cast<u16>(static_cast<u32>(ppu.bgy[1]) & 0xFFFF);
        case 0x0400003E: return static_cast<u16>((static_cast<u32>(ppu.bgy[1]) >> 16) & 0xFFFF);
        case 0x04000040: return ppu.winh[0];
        case 0x04000042: return ppu.winh[1];
        case 0x04000044: return ppu.winv[0];
        case 0x04000046: return ppu.winv[1];
        case 0x04000048: return ppu.winin;
        case 0x0400004A: return ppu.winout;
        case 0x0400004C: return ppu.mosaic;
        case 0x04000050: return ppu.bldcnt;
        case 0x04000052: return ppu.bldalpha;
        case 0x04000054: return ppu.bldy;
        case 0x04000060: return apu.readIO16(addr);
        case 0x04000062: return apu.readIO16(addr);
        case 0x04000064: return apu.readIO16(addr);
        case 0x04000068: return apu.readIO16(addr);
        case 0x0400006C: return apu.readIO16(addr);
        case 0x04000070: return apu.readIO16(addr);
        case 0x04000072: return apu.readIO16(addr);
        case 0x04000074: return apu.readIO16(addr);
        case 0x04000078: return apu.readIO16(addr);
        case 0x0400007C: return apu.readIO16(addr);
        case 0x040000B0: return dma.ch[0].src & 0xFFFF;
        case 0x040000B2: return (dma.ch[0].src >> 16) & 0xFFFF;
        case 0x040000B4: return dma.ch[0].dst & 0xFFFF;
        case 0x040000B6: return (dma.ch[0].dst >> 16) & 0xFFFF;
        case 0x040000B8: return dma.ch[0].count;
        case 0x040000BA: return dma.ch[0].control;
        case 0x040000BC: return dma.ch[1].src & 0xFFFF;
        case 0x040000BE: return (dma.ch[1].src >> 16) & 0xFFFF;
        case 0x040000C0: return dma.ch[1].dst & 0xFFFF;
        case 0x040000C2: return (dma.ch[1].dst >> 16) & 0xFFFF;
        case 0x040000C4: return dma.ch[1].count;
        case 0x040000C6: return dma.ch[1].control;
        case 0x040000C8: return dma.ch[2].src & 0xFFFF;
        case 0x040000CA: return (dma.ch[2].src >> 16) & 0xFFFF;
        case 0x040000CC: return dma.ch[2].dst & 0xFFFF;
        case 0x040000CE: return (dma.ch[2].dst >> 16) & 0xFFFF;
        case 0x040000D0: return dma.ch[2].count;
        case 0x040000D2: return dma.ch[2].control;
        case 0x040000D4: return dma.ch[3].src & 0xFFFF;
        case 0x040000D6: return (dma.ch[3].src >> 16) & 0xFFFF;
        case 0x040000D8: return dma.ch[3].dst & 0xFFFF;
        case 0x040000DA: return (dma.ch[3].dst >> 16) & 0xFFFF;
        case 0x040000DC: return dma.ch[3].count;
        case 0x040000DE: return dma.ch[3].control;
        case 0x04000100: return timers.t[0].counter;
        case 0x04000102: return timers.t[0].control;
        case 0x04000104: return timers.t[1].counter;
        case 0x04000106: return timers.t[1].control;
        case 0x04000108: return timers.t[2].counter;
        case 0x0400010A: return timers.t[2].control;
        case 0x0400010C: return timers.t[3].counter;
        case 0x0400010E: return timers.t[3].control;
        case 0x04000080: return apu.readSOUNDCNT_L();
        case 0x04000082: return apu.readSOUNDCNT_H();
        case 0x04000084: return apu.readSOUNDCNT_X();
        case 0x04000088: return apu.readIO16(addr);
        case 0x04000130: return keypad.keyinput();
        case 0x04000132: return keypad.keycnt;
        case 0x04000200: return irq.IE & 0x3FFF;
        case 0x04000202: return irq.IF & 0x3FFF;
        case 0x04000204: return waitcnt;
        case 0x04000208: return irq.IME;
        case 0x04000300: return static_cast<u16>(io[0x300] | (static_cast<u16>(haltcnt) << 8));
    }
    return static_cast<u16>(io[ioAddr] | (io[(ioAddr + 1) & 0x3FF] << 8));
}

u32 Bus::readIO32(u32 addr) {
    return static_cast<u32>(readIO16(addr) | (readIO16(addr + 2) << 16));
}

void Bus::writeIO16(u32 addr, u16 value) {
    u32 ioAddr = addr & 0x3FF;
    auto shouldLogChangedIo16 = [&](u32 fullAddr) -> bool {
        static std::array<u16, 1024> lastIo{};
        static std::array<bool, 1024> seen{};
        u32 idx = fullAddr & 0x3FF;
        if (!seen[idx]) {
            seen[idx] = true;
            lastIo[idx] = value;
            return true;
        }
        if (lastIo[idx] == value) return false;
        lastIo[idx] = value;
        return true;
    };
    auto logPpuCtrlWrite = [&](const char* regName) {
        if (!emudebug::Settings::instance().enabledFor(emudebug::GPU)) return;
        if (!shouldLogChangedIo16(addr)) return;
        static int ppuCtrlBudget = 512;
        if (ppuCtrlBudget <= 0) return;
        --ppuCtrlBudget;
        std::ostringstream ss;
        ss << regName
           << " vcount=" << ppu.vcount
           << " value=0x" << std::hex << value
           << std::dec;
        emulog::Logger::instance().log(emulog::Level::Debug, "GPU", ss.str());
    };
    auto logAffineWrite = [&](const char* regName) {
        if (!emudebug::Settings::instance().enabledFor(emudebug::GPU)) return;
        if (!shouldLogChangedIo16(addr)) return;
        static int affineWriteBudget = 2048;
        if (affineWriteBudget <= 0) return;
        --affineWriteBudget;
        std::ostringstream ss;
        ss << regName
           << " vcount=" << ppu.vcount
           << " value=0x" << std::hex << value
           << std::dec;
        emulog::Logger::instance().log(emulog::Level::Debug, "GPU", ss.str());
    };
    if (addr >= 0x04000090 && addr <= 0x0400009E && (addr & 1u) == 0) {
        if (apu.writeIO16(addr, value)) return;
    }
    switch (addr) {
        case 0x04000000: ppu.dispcnt = value; logPpuCtrlWrite("DISPCNT"); return;
        case 0x04000004: ppu.writeDISPSTAT(value); return;
        case 0x04000008: ppu.bgcnt[0] = value; logPpuCtrlWrite("BG0CNT"); return;
        case 0x0400000A: ppu.bgcnt[1] = value; logPpuCtrlWrite("BG1CNT"); return;
        case 0x0400000C: ppu.bgcnt[2] = value; logPpuCtrlWrite("BG2CNT"); return;
        case 0x0400000E: ppu.bgcnt[3] = value; logPpuCtrlWrite("BG3CNT"); return;
        case 0x04000010: ppu.bghofs[0] = value & 0x1FF; return;
        case 0x04000012: ppu.bgvofs[0] = value & 0x1FF; return;
        case 0x04000014: ppu.bghofs[1] = value & 0x1FF; return;
        case 0x04000016: ppu.bgvofs[1] = value & 0x1FF; return;
        case 0x04000018: ppu.bghofs[2] = value & 0x1FF; return;
        case 0x0400001A: ppu.bgvofs[2] = value & 0x1FF; return;
        case 0x0400001C: ppu.bghofs[3] = value & 0x1FF; return;
        case 0x0400001E: ppu.bgvofs[3] = value & 0x1FF; return;
        case 0x04000020: ppu.bgpa[0] = value; logAffineWrite("BG2PA"); return;
        case 0x04000022: ppu.bgpb[0] = value; logAffineWrite("BG2PB"); return;
        case 0x04000024: ppu.bgpc[0] = value; logAffineWrite("BG2PC"); return;
        case 0x04000026: ppu.bgpd[0] = value; logAffineWrite("BG2PD"); return;
        case 0x04000028: {
            u32 raw = (static_cast<u32>(ppu.bgx[0]) & 0xFFFF0000u) | value;
            ppu.bgx[0] = signExtend28(raw);
            ppu.syncAffineReferenceX(0);
            logAffineWrite("BG2X_L");
            return;
        }
        case 0x0400002A: {
            u32 raw = (static_cast<u32>(ppu.bgx[0]) & 0x0000FFFFu) | (static_cast<u32>(value) << 16);
            ppu.bgx[0] = signExtend28(raw);
            ppu.syncAffineReferenceX(0);
            logAffineWrite("BG2X_H");
            return;
        }
        case 0x0400002C: {
            u32 raw = (static_cast<u32>(ppu.bgy[0]) & 0xFFFF0000u) | value;
            ppu.bgy[0] = signExtend28(raw);
            ppu.syncAffineReferenceY(0);
            logAffineWrite("BG2Y_L");
            return;
        }
        case 0x0400002E: {
            u32 raw = (static_cast<u32>(ppu.bgy[0]) & 0x0000FFFFu) | (static_cast<u32>(value) << 16);
            ppu.bgy[0] = signExtend28(raw);
            ppu.syncAffineReferenceY(0);
            logAffineWrite("BG2Y_H");
            return;
        }
        case 0x04000030: ppu.bgpa[1] = value; logAffineWrite("BG3PA"); return;
        case 0x04000032: ppu.bgpb[1] = value; logAffineWrite("BG3PB"); return;
        case 0x04000034: ppu.bgpc[1] = value; logAffineWrite("BG3PC"); return;
        case 0x04000036: ppu.bgpd[1] = value; logAffineWrite("BG3PD"); return;
        case 0x04000038: {
            u32 raw = (static_cast<u32>(ppu.bgx[1]) & 0xFFFF0000u) | value;
            ppu.bgx[1] = signExtend28(raw);
            ppu.syncAffineReferenceX(1);
            logAffineWrite("BG3X_L");
            return;
        }
        case 0x0400003A: {
            u32 raw = (static_cast<u32>(ppu.bgx[1]) & 0x0000FFFFu) | (static_cast<u32>(value) << 16);
            ppu.bgx[1] = signExtend28(raw);
            ppu.syncAffineReferenceX(1);
            logAffineWrite("BG3X_H");
            return;
        }
        case 0x0400003C: {
            u32 raw = (static_cast<u32>(ppu.bgy[1]) & 0xFFFF0000u) | value;
            ppu.bgy[1] = signExtend28(raw);
            ppu.syncAffineReferenceY(1);
            logAffineWrite("BG3Y_L");
            return;
        }
        case 0x0400003E: {
            u32 raw = (static_cast<u32>(ppu.bgy[1]) & 0x0000FFFFu) | (static_cast<u32>(value) << 16);
            ppu.bgy[1] = signExtend28(raw);
            ppu.syncAffineReferenceY(1);
            logAffineWrite("BG3Y_H");
            return;
        }
        case 0x04000040: ppu.winh[0] = value; return;
        case 0x04000042: ppu.winh[1] = value; return;
        case 0x04000044: ppu.winv[0] = value; return;
        case 0x04000046: ppu.winv[1] = value; return;
        case 0x04000048: ppu.winin = value; return;
        case 0x0400004A: ppu.winout = value; return;
        case 0x0400004C: ppu.mosaic = value; return;
        case 0x04000050: ppu.bldcnt = value; return;
        case 0x04000052: ppu.bldalpha = value; return;
        case 0x04000054: ppu.bldy = value; return;
        case 0x04000060:
        case 0x04000062:
        case 0x04000064:
        case 0x04000068:
        case 0x0400006C:
        case 0x04000070:
        case 0x04000072:
        case 0x04000074:
        case 0x04000078:
        case 0x0400007C:
        case 0x04000088:
            if (apu.writeIO16(addr, value)) return;
            break;
        case 0x040000B0: dma.ch[0].src = (dma.ch[0].src & 0xFFFF0000) | value; return;
        case 0x040000B2: dma.ch[0].src = ((dma.ch[0].src & 0x0000FFFF) | (u32(value) << 16)) & 0x0FFFFFFF; return;
        case 0x040000B4: dma.ch[0].dst = (dma.ch[0].dst & 0xFFFF0000) | value; return;
        case 0x040000B6: dma.ch[0].dst = ((dma.ch[0].dst & 0x0000FFFF) | (u32(value) << 16)) & 0x0FFFFFFF; return;
        case 0x040000B8: dma.ch[0].count = value & 0x3FFF; return;
        case 0x040000BA: dma.writeControl(*this, 0, value); return;
        case 0x040000BC: dma.ch[1].src = (dma.ch[1].src & 0xFFFF0000) | value; return;
        case 0x040000BE: dma.ch[1].src = ((dma.ch[1].src & 0x0000FFFF) | (u32(value) << 16)) & 0x0FFFFFFF; return;
        case 0x040000C0: dma.ch[1].dst = (dma.ch[1].dst & 0xFFFF0000) | value; return;
        case 0x040000C2: dma.ch[1].dst = ((dma.ch[1].dst & 0x0000FFFF) | (u32(value) << 16)) & 0x0FFFFFFF; return;
        case 0x040000C4: dma.ch[1].count = value & 0x3FFF; return;
        case 0x040000C6: dma.writeControl(*this, 1, value); return;
        case 0x040000C8: dma.ch[2].src = (dma.ch[2].src & 0xFFFF0000) | value; return;
        case 0x040000CA: dma.ch[2].src = ((dma.ch[2].src & 0x0000FFFF) | (u32(value) << 16)) & 0x0FFFFFFF; return;
        case 0x040000CC: dma.ch[2].dst = (dma.ch[2].dst & 0xFFFF0000) | value; return;
        case 0x040000CE: dma.ch[2].dst = ((dma.ch[2].dst & 0x0000FFFF) | (u32(value) << 16)) & 0x0FFFFFFF; return;
        case 0x040000D0: dma.ch[2].count = value & 0x3FFF; return;
        case 0x040000D2: dma.writeControl(*this, 2, value); return;
        case 0x040000D4: dma.ch[3].src = (dma.ch[3].src & 0xFFFF0000) | value; return;
        case 0x040000D6: dma.ch[3].src = ((dma.ch[3].src & 0x0000FFFF) | (u32(value) << 16)) & 0x0FFFFFFF; return;
        case 0x040000D8: dma.ch[3].dst = (dma.ch[3].dst & 0xFFFF0000) | value; return;
        case 0x040000DA: dma.ch[3].dst = ((dma.ch[3].dst & 0x0000FFFF) | (u32(value) << 16)) & 0x0FFFFFFF; return;
        case 0x040000DC: dma.ch[3].count = value; return;
        case 0x040000DE: dma.writeControl(*this, 3, value); return;
        case 0x04000100: timers.t[0].reload = value; return;
        case 0x04000102: {
            bool wasEnabled = (timers.t[0].control & (1 << 7)) != 0;
            timers.t[0].control = value;
            bool nowEnabled = (timers.t[0].control & (1 << 7)) != 0;
            if (!wasEnabled && nowEnabled) {
                timers.t[0].counter = timers.t[0].reload;
                timers.t[0].prescalerCounter = 0;
            }
            if (!nowEnabled) timers.t[0].prescalerCounter = 0;
            return;
        }
        case 0x04000104: timers.t[1].reload = value; return;
        case 0x04000106: {
            bool wasEnabled = (timers.t[1].control & (1 << 7)) != 0;
            timers.t[1].control = value;
            bool nowEnabled = (timers.t[1].control & (1 << 7)) != 0;
            if (!wasEnabled && nowEnabled) {
                timers.t[1].counter = timers.t[1].reload;
                timers.t[1].prescalerCounter = 0;
            }
            if (!nowEnabled) timers.t[1].prescalerCounter = 0;
            return;
        }
        case 0x04000108: timers.t[2].reload = value; return;
        case 0x0400010A: {
            bool wasEnabled = (timers.t[2].control & (1 << 7)) != 0;
            timers.t[2].control = value;
            bool nowEnabled = (timers.t[2].control & (1 << 7)) != 0;
            if (!wasEnabled && nowEnabled) {
                timers.t[2].counter = timers.t[2].reload;
                timers.t[2].prescalerCounter = 0;
            }
            if (!nowEnabled) timers.t[2].prescalerCounter = 0;
            return;
        }
        case 0x0400010C: timers.t[3].reload = value; return;
        case 0x0400010E: {
            bool wasEnabled = (timers.t[3].control & (1 << 7)) != 0;
            timers.t[3].control = value;
            bool nowEnabled = (timers.t[3].control & (1 << 7)) != 0;
            if (!wasEnabled && nowEnabled) {
                timers.t[3].counter = timers.t[3].reload;
                timers.t[3].prescalerCounter = 0;
            }
            if (!nowEnabled) timers.t[3].prescalerCounter = 0;
            return;
        }
        case 0x04000080: apu.writeSOUNDCNT_L(value); return;
        case 0x04000082: apu.writeSOUNDCNT_H(value); return;
        case 0x04000084: apu.writeSOUNDCNT_X(value); return;
        case 0x040000A0:
        case 0x040000A2: apu.writeFIFOA16(value); return;
        case 0x040000A4:
        case 0x040000A6: apu.writeFIFOB16(value); return;
        case 0x04000132: keypad.keycnt = value; return;
        case 0x04000200: irq.IE = value & 0x3FFF; return;
        case 0x04000202: irq.IF &= static_cast<u16>(~value & 0x3FFF); return;
        case 0x04000204: waitcnt = value; return;
        case 0x04000208: irq.IME = value & 1; return;
        case 0x04000300:
            io[0x300] = static_cast<u8>(value & 0xFF);
            haltcnt = static_cast<u8>((value >> 8) & 0xFF);
            io[0x301] = haltcnt;
            if ((haltcnt & 0x80) == 0) {
                haltMode = true;
                intrWaitActive = false;
                intrWaitMask = 0;
            }
            return;
    }
    io[ioAddr] = static_cast<u8>(value & 0xFF);
    io[(ioAddr + 1) & 0x3FF] = static_cast<u8>(value >> 8);
}

void Bus::writeIO32(u32 addr, u32 value) {
    writeIO16(addr, static_cast<u16>(value & 0xFFFF));
    writeIO16(addr + 2, static_cast<u16>((value >> 16) & 0xFFFF));
}

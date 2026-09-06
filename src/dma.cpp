#include "dma.hpp"
#include "bus.hpp"
#include "debug_trace.hpp"
#include "emu_log.hpp"
#include <sstream>

static bool isAffineIoReg(u32 addr) {
    u32 a = addr & ~1u;
    return (a >= 0x04000020u && a <= 0x0400003Eu);
}

DMAController::DMAController(InterruptController& irq) : irqc(irq) {
    reset();
}

void DMAController::reset() {
    for (auto& c : ch) c = {};
}

u32 DMAController::applyAddrMask(int channel, u32 addr, bool isDst) {
    (void)isDst;
    // GBA DMA address registers expose upper bits only partially; masking here keeps
    // transfers in the 28-bit bus space and avoids invalid high-bit drift.
    u32 masked = addr & 0x0FFFFFFFu;
    // DMA0/1/2 transfer count is 14-bit; address masking is same practical bus range.
    // Keep all channels in 28-bit range for stability.
    (void)channel;
    return masked;
}

bool DMAController::cpuBusLocked() const {
    for (const auto& c : ch) {
        if (c.active) return true;
    }
    return false;
}

static inline int dma_irq_bit(int i) {
    return IRQ_DMA0 << i;
}

bool DMAController::timingMatch(int i, int timingMode) const {
    int timing = (ch[i].control >> 12) & 0x3;
    return timing == timingMode;
}

void DMAController::startChannel(int i) {
    auto& c = ch[i];
    if (!(c.control & (1 << 15))) return;
    if (c.active) return;
    int timing = (c.control >> 12) & 0x3;
    // Timing=3 is not valid on DMA0.
    if (i == 0 && timing == 3) return;
    if (!c.latched) {
        c.internalSrc = applyAddrMask(i, c.src, false);
        c.internalDst = applyAddrMask(i, c.dst, true);
        c.latched = true;
    } else {
        int dstCtrl = (c.control >> 5) & 0x3;
        if (dstCtrl == 3) c.internalDst = applyAddrMask(i, c.dst, true);
    }
    // GBA DMA word-count zero semantics:
    // CH0-2 => 0x4000 units, CH3 => 0x10000 units.
    c.internalCount = c.count ? static_cast<u32>(c.count) : static_cast<u32>((i == 3) ? 0x10000 : 0x4000);
    c.startupDelay = 2;
    c.active = true;
    if (emudebug::Settings::instance().enabledFor(emudebug::DMA)) {
        // DMA channels can restart many thousands of times per second (HBlank/VBlank/
        // FIFO timed transfers), so this log is rate-limited per channel to avoid
        // flooding the logger and stalling the emulation thread.
        static int startLogBudget[4] = {256, 256, 256, 256};
        if (startLogBudget[i] > 0) {
            --startLogBudget[i];
            std::ostringstream ss;
            ss << "CH" << i
               << " start timing=" << ((c.control >> 12) & 0x3)
               << " src=0x" << std::hex << c.internalSrc
               << " dst=0x" << c.internalDst
               << " count=0x" << c.internalCount
               << std::dec;
            emulog::Logger::instance().log(emulog::Level::Debug, "DMA", ss.str());
        }
    }
}

void DMAController::writeControl(Bus& bus, int i, u16 value) {
    auto& c = ch[i];
    const u16 writableMaskCommon = static_cast<u16>(0xF7E0); // bits 5..15 (except bit11 ch3-only semantic)
    value &= writableMaskCommon;
    if (i != 3) value &= static_cast<u16>(~(1u << 11));
    // Force reserved timing on DMA0 back to immediate.
    if (i == 0 && ((value >> 12) & 0x3) == 3) {
        value &= static_cast<u16>(~(0x3 << 12));
    }
    u16 old = c.control;
    bool oldEnable = (old & (1 << 15)) != 0;
    bool newEnable = (value & (1 << 15)) != 0;
    c.control = value;

    if (!newEnable) {
        c.active = false;
        c.latched = false;
        c.internalCount = 0;
        c.startupDelay = 0;
        return;
    }
    if (!oldEnable) {
        c.latched = false;
        if (((value >> 12) & 0x3) == 0) {
            startChannel(i);
        }
    }
}

void DMAController::triggerImmediate(Bus& bus) {
    (void)bus;
    for (int i = 0; i < 4; ++i) {
        if (((ch[i].control >> 12) & 0x3) == 0) {
            startChannel(i);
        }
    }
}

void DMAController::triggerVBlank() {
    for (int i = 0; i < 4; ++i) if (timingMatch(i, 1)) startChannel(i);
}

void DMAController::triggerHBlank() {
    for (int i = 0; i < 4; ++i) if (timingMatch(i, 2)) startChannel(i);
}

void DMAController::triggerSoundFIFO(Bus& bus, bool fifoB) {
    (void)bus;
    const u32 fifoAddr = fifoB ? 0x040000A4u : 0x040000A0u;
    for (int i = 1; i <= 2; ++i) {
        auto& c = ch[i];
        if (!(c.control & (1 << 15))) continue;
        if (((c.control >> 12) & 0x3) != 3) continue;
        if ((c.dst & ~3u) != fifoAddr) continue;

        if (!c.latched) {
            c.internalSrc = applyAddrMask(i, c.src, false);
            c.internalDst = applyAddrMask(i, fifoAddr, true);
            c.latched = true;
        }
        // FIFO DMA on GBA is always 32-bit with fixed destination.
        c.control = static_cast<u16>((c.control | (1u << 10)) & ~(0x3u << 5));
        c.control |= static_cast<u16>(2u << 5);
        c.internalDst = applyAddrMask(i, fifoAddr, true);
        if (!c.active && c.internalCount == 0) {
            c.startupDelay = 2;
        }
        constexpr u32 maxCount = 0x10000u;
        c.internalCount = (c.internalCount > (maxCount - 4u)) ? maxCount : (c.internalCount + 4u);
        c.active = true;
        if (emudebug::Settings::instance().enabledFor(emudebug::DMA)) {
            // Sound FIFO DMA re-triggers on every FIFO refill (can be thousands of
            // times per second during audio playback). Rate-limit hard so Debug
            // logging doesn't itself become the bottleneck.
            static int fifoLogBudget = 128;
            if (fifoLogBudget > 0) {
                --fifoLogBudget;
                std::ostringstream ss;
                ss << "FIFO DMA CH" << i << " -> " << (fifoB ? "B" : "A")
                   << " queuedWords=" << c.internalCount;
                emulog::Logger::instance().log(emulog::Level::Debug, "DMA", ss.str());
            }
        }
        return;
    }
}

void DMAController::tick(Bus& bus, int cycles) {
    int budget = cycles < 1 ? 1 : cycles;
    int guard = 0;
    while (budget > 0 && guard < 16384) {
        int channel = -1;
        for (int i = 0; i < 4; ++i) {
            if (ch[i].active) {
                channel = i;
                break;
            }
        }
        if (channel < 0) break;
        // Run one transfer unit at a time to re-evaluate channel priority after each unit.
        int spent = runChannel(bus, channel, budget, 1);
        if (spent <= 0) {
            // Forward progress guard in rare edge-cases (should rarely happen).
            spent = 1;
        }
        budget -= spent;
        ++guard;
    }
}

int DMAController::runChannel(Bus& bus, int i, int budgetCycles, int maxTransfers) {
    auto& c = ch[i];
    if (!c.active) return 0;

    bool transfer32 = c.control & (1 << 10);
    int srcCtrl = (c.control >> 7) & 0x3;
    int dstCtrl = (c.control >> 5) & 0x3;
    bool repeat = c.control & (1 << 9);
    bool irq = c.control & (1 << 14);
    int spent = 0;

    if (c.startupDelay > 0) {
        if (budgetCycles >= 0) {
            int remain = budgetCycles - spent;
            if (remain <= 0) return spent;
            int consume = (c.startupDelay < remain) ? c.startupDelay : remain;
            c.startupDelay -= consume;
            spent += consume;
            if (c.startupDelay > 0) return spent;
        } else {
            spent += c.startupDelay;
            c.startupDelay = 0;
        }
    }

    bool seqSrc = false;
    bool seqDst = false;
    int transfersDone = 0;
    while (c.internalCount > 0) {
        if (maxTransfers >= 0 && transfersDone >= maxTransfers) break;
        int unitCycles = transfer32
            ? (bus.accessTime32(c.internalSrc, seqSrc) + bus.accessTime32(c.internalDst, seqDst))
            : (bus.accessTime16(c.internalSrc, seqSrc) + bus.accessTime16(c.internalDst, seqDst));
        if (budgetCycles >= 0 && spent > 0 && spent + unitCycles > budgetCycles) break;
        if (transfer32) {
            u32 value = bus.read32(c.internalSrc);
            if (emudebug::Settings::instance().enabledFor(emudebug::DMA) && isAffineIoReg(c.internalDst)) {
                static int affineDmaLogBudget = 512;
                if (affineDmaLogBudget > 0) {
                    --affineDmaLogBudget;
                    std::ostringstream ss;
                    ss << "CH" << i
                       << " HDMA32 vcount=" << bus.ppu.vcount
                       << " dst=0x" << std::hex << c.internalDst
                       << " src=0x" << c.internalSrc
                       << " val=0x" << value
                       << std::dec;
                    emulog::Logger::instance().log(emulog::Level::Debug, "DMA", ss.str());
                }
            }
            bus.write32(c.internalDst, value);
            switch (srcCtrl) {
                case 0: c.internalSrc = applyAddrMask(i, c.internalSrc + 4, false); break;
                case 1: c.internalSrc = applyAddrMask(i, c.internalSrc - 4, false); break;
                case 2: break;
                case 3: c.internalSrc = applyAddrMask(i, c.internalSrc + 4, false); break;
            }
            switch (dstCtrl) {
                case 0: c.internalDst = applyAddrMask(i, c.internalDst + 4, true); break;
                case 1: c.internalDst = applyAddrMask(i, c.internalDst - 4, true); break;
                case 2: break;
                case 3: c.internalDst = applyAddrMask(i, c.internalDst + 4, true); break;
            }
        } else {
            u16 value = bus.read16(c.internalSrc);
            if (emudebug::Settings::instance().enabledFor(emudebug::DMA) && isAffineIoReg(c.internalDst)) {
                static int affineDmaLogBudget16 = 512;
                if (affineDmaLogBudget16 > 0) {
                    --affineDmaLogBudget16;
                    std::ostringstream ss;
                    ss << "CH" << i
                       << " HDMA16 vcount=" << bus.ppu.vcount
                       << " dst=0x" << std::hex << c.internalDst
                       << " src=0x" << c.internalSrc
                       << " val=0x" << value
                       << std::dec;
                    emulog::Logger::instance().log(emulog::Level::Debug, "DMA", ss.str());
                }
            }
            bus.write16(c.internalDst, value);
            switch (srcCtrl) {
                case 0: c.internalSrc = applyAddrMask(i, c.internalSrc + 2, false); break;
                case 1: c.internalSrc = applyAddrMask(i, c.internalSrc - 2, false); break;
                case 2: break;
                case 3: c.internalSrc = applyAddrMask(i, c.internalSrc + 2, false); break;
            }
            switch (dstCtrl) {
                case 0: c.internalDst = applyAddrMask(i, c.internalDst + 2, true); break;
                case 1: c.internalDst = applyAddrMask(i, c.internalDst - 2, true); break;
                case 2: break;
                case 3: c.internalDst = applyAddrMask(i, c.internalDst + 2, true); break;
            }
        }
        seqSrc = true;
        seqDst = true;
        --c.internalCount;
        ++transfersDone;
        spent += unitCycles;
    }

    if (c.internalCount == 0) {
        c.active = false;
        if (!repeat || (((c.control >> 12) & 0x3) == 0)) {
            c.control &= ~(1 << 15);
            c.latched = false;
        }
        if (irq) irqc.request(static_cast<u16>(dma_irq_bit(i)));
        if (emudebug::Settings::instance().enabledFor(emudebug::DMA)) {
            // Same rationale as the "start" log above: repeatable/FIFO channels can
            // complete extremely often, so cap it per channel.
            static int completeLogBudget[4] = {256, 256, 256, 256};
            if (completeLogBudget[i] > 0) {
                --completeLogBudget[i];
                std::ostringstream ss;
                ss << "CH" << i << " complete"
                   << " repeat=" << (repeat ? 1 : 0)
                   << " irq=" << (irq ? 1 : 0)
                   << " dst=0x" << std::hex << c.internalDst
                   << " src=0x" << c.internalSrc
                   << std::dec;
                emulog::Logger::instance().log(emulog::Level::Debug, "DMA", ss.str());
            }
        }
    }
    return spent;
}

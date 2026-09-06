#include "swi.hpp"
#include "bus.hpp"
#include "cpu.hpp"
#include "emu_log.hpp"
#include "interrupts.hpp"
#include <cmath>
#include <sstream>

static u32 reg(CPU& cpu, int i) { return cpu.rawReg(i); }
static void setReg(CPU& cpu, int i, u32 v) { cpu.setRawReg(i, v); }

bool SWI::handle(Bus& bus, CPU& cpu, u32 number) {
    auto intrWait = [&](bool discardOld, u16 waitMask) {
        u16 mask = static_cast<u16>(waitMask & 0x3FFF);
        if (mask == 0) return;
        bus.irq.IE |= mask;
        bus.irq.IME = 1;
        if (discardOld) bus.irq.IF &= static_cast<u16>(~mask);
        // Previously, if the awaited bit was already set in IF at call time, we
        // acked it and returned immediately without ever dispatching a real
        // interrupt. That skipped the game's actual interrupt handler, which
        // this ROM (and many others) depends on to do real per-frame work.
        // Instead, always halt for at least one tick: Bus::tick() will see the
        // pending bit right away and un-halt us so the CPU's normal interrupt
        // dispatch (checkInterrupts/enterIRQ) runs the real handler, exactly as
        // real hardware does. The extra latency is at most a few cycles.
        bus.intrWaitActive = true;
        bus.intrWaitMask = mask;
        bus.haltMode = true;
    };

    switch (number) {
        case 0x00: {
            setReg(cpu, 13, 0x03007F00);
            setReg(cpu, 14, 0);
            setReg(cpu, 15, 0x08000000);
            return true;
        }
        case 0x01: {
            u32 flags = reg(cpu, 0);
            if (flags & (1 << 0)) bus.ewram.fill(0);
            if (flags & (1 << 1)) bus.iwram.fill(0);
            if (flags & (1 << 2)) bus.palette.fill(0);
            if (flags & (1 << 3)) bus.vram.fill(0);
            if (flags & (1 << 4)) bus.oam.fill(0);
            return true;
        }
        case 0x04: {
            bool discard = (reg(cpu, 0) & 1u) != 0;
            u16 mask = static_cast<u16>(reg(cpu, 1) & 0x3FFF);
            intrWait(discard, mask);
            return true;
        }
        case 0x05:
            intrWait(true, IRQ_VBLANK);
            return true;
        case 0x06: {
            s32 num = static_cast<s32>(reg(cpu, 0));
            s32 den = static_cast<s32>(reg(cpu, 1));
            if (den == 0) {
                setReg(cpu, 0, 0);
                setReg(cpu, 1, static_cast<u32>(num));
                setReg(cpu, 3, 0);
            } else {
                s32 q = num / den;
                s32 r = num % den;
                setReg(cpu, 0, static_cast<u32>(q));
                setReg(cpu, 1, static_cast<u32>(r));
                setReg(cpu, 3, static_cast<u32>(std::abs(q)));
            }
            return true;
        }
        case 0x08: {
            u32 v = reg(cpu, 0);
            setReg(cpu, 0, static_cast<u32>(std::sqrt(static_cast<double>(v))));
            return true;
        }
        case 0x0B: {
            u32 src = reg(cpu, 0), dst = reg(cpu, 1), cnt = reg(cpu, 2);
            bool fill = cnt & (1u << 24), word = cnt & (1u << 26);
            u32 length = cnt & 0x1FFFFF;
            if (word) {
                u32 value = bus.read32(src);
                for (u32 i = 0; i < length; ++i) {
                    u32 out = fill ? value : bus.read32(src + i * 4);
                    bus.write32(dst + i * 4, out);
                }
            } else {
                u16 value = bus.read16(src);
                for (u32 i = 0; i < length; ++i) {
                    u16 out = fill ? value : bus.read16(src + i * 2);
                    bus.write16(dst + i * 2, out);
                }
            }
            return true;
        }
        case 0x0C: {
            u32 src = reg(cpu, 0), dst = reg(cpu, 1), cnt = reg(cpu, 2);
            bool fill = cnt & (1u << 24);
            // CpuFastSet's length field is already a WORD count (not a block
            // count), max 0x1FFFFF words. Hardware copies in 8-word (32 byte)
            // chunks and only rounds the requested length UP to the next
            // multiple of 8 -- it does not multiply the length by 8. The old
            // "* 8" here inflated every CpuFastSet call by 8x, so any game
            // that used this SWI for a large buffer copy/clear (e.g. this
            // ROM's NES-frame-buffer setup) ended up doing an 8x larger than
            // intended copy. That stalled the emulation thread for seconds
            // and starved the PPU/timers of ticks, which is what produced
            // this bug report's symptom: everything (GPU/DMA/CPU debug
            // state) freezes solid right after the intro finishes loading.
            u32 length = (cnt & 0x1FFFFF);
            length = (length + 7u) & ~7u;
            u32 value = bus.read32(src);
            for (u32 i = 0; i < length; ++i) {
                u32 out = fill ? value : bus.read32(src + i * 4);
                bus.write32(dst + i * 4, out);
            }
            return true;
        }
        default:
            {
                // A game polling an unimplemented SWI in a loop would otherwise flood
                // the logger; cap it.
                static int unhandledSwiLogBudget = 256;
                if (unhandledSwiLogBudget > 0) {
                    --unhandledSwiLogBudget;
                    std::ostringstream ss;
                    ss << "Unhandled SWI 0x" << std::hex << number;
                    emulog::Logger::instance().log(emulog::Level::Stub, "SWI", ss.str());
                }
            }
            return false;
    }
}

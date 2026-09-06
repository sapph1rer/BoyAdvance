#include "cpu.hpp"
#include "emu_log.hpp"
#include "swi.hpp"
#include "state_io.hpp"
#include <iostream>
#include <sstream>

static int mulCycleCount(u32 v) {
    // ARM7TDMI-style variable multiply timing approximation.
    if ((v & 0xFFFFFF00u) == 0 || (v & 0xFFFFFF00u) == 0xFFFFFF00u) return 1;
    if ((v & 0xFFFF0000u) == 0 || (v & 0xFFFF0000u) == 0xFFFF0000u) return 2;
    if ((v & 0xFF000000u) == 0 || (v & 0xFF000000u) == 0xFF000000u) return 3;
    return 4;
}

CPU::CPU(Bus& b) : bus(b) {
    reset(true);
}

bool CPU::saveState(std::ostream& os) const {
    using namespace stateio;
    if (!writeArray(os, regs)) return false;
    for (int i = 0; i < 5; ++i) if (!writePod(os, bank.r8_r12_usr[i])) return false;
    for (int i = 0; i < 5; ++i) if (!writePod(os, bank.r8_r12_fiq[i])) return false;
    if (!writePod(os, bank.r13_usr)) return false;
    if (!writePod(os, bank.r14_usr)) return false;
    if (!writePod(os, bank.r13_fiq)) return false;
    if (!writePod(os, bank.r14_fiq)) return false;
    if (!writePod(os, bank.r13_svc)) return false;
    if (!writePod(os, bank.r14_svc)) return false;
    if (!writePod(os, bank.r13_abt)) return false;
    if (!writePod(os, bank.r14_abt)) return false;
    if (!writePod(os, bank.r13_irq)) return false;
    if (!writePod(os, bank.r14_irq)) return false;
    if (!writePod(os, bank.r13_und)) return false;
    if (!writePod(os, bank.r14_und)) return false;
    if (!writePod(os, bank.spsr_fiq)) return false;
    if (!writePod(os, bank.spsr_svc)) return false;
    if (!writePod(os, bank.spsr_abt)) return false;
    if (!writePod(os, bank.spsr_irq)) return false;
    if (!writePod(os, bank.spsr_und)) return false;
    if (!writePod(os, cpsr)) return false;
    if (!writePod(os, thumbBLHigh)) return false;
    if (!writeBool(os, thumbBLPrefixValid)) return false;
    if (!writeBool(os, lastFetchValid)) return false;
    if (!writeBool(os, lastFetchThumb)) return false;
    if (!writePod(os, lastFetchAddr)) return false;
    return true;
}

bool CPU::loadState(std::istream& is) {
    using namespace stateio;
    if (!readArray(is, regs)) return false;
    for (int i = 0; i < 5; ++i) if (!readPod(is, bank.r8_r12_usr[i])) return false;
    for (int i = 0; i < 5; ++i) if (!readPod(is, bank.r8_r12_fiq[i])) return false;
    if (!readPod(is, bank.r13_usr)) return false;
    if (!readPod(is, bank.r14_usr)) return false;
    if (!readPod(is, bank.r13_fiq)) return false;
    if (!readPod(is, bank.r14_fiq)) return false;
    if (!readPod(is, bank.r13_svc)) return false;
    if (!readPod(is, bank.r14_svc)) return false;
    if (!readPod(is, bank.r13_abt)) return false;
    if (!readPod(is, bank.r14_abt)) return false;
    if (!readPod(is, bank.r13_irq)) return false;
    if (!readPod(is, bank.r14_irq)) return false;
    if (!readPod(is, bank.r13_und)) return false;
    if (!readPod(is, bank.r14_und)) return false;
    if (!readPod(is, bank.spsr_fiq)) return false;
    if (!readPod(is, bank.spsr_svc)) return false;
    if (!readPod(is, bank.spsr_abt)) return false;
    if (!readPod(is, bank.spsr_irq)) return false;
    if (!readPod(is, bank.spsr_und)) return false;
    if (!readPod(is, cpsr)) return false;
    if (!readPod(is, thumbBLHigh)) return false;
    if (!readBool(is, thumbBLPrefixValid)) return false;
    if (!readBool(is, lastFetchValid)) return false;
    if (!readBool(is, lastFetchThumb)) return false;
    if (!readPod(is, lastFetchAddr)) return false;
    return true;
}

void CPU::reset(bool skipBIOS) {
    regs.fill(0);
    bank = {};
    for (int i = 0; i < 5; ++i) bank.r8_r12_usr[i] = 0;
    bank.r13_usr = 0x03007F00;
    bank.r14_usr = 0;
    bank.r13_svc = 0x03007FE0;
    bank.r13_irq = 0x03007FA0;
    bank.r13_und = 0x03007F40;

    if (skipBIOS) {
        // Approximate post-BIOS CPU state used by commercial games.
        cpsr = SYS;
        setIRQDisabled(false);
        setFIQDisabled(false);
        setThumbMode(false);
        for (int i = 0; i < 5; ++i) regs[8 + i] = bank.r8_r12_usr[i];
        regs[13] = bank.r13_usr;
        regs[14] = bank.r14_usr;
        regs[15] = 0x08000000;
    } else {
        cpsr = SVC;
        setIRQDisabled(true);
        setFIQDisabled(true);
        setThumbMode(false);
        for (int i = 0; i < 5; ++i) regs[8 + i] = bank.r8_r12_usr[i];
        regs[13] = bank.r13_svc;
        regs[14] = bank.r14_svc;
        regs[15] = 0x00000000;
    }
    thumbBLPrefixValid = false;
    lastFetchValid = false;
    lastFetchThumb = false;
    lastFetchAddr = 0;
    bus.invalidateSequentialTiming();
}

u32 CPU::regRead(int idx) const {
    if (idx == 15) return thumbMode() ? pcVisibleTHUMB() : pcVisibleARM();
    return regs[idx];
}

void CPU::regWrite(int idx, u32 value) {
    if (idx == 15) regs[15] = thumbMode() ? align2(value) : align4(value);
    else regs[idx] = value;
}

u32 CPU::pcVisibleARM() const { return regs[15] + 4; }
u32 CPU::pcVisibleTHUMB() const { return regs[15] + 2; }

u32* CPU::currentSPSR() {
    switch (currentMode()) {
        case FIQ: return &bank.spsr_fiq;
        case SVC: return &bank.spsr_svc;
        case ABT: return &bank.spsr_abt;
        case IRQ: return &bank.spsr_irq;
        case UND: return &bank.spsr_und;
        default: return nullptr;
    }
}

void CPU::switchMode(Mode newMode) {
    Mode old = currentMode();
    if (old == newMode) return;

    auto saveMode = [&](Mode mode) {
        switch (mode) {
            case USER:
            case SYS:
                for (int i = 0; i < 5; ++i) bank.r8_r12_usr[i] = regs[8 + i];
                bank.r13_usr = regs[13]; bank.r14_usr = regs[14];
                break;
            case FIQ:
                for (int i = 0; i < 5; ++i) bank.r8_r12_fiq[i] = regs[8 + i];
                bank.r13_fiq = regs[13]; bank.r14_fiq = regs[14]; break;
            case SVC:
                for (int i = 0; i < 5; ++i) bank.r8_r12_usr[i] = regs[8 + i];
                bank.r13_svc = regs[13]; bank.r14_svc = regs[14]; break;
            case ABT:
                for (int i = 0; i < 5; ++i) bank.r8_r12_usr[i] = regs[8 + i];
                bank.r13_abt = regs[13]; bank.r14_abt = regs[14]; break;
            case IRQ:
                for (int i = 0; i < 5; ++i) bank.r8_r12_usr[i] = regs[8 + i];
                bank.r13_irq = regs[13]; bank.r14_irq = regs[14]; break;
            case UND:
                for (int i = 0; i < 5; ++i) bank.r8_r12_usr[i] = regs[8 + i];
                bank.r13_und = regs[13]; bank.r14_und = regs[14]; break;
            default: break;
        }
    };
    auto loadMode = [&](Mode mode) {
        switch (mode) {
            case FIQ:
                for (int i = 0; i < 5; ++i) regs[8 + i] = bank.r8_r12_fiq[i];
                regs[13] = bank.r13_fiq; regs[14] = bank.r14_fiq; break;
            case USER:
            case SYS:
                for (int i = 0; i < 5; ++i) regs[8 + i] = bank.r8_r12_usr[i];
                regs[13] = bank.r13_usr; regs[14] = bank.r14_usr; break;
            case SVC:
                for (int i = 0; i < 5; ++i) regs[8 + i] = bank.r8_r12_usr[i];
                regs[13] = bank.r13_svc; regs[14] = bank.r14_svc; break;
            case ABT:
                for (int i = 0; i < 5; ++i) regs[8 + i] = bank.r8_r12_usr[i];
                regs[13] = bank.r13_abt; regs[14] = bank.r14_abt; break;
            case IRQ:
                for (int i = 0; i < 5; ++i) regs[8 + i] = bank.r8_r12_usr[i];
                regs[13] = bank.r13_irq; regs[14] = bank.r14_irq; break;
            case UND:
                for (int i = 0; i < 5; ++i) regs[8 + i] = bank.r8_r12_usr[i];
                regs[13] = bank.r13_und; regs[14] = bank.r14_und; break;
            default: break;
        }
    };

    saveMode(old);
    loadMode(newMode);
    cpsr = (cpsr & ~0x1Fu) | static_cast<u32>(newMode);
}

u32 CPU::readUserReg(int idx) const {
    if (idx <= 7) return regs[idx];
    if (idx <= 12) {
        if (currentMode() == FIQ) return bank.r8_r12_usr[idx - 8];
        return regs[idx];
    }
    if (idx == 13) {
        if (currentMode() == USER || currentMode() == SYS) return regs[13];
        return bank.r13_usr;
    }
    if (idx == 14) {
        if (currentMode() == USER || currentMode() == SYS) return regs[14];
        return bank.r14_usr;
    }
    return regRead(idx);
}

void CPU::writeUserReg(int idx, u32 value) {
    if (idx <= 7) {
        regs[idx] = value;
        return;
    }
    if (idx <= 12) {
        if (currentMode() == FIQ) bank.r8_r12_usr[idx - 8] = value;
        else regs[idx] = value;
        return;
    }
    if (idx == 13) {
        if (currentMode() == USER || currentMode() == SYS) regs[13] = value;
        else bank.r13_usr = value;
        return;
    }
    if (idx == 14) {
        if (currentMode() == USER || currentMode() == SYS) regs[14] = value;
        else bank.r14_usr = value;
        return;
    }
    regWrite(idx, value);
}

void CPU::restoreCPSRFromSPSRIfPossible() {
    if (u32* spsr = currentSPSR()) {
        u32 newCpsr = *spsr;
        Mode oldMode = currentMode();
        Mode newMode = static_cast<Mode>(newCpsr & 0x1F);
        if (oldMode != newMode) switchMode(newMode);
        cpsr = (cpsr & 0x1Fu) | (newCpsr & ~0x1Fu);
    }
}

void CPU::enterIRQ() {
    u32 oldCpsr = cpsr;
    switchMode(IRQ);
    if (u32* spsr = currentSPSR()) *spsr = oldCpsr;
    // IRQ returns through SUBS PC,LR,#4 in BIOS handler.
    regs[14] = regs[15] + 4;
    setThumbMode(false);
    setIRQDisabled(true);
    regs[15] = 0x18;
    flushPipelineAfterControlFlowChange(true, false);
}

void CPU::enterSWI() {
    u32 oldCpsr = cpsr;
    switchMode(SVC);
    if (u32* spsr = currentSPSR()) *spsr = oldCpsr;
    regs[14] = regs[15];
    setThumbMode(false);
    setIRQDisabled(true);
    regs[15] = 0x08;
    flushPipelineAfterControlFlowChange(true, false);
}

void CPU::enterUndefined() {
    u32 oldCpsr = cpsr;
    switchMode(UND);
    if (u32* spsr = currentSPSR()) *spsr = oldCpsr;
    regs[14] = regs[15];
    setThumbMode(false);
    setIRQDisabled(true);
    regs[15] = 0x04;
    flushPipelineAfterControlFlowChange(true, false);
}

void CPU::checkInterrupts() {
    if (!irqDisabled() && bus.irq.pending()) enterIRQ();
}

u32 CPU::fetchARM() {
    u32 addr = align4(regs[15]);
    u32 op = bus.read32(addr);
    if ((bus.waitcnt & (1 << 14)) &&
        addr >= 0x08000000 && addr <= 0x0DFFFFFF &&
        lastFetchValid && !lastFetchThumb && addr == (lastFetchAddr + 4) &&
        bus.lastAccessCycles > 1) {
        bus.lastAccessCycles -= 1;
    }
    bus.latchPrefetch32(op);
    if (addr <= 0x00003FFF) bus.latchFetchedBiosOpcode32(op);
    regs[15] = addr + 4;
    lastFetchValid = true;
    lastFetchThumb = false;
    lastFetchAddr = addr;
    return op;
}

u16 CPU::fetchTHUMB() {
    u32 addr = align2(regs[15]);
    u16 op = bus.read16(addr);
    if ((bus.waitcnt & (1 << 14)) &&
        addr >= 0x08000000 && addr <= 0x0DFFFFFF &&
        lastFetchValid && lastFetchThumb && addr == (lastFetchAddr + 2) &&
        bus.lastAccessCycles > 1) {
        bus.lastAccessCycles -= 1;
    }
    u32 latch = static_cast<u32>(op) | (static_cast<u32>(op) << 16);
    bus.latchPrefetch32(latch);
    if (addr <= 0x00003FFF) {
        bus.latchFetchedBiosOpcode32(latch);
    }
    regs[15] = addr + 2;
    lastFetchValid = true;
    lastFetchThumb = true;
    lastFetchAddr = addr;
    return op;
}

bool CPU::conditionPassed(u32 cond) const {
    switch (cond) {
        case 0x0: return flagZ();
        case 0x1: return !flagZ();
        case 0x2: return flagC();
        case 0x3: return !flagC();
        case 0x4: return flagN();
        case 0x5: return !flagN();
        case 0x6: return flagV();
        case 0x7: return !flagV();
        case 0x8: return flagC() && !flagZ();
        case 0x9: return !flagC() || flagZ();
        case 0xA: return flagN() == flagV();
        case 0xB: return flagN() != flagV();
        case 0xC: return !flagZ() && (flagN() == flagV());
        case 0xD: return flagZ() || (flagN() != flagV());
        case 0xE: return true;
        default: return false;
    }
}

CPU::ShiftResult CPU::decodeImmediateShift(u32 value, u32 type, u32 amount) const {
    ShiftResult r{value, flagC()};
    switch (type) {
        case 0:
            if (amount) { r.carry = bit(value, 32 - amount); r.value = value << amount; }
            break;
        case 1:
            if (amount == 0) amount = 32;
            r.carry = bit(value, amount - 1);
            r.value = (amount == 32) ? 0 : (value >> amount);
            break;
        case 2:
            if (amount == 0) amount = 32;
            r.carry = bit(value, amount - 1);
            r.value = (amount >= 32) ? (bit(value,31) ? 0xFFFFFFFFu : 0u) : static_cast<u32>(static_cast<s32>(value) >> amount);
            break;
        case 3:
            if (amount == 0) { r.carry = bit(value,0); r.value = (flagC() ? 0x80000000u : 0u) | (value >> 1); }
            else { r.value = ror32(value, amount); r.carry = bit(r.value, 31); }
            break;
    }
    return r;
}

CPU::ShiftResult CPU::decodeRegisterShift(u32 value, u32 type, u32 amount, bool oldCarry) const {
    ShiftResult r{value, oldCarry};
    if (amount == 0) return r;
    switch (type) {
        case 0:
            if (amount < 32) { r.carry = bit(value, 32 - amount); r.value = value << amount; }
            else if (amount == 32) { r.carry = bit(value, 0); r.value = 0; }
            else { r.carry = false; r.value = 0; }
            break;
        case 1:
            if (amount < 32) { r.carry = bit(value, amount - 1); r.value = value >> amount; }
            else if (amount == 32) { r.carry = bit(value,31); r.value = 0; }
            else { r.carry = false; r.value = 0; }
            break;
        case 2:
            if (amount < 32) { r.carry = bit(value, amount - 1); r.value = static_cast<u32>(static_cast<s32>(value) >> amount); }
            else { r.carry = bit(value,31); r.value = bit(value,31) ? 0xFFFFFFFFu : 0u; }
            break;
        case 3:
            amount &= 0xFF;
            if ((amount & 31) == 0) { r.carry = bit(value,31); r.value = value; }
            else { r.value = ror32(value, amount & 31); r.carry = bit(r.value,31); }
            break;
    }
    return r;
}

CPU::ShiftResult CPU::decodeARMOperand2(u32 op, bool& carryOut) const {
    ShiftResult r{};
    if (bit(op, 25)) {
        u32 imm = op & 0xFF;
        u32 rot = ((op >> 8) & 0xF) * 2;
        r.value = ror32(imm, rot);
        r.carry = (rot == 0) ? flagC() : bit(r.value, 31);
    } else {
        u32 rm = op & 0xF;
        u32 value = (rm == 15) ? pcVisibleARM() : regs[rm];
        u32 shiftType = (op >> 5) & 0x3;
        if (!bit(op, 4)) {
            u32 shiftImm = (op >> 7) & 0x1F;
            r = decodeImmediateShift(value, shiftType, shiftImm);
        } else {
            u32 rs = (op >> 8) & 0xF;
            u32 amount = regRead(rs) & 0xFF;
            r = decodeRegisterShift(value, shiftType, amount, flagC());
        }
    }
    carryOut = r.carry;
    return r;
}

void CPU::setNZ(u32 v) {
    setN(bit(v, 31));
    setZ(v == 0);
}

void CPU::flushPipelineAfterControlFlowChange(bool clearThumbBLPrefix, bool addBranchPenalty) {
    if (clearThumbBLPrefix) thumbBLPrefixValid = false;
    lastFetchValid = false;
    bus.invalidateSequentialTiming();
    if (addBranchPenalty) bus.lastAccessCycles += 2;
}

void CPU::step() {
    checkInterrupts();
    bus.lastAccessCycles = 0;
    bus.setCpuExecAddress(regs[15]);
    if (thumbMode()) {
        u16 op = fetchTHUMB();
        execTHUMB(op);
    } else {
        u32 op = fetchARM();
        execARM(op);
    }
    int cycles = bus.lastAccessCycles;
    if (cycles < 1) cycles = 1;
    bus.tick(cycles);
}

void CPU::execARM(u32 op) {
    u32 cond = op >> 28;
    if (!conditionPassed(cond)) return;
    if ((op & 0x0FFFFFF0) == 0x012FFF10) { armBranchExchange(op); return; }
    if ((op & 0x0FBF0FFF) == 0x010F0000 || (op & 0x0DBFF000) == 0x0129F000) { armPSRTransfer(op); return; }
    if ((op & 0x0FF00FF0) == 0x01000090) { armSwap(op); return; }
    if ((op & 0x0F8000F0) == 0x00800090) { armMultiplyLong(op); return; }
    if ((op & 0x0FC000F0) == 0x00000090) { armMultiply(op); return; }
    if ((op & 0x0E000090) == 0x00000090 && (op & 0x60) != 0) { armHalfwordTransfer(op); return; }
    u32 top = (op >> 25) & 0x7;
    u32 top2 = (op >> 26) & 0x3;
    if (top == 0b101) { armBranch(op); return; }
    if ((op & 0x0F000000) == 0x0F000000) { armSoftwareInterrupt(op); return; }
    if (top2 == 0b00) { armDataProcessing(op); return; }
    if (top2 == 0b01) { armSingleDataTransfer(op); return; }
    if (top2 == 0b10) { armBlockTransfer(op); return; }
    {
        // A stuck decode loop can hit this every instruction; cap it so logging
        // itself doesn't stall emulation.
        static int undefinedArmLogBudget = 256;
        if (undefinedArmLogBudget > 0) {
            --undefinedArmLogBudget;
            std::ostringstream ss;
            ss << "Undefined ARM op=0x" << std::hex << op << " at pc=0x" << regs[15];
            emulog::Logger::instance().log(emulog::Level::Warning, "CPU", ss.str());
        }
    }
    enterUndefined();
}

void CPU::armSwap(u32 op) {
    bool byte = bit(op, 22);
    u32 rn = (op >> 16) & 0xF;
    u32 rd = (op >> 12) & 0xF;
    u32 rm = op & 0xF;

    u32 addr = regRead(rn);
    u32 src = regRead(rm);
    if (byte) {
        u32 old = bus.read8(addr);
        bus.write8(addr, static_cast<u8>(src & 0xFF));
        regWrite(rd, old);
        if (rd == 15) flushPipelineAfterControlFlowChange(false, true);
    } else {
        u32 old = ror32(bus.read32(addr & ~3u), static_cast<int>((addr & 3) * 8));
        bus.write32(addr & ~3u, src);
        regWrite(rd, old);
        if (rd == 15) flushPipelineAfterControlFlowChange(false, true);
    }
}

void CPU::armBranchExchange(u32 op) {
    u32 rm = op & 0xF;
    u32 target = regRead(rm);
    setThumbMode(target & 1);
    regs[15] = thumbMode() ? align2(target) : align4(target);
    flushPipelineAfterControlFlowChange(false, true);
}

void CPU::armBranch(u32 op) {
    bool link = bit(op, 24);
    // Avoid UB from left-shifting negative signed values.
    s32 imm = sign_extend24(op & 0x00FFFFFF) * 4;
    u32 oldPC = pcVisibleARM();
    if (link) regs[14] = oldPC - 4;
    regs[15] = oldPC + imm;
    flushPipelineAfterControlFlowChange(false, true);
}

void CPU::armDataProcessing(u32 op) {
    u32 opcode = (op >> 21) & 0xF;
    bool setFlags = bit(op, 20);
    u32 rn = (op >> 16) & 0xF;
    u32 rd = (op >> 12) & 0xF;
    // Register-specified shift (Operand2 with bit4=1) costs one extra internal cycle.
    if (!bit(op, 25) && bit(op, 4)) bus.lastAccessCycles += 1;

    bool shCarry = flagC();
    ShiftResult op2s = decodeARMOperand2(op, shCarry);
    u32 op2 = op2s.value;
    u32 a = regRead(rn);
    u32 result = 0;

    auto writeLogical = [&](u32 v) {
        regWrite(rd, v);
        if (setFlags) {
            setNZ(v); setC(shCarry);
            if (rd == 15) {
                bool hadSPSR = currentSPSR() != nullptr;
                restoreCPSRFromSPSRIfPossible();
                if (hadSPSR) regs[15] = thumbMode() ? align2(v) : align4(v);
            }
        }
        if (rd == 15) flushPipelineAfterControlFlowChange(false, true);
    };
    auto writeArithmetic = [&](u32 v, bool c, bool ov) {
        regWrite(rd, v);
        if (setFlags) {
            setNZ(v); setC(c); setV(ov);
            if (rd == 15) {
                bool hadSPSR = currentSPSR() != nullptr;
                restoreCPSRFromSPSRIfPossible();
                if (hadSPSR) regs[15] = thumbMode() ? align2(v) : align4(v);
            }
        }
        if (rd == 15) flushPipelineAfterControlFlowChange(false, true);
    };

    switch (opcode) {
        case 0x0: result = a & op2; writeLogical(result); break;
        case 0x1: result = a ^ op2; writeLogical(result); break;
        case 0x2: result = a - op2; writeArithmetic(result, a >= op2, sub_overflow(a, op2, result)); break;
        case 0x3: result = op2 - a; writeArithmetic(result, op2 >= a, sub_overflow(op2, a, result)); break;
        case 0x4: result = a + op2; writeArithmetic(result, add_carry(a, op2), add_overflow(a, op2, result)); break;
        case 0x5: { u32 c = flagC() ? 1 : 0; u32 tmp = a + op2; result = tmp + c; writeArithmetic(result, add_carry(a, op2) || add_carry(tmp, c), add_overflow(a, op2, tmp) || add_overflow(tmp, c, result)); break; }
        case 0x6: { u32 c = flagC() ? 1 : 0; u32 sub = op2 + (1 - c); result = a - sub; writeArithmetic(result, a >= sub, sub_overflow(a, sub, result)); break; }
        case 0x7: { u32 c = flagC() ? 1 : 0; u32 sub = a + (1 - c); result = op2 - sub; writeArithmetic(result, op2 >= sub, sub_overflow(op2, sub, result)); break; }
        case 0x8: result = a & op2; setNZ(result); setC(shCarry); break;
        case 0x9: result = a ^ op2; setNZ(result); setC(shCarry); break;
        case 0xA: result = a - op2; setNZ(result); setC(a >= op2); setV(sub_overflow(a, op2, result)); break;
        case 0xB: result = a + op2; setNZ(result); setC(add_carry(a, op2)); setV(add_overflow(a, op2, result)); break;
        case 0xC: result = a | op2; writeLogical(result); break;
        case 0xD: result = op2; writeLogical(result); break;
        case 0xE: result = a & ~op2; writeLogical(result); break;
        case 0xF: result = ~op2; writeLogical(result); break;
    }
}

void CPU::armPSRTransfer(u32 op) {
    bool psr = bit(op, 22);
    bool reg = bit(op, 21);
    if (!reg) {
        u32 rd = (op >> 12) & 0xF;
        u32 value = psr ? (currentSPSR() ? *currentSPSR() : 0) : cpsr;
        regWrite(rd, value);
        if (rd == 15) flushPipelineAfterControlFlowChange(false, true);
        return;
    }

    u32 fieldMask = 0;
    if (bit(op, 16)) fieldMask |= 0x000000FFu;
    if (bit(op, 17)) fieldMask |= 0x0000FF00u;
    if (bit(op, 18)) fieldMask |= 0x00FF0000u;
    if (bit(op, 19)) fieldMask |= 0xFF000000u;

    u32 operand = 0;
    if (bit(op, 25)) {
        u32 imm = op & 0xFF;
        u32 rot = ((op >> 8) & 0xF) * 2;
        operand = ror32(imm, rot);
    } else {
        operand = regRead(op & 0xF);
    }

    if (psr) {
        if (u32* spsr = currentSPSR()) *spsr = (*spsr & ~fieldMask) | (operand & fieldMask);
    } else {
        u32 newCpsr = (cpsr & ~fieldMask) | (operand & fieldMask);
        Mode oldMode = currentMode();
        Mode newMode = static_cast<Mode>(newCpsr & 0x1F);
        if (oldMode != newMode) switchMode(newMode);
        cpsr = (cpsr & 0x1Fu) | (newCpsr & ~0x1Fu);
    }
}

void CPU::armMultiply(u32 op) {
    bool accumulate = bit(op, 21);
    bool setFlags = bit(op, 20);
    u32 rd = (op >> 16) & 0xF;
    u32 rn = (op >> 12) & 0xF;
    u32 rs = (op >> 8) & 0xF;
    u32 rm = op & 0xF;
    u32 result = regRead(rm) * regRead(rs);
    bus.lastAccessCycles += mulCycleCount(regRead(rs));
    if (accumulate) result += regRead(rn);
    if (accumulate) bus.lastAccessCycles += 1;
    regWrite(rd, result);
    if (setFlags) setNZ(result);
}

void CPU::armMultiplyLong(u32 op) {
    bool U = bit(op, 22);
    bool A = bit(op, 21);
    bool S = bit(op, 20);
    u32 rdHi = (op >> 16) & 0xF;
    u32 rdLo = (op >> 12) & 0xF;
    u32 rs = (op >> 8) & 0xF;
    u32 rm = op & 0xF;

    uint64_t result = 0;
    if (U) {
        int64_t mul = static_cast<int64_t>(static_cast<int32_t>(regRead(rm))) *
                      static_cast<int64_t>(static_cast<int32_t>(regRead(rs)));
        result = static_cast<uint64_t>(mul);
    } else {
        result = static_cast<uint64_t>(regRead(rm)) * static_cast<uint64_t>(regRead(rs));
    }

    if (A) {
        uint64_t acc = (static_cast<uint64_t>(regRead(rdHi)) << 32) | regRead(rdLo);
        result += acc;
    }
    bus.lastAccessCycles += mulCycleCount(regRead(rs)) + 1 + (A ? 1 : 0);

    regWrite(rdLo, static_cast<u32>(result & 0xFFFFFFFFu));
    regWrite(rdHi, static_cast<u32>(result >> 32));
    if (S) {
        setN((result >> 63) != 0);
        setZ(result == 0);
    }
}

void CPU::armSingleDataTransfer(u32 op) {
    bool I = bit(op, 25), P = bit(op, 24), U = bit(op, 23), B = bit(op, 22), W = bit(op, 21), L = bit(op, 20);
    u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
    u32 base = regRead(rn), offset = 0;
    if (!I) offset = op & 0xFFF;
    else {
        u32 rm = op & 0xF;
        u32 shiftType = (op >> 5) & 0x3;
        u32 shiftImm = (op >> 7) & 0x1F;
        offset = decodeImmediateShift(regRead(rm), shiftType, shiftImm).value;
    }
    u32 addr = base;
    if (P) addr = U ? base + offset : base - offset;
    if (L) {
        u32 value = 0;
        if (B) value = bus.read8(addr);
        else value = ror32(bus.read32(addr & ~3u), (addr & 3) * 8);
        regWrite(rd, value);
        if (rd == 15) flushPipelineAfterControlFlowChange(false, true);
    } else {
        u32 value = regRead(rd);
        if (rd == 15) value += 4;
        if (B) bus.write8(addr, value & 0xFF);
        else bus.write32(addr & ~3u, value);
    }
    if (!P) base = U ? base + offset : base - offset;
    else if (W) base = U ? base + offset : base - offset;
    if ((W || !P) && !(L && rd == rn)) regs[rn] = base;
}

void CPU::armHalfwordTransfer(u32 op) {
    bool P = bit(op,24), U = bit(op,23), I = bit(op,22), W = bit(op,21), L = bit(op,20);
    u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF, sh = (op >> 5) & 0x3;
    u32 offset = I ? ((((op >> 8) & 0xF) << 4) | (op & 0xF)) : regRead(op & 0xF);
    u32 base = regRead(rn), addr = base;
    if (P) addr = U ? base + offset : base - offset;
    if (L) {
        u32 value = 0;
        switch (sh) {
            case 1: value = bus.read16(addr & ~1u); if (addr & 1) value = ror32(value, 8); break;
            case 2: value = static_cast<u32>(static_cast<s32>(static_cast<int8_t>(bus.read8(addr)))); break;
            case 3: value = (addr & 1) ? static_cast<u32>(static_cast<s32>(static_cast<int8_t>(bus.read8(addr))))
                                        : static_cast<u32>(static_cast<s32>(static_cast<int16_t>(bus.read16(addr)))); break;
        }
        regWrite(rd, value);
        if (rd == 15) flushPipelineAfterControlFlowChange(false, true);
    } else if (sh == 1) {
        bus.write16(addr & ~1u, regRead(rd) & 0xFFFF);
    }
    if (!P) base = U ? base + offset : base - offset;
    else if (W) base = U ? base + offset : base - offset;
    if ((W || !P) && !(L && rd == rn)) regs[rn] = base;
}

void CPU::armBlockTransfer(u32 op) {
    bool P = bit(op,24), U = bit(op,23), S = bit(op,22), W = bit(op,21), L = bit(op,20);
    u32 rn = (op >> 16) & 0xF;
    u32 regList = op & 0xFFFF;
    bool emptyRlist = regList == 0;
    if (emptyRlist) regList = 1u << 15;
    bool pcInList = bit(regList, 15);
    bool userTransfer = S && !(L && pcInList);

    int count = 0; for (int i = 0; i < 16; ++i) if (bit(regList,i)) ++count;
    if (emptyRlist) count = 16;
    u32 base = regs[rn];
    u32 startAddr = U ? (P ? base + 4 : base) : (P ? base - 4 * count : base - 4 * (count - 1));
    u32 finalBase = U ? (base + 4 * count) : (base - 4 * count);
    int firstReg = 16;
    u32 loadedPC = 0;
    bool loadedPCValid = false;
    for (int i = 0; i < 16; ++i) {
        if (bit(regList, i)) {
            firstReg = i;
            break;
        }
    }
    u32 addr = startAddr;
    for (int i = 0; i < 16; ++i) {
        if (!bit(regList, i)) continue;
        if (L) {
            u32 v = bus.read32(addr);
            if (userTransfer) writeUserReg(i, v);
            else if (i == 15) {
                loadedPC = v;
                loadedPCValid = true;
                regWrite(15, v & ~3u);
            }
            else regs[i] = v;
        } else {
            u32 v = 0;
            if (userTransfer) {
                v = (i == 15) ? (pcVisibleARM() + 4) : readUserReg(i);
            } else {
                if (i == 15) v = pcVisibleARM() + 4;
                else v = regs[i];
                if (W && i == static_cast<int>(rn) && bit(regList, rn)) {
                    v = (rn == static_cast<u32>(firstReg)) ? base : finalBase;
                }
            }
            bus.write32(addr, v);
        }
        addr += 4;
    }
    if (W && !userTransfer) {
        if (!(L && bit(regList, rn))) regs[rn] = finalBase;
    }
    if (L && pcInList && loadedPCValid) {
        if (S) {
            bool hadSPSR = currentSPSR() != nullptr;
            restoreCPSRFromSPSRIfPossible();
            if (hadSPSR) regs[15] = thumbMode() ? align2(loadedPC) : align4(loadedPC);
        }
        flushPipelineAfterControlFlowChange(false, true);
    }
}

void CPU::armSoftwareInterrupt(u32 op) {
    u32 swiNumber = op & 0xFF;
    if (!bus.bios.loaded() && SWI::handle(bus, *this, swiNumber)) return;
    enterSWI();
}

void CPU::execTHUMB(u16 op) {
    if ((op & 0xF800) == 0x1800) { thumbAddSub(op); return; }
    if ((op & 0xE000) == 0x0000) { thumbMoveShifted(op); return; }
    if ((op & 0xE000) == 0x2000) { thumbMovCmpAddSubImm(op); return; }
    if ((op & 0xFC00) == 0x4000) { thumbALU(op); return; }
    if ((op & 0xFC00) == 0x4400) { thumbHiRegBx(op); return; }
    if ((op & 0xF800) == 0x4800) { thumbPcRelativeLoad(op); return; }
    if ((op & 0xF200) == 0x5200) { thumbLoadStoreSigned(op); return; }
    if ((op & 0xF200) == 0x5000) { thumbLoadStoreRegOffset(op); return; }
    if ((op & 0xE000) == 0x6000) { thumbLoadStoreImmOffset(op); return; }
    if ((op & 0xF000) == 0x8000) { thumbLoadStoreHalfword(op); return; }
    if ((op & 0xF000) == 0x9000) { thumbSPRelativeLoadStore(op); return; }
    if ((op & 0xF000) == 0xA000) { thumbLoadAddress(op); return; }
    if ((op & 0xFF00) == 0xB000) { thumbAddOffsetToSP(op); return; }
    if ((op & 0xF600) == 0xB400) { thumbPushPop(op); return; }
    if ((op & 0xF000) == 0xC000) { thumbMultipleLoadStore(op); return; }
    if ((op & 0xFF00) == 0xDF00) { thumbSoftwareInterrupt(op); return; }
    if ((op & 0xF000) == 0xD000) { thumbCondBranch(op); return; }
    if ((op & 0xF800) == 0xE000) { thumbUncondBranch(op); return; }
    if ((op & 0xF000) == 0xF000) { thumbLongBranchLink(op); return; }
    {
        static int undefinedThumbLogBudget = 256;
        if (undefinedThumbLogBudget > 0) {
            --undefinedThumbLogBudget;
            std::ostringstream ss;
            ss << "Undefined THUMB op=0x" << std::hex << op << " at pc=0x" << regs[15];
            emulog::Logger::instance().log(emulog::Level::Warning, "CPU", ss.str());
        }
    }
    enterUndefined();
}

void CPU::thumbMoveShifted(u16 op) {
    u32 type = (op >> 11) & 0x3, imm5 = (op >> 6) & 0x1F, rs = (op >> 3) & 0x7, rd = op & 0x7;
    ShiftResult r = decodeImmediateShift(regs[rs], type, imm5);
    regs[rd] = r.value; setNZ(r.value); setC(r.carry);
}

void CPU::thumbAddSub(u16 op) {
    bool immediate = bit(op,10), sub = bit(op,9);
    u32 rn_or_imm = (op >> 6) & 0x7, rs = (op >> 3) & 0x7, rd = op & 0x7;
    u32 a = regs[rs], b = immediate ? rn_or_imm : regs[rn_or_imm];
    u32 r = sub ? (a - b) : (a + b);
    regs[rd] = r; setNZ(r);
    if (sub) { setC(a >= b); setV(sub_overflow(a,b,r)); }
    else { setC(add_carry(a,b)); setV(add_overflow(a,b,r)); }
}

void CPU::thumbMovCmpAddSubImm(u16 op) {
    u32 type = (op >> 11) & 0x3, rd = (op >> 8) & 0x7, imm = op & 0xFF;
    switch (type) {
        case 0: regs[rd] = imm; setNZ(regs[rd]); break;
        case 1: { u32 r = regs[rd] - imm; setNZ(r); setC(regs[rd] >= imm); setV(sub_overflow(regs[rd], imm, r)); break; }
        case 2: { u32 a = regs[rd], r = a + imm; regs[rd] = r; setNZ(r); setC(add_carry(a, imm)); setV(add_overflow(a, imm, r)); break; }
        case 3: { u32 a = regs[rd], r = a - imm; regs[rd] = r; setNZ(r); setC(a >= imm); setV(sub_overflow(a, imm, r)); break; }
    }
}

void CPU::thumbALU(u16 op) {
    u32 alu = (op >> 6) & 0xF, rs = (op >> 3) & 0x7, rd = op & 0x7;
    u32 a = regs[rd], b = regs[rs], r = 0;
    switch (alu) {
        case 0x0: r = a & b; regs[rd] = r; setNZ(r); break;
        case 0x1: r = a ^ b; regs[rd] = r; setNZ(r); break;
        case 0x2: { auto s = decodeRegisterShift(a,0,b & 0xFF, flagC()); regs[rd] = s.value; setNZ(s.value); setC(s.carry); break; }
        case 0x3: { auto s = decodeRegisterShift(a,1,b & 0xFF, flagC()); regs[rd] = s.value; setNZ(s.value); setC(s.carry); break; }
        case 0x4: { auto s = decodeRegisterShift(a,2,b & 0xFF, flagC()); regs[rd] = s.value; setNZ(s.value); setC(s.carry); break; }
        case 0x5: { u32 c = flagC() ? 1 : 0; r = a + b + c; regs[rd] = r; setNZ(r); setC(add_carry(a,b) || add_carry(a+b,c)); setV(add_overflow(a,b,a+b) || add_overflow(a+b,c,r)); break; }
        case 0x6: { u32 c = flagC() ? 1 : 0; u32 sub = b + (1 - c); r = a - sub; regs[rd] = r; setNZ(r); setC(a >= sub); setV(sub_overflow(a,sub,r)); break; }
        case 0x7: { auto s = decodeRegisterShift(a,3,b & 0xFF, flagC()); regs[rd] = s.value; setNZ(s.value); setC(s.carry); break; }
        case 0x8: r = a & b; setNZ(r); break;
        case 0x9: r = 0 - b; regs[rd] = r; setNZ(r); setC(0 >= b); setV(sub_overflow(0,b,r)); break;
        case 0xA: r = a - b; setNZ(r); setC(a >= b); setV(sub_overflow(a,b,r)); break;
        case 0xB: r = a + b; setNZ(r); setC(add_carry(a,b)); setV(add_overflow(a,b,r)); break;
        case 0xC: r = a | b; regs[rd] = r; setNZ(r); break;
        case 0xD: r = a * b; regs[rd] = r; setNZ(r); break;
        case 0xE: r = a & ~b; regs[rd] = r; setNZ(r); break;
        case 0xF: r = ~b; regs[rd] = r; setNZ(r); break;
    }
    if (alu == 0xD) bus.lastAccessCycles += mulCycleCount(b);
}

void CPU::thumbHiRegBx(u16 op) {
    u32 opcode = (op >> 8) & 0x3, h1 = bit(op,7), h2 = bit(op,6);
    u32 rs = ((op >> 3) & 0x7) | (h2 << 3);
    u32 rd = (op & 0x7) | (h1 << 3);
    switch (opcode) {
        case 0:
            regWrite(rd, regRead(rd) + regRead(rs));
            if (rd == 15) flushPipelineAfterControlFlowChange(false, true);
            break;
        case 1: { u32 a = regRead(rd), b = regRead(rs), r = a - b; setNZ(r); setC(a >= b); setV(sub_overflow(a,b,r)); break; }
        case 2:
            regWrite(rd, regRead(rs));
            if (rd == 15) flushPipelineAfterControlFlowChange(false, true);
            break;
        case 3: {
            u32 target = regRead(rs);
            setThumbMode(target & 1);
            regs[15] = thumbMode() ? align2(target) : align4(target);
            flushPipelineAfterControlFlowChange(false, true);
            break;
        }
    }
}

void CPU::thumbPcRelativeLoad(u16 op) {
    u32 rd = (op >> 8) & 0x7;
    u32 word8 = op & 0xFF;
    u32 addr = align4(pcVisibleTHUMB()) + (word8 << 2);
    regs[rd] = bus.read32(addr);
}

void CPU::thumbLoadStoreRegOffset(u16 op) {
    bool L = bit(op,11), B = bit(op,10);
    u32 ro = (op >> 6) & 0x7, rb = (op >> 3) & 0x7, rd = op & 0x7;
    u32 addr = regs[rb] + regs[ro];
    if (L) {
        if (B) regs[rd] = bus.read8(addr);
        else regs[rd] = ror32(bus.read32(addr & ~3u), (addr & 3) * 8);
    } else {
        if (B) bus.write8(addr, regs[rd] & 0xFF);
        else bus.write32(addr & ~3u, regs[rd]);
    }
}

void CPU::thumbLoadStoreSigned(u16 op) {
    u32 subop = (op >> 10) & 0x3, ro = (op >> 6) & 0x7, rb = (op >> 3) & 0x7, rd = op & 0x7;
    u32 addr = regs[rb] + regs[ro];
    switch (subop) {
        case 0: bus.write16(addr & ~1u, regs[rd] & 0xFFFF); break;
        case 1: regs[rd] = static_cast<u32>(static_cast<s32>(static_cast<int8_t>(bus.read8(addr)))); break;
        case 2: {
            u32 v = bus.read16(addr & ~1u);
            if (addr & 1) v = ror32(v, 8);
            regs[rd] = v;
            break;
        }
        case 3:
            if (addr & 1) regs[rd] = static_cast<u32>(static_cast<s32>(static_cast<int8_t>(bus.read8(addr))));
            else regs[rd] = static_cast<u32>(static_cast<s32>(static_cast<int16_t>(bus.read16(addr))));
            break;
    }
}

void CPU::thumbLoadStoreImmOffset(u16 op) {
    u32 top = (op >> 11) & 0x3, imm5 = (op >> 6) & 0x1F, rb = (op >> 3) & 0x7, rd = op & 0x7;
    bool B = (top & 0x2) != 0;
    bool L = (top & 0x1) != 0;
    u32 offset = B ? imm5 : (imm5 << 2), addr = regs[rb] + offset;
    if (L) {
        if (B) regs[rd] = bus.read8(addr);
        else regs[rd] = ror32(bus.read32(addr & ~3u), (addr & 3) * 8);
    } else {
        if (B) bus.write8(addr, regs[rd] & 0xFF);
        else bus.write32(addr & ~3u, regs[rd]);
    }
}

void CPU::thumbLoadStoreHalfword(u16 op) {
    bool L = bit(op,11);
    u32 imm5 = (op >> 6) & 0x1F, rb = (op >> 3) & 0x7, rd = op & 0x7;
    u32 addr = regs[rb] + (imm5 << 1);
    if (L) {
        u32 v = bus.read16(addr & ~1u);
        if (addr & 1) v = ror32(v, 8);
        regs[rd] = v;
    }
    else bus.write16(addr & ~1u, regs[rd] & 0xFFFF);
}

void CPU::thumbSPRelativeLoadStore(u16 op) {
    bool L = bit(op,11);
    u32 rd = (op >> 8) & 0x7, word8 = op & 0xFF;
    u32 addr = regs[13] + (word8 << 2);
    if (L) regs[rd] = bus.read32(addr);
    else bus.write32(addr, regs[rd]);
}

void CPU::thumbLoadAddress(u16 op) {
    bool sp = bit(op,11);
    u32 rd = (op >> 8) & 0x7, word8 = op & 0xFF;
    u32 base = sp ? regs[13] : align4(pcVisibleTHUMB());
    regs[rd] = base + (word8 << 2);
}

void CPU::thumbAddOffsetToSP(u16 op) {
    bool neg = bit(op,7);
    u32 imm = (op & 0x7F) << 2;
    regs[13] = neg ? (regs[13] - imm) : (regs[13] + imm);
}

void CPU::thumbPushPop(u16 op) {
    bool L = bit(op,11), R = bit(op,8);
    u32 regList = op & 0xFF;
    if (!L) {
        if (R) { regs[13] -= 4; bus.write32(regs[13], regs[14]); }
        for (int i = 7; i >= 0; --i) if (bit(regList,i)) { regs[13] -= 4; bus.write32(regs[13], regs[i]); }
    } else {
        for (int i = 0; i < 8; ++i) if (bit(regList,i)) { regs[i] = bus.read32(regs[13]); regs[13] += 4; }
        if (R) {
            u32 target = bus.read32(regs[13]);
            setThumbMode((target & 1u) != 0);
            regs[15] = thumbMode() ? align2(target) : align4(target);
            regs[13] += 4;
            flushPipelineAfterControlFlowChange(false, true);
        }
    }
}

void CPU::thumbMultipleLoadStore(u16 op) {
    bool L = bit(op,11);
    u32 rb = (op >> 8) & 0x7, regList = op & 0xFF;
    u32 addr = regs[rb];
    bool emptyRlist = regList == 0;
    if (emptyRlist) {
        if (L) {
            u32 target = bus.read32(addr);
            setThumbMode((target & 1u) != 0);
            regs[15] = thumbMode() ? align2(target) : align4(target);
            flushPipelineAfterControlFlowChange(false, true);
        }
        else bus.write32(addr, pcVisibleTHUMB());
        regs[rb] = addr + 0x40;
        return;
    }

    for (int i = 0; i < 8; ++i) {
        if (!bit(regList,i)) continue;
        if (L) regs[i] = bus.read32(addr);
        else bus.write32(addr, regs[i]);
        addr += 4;
    }
    if (!(L && bit(regList, rb))) regs[rb] = addr;
}

void CPU::thumbCondBranch(u16 op) {
    u32 cond = (op >> 8) & 0xF;
    if (cond == 0xF) return;
    if (conditionPassed(cond)) {
        regs[15] = pcVisibleTHUMB() + (sign_extend8(op & 0xFF) * 2);
        flushPipelineAfterControlFlowChange(false, true);
    }
}

void CPU::thumbUncondBranch(u16 op) {
    regs[15] = pcVisibleTHUMB() + (sign_extend11(op & 0x7FF) * 2);
    flushPipelineAfterControlFlowChange(false, true);
}

void CPU::thumbLongBranchLink(u16 op) {
    bool h = bit(op,11);
    u32 off11 = op & 0x7FF;
    if (!h) {
        // Avoid UB from left-shifting negative signed values.
        s32 offset = sign_extend11(off11) * 4096;
        regs[14] = pcVisibleTHUMB() + offset;
        thumbBLPrefixValid = true;
    } else {
        if (!thumbBLPrefixValid) return;
        u32 next = pcVisibleTHUMB();
        u32 target = regs[14] + (off11 << 1);
        regs[14] = (next - 2) | 1;
        regs[15] = target;
        thumbBLPrefixValid = false;
        flushPipelineAfterControlFlowChange(false, true);
    }
}

void CPU::thumbSoftwareInterrupt(u16 op) {
    u32 swiNumber = op & 0xFF;
    if (!bus.bios.loaded() && SWI::handle(bus, *this, swiNumber)) return;
    enterSWI();
}

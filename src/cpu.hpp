#pragma once
#include <array>
#include <iosfwd>
#include "types.hpp"
#include "bus.hpp"
#include "util.hpp"

class SWI;

class CPU {
public:
    explicit CPU(Bus& bus);

    void reset(bool skipBIOS = true);
    void step();
    bool saveState(std::ostream& os) const;
    bool loadState(std::istream& is);

    u32 pc() const { return regs[15]; }
    u32 cpsrValue() const { return cpsr; }
    u32 rawReg(int idx) const { return regs[idx]; }
    void setRawReg(int idx, u32 v) { regs[idx] = v; }

private:
    friend class SWI;

    enum Mode : u32 {
        USER = 0x10,
        FIQ  = 0x11,
        IRQ  = 0x12,
        SVC  = 0x13,
        ABT  = 0x17,
        UND  = 0x1B,
        SYS  = 0x1F
    };

    struct Banked {
        u32 r8_r12_usr[5]{};
        u32 r8_r12_fiq[5]{};
        u32 r13_usr = 0, r14_usr = 0;
        u32 r13_fiq = 0, r14_fiq = 0;
        u32 r13_svc = 0, r14_svc = 0;
        u32 r13_abt = 0, r14_abt = 0;
        u32 r13_irq = 0, r14_irq = 0;
        u32 r13_und = 0, r14_und = 0;

        u32 spsr_fiq = 0;
        u32 spsr_svc = 0;
        u32 spsr_abt = 0;
        u32 spsr_irq = 0;
        u32 spsr_und = 0;
    };

    Bus& bus;
    std::array<u32, 16> regs{};
    Banked bank{};
    u32 cpsr = 0;

    u16 thumbBLHigh = 0;
    bool thumbBLPrefixValid = false;
    bool lastFetchValid = false;
    bool lastFetchThumb = false;
    u32 lastFetchAddr = 0;

    u32 fetchARM();
    u16 fetchTHUMB();

    void execARM(u32 op);
    void execTHUMB(u16 op);

    bool conditionPassed(u32 cond) const;

    bool flagN() const { return bit(cpsr, 31); }
    bool flagZ() const { return bit(cpsr, 30); }
    bool flagC() const { return bit(cpsr, 29); }
    bool flagV() const { return bit(cpsr, 28); }

    void setN(bool v) { cpsr = set_bit(cpsr, 31, v); }
    void setZ(bool v) { cpsr = set_bit(cpsr, 30, v); }
    void setC(bool v) { cpsr = set_bit(cpsr, 29, v); }
    void setV(bool v) { cpsr = set_bit(cpsr, 28, v); }

    bool irqDisabled() const { return bit(cpsr, 7); }
    bool thumbMode() const { return bit(cpsr, 5); }
    void setThumbMode(bool v) { cpsr = set_bit(cpsr, 5, v); }
    void setIRQDisabled(bool v) { cpsr = set_bit(cpsr, 7, v); }
    void setFIQDisabled(bool v) { cpsr = set_bit(cpsr, 6, v); }

    Mode currentMode() const { return static_cast<Mode>(cpsr & 0x1F); }
    void switchMode(Mode newMode);
    u32* currentSPSR();
    void restoreCPSRFromSPSRIfPossible();

    void checkInterrupts();
    void enterIRQ();
    void enterSWI();
    void enterUndefined();
    void flushPipelineAfterControlFlowChange(bool clearThumbBLPrefix, bool addBranchPenalty);

    u32 regRead(int idx) const;
    void regWrite(int idx, u32 value);
    u32 pcVisibleARM() const;
    u32 pcVisibleTHUMB() const;
    u32 readUserReg(int idx) const;
    void writeUserReg(int idx, u32 value);

    struct ShiftResult {
        u32 value = 0;
        bool carry = false;
    };

    ShiftResult decodeImmediateShift(u32 value, u32 type, u32 amount) const;
    ShiftResult decodeRegisterShift(u32 value, u32 type, u32 amount, bool oldCarry) const;
    ShiftResult decodeARMOperand2(u32 op, bool& carryOut) const;

    void setNZ(u32 v);

    void armBranchExchange(u32 op);
    void armBranch(u32 op);
    void armDataProcessing(u32 op);
    void armPSRTransfer(u32 op);
    void armSwap(u32 op);
    void armMultiply(u32 op);
    void armMultiplyLong(u32 op);
    void armSingleDataTransfer(u32 op);
    void armHalfwordTransfer(u32 op);
    void armBlockTransfer(u32 op);
    void armSoftwareInterrupt(u32 op);

    void thumbMoveShifted(u16 op);
    void thumbAddSub(u16 op);
    void thumbMovCmpAddSubImm(u16 op);
    void thumbALU(u16 op);
    void thumbHiRegBx(u16 op);
    void thumbPcRelativeLoad(u16 op);
    void thumbLoadStoreRegOffset(u16 op);
    void thumbLoadStoreSigned(u16 op);
    void thumbLoadStoreImmOffset(u16 op);
    void thumbLoadStoreHalfword(u16 op);
    void thumbSPRelativeLoadStore(u16 op);
    void thumbLoadAddress(u16 op);
    void thumbAddOffsetToSP(u16 op);
    void thumbPushPop(u16 op);
    void thumbMultipleLoadStore(u16 op);
    void thumbCondBranch(u16 op);
    void thumbUncondBranch(u16 op);
    void thumbLongBranchLink(u16 op);
    void thumbSoftwareInterrupt(u16 op);
};

#pragma once
#include <array>
#include "types.hpp"
#include "interrupts.hpp"

class Bus;

struct DMAChannel {
    u32 src = 0;
    u32 dst = 0;
    u16 count = 0;
    u16 control = 0;

    u32 internalSrc = 0;
    u32 internalDst = 0;
    u32 internalCount = 0;
    int startupDelay = 0;

    bool active = false;
    bool latched = false;
};

class DMAController {
public:
    explicit DMAController(InterruptController& irq);

    void reset();
    void tick(Bus& bus, int cycles);

    void triggerVBlank();
    void triggerHBlank();
    void triggerImmediate(Bus& bus);
    void triggerSoundFIFO(Bus& bus, bool fifoB);
    void writeControl(Bus& bus, int channel, u16 value);
    bool cpuBusLocked() const;

    std::array<DMAChannel, 4> ch{};

private:
    InterruptController& irqc;

    void startChannel(int i);
    int runChannel(Bus& bus, int i, int budgetCycles = -1, int maxTransfers = -1);
    bool timingMatch(int i, int timingMode) const;
    static u32 applyAddrMask(int channel, u32 addr, bool isDst);
};

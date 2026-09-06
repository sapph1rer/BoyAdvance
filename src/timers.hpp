#pragma once
#include <array>
#include "types.hpp"
#include "interrupts.hpp"

struct TimerUnit {
    u16 reload = 0;
    u16 counter = 0;
    u16 control = 0;
    int prescalerCounter = 0;
};

class Timers {
public:
    explicit Timers(InterruptController& irq);

    void reset();
    void tick(int cycles);

    std::array<TimerUnit, 4> t{};
    std::array<int, 4> overflowCount{};

private:
    InterruptController& irqc;

    int prescalerValue(u16 control) const;
    void overflow(int idx);
};

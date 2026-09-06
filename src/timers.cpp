#include "timers.hpp"

Timers::Timers(InterruptController& irq) : irqc(irq) {
    reset();
}

void Timers::reset() {
    for (auto& x : t) {
        x.reload = 0;
        x.counter = 0;
        x.control = 0;
        x.prescalerCounter = 0;
    }
    overflowCount.fill(0);
}

int Timers::prescalerValue(u16 control) const {
    switch (control & 0b11) {
        case 0: return 1;
        case 1: return 64;
        case 2: return 256;
        case 3: return 1024;
    }
    return 1;
}

void Timers::overflow(int idx) {
    t[idx].counter = t[idx].reload;
    overflowCount[idx]++;
    if (t[idx].control & (1 << 6)) {
        irqc.request(static_cast<u16>(IRQ_TIMER0 << idx));
    }
}

void Timers::tick(int cycles) {
    overflowCount.fill(0);

    auto pulseCountUpChain = [&](int idx, int pulses, const auto& self) -> void {
        if (idx < 0 || idx >= 4 || pulses <= 0) return;
        auto& tm = t[idx];
        bool enable = (tm.control & (1 << 7)) != 0;
        bool countUp = (tm.control & (1 << 2)) != 0;
        if (!enable || !countUp) return;
        int cascaded = 0;
        for (int p = 0; p < pulses; ++p) {
            tm.counter++;
            if (tm.counter == 0) {
                overflow(idx);
                ++cascaded;
            }
        }
        if (cascaded > 0 && idx < 3) self(idx + 1, cascaded, self);
    };

    for (int i = 0; i < 4; ++i) {
        auto& tm = t[i];
        bool enable = tm.control & (1 << 7);
        bool countUp = tm.control & (1 << 2);
        if (!enable || countUp) continue;

        int div = prescalerValue(tm.control);
        tm.prescalerCounter += cycles;

        while (tm.prescalerCounter >= div) {
            tm.prescalerCounter -= div;
            tm.counter++;
            if (tm.counter == 0) {
                overflow(i);
                if (i < 3) pulseCountUpChain(i + 1, 1, pulseCountUpChain);
            }
        }
    }
}

#pragma once
#include "types.hpp"

class Keypad {
public:
    enum Key {
        A      = 0,
        B      = 1,
        SELECT = 2,
        START  = 3,
        RIGHT  = 4,
        LEFT   = 5,
        UP     = 6,
        DOWN   = 7,
        R      = 8,
        L      = 9
    };

    void set(Key k, bool pressed);
    bool isPressed(Key k) const;
    void clear();
    u16 keyinput() const { return reg_keyinput; }

    u16 keycnt = 0;

private:
    u16 reg_keyinput = 0x03FF;
};

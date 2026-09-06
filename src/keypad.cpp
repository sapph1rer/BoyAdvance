#include "keypad.hpp"

void Keypad::set(Key k, bool pressed) {
    if (pressed) reg_keyinput &= ~(1u << k);
    else         reg_keyinput |=  (1u << k);
}

bool Keypad::isPressed(Key k) const {
    return ((reg_keyinput >> k) & 1u) == 0;
}

void Keypad::clear() {
    reg_keyinput = 0x03FF;
}

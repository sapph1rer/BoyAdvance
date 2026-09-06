#include <iostream>
#include "bus.hpp"

int main() {
    Bus bus;
    bus.resetRuntimeState();
    bus.backup.init(BackupDevice::Type::FLASH128K);

    bus.write16(0x0E005555, 0x00AA);
    bus.write16(0x0E002AAA, 0x0055);
    bus.write16(0x0E005555, 0x00A0);
    bus.write16(0x0E000123, 0x003C);

    unsigned v = bus.read8(0x0E000123);
    std::cout << "flash_byte=" << v << "\n";
    return (v == 0x3C) ? 0 : 2;
}

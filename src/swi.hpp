#pragma once
#include "types.hpp"

class CPU;
class Bus;

class SWI {
public:
    static bool handle(Bus& bus, CPU& cpu, u32 number);
};

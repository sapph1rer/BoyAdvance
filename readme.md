# BoyAdvance

An emulator for Game Boy Advance written in C++17, focused on accuracy, modular architecture, and developer tools.

---

## Key Features

* **CPU Core**: Complete ARM7TDMI processor implementation supporting both 32-bit ARM and 16-bit THUMB instruction sets[cite: 2].
* **Graphics (PPU)**: Full support for GBA display modes (Modes 0–5), including Tilemap backgrounds, Affine (Mode 7 style) background transformations, Sprites (OBJ) with transformation matrices, Windows, and Alpha Blending effects[cite: 2].
* **Audio (APU)**: DirectSound FIFO channels (A/B) combined with legacy Game Boy sound channels (Square 1 & 2, Wave, Noise)[cite: 2].
* **System Interconnect**: Accurate memory mapping, 4-channel DMA engine, 4-channel hardware timer system with cascading support, and interrupt management (IE/IF/IME)[cite: 2].
* **Automated Save Detection**: Built-in cartridge analyzer to auto-detect and emulate SRAM, EEPROM, and FLASH (64K / 128K) backup types without manual setup[cite: 2].
* **HLE & Embedded BIOS**: Supports official GBA BIOS images (`bios.bin`) as well as an integrated High-Level Emulation (HLE) fallback mechanism for instant boot[cite: 2].
* **Dual Frontends**: 
  * **SDL2**: Minimalist, low-latency rendering and audio output interface[cite: 2].
  * **Qt GUI**: Feature-rich user interface for ROM management and debugging[cite: 2].
* **State I/O & Debugging**: Save State serialization/deserialization support and execution trace logging engines[cite: 2].

---

## Project Architecture

```text
BoyAdvance/
├── src/
│   ├── cpu.cpp / cpu.hpp            # ARM7TDMI CPU interpreter core
│   ├── ppu.cpp / ppu.hpp            # Picture Processing Unit & Scanline Renderer
│   ├── apu.cpp / apu.hpp            # Audio Processing Unit & DirectSound
│   ├── bus.cpp / bus.hpp            # Memory Bus router & Memory Mapping
│   ├── dma.cpp / dma.hpp            # Direct Memory Access controller
│   ├── timers.cpp / timers.hpp      # Hardware Timers (0-3)
│   ├── interrupts.cpp / .hpp        # Interrupt Request Manager
│   ├── cartridge.cpp / .hpp         # ROM Cartridge loader & Header parser
│   ├── save_detect.cpp / .hpp       # Save type auto-analyzer
│   ├── backup.cpp / backup.hpp      # Flash / SRAM / EEPROM emulation
│   ├── bios.cpp / bios_embedded.hpp # Official BIOS loader & HLE implementation
│   ├── swi.cpp / swi.hpp            # Software Interrupt handlers
│   ├── state_io.hpp                 # State saving & loading serializer
│   ├── debug_trace.hpp              # Execution trace logging engine
│   ├── frontend_sdl.cpp / .hpp      # SDL2 window, audio, & input wrapper
│   └── main_qt.cpp                 # Qt GUI frontend application
├── build_qt_gui.ps1                 # Qt build automation script
└── package_release.ps1             # Release packaging utility
```[cite: 2]

---

## Building & Installation

### Prerequisites
* **Compiler**: GCC 10+, Clang 11+, or MSVC 2019+ (C++17 standard required)
* **Build System**: CMake 3.16+
* **Dependencies**: 
  * `SDL2` (Required for SDL frontend)
  * `Qt 5` or `Qt 6` (Optional for Qt GUI frontend)

### 1. Clone the Repository
```bash
git clone [https://github.com/your-username/BoyAdvance.git](https://github.com/your-username/BoyAdvance.git)
cd BoyAdvance
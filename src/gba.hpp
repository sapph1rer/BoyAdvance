#pragma once
#include <string>
#include "bus.hpp"
#include "cpu.hpp"
#include "save_detect.hpp"

class GBA {
public:
    GBA();

    bool loadBIOS(const std::string& path);
    bool loadROM(const std::string& path);

    bool configureBackupFromROM();
    bool loadSaveForROM(const std::string& romPath);
    bool saveSaveForROM(const std::string& romPath, bool force = false);
    bool saveStateFile(const std::string& path) const;
    bool loadStateFile(const std::string& path);
    std::string makeSavePath(const std::string& romPath) const;
    std::string makeStatePath(const std::string& romPath, int slot = 1) const;

    // Directories used for newly-written .sav / .gstate* files, kept
    // separate from wherever the ROM itself lives. Call before loading a
    // ROM; each is created on demand. When unset, .sav/.gstate files fall
    // back to sitting next to the ROM (the old behavior).
    void setSaveDirectory(const std::string& dir);
    void setStateDirectory(const std::string& dir);

    void reset(bool skipBIOS = true);
    void runCycles(int cycles);
    void runFrame();

    CPU cpu;
    Bus bus;

private:
    std::string saveDirectory;
    std::string stateDirectory;
};

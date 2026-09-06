#include "save_detect.hpp"
#include <array>
#include <string>

static bool containsTag(const std::vector<u8>& rom, const std::string& tag) {
    if (tag.empty() || rom.size() < tag.size()) return false;
    for (size_t i = 0; i + tag.size() <= rom.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < tag.size(); ++j) {
            if (rom[i + j] != static_cast<u8>(tag[j])) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

SaveDetectResult detectSaveType(const std::vector<u8>& romData) {
    if (containsTag(romData, "FLASH1M_V") || containsTag(romData, "FLASH1M_")) {
        return { BackupDevice::Type::FLASH128K, "FLASH1M" };
    }
    if (containsTag(romData, "FLASH512_V") || containsTag(romData, "FLASH_V") || containsTag(romData, "FLASH_")) {
        return { BackupDevice::Type::FLASH64K, "FLASH" };
    }
    if (containsTag(romData, "SRAM_V") || containsTag(romData, "SRAM_F_V") || containsTag(romData, "SRAM_")) {
        return { BackupDevice::Type::SRAM, "SRAM" };
    }
    if (containsTag(romData, "EEPROM_V") || containsTag(romData, "EEPROM_")) {
        // Default to 512B and let EEPROM serial protocol auto-upsize to 8KB
        // when 10-bit address commands are observed.
        return { BackupDevice::Type::EEPROM512, "EEPROM" };
    }

    // Header fallback for common titles with patched/stripped save strings.
    if (romData.size() >= 0xB0) {
        std::string code;
        code.reserve(4);
        for (size_t i = 0; i < 4; ++i) code.push_back(static_cast<char>(romData[0xAC + i]));
        static const std::array<const char*, 16> pokemonFlash1M = {
            "BPRE", "BPRJ", "BPRS", "BPRF",
            "BPGE", "BPGJ", "BPGS", "BPGF",
            "AXVE", "AXVJ", "AXVS", "AXVF",
            "AXPE", "AXPJ", "AXPS", "AXPF"
        };
        for (const char* c : pokemonFlash1M) {
            if (code == c) return { BackupDevice::Type::FLASH128K, "HEADER_FLASH1M" };
        }
    }

    return {};
}

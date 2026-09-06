#include "gba.hpp"
#include <filesystem>
#include <fstream>

static u64 fnv1a64(const std::vector<u8>& data) {
    u64 h = 1469598103934665603ull;
    for (u8 b : data) {
        h ^= static_cast<u64>(b);
        h *= 1099511628211ull;
    }
    return h;
}

GBA::GBA() : cpu(bus) {}

bool GBA::loadBIOS(const std::string& path) { return bus.loadBIOS(path); }
bool GBA::loadROM(const std::string& path) { return bus.loadROM(path); }

bool GBA::configureBackupFromROM() {
    SaveDetectResult r = detectSaveType(bus.cart.data());
    return bus.backup.init(r.type);
}

void GBA::setSaveDirectory(const std::string& dir) { saveDirectory = dir; }
void GBA::setStateDirectory(const std::string& dir) { stateDirectory = dir; }

// Battery saves (.sav) and save states (.gstate*) are kept in their own
// dedicated folders rather than next to the ROM file. This keeps ROM
// directories clean (handy when ROMs live in a read-only or synced folder)
// and keeps all of an emulator instance's save data in one predictable
// place. Old saves that already exist next to the ROM are still picked up
// automatically when loading, so upgrading doesn't orphan existing saves.
std::string GBA::makeSavePath(const std::string& romPath) const {
    std::filesystem::path rom(romPath);
    std::filesystem::path name = rom.filename();
    name.replace_extension(".sav");
    if (!saveDirectory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(saveDirectory, ec);
        return (std::filesystem::path(saveDirectory) / name).string();
    }
    std::filesystem::path p = rom;
    p.replace_extension(".sav");
    return p.string();
}

std::string GBA::makeStatePath(const std::string& romPath, int slot) const {
    std::filesystem::path rom(romPath);
    std::filesystem::path name = rom.stem();
    std::string ext = ".gstate" + std::to_string(slot);
    name += ext;
    if (!stateDirectory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(stateDirectory, ec);
        return (std::filesystem::path(stateDirectory) / name).string();
    }
    std::filesystem::path p = rom;
    p.replace_extension(ext);
    return p.string();
}

static std::string makeLegacySavePath(const std::string& romPath) {
    // Where .sav files used to be written: right next to the ROM. Kept as a
    // read fallback only, so pre-existing saves are still found.
    std::filesystem::path p(romPath);
    p.replace_extension(".sav");
    return p.string();
}

static std::string makeFallbackSavePath(const std::string& romPath) {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::current_path(ec);
    if (ec) base = std::filesystem::path(".");
    std::filesystem::path saves = base / "saves";
    std::filesystem::path rom(romPath);
    std::filesystem::path out = saves / rom.filename();
    out.replace_extension(".sav");
    return out.string();
}

bool GBA::loadSaveForROM(const std::string& romPath) {
    if (bus.loadSaveFile(makeSavePath(romPath))) return true;
    if (bus.loadSaveFile(makeLegacySavePath(romPath))) return true;
    return bus.loadSaveFile(makeFallbackSavePath(romPath));
}

bool GBA::saveSaveForROM(const std::string& romPath, bool force) {
    if (bus.backup.saveToFile(makeSavePath(romPath), force)) return true;
    return bus.backup.saveToFile(makeFallbackSavePath(romPath), force);
}

bool GBA::saveStateFile(const std::string& path) const {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) return false;
    const char magic[4] = { 'G', 'B', 'A', 'S' };
    const u32 version = 8;
    const u32 romSize = static_cast<u32>(bus.cart.data().size());
    const u64 romHash = fnv1a64(bus.cart.data());
    os.write(magic, 4);
    os.write(reinterpret_cast<const char*>(&version), sizeof(version));
    os.write(reinterpret_cast<const char*>(&romSize), sizeof(romSize));
    os.write(reinterpret_cast<const char*>(&romHash), sizeof(romHash));
    if (!cpu.saveState(os)) return false;
    if (!bus.saveState(os)) return false;
    return static_cast<bool>(os);
}

bool GBA::loadStateFile(const std::string& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return false;
    char magic[4]{};
    u32 version = 0;
    u32 romSize = 0;
    u64 romHash = 0;
    is.read(magic, 4);
    is.read(reinterpret_cast<char*>(&version), sizeof(version));
    is.read(reinterpret_cast<char*>(&romSize), sizeof(romSize));
    is.read(reinterpret_cast<char*>(&romHash), sizeof(romHash));
    if (!is) return false;
    if (!(magic[0] == 'G' && magic[1] == 'B' && magic[2] == 'A' && magic[3] == 'S')) return false;
    if (version != 8) return false;
    if (romSize != static_cast<u32>(bus.cart.data().size())) return false;
    if (romHash != fnv1a64(bus.cart.data())) return false;
    if (!cpu.loadState(is)) return false;
    if (!bus.loadState(is)) return false;
    return true;
}

void GBA::reset(bool skipBIOS) {
    bus.resetRuntimeState();
    cpu.reset(skipBIOS);
}

void GBA::runCycles(int cycles) {
    int elapsed = 0;
    while (elapsed < cycles) {
        if (bus.haltMode) {
            int remain = cycles - elapsed;
            int slice = remain > 512 ? 512 : remain;
            if (slice < 1) slice = 1;
            bus.tick(slice);
            bus.lastAccessCycles = slice;
            elapsed += slice;
            if (bus.irq.wakePending()) bus.haltMode = false;
            continue;
        }
        if (bus.dma.cpuBusLocked()) {
            int remain = cycles - elapsed;
            int slice = remain > 8 ? 8 : remain;
            int toPpuEvent = bus.ppu.cyclesUntilNextModeEvent();
            if (toPpuEvent > 0 && toPpuEvent < slice) slice = toPpuEvent;
            if (slice < 1) slice = 1;
            bus.tick(slice);
            // CPU regains the bus after DMA slices as a non-sequential master access.
            bus.invalidateSequentialTiming();
            bus.lastAccessCycles = slice;
            elapsed += slice;
            continue;
        }
        cpu.step();
        int stepCycles = bus.lastAccessCycles;
        if (stepCycles < 1) stepCycles = 1;
        elapsed += stepCycles;
    }
}

void GBA::runFrame() {
    const u64 startFrame = bus.ppu.frameCount();
    int guard = 0;
    while (bus.ppu.frameCount() == startFrame && guard < 4000) {
        runCycles(128);
        ++guard;
    }
}

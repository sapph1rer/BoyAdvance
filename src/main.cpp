#include <iostream>
#include <string>
#include <filesystem>
#include <array>
#include <algorithm>
#include <cctype>
#include <vector>
#include <SDL.h>

#include "debug_trace.hpp"
#include "emu_log.hpp"
#include "gba.hpp"
#include "frontend_sdl.hpp"

namespace fs = std::filesystem;

static void printUsage(const char* exe, std::ostream& out = std::cout) {
    out
        << "GBA Emulator (work in progress)\n\n"
        << "Usage:\n"
        << "  " << exe << " -h | --help | help\n"
        << "  " << exe << " demo [--scale N] [--debug]\n"
        << "  " << exe << " load <rom.gba> [bios.bin] [--run-bios|--skip-bios] [--scale N] [--debug]\n"
        << "  " << exe << " <rom.gba> [bios.bin] [--run-bios|--skip-bios] [--scale N] [--debug]\n\n"
        << "Controls:\n"
        << "  D-Pad: Arrow keys, A: X, B: Z, L: A, R: S\n"
        << "  Start: Enter, Select: Backspace, Quit: Esc or close window\n\n"
        << "Notes:\n"
        << "  - This emulator core is still incomplete; many commercial games may not boot yet.\n"
        << "  - Demo mode renders an internal animated test pattern to verify video/input path.\n";
}

static bool isHelpToken(const std::string& token) {
    return token == "-h" || token == "--help" || token == "help";
}

static bool hasGbaExtension(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".gba";
}

static bool parseScaleValue(const std::string& s, int& outScale) {
    try {
        size_t pos = 0;
        int value = std::stoi(s, &pos, 10);
        if (pos != s.size() || value < 1 || value > 8) return false;
        outScale = value;
        return true;
    } catch (...) {
        return false;
    }
}

static int wrap5bit(int v) {
    int w = v % 32;
    return w < 0 ? w + 32 : w;
}

static u16 rgb5(int r, int g, int b) {
    r = std::clamp(r, 0, 31);
    g = std::clamp(g, 0, 31);
    b = std::clamp(b, 0, 31);
    return static_cast<u16>(r | (g << 5) | (b << 10));
}

static void renderSoftwareDemoFrame(
    std::array<u16, PPU::SCREEN_W * PPU::SCREEN_H>& frame,
    u32 tick,
    int camX,
    int camY
) {
    for (int y = 0; y < PPU::SCREEN_H; ++y) {
        for (int x = 0; x < PPU::SCREEN_W; ++x) {
            int wx = x + camX;
            int wy = y + camY;
            int r = wrap5bit((wx / 4) + static_cast<int>(tick / 2));
            int g = wrap5bit((wy / 3) + static_cast<int>(tick / 3));
            int b = wrap5bit(((wx ^ wy) / 5) + static_cast<int>(tick / 4));
            if ((((wx / 16) + (wy / 16)) & 1) == 0) {
                r = std::min(31, r + 4);
                b = std::min(31, b + 4);
            }
            frame[y * PPU::SCREEN_W + x] = rgb5(r, g, b);
        }
    }

    int cx = (static_cast<int>(tick) * 2 + camX) % PPU::SCREEN_W;
    if (cx < 0) cx += PPU::SCREEN_W;
    int cy = (static_cast<int>(tick) + camY) % PPU::SCREEN_H;
    if (cy < 0) cy += PPU::SCREEN_H;

    for (int dx = -20; dx <= 20; ++dx) {
        int x = cx + dx;
        if (x >= 0 && x < PPU::SCREEN_W) frame[cy * PPU::SCREEN_W + x] = rgb5(31, 31, 31);
    }
    for (int dy = -20; dy <= 20; ++dy) {
        int y = cy + dy;
        if (y >= 0 && y < PPU::SCREEN_H) frame[y * PPU::SCREEN_W + cx] = rgb5(31, 31, 31);
    }
}

static bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

static std::string findDefaultBiosPath(const char* argv0) {
    static const std::array<const char*, 2> biosNames = { "bios.bin", "gba_bios.bin" };
    std::vector<fs::path> roots;

    std::error_code ec;
    roots.push_back(fs::current_path(ec));

    if (argv0 && *argv0) {
        fs::path exePath = fs::absolute(fs::path(argv0), ec);
        if (!ec && exePath.has_parent_path()) {
            roots.push_back(exePath.parent_path());
        }
    }

    std::vector<fs::path> searchRoots;
    for (const fs::path& r : roots) {
        searchRoots.push_back(r);
        searchRoots.push_back(r / "bios");
    }

    for (const fs::path& root : searchRoots) {
        for (const char* name : biosNames) {
            fs::path candidate = root / name;
            if (fileExists(candidate.string())) return candidate.string();
        }
    }
    return {};
}

struct CliOptions {
    enum class Mode { Help, Load, Demo, Invalid };
    enum class BiosBoot { Auto, Run, Skip };

    Mode mode = Mode::Invalid;
    BiosBoot biosBoot = BiosBoot::Auto;
    std::string romPath;
    std::string biosPath;
    int scale = 3;
    bool debug = false;
    std::string error;
};

static bool parseCommonOption(const std::string& arg, int& i, int argc, char** argv, CliOptions& opts) {
    if (arg == "--scale") {
        if (i + 1 >= argc) {
            opts.error = "Missing value after --scale";
            return false;
        }
        int parsedScale = 0;
        if (!parseScaleValue(argv[++i], parsedScale)) {
            opts.error = "Invalid scale. Use --scale 1..8";
            return false;
        }
        opts.scale = parsedScale;
        return true;
    }
    const std::string scalePrefix = "--scale=";
    if (arg.rfind(scalePrefix, 0) == 0) {
        int parsedScale = 0;
        if (!parseScaleValue(arg.substr(scalePrefix.size()), parsedScale)) {
            opts.error = "Invalid scale. Use --scale 1..8";
            return false;
        }
        opts.scale = parsedScale;
        return true;
    }
    if (arg == "--run-bios") {
        if (opts.biosBoot == CliOptions::BiosBoot::Skip) {
            opts.error = "Cannot use --run-bios together with --skip-bios";
            return false;
        }
        opts.biosBoot = CliOptions::BiosBoot::Run;
        return true;
    }
    if (arg == "--skip-bios") {
        if (opts.biosBoot == CliOptions::BiosBoot::Run) {
            opts.error = "Cannot use --skip-bios together with --run-bios";
            return false;
        }
        opts.biosBoot = CliOptions::BiosBoot::Skip;
        return true;
    }
    if (arg == "--debug") {
        opts.debug = true;
        return true;
    }
    if (isHelpToken(arg)) {
        opts.mode = CliOptions::Mode::Help;
        return true;
    }
    return false;
}

static CliOptions parseArgs(int argc, char** argv) {
    CliOptions opts;
    if (argc <= 1) {
        opts.mode = CliOptions::Mode::Help;
        return opts;
    }

    std::string first = argv[1];
    if (isHelpToken(first)) {
        opts.mode = CliOptions::Mode::Help;
        return opts;
    }

    int i = 1;
    if (first == "demo") {
        opts.mode = CliOptions::Mode::Demo;
        ++i;
        while (i < argc) {
            std::string arg = argv[i];
            if (!parseCommonOption(arg, i, argc, argv, opts)) {
                opts.mode = CliOptions::Mode::Invalid;
                if (opts.error.empty()) opts.error = "Unknown option for demo: " + arg;
                return opts;
            }
            ++i;
        }
        return opts;
    }

    if (first == "load") {
        if (argc >= 3 && isHelpToken(argv[2])) {
            opts.mode = CliOptions::Mode::Help;
            return opts;
        }
        opts.mode = CliOptions::Mode::Load;
        if (argc < 3) {
            opts.mode = CliOptions::Mode::Invalid;
            opts.error = "Missing ROM path after 'load'";
            return opts;
        }
        opts.romPath = argv[2];
        i = 3;
    } else if (hasGbaExtension(first)) {
        opts.mode = CliOptions::Mode::Load;
        opts.romPath = first;
        i = 2;
    } else {
        opts.mode = CliOptions::Mode::Invalid;
        opts.error = "Unknown command: " + first;
        return opts;
    }

    while (i < argc) {
        std::string arg = argv[i];
        if (parseCommonOption(arg, i, argc, argv, opts)) {
            ++i;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            opts.mode = CliOptions::Mode::Invalid;
            if (opts.error.empty()) opts.error = "Unknown option: " + arg;
            return opts;
        }
        if (opts.biosPath.empty()) {
            opts.biosPath = arg;
            ++i;
            continue;
        }
        opts.mode = CliOptions::Mode::Invalid;
        opts.error = "Too many positional arguments";
        return opts;
    }

    return opts;
}

int main(int argc, char** argv) {
    CliOptions opts = parseArgs(argc, argv);
    if (opts.mode == CliOptions::Mode::Help) {
        printUsage(argv[0]);
        return 0;
    }
    if (opts.mode == CliOptions::Mode::Invalid) {
        if (!opts.error.empty()) std::cerr << "Error: " << opts.error << "\n\n";
        printUsage(argv[0], std::cerr);
        return 1;
    }

    constexpr double targetFrameSec = 1.0 / 60.0;
    if (opts.debug) {
        emudebug::Settings::instance().setEnabled(true);
        emudebug::Settings::instance().setDomains(
            emudebug::CPU | emudebug::GPU | emudebug::DMA | emudebug::RAM | emudebug::BIOS | emudebug::ROM);
        emudebug::Settings::instance().setFrameInterval(30);
        emulog::Logger::instance().log(emulog::Level::Info, "DEBUG", "CLI debug mode enabled");
    }
    size_t printedLogCount = 0;
    auto flushLogs = [&]() {
        if (!opts.debug) return;
        auto entries = emulog::Logger::instance().snapshot();
        if (printedLogCount > entries.size()) printedLogCount = 0;
        for (size_t i = printedLogCount; i < entries.size(); ++i) {
            std::cout << "[" << emulog::levelName(entries[i].level) << "]"
                      << "[" << entries[i].tag << "] "
                      << entries[i].message << "\n";
        }
        printedLogCount = entries.size();
    };

    if (opts.mode == CliOptions::Mode::Demo) {
        FrontendSDL frontend;
        if (!frontend.init(opts.scale)) return 1;
        std::cout << "Running demo mode (software pattern). Use arrow keys to pan.\n";
        std::array<u16, PPU::SCREEN_W * PPU::SCREEN_H> demoFrame{};
        int camX = 0;
        int camY = 0;
        u32 tick = 0;
        Keypad demoKeypad;
        while (frontend.processEvents(demoKeypad)) {
            u64 frameStart = SDL_GetPerformanceCounter();
            if (demoKeypad.isPressed(Keypad::LEFT))  camX -= 2;
            if (demoKeypad.isPressed(Keypad::RIGHT)) camX += 2;
            if (demoKeypad.isPressed(Keypad::UP))    camY -= 2;
            if (demoKeypad.isPressed(Keypad::DOWN))  camY += 2;
            renderSoftwareDemoFrame(demoFrame, tick++, camX, camY);
            frontend.present(demoFrame);
            flushLogs();
            u64 frameEnd = SDL_GetPerformanceCounter();
            double frameSec = static_cast<double>(frameEnd - frameStart) / static_cast<double>(SDL_GetPerformanceFrequency());
            if (frameSec < targetFrameSec) {
                u32 ms = static_cast<u32>((targetFrameSec - frameSec) * 1000.0);
                if (ms > 0) SDL_Delay(ms);
            }
        }
        return 0;
    }

    GBA gba;
    if (!fileExists(opts.romPath)) {
        std::cerr << "ROM file not found: " << opts.romPath << "\n";
        return 1;
    }

    // Keep .sav / .gstate* files in their own folders next to the
    // executable rather than beside the ROM.
    {
        std::error_code ec;
        fs::path exeDir = fs::current_path(ec);
        if (*argv[0]) {
            fs::path exePath = fs::absolute(fs::path(argv[0]), ec);
            if (!ec && exePath.has_parent_path()) exeDir = exePath.parent_path();
        }
        gba.setSaveDirectory((exeDir / "saves").string());
        gba.setStateDirectory((exeDir / "states").string());
    }

    std::string biosPath = opts.biosPath;
    if (biosPath.empty()) biosPath = findDefaultBiosPath(argv[0]);

    if (!biosPath.empty()) {
        if (!fileExists(biosPath)) {
            std::cerr << "BIOS file not found: " << biosPath << "\n";
            return 1;
        }
        if (!gba.loadBIOS(biosPath)) {
            std::cerr << "Failed to load BIOS: " << biosPath << "\n";
            return 1;
        }
    }
    if (!gba.loadROM(opts.romPath)) {
        std::cerr << "Failed to load ROM: " << opts.romPath << "\n";
        return 1;
    }
    if (!gba.configureBackupFromROM()) {
        std::cerr << "Failed to configure backup device.\n";
        return 1;
    }

    if (opts.biosBoot == CliOptions::BiosBoot::Run && biosPath.empty()) {
        std::cerr << "Error: --run-bios requires a BIOS file path.\n";
        return 1;
    }
    bool skipBIOS = true;
    if (opts.biosBoot == CliOptions::BiosBoot::Run) skipBIOS = false;
    else if (opts.biosBoot == CliOptions::BiosBoot::Auto) skipBIOS = biosPath.empty();
    gba.reset(skipBIOS);

    FrontendSDL frontend;
    if (!frontend.init(opts.scale)) return 1;
    gba.bus.apu.setOutputRate(frontend.audioSampleRate());

    std::cout << "Loaded ROM: " << opts.romPath << "\n";
    std::cout << "Detected save type: " << gba.bus.backup.typeName() << "\n";
    if (!biosPath.empty()) std::cout << "Loaded BIOS: " << biosPath << "\n";
    if (skipBIOS) std::cout << "Boot mode: skip BIOS\n";
    else std::cout << "Boot mode: run BIOS\n";

    if (gba.bus.backup.type() != BackupDevice::Type::None) {
        std::string savePath = gba.makeSavePath(opts.romPath);
        if (gba.loadSaveForROM(opts.romPath)) std::cout << "Loaded save: " << savePath << "\n";
        else std::cout << "No existing save loaded, will use: " << savePath << "\n";
    }
    const u32 autosaveMs = 5000;
    u32 lastAutosaveTick = SDL_GetTicks();

    u64 perfFreq = SDL_GetPerformanceFrequency();
    u64 lastCounter = SDL_GetPerformanceCounter();
    double frameAccum = 0.0;
    while (frontend.processEvents(gba.bus.keypad)) {
        u64 nowCounter = SDL_GetPerformanceCounter();
        double dt = static_cast<double>(nowCounter - lastCounter) / static_cast<double>(perfFreq);
        lastCounter = nowCounter;
        if (dt > 0.25) dt = 0.25;
        frameAccum += dt;

        int ranFrames = 0;
        while (frameAccum >= targetFrameSec && ranFrames < 4) {
            gba.runFrame();
            frontend.queueAudio(gba.bus.apu.consumeSamples());
            frameAccum -= targetFrameSec;
            ++ranFrames;
        }

        if (ranFrames > 0) {
            frontend.present(gba.bus.ppu.framebuffer());
        } else {
            SDL_Delay(1);
        }
        flushLogs();

        if (gba.bus.backup.type() != BackupDevice::Type::None && gba.bus.backup.isDirty()) {
            u32 now = SDL_GetTicks();
            if (now - lastAutosaveTick >= autosaveMs) {
                gba.saveSaveForROM(opts.romPath, true);
                lastAutosaveTick = now;
            }
        }
    }
    flushLogs();

    if (gba.bus.backup.type() != BackupDevice::Type::None) {
        std::string savePath = gba.makeSavePath(opts.romPath);
        if (gba.saveSaveForROM(opts.romPath, true)) std::cout << "Saved backup to: " << savePath << "\n";
        else std::cerr << "Warning: failed to save backup to: " << savePath << "\n";
    }
    return 0;
}

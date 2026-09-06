#pragma once
#include <array>
#include <iosfwd>
#include "types.hpp"
#include "interrupts.hpp"

class PPU {
public:
    static constexpr int SCREEN_W = 240;
    static constexpr int SCREEN_H = 160;

    explicit PPU(InterruptController& irq);

    void reset();
    void tick(
        int cycles,
        const std::array<u8, 96 * 1024>& vramBytes,
        const std::array<u8, 1024>& paletteBytes,
        const std::array<u8, 1024>& oamBytes
    );

    const std::array<u16, SCREEN_W * SCREEN_H>& framebuffer() const { return framebuf; }
    u64 frameCount() const { return frameCounter; }
    void writeDISPSTAT(u16 value);
    void syncAffineReference(int idx);
    void syncAffineReferenceX(int idx);
    void syncAffineReferenceY(int idx);
    int consumeHBlankEvents();
    int consumeVBlankEvents();
    int cyclesUntilNextModeEvent() const;
    bool saveState(std::ostream& os) const;
    bool loadState(std::istream& is);

    u16 dispcnt = 0;
    u16 dispstat = 0;
    u16 vcount = 0;

    u16 bgcnt[4] = {};
    u16 bghofs[4] = {};
    u16 bgvofs[4] = {};
    u16 bgpa[2] = {};
    u16 bgpb[2] = {};
    u16 bgpc[2] = {};
    u16 bgpd[2] = {};
    s32 bgx[2] = {};
    s32 bgy[2] = {};

    u16 winh[2] = {};
    u16 winv[2] = {};
    u16 winin = 0;
    u16 winout = 0;

    u16 bldcnt = 0;
    u16 bldalpha = 0;
    u16 bldy = 0;
    u16 mosaic = 0;

private:
    InterruptController& irqc;
    int dotCycles = 0;
    bool vblankRaisedThisFrame = false;
    u64 frameCounter = 0;
    int pendingHBlankEvents = 0;
    int pendingVBlankEvents = 0;
    bool lineStartPendingRender = true;

    std::array<u16, SCREEN_W * SCREEN_H> framebuf{};
    std::array<u16, SCREEN_W> lineTopColor{};
    std::array<u16, SCREEN_W> lineSecondColor{};
    std::array<u16, SCREEN_W> lineThirdColor{};
    std::array<u8, SCREEN_W> lineTopPrio{};
    std::array<u8, SCREEN_W> lineSecondPrio{};
    std::array<u8, SCREEN_W> lineThirdPrio{};
    std::array<u8, SCREEN_W> lineTopLayer{};
    std::array<u8, SCREEN_W> lineSecondLayer{};
    std::array<u8, SCREEN_W> lineThirdLayer{};
    std::array<bool, SCREEN_W> lineTopSemiTransparent{};
    std::array<bool, SCREEN_W> lineObjWindow{};
    std::array<u8, SCREEN_W> lineWindowMask{};
    u16 lineBackdropColor = 0;
    s32 bgxInternal[2] = {};
    s32 bgyInternal[2] = {};
    s32 affinePaLatched[2] = {};
    s32 affinePbLatched[2] = {};
    s32 affinePcLatched[2] = {};
    s32 affinePdLatched[2] = {};
    bool affineRefWrittenX[2] = {};
    bool affineRefWrittenY[2] = {};

    void enterHBlank();
    void enterVBlank();
    void updateVCounterFlag(bool requestIrq);
    void clearLine(int y);

    void renderCurrentLine(
        const std::array<u8, 96 * 1024>& vramBytes,
        const std::array<u8, 1024>& paletteBytes,
        const std::array<u8, 1024>& oamBytes
    );

    void renderMode3Line(const std::array<u8, 96 * 1024>& vramBytes);
    void renderMode4Line(
        const std::array<u8, 96 * 1024>& vramBytes,
        const std::array<u8, 1024>& paletteBytes
    );
    void renderMode5Line(const std::array<u8, 96 * 1024>& vramBytes);
    void renderMode0BGLine(
        int bg,
        const std::array<u8, 96 * 1024>& vramBytes,
        const std::array<u8, 1024>& paletteBytes
    );
    void renderAffineBGLine(
        int bg,
        const std::array<u8, 96 * 1024>& vramBytes,
        const std::array<u8, 1024>& paletteBytes
    );
    void renderOBJLine(
        const std::array<u8, 96 * 1024>& vramBytes,
        const std::array<u8, 1024>& paletteBytes,
        const std::array<u8, 1024>& oamBytes
    );
    void renderOBJWindowMaskLine(
        const std::array<u8, 96 * 1024>& vramBytes,
        const std::array<u8, 1024>& oamBytes
    );

    void finalizeLine(int y);
    void plotPixel(int x, int y, u16 color, u8 prio, u8 layerId, bool semiTransparent);

    bool isWindowEnabledAny() const;
    bool inWindowRect(int win, int x, int y) const;
    u8 windowMaskForPixel(int x, int y) const;
    void buildWindowMaskLine(int y);
    bool layerAllowedByWindow(u8 layerId, int x, int y) const;
    bool effectsAllowedByWindow(int x, int y) const;

    u16 blendColorsAlpha(u16 a, u16 b, int eva, int evb) const;
    u16 brightenColor(u16 c, int evy) const;
    u16 darkenColor(u16 c, int evy) const;

    static u16 readPaletteColor(const std::array<u8, 1024>& paletteBytes, int index);
    static u16 readMapEntry(const std::array<u8, 96 * 1024>& vramBytes, int mapBase, int index);
    static int objWidth(u16 attr0, u16 attr1);
    static int objHeight(u16 attr0, u16 attr1);
};

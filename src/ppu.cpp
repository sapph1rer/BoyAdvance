#include "ppu.hpp"
#include "state_io.hpp"
#include <algorithm>

enum : u8 {
    LAYER_BG0 = 0,
    LAYER_BG1 = 1,
    LAYER_BG2 = 2,
    LAYER_BG3 = 3,
    LAYER_OBJ = 4,
    LAYER_BD  = 5
};

static inline int layerTieRank(u8 layer) {
    switch (layer) {
        case LAYER_OBJ: return 0;
        case LAYER_BG0: return 1;
        case LAYER_BG1: return 2;
        case LAYER_BG2: return 3;
        case LAYER_BG3: return 4;
        default: return 5;
    }
}

static inline bool pixelInFront(u8 prio, u8 layer, u8 curPrio, u8 curLayer) {
    if (prio != curPrio) return prio < curPrio;
    int a = layerTieRank(layer);
    int b = layerTieRank(curLayer);
    if (a != b) return a < b;
    // Same layer/same priority: later draw wins (needed for OBJ index ordering).
    return true;
}

static inline s32 wrapAffineRef28(s32 v) {
    u32 raw = static_cast<u32>(v) & 0x0FFFFFFFu;
    if (raw & 0x08000000u) raw |= 0xF0000000u;
    return static_cast<s32>(raw);
}

PPU::PPU(InterruptController& irq) : irqc(irq) {
    reset();
}

void PPU::reset() {
    dispcnt = 0;
    dispstat = 0;
    vcount = 0;
    dotCycles = 0;
    vblankRaisedThisFrame = false;
    frameCounter = 0;
    pendingHBlankEvents = 0;
    pendingVBlankEvents = 0;
    lineStartPendingRender = true;
    framebuf.fill(0);
    for (int i = 0; i < 4; ++i) {
        bgcnt[i] = 0;
        bghofs[i] = 0;
        bgvofs[i] = 0;
    }
    for (int i = 0; i < 2; ++i) {
        bgpa[i] = 0;
        bgpb[i] = 0;
        bgpc[i] = 0;
        bgpd[i] = 0;
        bgx[i] = 0;
        bgy[i] = 0;
        bgxInternal[i] = 0;
        bgyInternal[i] = 0;
        affinePaLatched[i] = 0;
        affinePbLatched[i] = 0;
        affinePcLatched[i] = 0;
        affinePdLatched[i] = 0;
        affineRefWrittenX[i] = false;
        affineRefWrittenY[i] = false;
    }
    winh[0] = winh[1] = 0;
    winv[0] = winv[1] = 0;
    winin = winout = 0;
    bldcnt = bldalpha = bldy = 0;
    mosaic = 0;
    lineTopColor.fill(0);
    lineSecondColor.fill(0);
    lineThirdColor.fill(0);
    lineTopPrio.fill(4);
    lineSecondPrio.fill(4);
    lineThirdPrio.fill(4);
    lineTopLayer.fill(LAYER_BD);
    lineSecondLayer.fill(LAYER_BD);
    lineThirdLayer.fill(LAYER_BD);
    lineTopSemiTransparent.fill(false);
    lineObjWindow.fill(false);
    lineWindowMask.fill(0x3F);
    lineBackdropColor = 0;
    updateVCounterFlag(false);
}

int PPU::consumeHBlankEvents() {
    int n = pendingHBlankEvents;
    pendingHBlankEvents = 0;
    return n;
}

int PPU::consumeVBlankEvents() {
    int n = pendingVBlankEvents;
    pendingVBlankEvents = 0;
    return n;
}

int PPU::cyclesUntilNextModeEvent() const {
    if (dotCycles < 960) return 960 - dotCycles;
    if (dotCycles < 1232) return 1232 - dotCycles;
    return 1;
}

bool PPU::saveState(std::ostream& os) const {
    using namespace stateio;
    if (!writePod(os, dispcnt)) return false;
    if (!writePod(os, dispstat)) return false;
    if (!writePod(os, vcount)) return false;
    for (int i = 0; i < 4; ++i) if (!writePod(os, bgcnt[i])) return false;
    for (int i = 0; i < 4; ++i) if (!writePod(os, bghofs[i])) return false;
    for (int i = 0; i < 4; ++i) if (!writePod(os, bgvofs[i])) return false;
    for (int i = 0; i < 2; ++i) if (!writePod(os, bgpa[i])) return false;
    for (int i = 0; i < 2; ++i) if (!writePod(os, bgpb[i])) return false;
    for (int i = 0; i < 2; ++i) if (!writePod(os, bgpc[i])) return false;
    for (int i = 0; i < 2; ++i) if (!writePod(os, bgpd[i])) return false;
    for (int i = 0; i < 2; ++i) if (!writePod(os, bgx[i])) return false;
    for (int i = 0; i < 2; ++i) if (!writePod(os, bgy[i])) return false;
    if (!writePod(os, winh[0])) return false;
    if (!writePod(os, winh[1])) return false;
    if (!writePod(os, winv[0])) return false;
    if (!writePod(os, winv[1])) return false;
    if (!writePod(os, winin)) return false;
    if (!writePod(os, winout)) return false;
    if (!writePod(os, bldcnt)) return false;
    if (!writePod(os, bldalpha)) return false;
    if (!writePod(os, bldy)) return false;
    if (!writePod(os, mosaic)) return false;
    if (!writePod(os, lineBackdropColor)) return false;
    if (!writePod(os, dotCycles)) return false;
    if (!writeBool(os, vblankRaisedThisFrame)) return false;
    if (!writePod(os, pendingHBlankEvents)) return false;
    if (!writePod(os, pendingVBlankEvents)) return false;
    if (!writePod(os, bgxInternal[0])) return false;
    if (!writePod(os, bgxInternal[1])) return false;
    if (!writePod(os, bgyInternal[0])) return false;
    if (!writePod(os, bgyInternal[1])) return false;
    if (!writePod(os, frameCounter)) return false;
    if (!writeArray(os, framebuf)) return false;
    return true;
}

bool PPU::loadState(std::istream& is) {
    using namespace stateio;
    if (!readPod(is, dispcnt)) return false;
    if (!readPod(is, dispstat)) return false;
    if (!readPod(is, vcount)) return false;
    for (int i = 0; i < 4; ++i) if (!readPod(is, bgcnt[i])) return false;
    for (int i = 0; i < 4; ++i) if (!readPod(is, bghofs[i])) return false;
    for (int i = 0; i < 4; ++i) if (!readPod(is, bgvofs[i])) return false;
    for (int i = 0; i < 2; ++i) if (!readPod(is, bgpa[i])) return false;
    for (int i = 0; i < 2; ++i) if (!readPod(is, bgpb[i])) return false;
    for (int i = 0; i < 2; ++i) if (!readPod(is, bgpc[i])) return false;
    for (int i = 0; i < 2; ++i) if (!readPod(is, bgpd[i])) return false;
    for (int i = 0; i < 2; ++i) if (!readPod(is, bgx[i])) return false;
    for (int i = 0; i < 2; ++i) if (!readPod(is, bgy[i])) return false;
    if (!readPod(is, winh[0])) return false;
    if (!readPod(is, winh[1])) return false;
    if (!readPod(is, winv[0])) return false;
    if (!readPod(is, winv[1])) return false;
    if (!readPod(is, winin)) return false;
    if (!readPod(is, winout)) return false;
    if (!readPod(is, bldcnt)) return false;
    if (!readPod(is, bldalpha)) return false;
    if (!readPod(is, bldy)) return false;
    if (!readPod(is, mosaic)) return false;
    if (!readPod(is, lineBackdropColor)) return false;
    if (!readPod(is, dotCycles)) return false;
    if (!readBool(is, vblankRaisedThisFrame)) return false;
    if (!readPod(is, pendingHBlankEvents)) return false;
    if (!readPod(is, pendingVBlankEvents)) return false;
    if (!readPod(is, bgxInternal[0])) return false;
    if (!readPod(is, bgxInternal[1])) return false;
    if (!readPod(is, bgyInternal[0])) return false;
    if (!readPod(is, bgyInternal[1])) return false;
    if (!readPod(is, frameCounter)) return false;
    if (!readArray(is, framebuf)) return false;

    for (int i = 0; i < 2; ++i) {
        bgx[i] = wrapAffineRef28(bgx[i]);
        bgy[i] = wrapAffineRef28(bgy[i]);
        bgxInternal[i] = wrapAffineRef28(bgxInternal[i]);
        bgyInternal[i] = wrapAffineRef28(bgyInternal[i]);
        affinePaLatched[i] = static_cast<int16_t>(bgpa[i]);
        affinePbLatched[i] = static_cast<int16_t>(bgpb[i]);
        affinePcLatched[i] = static_cast<int16_t>(bgpc[i]);
        affinePdLatched[i] = static_cast<int16_t>(bgpd[i]);
    }

    lineTopColor.fill(0);
    lineSecondColor.fill(0);
    lineThirdColor.fill(0);
    lineTopPrio.fill(4);
    lineSecondPrio.fill(4);
    lineThirdPrio.fill(4);
    lineTopLayer.fill(LAYER_BD);
    lineSecondLayer.fill(LAYER_BD);
    lineThirdLayer.fill(LAYER_BD);
    lineTopSemiTransparent.fill(false);
    lineObjWindow.fill(false);
    lineWindowMask.fill(0x3F);
    affineRefWrittenX[0] = affineRefWrittenX[1] = false;
    affineRefWrittenY[0] = affineRefWrittenY[1] = false;
    lineStartPendingRender = (dotCycles == 0 && vcount < SCREEN_H);
    return true;
}

void PPU::writeDISPSTAT(u16 value) {
    constexpr u16 writable = static_cast<u16>((1 << 3) | (1 << 4) | (1 << 5) | 0xFF00);
    dispstat = static_cast<u16>((dispstat & ~writable) | (value & writable));
    updateVCounterFlag(false);
}

void PPU::syncAffineReference(int idx) {
    if (idx < 0 || idx > 1) return;
    bgxInternal[idx] = wrapAffineRef28(bgx[idx]);
    bgyInternal[idx] = wrapAffineRef28(bgy[idx]);
    affineRefWrittenX[idx] = true;
    affineRefWrittenY[idx] = true;
}

void PPU::syncAffineReferenceX(int idx) {
    if (idx < 0 || idx > 1) return;
    bgxInternal[idx] = wrapAffineRef28(bgx[idx]);
    affineRefWrittenX[idx] = true;
}

void PPU::syncAffineReferenceY(int idx) {
    if (idx < 0 || idx > 1) return;
    bgyInternal[idx] = wrapAffineRef28(bgy[idx]);
    affineRefWrittenY[idx] = true;
}

void PPU::enterHBlank() {
    dispstat |= (1 << 1);
    if (vcount < 160) ++pendingHBlankEvents;
    if (dispstat & (1 << 4)) irqc.request(IRQ_HBLANK);
}

void PPU::enterVBlank() {
    dispstat |= (1 << 0);
    ++pendingVBlankEvents;
    if (!vblankRaisedThisFrame) {
        vblankRaisedThisFrame = true;
        if (dispstat & (1 << 3)) irqc.request(IRQ_VBLANK);
    }
}

void PPU::updateVCounterFlag(bool requestIrq) {
    bool wasMatch = (dispstat & (1 << 2)) != 0;
    u16 vcountSetting = static_cast<u16>((dispstat >> 8) & 0xFF);
    bool match = (vcount & 0xFF) == vcountSetting;
    if (match) dispstat |= (1 << 2);
    else dispstat &= ~(1 << 2);
    if (requestIrq && match && !wasMatch && (dispstat & (1 << 5))) {
        irqc.request(IRQ_VCOUNT);
    }
}

bool PPU::isWindowEnabledAny() const {
    return (dispcnt & ((1 << 13) | (1 << 14) | (1 << 15))) != 0;
}

bool PPU::inWindowRect(int win, int x, int y) const {
    u16 h = winh[win];
    u16 v = winv[win];
    int x1 = (h >> 8) & 0xFF;
    int x2 = h & 0xFF;
    int y1 = (v >> 8) & 0xFF;
    int y2 = v & 0xFF;
    bool inX = (x1 <= x2) ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
    bool inY = (y1 <= y2) ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
    return inX && inY;
}

u8 PPU::windowMaskForPixel(int x, int y) const {
    if ((dispcnt & (1 << 13)) && inWindowRect(0, x, y)) return static_cast<u8>(winin & 0x3F);
    if ((dispcnt & (1 << 14)) && inWindowRect(1, x, y)) return static_cast<u8>((winin >> 8) & 0x3F);
    if ((dispcnt & (1 << 15)) && x >= 0 && x < SCREEN_W && lineObjWindow[x]) {
        return static_cast<u8>((winout >> 8) & 0x3F);
    }
    return static_cast<u8>(winout & 0x3F);
}

bool PPU::layerAllowedByWindow(u8 layerId, int x, int y) const {
    if (!isWindowEnabledAny()) return true;
    if (x < 0 || x >= SCREEN_W) return true;
    u8 mask = lineWindowMask[x];
    if (layerId <= LAYER_BG3) return (mask >> layerId) & 1;
    if (layerId == LAYER_OBJ) return (mask >> 4) & 1;
    return true;
}

bool PPU::effectsAllowedByWindow(int x, int y) const {
    if (!isWindowEnabledAny()) return true;
    if (x < 0 || x >= SCREEN_W) return true;
    return (lineWindowMask[x] >> 5) & 1;
}

void PPU::buildWindowMaskLine(int y) {
    if (!isWindowEnabledAny()) {
        lineWindowMask.fill(0x3F);
        return;
    }
    for (int x = 0; x < SCREEN_W; ++x) {
        lineWindowMask[x] = windowMaskForPixel(x, y);
    }
}

u16 PPU::blendColorsAlpha(u16 a, u16 b, int eva, int evb) const {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int r = std::min(31, (ar * eva + br * evb) / 16);
    int g = std::min(31, (ag * eva + bg * evb) / 16);
    int bl = std::min(31, (ab * eva + bb * evb) / 16);
    return static_cast<u16>(r | (g << 5) | (bl << 10));
}

u16 PPU::brightenColor(u16 c, int evy) const {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = r + ((31 - r) * evy) / 16;
    g = g + ((31 - g) * evy) / 16;
    b = b + ((31 - b) * evy) / 16;
    return static_cast<u16>(std::min(31, r) | (std::min(31, g) << 5) | (std::min(31, b) << 10));
}

u16 PPU::darkenColor(u16 c, int evy) const {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = r - (r * evy) / 16;
    g = g - (g * evy) / 16;
    b = b - (b * evy) / 16;
    return static_cast<u16>(std::max(0, r) | (std::max(0, g) << 5) | (std::max(0, b) << 10));
}

void PPU::clearLine(int y) {
    for (int x = 0; x < SCREEN_W; ++x) {
        framebuf[y * SCREEN_W + x] = 0;
        lineTopColor[x] = 0;
        lineSecondColor[x] = 0;
        lineThirdColor[x] = 0;
        lineTopPrio[x] = 4;
        lineSecondPrio[x] = 4;
        lineThirdPrio[x] = 4;
        lineTopLayer[x] = LAYER_BD;
        lineSecondLayer[x] = LAYER_BD;
        lineThirdLayer[x] = LAYER_BD;
        lineTopSemiTransparent[x] = false;
        lineObjWindow[x] = false;
    }
}

void PPU::plotPixel(int x, int y, u16 color, u8 prio, u8 layerId, bool semiTransparent) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    if (!layerAllowedByWindow(layerId, x, y)) return;
    if (pixelInFront(prio, layerId, lineTopPrio[x], lineTopLayer[x])) {
        lineThirdColor[x] = lineSecondColor[x];
        lineThirdPrio[x] = lineSecondPrio[x];
        lineThirdLayer[x] = lineSecondLayer[x];
        lineSecondColor[x] = lineTopColor[x];
        lineSecondPrio[x] = lineTopPrio[x];
        lineSecondLayer[x] = lineTopLayer[x];
        lineTopColor[x] = color;
        lineTopPrio[x] = prio;
        lineTopLayer[x] = layerId;
        lineTopSemiTransparent[x] = semiTransparent;
    } else if (pixelInFront(prio, layerId, lineSecondPrio[x], lineSecondLayer[x])) {
        lineThirdColor[x] = lineSecondColor[x];
        lineThirdPrio[x] = lineSecondPrio[x];
        lineThirdLayer[x] = lineSecondLayer[x];
        lineSecondColor[x] = color;
        lineSecondPrio[x] = prio;
        lineSecondLayer[x] = layerId;
    } else if (pixelInFront(prio, layerId, lineThirdPrio[x], lineThirdLayer[x])) {
        lineThirdColor[x] = color;
        lineThirdPrio[x] = prio;
        lineThirdLayer[x] = layerId;
    }
}

u16 PPU::readPaletteColor(const std::array<u8, 1024>& paletteBytes, int index) {
    int off = (index & 0x1FF) * 2;
    return static_cast<u16>(paletteBytes[off] | (paletteBytes[off + 1] << 8));
}

u16 PPU::readMapEntry(const std::array<u8, 96 * 1024>& vramBytes, int mapBase, int index) {
    int off = mapBase + index * 2;
    // BG text/affine map data is fetched from BG VRAM window (0x00000-0x0FFFF).
    int lo = off & 0xFFFF;
    int hi = (off + 1) & 0xFFFF;
    return static_cast<u16>(vramBytes[static_cast<size_t>(lo)] | (vramBytes[static_cast<size_t>(hi)] << 8));
}

void PPU::renderMode3Line(const std::array<u8, 96 * 1024>& vramBytes) {
    if ((dispcnt & 0x7) != 3 || vcount >= SCREEN_H) return;
    if (!(dispcnt & (1 << 10))) return; // BG2 enable
    int y = static_cast<int>(vcount);
    u8 prio = static_cast<u8>(bgcnt[2] & 0x3);
    bool mosaicEnable = (bgcnt[2] & (1 << 6)) != 0;
    int bgMosH = (mosaic & 0xF) + 1;
    int bgMosV = ((mosaic >> 4) & 0xF) + 1;
    int sampleY = mosaicEnable ? (y - (y % bgMosV)) : y;
    for (int x = 0; x < SCREEN_W; ++x) {
        int sampleX = mosaicEnable ? (x - (x % bgMosH)) : x;
        int off = (sampleY * SCREEN_W + sampleX) * 2;
        u16 c = static_cast<u16>(vramBytes[static_cast<size_t>(off)] |
                                 (vramBytes[static_cast<size_t>(off + 1)] << 8));
        plotPixel(x, y, c, prio, LAYER_BG2, false);
    }
}

void PPU::renderMode4Line(const std::array<u8, 96 * 1024>& vramBytes, const std::array<u8, 1024>& paletteBytes) {
    if ((dispcnt & 0x7) != 4 || vcount >= SCREEN_H) return;
    if (!(dispcnt & (1 << 10))) return; // BG2 enable
    int pageBase = (dispcnt & (1 << 4)) ? 0xA000 : 0x0000;
    int y = static_cast<int>(vcount);
    u8 prio = static_cast<u8>(bgcnt[2] & 0x3);
    bool mosaicEnable = (bgcnt[2] & (1 << 6)) != 0;
    int bgMosH = (mosaic & 0xF) + 1;
    int bgMosV = ((mosaic >> 4) & 0xF) + 1;
    int sampleY = mosaicEnable ? (y - (y % bgMosV)) : y;
    int lineBase = pageBase + sampleY * SCREEN_W;
    for (int x = 0; x < SCREEN_W; ++x) {
        int sampleX = mosaicEnable ? (x - (x % bgMosH)) : x;
        u8 idx = vramBytes[lineBase + sampleX];
        plotPixel(x, y, readPaletteColor(paletteBytes, idx), prio, LAYER_BG2, false);
    }
}

void PPU::renderMode5Line(const std::array<u8, 96 * 1024>& vramBytes) {
    if ((dispcnt & 0x7) != 5 || vcount >= SCREEN_H) return;
    if (!(dispcnt & (1 << 10))) return; // BG2 enable
    int pageBase = (dispcnt & (1 << 4)) ? 0xA000 : 0x0000;
    int y = static_cast<int>(vcount);
    u8 prio = static_cast<u8>(bgcnt[2] & 0x3);
    bool mosaicEnable = (bgcnt[2] & (1 << 6)) != 0;
    int bgMosH = (mosaic & 0xF) + 1;
    int bgMosV = ((mosaic >> 4) & 0xF) + 1;
    int sampleY = mosaicEnable ? (y - (y % bgMosV)) : y;
    if (sampleY >= 128) return;
    for (int x = 0; x < SCREEN_W; ++x) {
        int sampleX = mosaicEnable ? (x - (x % bgMosH)) : x;
        if (sampleX >= 160) continue;
        int off = pageBase + ((sampleY * 160 + sampleX) * 2);
        if (off + 1 >= static_cast<int>(vramBytes.size())) continue;
        u16 c = static_cast<u16>(vramBytes[off] | (vramBytes[off + 1] << 8));
        plotPixel(x, y, c, prio, LAYER_BG2, false);
    }
}

void PPU::renderAffineBGLine(int bg, const std::array<u8, 96 * 1024>& vramBytes, const std::array<u8, 1024>& paletteBytes) {
    if (vcount >= SCREEN_H) return;
    if (bg < 2 || bg > 3) return;
    if (!(dispcnt & (1 << (8 + bg)))) return;

    int idx = bg - 2;
    u16 cnt = bgcnt[bg];
    u8 prio = cnt & 0x3;
    int charBase = ((cnt >> 2) & 0x3) * 0x4000;
    int screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int sizeSel = (cnt >> 14) & 0x3;
    int sizePx = 128 << sizeSel;
    int sizeMask = sizePx - 1;
    bool wrap = (cnt & (1 << 13)) != 0;

    s32 pa = affinePaLatched[idx];
    s32 pc = affinePcLatched[idx];
    s32 pbStep = affinePbLatched[idx];
    s32 pdStep = affinePdLatched[idx];
    bool mosaicEnable = (cnt & (1 << 6)) != 0;
    int bgMosH = (mosaic & 0xF) + 1;
    int bgMosV = ((mosaic >> 4) & 0xF) + 1;
    int y = static_cast<int>(vcount);
    int mosaicYOffset = mosaicEnable ? (y % bgMosV) : 0;
    s32 baseX = wrapAffineRef28(bgxInternal[idx] - pbStep * mosaicYOffset);
    s32 baseY = wrapAffineRef28(bgyInternal[idx] - pdStep * mosaicYOffset);
    int mapTiles = sizePx / 8;
    s32 curX = baseX;
    s32 curY = baseY;
    bool cachedVisible = false;
    u16 cachedColor = 0;

    for (int x = 0; x < SCREEN_W; ++x) {
        bool recalc = !mosaicEnable || (x % bgMosH) == 0;
        if (recalc) {
            int sx = curX >> 8;
            int sy = curY >> 8;
            cachedVisible = true;

            if (wrap) {
                sx &= sizeMask;
                sy &= sizeMask;
            } else if (sx < 0 || sy < 0 || sx >= sizePx || sy >= sizePx) {
                cachedVisible = false;
            }

            if (cachedVisible) {
                int tileX = sx >> 3;
                int tileY = sy >> 3;
                int mapAddr = screenBase + tileY * mapTiles + tileX;
                int tileNum = vramBytes[static_cast<size_t>(mapAddr & 0xFFFF)];
                int tileAddr = charBase + tileNum * 64 + ((sy & 7) << 3) + (sx & 7);
                u8 color = vramBytes[static_cast<size_t>(tileAddr & 0xFFFF)];
                if (color == 0) {
                    cachedVisible = false;
                } else {
                    cachedColor = readPaletteColor(paletteBytes, color);
                }
            }
        }

        if (cachedVisible) {
            plotPixel(x, y, cachedColor, prio, static_cast<u8>(LAYER_BG0 + bg), false);
        }

        curX = wrapAffineRef28(curX + pa);
        curY = wrapAffineRef28(curY + pc);
    }
}

void PPU::renderMode0BGLine(int bg, const std::array<u8, 96 * 1024>& vramBytes, const std::array<u8, 1024>& paletteBytes) {
    if (vcount >= SCREEN_H) return;
    if (!(dispcnt & (1 << (8 + bg)))) return;

    int y = static_cast<int>(vcount);
    u16 cnt = bgcnt[bg];
    u8 prio = cnt & 0x3;
    bool color256 = (cnt & (1 << 7)) != 0;
    int charBase = ((cnt >> 2) & 0x3) * 0x4000;
    int screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int size = (cnt >> 14) & 0x3;

    int mapWidth = 32, mapHeight = 32;
    if (size == 1) { mapWidth = 64; mapHeight = 32; }
    if (size == 2) { mapWidth = 32; mapHeight = 64; }
    if (size == 3) { mapWidth = 64; mapHeight = 64; }

    int scrollX = bghofs[bg] & (mapWidth * 8 - 1);
    int scrollY = bgvofs[bg] & (mapHeight * 8 - 1);
    bool mosaicEnable = (cnt & (1 << 6)) != 0;
    int bgMosH = (mosaic & 0xF) + 1;
    int bgMosV = ((mosaic >> 4) & 0xF) + 1;
    int sampleY = mosaicEnable ? (y - (y % bgMosV)) : y;

    for (int x = 0; x < SCREEN_W; ++x) {
        int sampleX = mosaicEnable ? (x - (x % bgMosH)) : x;
        int worldX = (sampleX + scrollX) & (mapWidth * 8 - 1);
        int worldY = (sampleY + scrollY) & (mapHeight * 8 - 1);
        int tileX = worldX / 8, tileY = worldY / 8;
        int px = worldX & 7, py = worldY & 7;

        int screenBlock = 0;
        if (mapWidth == 64 && tileX >= 32) screenBlock += 1;
        if (mapHeight == 64 && tileY >= 32) screenBlock += 2;

        int localTileX = tileX & 31, localTileY = tileY & 31;
        int mapIndex = localTileY * 32 + localTileX;
        int mapBase = screenBase + screenBlock * 0x800;

        u16 entry = readMapEntry(vramBytes, mapBase, mapIndex);
        int tileNum = entry & 0x3FF;
        bool hflip = (entry & (1 << 10)) != 0;
        bool vflip = (entry & (1 << 11)) != 0;
        int palBank = (entry >> 12) & 0xF;

        int tx = hflip ? (7 - px) : px;
        int ty = vflip ? (7 - py) : py;

        if (!color256) {
            int tileAddr = charBase + tileNum * 32 + ty * 4 + (tx / 2);
            u8 packed = vramBytes[static_cast<size_t>(tileAddr & 0xFFFF)];
            u8 color4 = (tx & 1) ? (packed >> 4) : (packed & 0xF);
            if (color4 == 0) continue;
            plotPixel(x, y, readPaletteColor(paletteBytes, palBank * 16 + color4), prio, static_cast<u8>(LAYER_BG0 + bg), false);
        } else {
            int tileAddr = charBase + tileNum * 64 + ty * 8 + tx;
            u8 color8 = vramBytes[static_cast<size_t>(tileAddr & 0xFFFF)];
            if (color8 == 0) continue;
            plotPixel(x, y, readPaletteColor(paletteBytes, color8), prio, static_cast<u8>(LAYER_BG0 + bg), false);
        }
    }
}

int PPU::objWidth(u16 attr0, u16 attr1) {
    int shape = (attr0 >> 14) & 0x3;
    int size = (attr1 >> 14) & 0x3;
    static const int table[3][4][2] = {
        { {8,8}, {16,16}, {32,32}, {64,64} },
        { {16,8}, {32,8}, {32,16}, {64,32} },
        { {8,16}, {8,32}, {16,32}, {32,64} }
    };
    if (shape >= 3) return 0;
    return table[shape][size][0];
}

int PPU::objHeight(u16 attr0, u16 attr1) {
    int shape = (attr0 >> 14) & 0x3;
    int size = (attr1 >> 14) & 0x3;
    static const int table[3][4][2] = {
        { {8,8}, {16,16}, {32,32}, {64,64} },
        { {16,8}, {32,8}, {32,16}, {64,32} },
        { {8,16}, {8,32}, {16,32}, {32,64} }
    };
    if (shape >= 3) return 0;
    return table[shape][size][1];
}

void PPU::renderOBJLine(const std::array<u8, 96 * 1024>& vramBytes, const std::array<u8, 1024>& paletteBytes, const std::array<u8, 1024>& oamBytes) {
    if (!(dispcnt & (1 << 12))) return;
    if (vcount >= SCREEN_H) return;

    int y = static_cast<int>(vcount);
    bool obj1D = (dispcnt & (1 << 6)) != 0;
    auto readAffineParam = [&](int matrix, int part) -> s32 {
        int paramObj = matrix * 4 + part;
        int paramOff = paramObj * 8 + 6;
        u16 raw = static_cast<u16>(oamBytes[paramOff] | (oamBytes[paramOff + 1] << 8));
        return static_cast<s32>(static_cast<int16_t>(raw));
    };

    for (int i = 127; i >= 0; --i) {
        int off = i * 8;
        u16 attr0 = static_cast<u16>(oamBytes[off] | (oamBytes[off + 1] << 8));
        u16 attr1 = static_cast<u16>(oamBytes[off + 2] | (oamBytes[off + 3] << 8));
        u16 attr2 = static_cast<u16>(oamBytes[off + 4] | (oamBytes[off + 5] << 8));

        int objMode = (attr0 >> 10) & 0x3;
        if (objMode == 2) continue;
        bool affine = (attr0 & (1 << 8)) != 0;
        bool doubleSize = (attr0 & (1 << 9)) != 0;
        if (!affine && doubleSize) continue;
        bool color256 = (attr0 & (1 << 13)) != 0;
        int objPrio = (attr2 >> 10) & 0x3;

        int objX = attr1 & 0x1FF;
        if (objX >= 256) objX -= 512;

        int w = objWidth(attr0, attr1);
        int h = objHeight(attr0, attr1);
        if (w == 0 || h == 0) continue;
        int renderW = (affine && doubleSize) ? (w * 2) : w;
        int renderH = (affine && doubleSize) ? (h * 2) : h;

        int objYRaw = attr0 & 0xFF;
        int objLineY = (y - objYRaw) & 0xFF;
        if (objLineY >= renderH) continue;
        bool mosaicEnable = (attr0 & (1 << 12)) != 0;
        int objMosH = ((mosaic >> 8) & 0xF) + 1;
        int objMosV = ((mosaic >> 12) & 0xF) + 1;
        int mosaicLineY = mosaicEnable ? (objLineY - (objLineY % objMosV)) : objLineY;

        bool hflip = !affine && ((attr1 & (1 << 12)) != 0);
        bool vflip = !affine && ((attr1 & (1 << 13)) != 0);

        int tileBase = attr2 & 0x3FF;
        if (color256) tileBase &= ~1;
        int palBank = (attr2 >> 12) & 0xF;
        int tileStep = color256 ? 2 : 1;
        int rowStride = obj1D ? ((w / 8) * tileStep) : 32;

        s32 pa = 256;
        s32 pb = 0;
        s32 pc = 0;
        s32 pd = 256;
        if (affine) {
            int matrix = (attr1 >> 9) & 0x1F;
            pa = readAffineParam(matrix, 0);
            pb = readAffineParam(matrix, 1);
            pc = readAffineParam(matrix, 2);
            pd = readAffineParam(matrix, 3);
        }

        for (int sx = 0; sx < renderW; ++sx) {
            int screenX = objX + sx;
            if (screenX < 0 || screenX >= SCREEN_W) continue;
            int renderSX = mosaicEnable ? (sx - (sx % objMosH)) : sx;

            int srcX = 0;
            int srcY = 0;
            if (affine) {
                int cx = renderW / 2;
                int cy = renderH / 2;
                int relX = renderSX - cx;
                int relY = mosaicLineY - cy;
                s32 texX = pa * relX + pb * relY + (w << 7);
                s32 texY = pc * relX + pd * relY + (h << 7);
                srcX = texX >> 8;
                srcY = texY >> 8;
                if (srcX < 0 || srcY < 0 || srcX >= w || srcY >= h) continue;
            } else {
                int localX = hflip ? (w - 1 - renderSX) : renderSX;
                int localY = vflip ? (h - 1 - mosaicLineY) : mosaicLineY;
                srcX = localX;
                srcY = localY;
            }

            int tileX = srcX / 8;
            int tileY = srcY / 8;
            int px = srcX & 7;
            int py = srcY & 7;
            int tileIndex = tileBase + tileY * rowStride + tileX * tileStep;
            bool bitmapMode = (dispcnt & 0x7) >= 3;
            int objTileBase = bitmapMode ? 0x14000 : 0x10000;
            int objTileMask = bitmapMode ? 0x3FFF : 0x7FFF;
            if (bitmapMode) tileIndex &= color256 ? 0xFF : 0x1FF;

            if (!color256) {
                int relAddr = tileIndex * 32 + py * 4 + (px / 2);
                int tileAddr = objTileBase + (relAddr & objTileMask);
                u8 packed = vramBytes[static_cast<size_t>(tileAddr)];
                u8 color4 = (px & 1) ? (packed >> 4) : (packed & 0xF);
                if (color4 == 0) continue;
                plotPixel(screenX, y, readPaletteColor(paletteBytes, 256 + palBank * 16 + color4), static_cast<u8>(objPrio), LAYER_OBJ, objMode == 1);
            } else {
                int relAddr = tileIndex * 32 + py * 8 + px;
                int tileAddr = objTileBase + (relAddr & objTileMask);
                u8 color8 = vramBytes[static_cast<size_t>(tileAddr)];
                if (color8 == 0) continue;
                plotPixel(screenX, y, readPaletteColor(paletteBytes, 256 + color8), static_cast<u8>(objPrio), LAYER_OBJ, objMode == 1);
            }
        }
    }
}

void PPU::renderOBJWindowMaskLine(const std::array<u8, 96 * 1024>& vramBytes, const std::array<u8, 1024>& oamBytes) {
    if (!(dispcnt & (1 << 15))) return;
    if (vcount >= SCREEN_H) return;

    int y = static_cast<int>(vcount);
    bool obj1D = (dispcnt & (1 << 6)) != 0;
    auto readAffineParam = [&](int matrix, int part) -> s32 {
        int paramObj = matrix * 4 + part;
        int paramOff = paramObj * 8 + 6;
        u16 raw = static_cast<u16>(oamBytes[paramOff] | (oamBytes[paramOff + 1] << 8));
        return static_cast<s32>(static_cast<int16_t>(raw));
    };

    for (int i = 127; i >= 0; --i) {
        int off = i * 8;
        u16 attr0 = static_cast<u16>(oamBytes[off] | (oamBytes[off + 1] << 8));
        u16 attr1 = static_cast<u16>(oamBytes[off + 2] | (oamBytes[off + 3] << 8));
        u16 attr2 = static_cast<u16>(oamBytes[off + 4] | (oamBytes[off + 5] << 8));

        int objMode = (attr0 >> 10) & 0x3;
        if (objMode != 2) continue;

        bool affine = (attr0 & (1 << 8)) != 0;
        bool doubleSize = (attr0 & (1 << 9)) != 0;
        if (!affine && doubleSize) continue;

        bool color256 = (attr0 & (1 << 13)) != 0;
        int objX = attr1 & 0x1FF;
        if (objX >= 256) objX -= 512;

        int w = objWidth(attr0, attr1);
        int h = objHeight(attr0, attr1);
        if (w == 0 || h == 0) continue;
        int renderW = (affine && doubleSize) ? (w * 2) : w;
        int renderH = (affine && doubleSize) ? (h * 2) : h;

        int objYRaw = attr0 & 0xFF;
        int objLineY = (y - objYRaw) & 0xFF;
        if (objLineY >= renderH) continue;
        bool mosaicEnable = (attr0 & (1 << 12)) != 0;
        int objMosH = ((mosaic >> 8) & 0xF) + 1;
        int objMosV = ((mosaic >> 12) & 0xF) + 1;
        int mosaicLineY = mosaicEnable ? (objLineY - (objLineY % objMosV)) : objLineY;

        bool hflip = !affine && ((attr1 & (1 << 12)) != 0);
        bool vflip = !affine && ((attr1 & (1 << 13)) != 0);

        int tileBase = attr2 & 0x3FF;
        if (color256) tileBase &= ~1;
        int tileStep = color256 ? 2 : 1;
        int rowStride = obj1D ? ((w / 8) * tileStep) : 32;

        s32 pa = 256;
        s32 pb = 0;
        s32 pc = 0;
        s32 pd = 256;
        if (affine) {
            int matrix = (attr1 >> 9) & 0x1F;
            pa = readAffineParam(matrix, 0);
            pb = readAffineParam(matrix, 1);
            pc = readAffineParam(matrix, 2);
            pd = readAffineParam(matrix, 3);
        }

        for (int sx = 0; sx < renderW; ++sx) {
            int screenX = objX + sx;
            if (screenX < 0 || screenX >= SCREEN_W) continue;
            int renderSX = mosaicEnable ? (sx - (sx % objMosH)) : sx;

            int srcX = 0;
            int srcY = 0;
            if (affine) {
                int cx = renderW / 2;
                int cy = renderH / 2;
                int relX = renderSX - cx;
                int relY = mosaicLineY - cy;
                s32 texX = pa * relX + pb * relY + (w << 7);
                s32 texY = pc * relX + pd * relY + (h << 7);
                srcX = texX >> 8;
                srcY = texY >> 8;
                if (srcX < 0 || srcY < 0 || srcX >= w || srcY >= h) continue;
            } else {
                int localX = hflip ? (w - 1 - renderSX) : renderSX;
                int localY = vflip ? (h - 1 - mosaicLineY) : mosaicLineY;
                srcX = localX;
                srcY = localY;
            }

            int tileX = srcX / 8;
            int tileY = srcY / 8;
            int px = srcX & 7;
            int py = srcY & 7;
            int tileIndex = tileBase + tileY * rowStride + tileX * tileStep;
            bool bitmapMode = (dispcnt & 0x7) >= 3;
            int objTileBase = bitmapMode ? 0x14000 : 0x10000;
            int objTileMask = bitmapMode ? 0x3FFF : 0x7FFF;
            if (bitmapMode) tileIndex &= color256 ? 0xFF : 0x1FF;

            if (!color256) {
                int relAddr = tileIndex * 32 + py * 4 + (px / 2);
                int tileAddr = objTileBase + (relAddr & objTileMask);
                u8 packed = vramBytes[static_cast<size_t>(tileAddr)];
                u8 color4 = (px & 1) ? (packed >> 4) : (packed & 0xF);
                if (color4 == 0) continue;
            } else {
                int relAddr = tileIndex * 32 + py * 8 + px;
                int tileAddr = objTileBase + (relAddr & objTileMask);
                u8 color8 = vramBytes[static_cast<size_t>(tileAddr)];
                if (color8 == 0) continue;
            }
            lineObjWindow[screenX] = true;
        }
    }
}

void PPU::finalizeLine(int y) {
    int eva = std::min(16, static_cast<int>(bldalpha & 0x1F));
    int evb = std::min(16, static_cast<int>((bldalpha >> 8) & 0x1F));
    int evy = std::min(16, static_cast<int>(bldy & 0x1F));
    int effect = (bldcnt >> 6) & 0x3;

    for (int x = 0; x < SCREEN_W; ++x) {
        u16 top = lineTopColor[x];
        u8 topLayer = lineTopLayer[x];
        bool fxAllowed = effectsAllowedByWindow(x, y);
        bool topIsFirst = ((bldcnt >> topLayer) & 1) != 0;
        u16 blendBackColor = lineSecondColor[x];
        u8 blendBackLayer = lineSecondLayer[x];
        bool sndIsSecond = ((bldcnt >> (8 + blendBackLayer)) & 1) != 0;
        if (!sndIsSecond) {
            u8 thirdLayer = lineThirdLayer[x];
            bool thirdIsSecond = ((bldcnt >> (8 + thirdLayer)) & 1) != 0;
            if (thirdIsSecond) {
                blendBackColor = lineThirdColor[x];
                blendBackLayer = thirdLayer;
                sndIsSecond = true;
            }
        }
        if (!sndIsSecond) {
            bool backdropIsSecond = ((bldcnt >> (8 + LAYER_BD)) & 1) != 0;
            if (backdropIsSecond) {
                blendBackColor = lineBackdropColor;
                blendBackLayer = LAYER_BD;
                sndIsSecond = true;
            }
        }
        (void)blendBackLayer;

        if (fxAllowed) {
            if (lineTopSemiTransparent[x] && sndIsSecond) {
                top = blendColorsAlpha(top, blendBackColor, eva, evb);
            } else if (effect == 1 && topIsFirst && sndIsSecond) {
                top = blendColorsAlpha(top, blendBackColor, eva, evb);
            } else if (effect == 2 && topIsFirst) {
                top = brightenColor(top, evy);
            } else if (effect == 3 && topIsFirst) {
                top = darkenColor(top, evy);
            }
        }
        framebuf[y * SCREEN_W + x] = top;
    }
}

void PPU::renderCurrentLine(const std::array<u8, 96 * 1024>& vramBytes, const std::array<u8, 1024>& paletteBytes, const std::array<u8, 1024>& oamBytes) {
    int y = static_cast<int>(vcount);
    if (y >= SCREEN_H) return;
    clearLine(y);
    u16 backdrop = readPaletteColor(paletteBytes, 0);
    lineBackdropColor = backdrop;
    for (int x = 0; x < SCREEN_W; ++x) {
        lineTopColor[x] = backdrop;
        lineTopLayer[x] = LAYER_BD;
        lineTopPrio[x] = 4;
        lineSecondColor[x] = backdrop;
        lineSecondLayer[x] = LAYER_BD;
        lineSecondPrio[x] = 4;
        lineThirdColor[x] = backdrop;
        lineThirdLayer[x] = LAYER_BD;
        lineThirdPrio[x] = 4;
    }

    if (dispcnt & (1 << 7)) {
        for (int x = 0; x < SCREEN_W; ++x) framebuf[y * SCREEN_W + x] = 0x7FFF;
        return;
    }

    renderOBJWindowMaskLine(vramBytes, oamBytes);
    buildWindowMaskLine(y);

    switch (dispcnt & 0x7) {
        case 0:
            for (int pr = 3; pr >= 0; --pr) {
                for (int bg = 3; bg >= 0; --bg) {
                    if ((bgcnt[bg] & 0x3) == pr) renderMode0BGLine(bg, vramBytes, paletteBytes);
                }
            }
            break;
        case 1:
            for (int pr = 3; pr >= 0; --pr) {
                if ((bgcnt[2] & 0x3) == pr) renderAffineBGLine(2, vramBytes, paletteBytes);
                for (int bg = 1; bg >= 0; --bg) {
                    if ((bgcnt[bg] & 0x3) == pr) renderMode0BGLine(bg, vramBytes, paletteBytes);
                }
            }
            break;
        case 2:
            for (int pr = 3; pr >= 0; --pr) {
                if ((bgcnt[3] & 0x3) == pr) renderAffineBGLine(3, vramBytes, paletteBytes);
                if ((bgcnt[2] & 0x3) == pr) renderAffineBGLine(2, vramBytes, paletteBytes);
            }
            break;
        case 3: renderMode3Line(vramBytes); break;
        case 4: renderMode4Line(vramBytes, paletteBytes); break;
        case 5: renderMode5Line(vramBytes); break;
        default: break;
    }

    renderOBJLine(vramBytes, paletteBytes, oamBytes);
    finalizeLine(y);
}

void PPU::tick(int cycles, const std::array<u8, 96 * 1024>& vramBytes, const std::array<u8, 1024>& paletteBytes, const std::array<u8, 1024>& oamBytes) {
    while (cycles > 0) {
        if (lineStartPendingRender && dotCycles == 0 && vcount < SCREEN_H) {
            for (int i = 0; i < 2; ++i) {
                affinePaLatched[i] = static_cast<int16_t>(bgpa[i]);
                affinePbLatched[i] = static_cast<int16_t>(bgpb[i]);
                affinePcLatched[i] = static_cast<int16_t>(bgpc[i]);
                affinePdLatched[i] = static_cast<int16_t>(bgpd[i]);
            }
            renderCurrentLine(vramBytes, paletteBytes, oamBytes);
            lineStartPendingRender = false;
        }

        int nextDot = (dotCycles < 960) ? 960 : 1232;
        int step = std::min(cycles, nextDot - dotCycles);
        dotCycles += step;
        cycles -= step;

        if (dotCycles == 960) {
            if (!(dispstat & (1 << 1))) enterHBlank();
        }

        if (dotCycles == 1232) {
            if (vcount < 160) {
                // If BGxX/BGxY was rewritten during HBlank, the written value should
                // become the next-line base directly (avoid an extra PB/PD step).
                if (!affineRefWrittenX[0]) {
                    bgxInternal[0] = wrapAffineRef28(bgxInternal[0] + affinePbLatched[0]);
                }
                if (!affineRefWrittenY[0]) {
                    bgyInternal[0] = wrapAffineRef28(bgyInternal[0] + affinePdLatched[0]);
                }
                if (!affineRefWrittenX[1]) {
                    bgxInternal[1] = wrapAffineRef28(bgxInternal[1] + affinePbLatched[1]);
                }
                if (!affineRefWrittenY[1]) {
                    bgyInternal[1] = wrapAffineRef28(bgyInternal[1] + affinePdLatched[1]);
                }
                affineRefWrittenX[0] = affineRefWrittenX[1] = false;
                affineRefWrittenY[0] = affineRefWrittenY[1] = false;
            }
            dotCycles = 0;
            dispstat &= ~(1 << 1);
            vcount++;
            if (vcount == 160) enterVBlank();
            if (vcount == 227) dispstat &= ~(1 << 0);
            if (vcount >= 228) {
                vcount = 0;
                vblankRaisedThisFrame = false;
                bgxInternal[0] = wrapAffineRef28(bgx[0]);
                bgyInternal[0] = wrapAffineRef28(bgy[0]);
                bgxInternal[1] = wrapAffineRef28(bgx[1]);
                bgyInternal[1] = wrapAffineRef28(bgy[1]);
                affineRefWrittenX[0] = affineRefWrittenX[1] = false;
                affineRefWrittenY[0] = affineRefWrittenY[1] = false;
                ++frameCounter;
            }
            updateVCounterFlag(true);
            lineStartPendingRender = (vcount < SCREEN_H);
        }
    }
}

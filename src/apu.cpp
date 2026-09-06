#include "apu.hpp"
#include "bus.hpp"
#include "state_io.hpp"
#include <algorithm>
#include <cmath>

void APU::setOutputRate(int hz) {
    if (hz <= 4000 || hz >= 192000) return;
    outputRate = hz;
    resamplePos = 0.0;
    mixQueue.clear();
    sampleQueue.clear();
}

void APU::reset() {
    soundcnt_l = 0;
    soundcnt_h = 0;
    soundcnt_x = 0;
    soundbias = 0x0200;
    nr10 = nr11 = nr12 = nr13 = nr14 = 0;
    nr21 = nr22 = nr23 = nr24 = 0;
    nr30 = nr31 = nr32 = nr33 = nr34 = 0;
    nr41 = nr42 = nr43 = nr44 = 0;
    waveRam.fill(0);
    sq1 = {};
    sq2 = {};
    wave = {};
    noise = {};
    fifoA.clear();
    fifoB.clear();
    sampleA = 0;
    sampleB = 0;
    mixAccum = 0;
    frameSeqAccum = 0;
    frameSeqStep = 0;
    mixQueue.clear();
    resamplePos = 0.0;
    sampleQueue.clear();
    lastOutL = 0;
    lastOutR = 0;
    directFilterL = 0.0;
    directFilterR = 0.0;
}

u16 APU::readSOUNDCNT_X() const {
    u16 status = static_cast<u16>(soundcnt_x & (1 << 7));
    if (sq1.enabled) status |= 1 << 0;
    if (sq2.enabled) status |= 1 << 1;
    if (wave.enabled) status |= 1 << 2;
    if (noise.enabled) status |= 1 << 3;
    return status;
}

void APU::writeSOUNDCNT_H(u16 value) {
    soundcnt_h = value;
    if (soundcnt_h & (1 << 11)) {
        fifoA.clear();
        sampleA = 0;
        soundcnt_h &= ~(1 << 11);
    }
    if (soundcnt_h & (1 << 15)) {
        fifoB.clear();
        sampleB = 0;
        soundcnt_h &= ~(1 << 15);
    }
}

void APU::writeSOUNDCNT_X(u16 value) {
    bool wasOn = (soundcnt_x & (1 << 7)) != 0;
    soundcnt_x = static_cast<u16>(value & (1 << 7));
    bool nowOn = (soundcnt_x & (1 << 7)) != 0;
    if (wasOn && !nowOn) {
        sq1.enabled = false;
        sq2.enabled = false;
        wave.enabled = false;
        noise.enabled = false;
        fifoA.clear();
        fifoB.clear();
        sampleA = 0;
        sampleB = 0;
        mixQueue.clear();
        sampleQueue.clear();
        lastOutL = 0;
        lastOutR = 0;
    }
}

u16 APU::readIO16(u32 addr) const {
    switch (addr) {
        case 0x04000060: return nr10;
        case 0x04000062: return static_cast<u16>(nr11 | (nr12 << 8));
        case 0x04000064: return static_cast<u16>(nr13 | (nr14 << 8));
        case 0x04000068: return static_cast<u16>(nr21 | (nr22 << 8));
        case 0x0400006C: return static_cast<u16>(nr23 | (nr24 << 8));
        case 0x04000070: return nr30;
        case 0x04000072: return static_cast<u16>(nr31 | (nr32 << 8));
        case 0x04000074: return static_cast<u16>(nr33 | (nr34 << 8));
        case 0x04000078: return static_cast<u16>(nr41 | (nr42 << 8));
        case 0x0400007C: return static_cast<u16>(nr43 | (nr44 << 8));
        case 0x04000080: return readSOUNDCNT_L();
        case 0x04000082: return readSOUNDCNT_H();
        case 0x04000084: return readSOUNDCNT_X();
        case 0x04000088: return soundbias;
        default:
            if (addr >= 0x04000090 && addr <= 0x0400009E && (addr & 1u) == 0) {
                int idx = static_cast<int>(addr - 0x04000090);
                int bankBase = wave.bankSelect ? 16 : 0;
                return static_cast<u16>(waveRam[bankBase + idx] | (waveRam[bankBase + idx + 1] << 8));
            }
            break;
    }
    return 0;
}

bool APU::writeIO16(u32 addr, u16 value) {
    auto dacOn = [](u8 env) { return (env & 0xF8) != 0; };
    switch (addr) {
        case 0x04000060:
            nr10 = static_cast<u8>(value & 0xFF);
            return true;
        case 0x04000062:
            nr11 = static_cast<u8>(value & 0xFF);
            nr12 = static_cast<u8>((value >> 8) & 0xFF);
            if (!dacOn(nr12)) sq1.enabled = false;
            return true;
        case 0x04000064:
            nr13 = static_cast<u8>(value & 0xFF);
            nr14 = static_cast<u8>((value >> 8) & 0xFF);
            if (nr14 & (1 << 7)) triggerSquare1();
            return true;
        case 0x04000068:
            nr21 = static_cast<u8>(value & 0xFF);
            nr22 = static_cast<u8>((value >> 8) & 0xFF);
            if (!dacOn(nr22)) sq2.enabled = false;
            return true;
        case 0x0400006C:
            nr23 = static_cast<u8>(value & 0xFF);
            nr24 = static_cast<u8>((value >> 8) & 0xFF);
            if (nr24 & (1 << 7)) triggerSquare2();
            return true;
        case 0x04000070:
            nr30 = static_cast<u8>(value & 0xFF);
            wave.dacEnable = (nr30 & (1 << 7)) != 0;
            wave.bankSelect = (nr30 & (1 << 5)) ? 1 : 0;
            wave.dimension64 = (nr30 & (1 << 6)) != 0;
            if (!wave.dacEnable) wave.enabled = false;
            return true;
        case 0x04000072:
            nr31 = static_cast<u8>(value & 0xFF);
            nr32 = static_cast<u8>((value >> 8) & 0xFF);
            wave.forceVolume75 = (nr32 & (1 << 7)) != 0;
            return true;
        case 0x04000074:
            nr33 = static_cast<u8>(value & 0xFF);
            nr34 = static_cast<u8>((value >> 8) & 0xFF);
            if (nr34 & (1 << 7)) triggerWave();
            return true;
        case 0x04000078:
            nr41 = static_cast<u8>(value & 0xFF);
            nr42 = static_cast<u8>((value >> 8) & 0xFF);
            if (!dacOn(nr42)) noise.enabled = false;
            return true;
        case 0x0400007C:
            nr43 = static_cast<u8>(value & 0xFF);
            nr44 = static_cast<u8>((value >> 8) & 0xFF);
            if (nr44 & (1 << 7)) triggerNoise();
            return true;
        case 0x04000080: writeSOUNDCNT_L(value); return true;
        case 0x04000082: writeSOUNDCNT_H(value); return true;
        case 0x04000084: writeSOUNDCNT_X(value); return true;
        case 0x04000088:
            soundbias = value;
            return true;
        default:
            if (addr >= 0x04000090 && addr <= 0x0400009E && (addr & 1u) == 0) {
                int idx = static_cast<int>(addr - 0x04000090);
                int bankBase = wave.bankSelect ? 16 : 0;
                waveRam[bankBase + idx] = static_cast<u8>(value & 0xFF);
                waveRam[bankBase + idx + 1] = static_cast<u8>((value >> 8) & 0xFF);
                return true;
            }
            break;
    }
    return false;
}

void APU::pushFIFOBytes(std::deque<int8_t>& fifo, u16 value) {
    if (fifo.size() < FIFO_MAX_BYTES) fifo.push_back(static_cast<int8_t>(value & 0xFF));
    if (fifo.size() < FIFO_MAX_BYTES) fifo.push_back(static_cast<int8_t>((value >> 8) & 0xFF));
}

void APU::writeFIFOA16(u16 value) {
    pushFIFOBytes(fifoA, value);
}

void APU::writeFIFOB16(u16 value) {
    pushFIFOBytes(fifoB, value);
}

void APU::clockFIFO(Bus& bus, bool channelB) {
    auto& fifo = channelB ? this->fifoB : this->fifoA;
    int8_t& out = channelB ? sampleB : sampleA;
    if (!fifo.empty()) {
        out = fifo.front();
        fifo.pop_front();
    }
    // On real hardware, an empty FIFO just keeps outputting whatever sample was
    // last latched (the DAC holds its last value) instead of snapping to
    // silence. Forcing `out = 0` here on every underrun produced an audible
    // click/crackle each time -- since this ROM streams its whole audio track
    // through the FIFO, any DMA refill jitter caused frequent tiny silence
    // gaps that were heard as crackling. Just holding the last sample removes
    // that click; the FIFO refill below still runs the same as before.
    if (fifo.size() <= 16) {
        bus.dma.triggerSoundFIFO(bus, channelB);
    }
}

void APU::onTimerOverflow(Bus& bus, int timerIndex, int count) {
    if ((soundcnt_x & (1 << 7)) == 0) return;
    bool timerA1 = (soundcnt_h & (1 << 10)) != 0;
    bool timerB1 = (soundcnt_h & (1 << 14)) != 0;

    for (int i = 0; i < count; ++i) {
        if ((timerA1 ? 1 : 0) == timerIndex) clockFIFO(bus, false);
        if ((timerB1 ? 1 : 0) == timerIndex) clockFIFO(bus, true);
    }
}

void APU::clockLength() {
    auto clockSq = [](SquareChan& ch) {
        if (!ch.enabled || !ch.lengthEnable || ch.lengthCounter <= 0) return;
        if (--ch.lengthCounter == 0) ch.enabled = false;
    };
    clockSq(sq1);
    clockSq(sq2);
    if (wave.enabled && wave.lengthEnable && wave.lengthCounter > 0) {
        if (--wave.lengthCounter == 0) wave.enabled = false;
    }
    if (noise.enabled && noise.lengthEnable && noise.lengthCounter > 0) {
        if (--noise.lengthCounter == 0) noise.enabled = false;
    }
}

void APU::clockEnvelope() {
    auto clockEnv = [](SquareChan& ch) {
        if (!ch.enabled || ch.envPeriod == 0) return;
        if (--ch.envTimer > 0) return;
        ch.envTimer = ch.envPeriod;
        if (ch.envInc) {
            if (ch.envVolume < 15) ch.envVolume++;
        } else {
            if (ch.envVolume > 0) ch.envVolume--;
        }
    };
    clockEnv(sq1);
    clockEnv(sq2);
    if (noise.enabled && noise.envPeriod != 0) {
        if (--noise.envTimer <= 0) {
            noise.envTimer = noise.envPeriod;
            if (noise.envInc) {
                if (noise.envVolume < 15) noise.envVolume++;
            } else {
                if (noise.envVolume > 0) noise.envVolume--;
            }
        }
    }
}

void APU::tickFrameSequencer(int cycles) {
    constexpr int frameSeqCycles = CPU_HZ / 512;
    frameSeqAccum += cycles;
    while (frameSeqAccum >= frameSeqCycles) {
        frameSeqAccum -= frameSeqCycles;
        if ((frameSeqStep & 1) == 0) clockLength();
        if (frameSeqStep == 2 || frameSeqStep == 6) clockSweep();
        if (frameSeqStep == 7) clockEnvelope();
        frameSeqStep = (frameSeqStep + 1) & 7;
    }
}

int APU::sweepComputeNextFreq(int shadow, bool negate, int shift) const {
    int delta = (shift > 0) ? (shadow >> shift) : 0;
    return negate ? (shadow - delta) : (shadow + delta);
}

void APU::clockSweep() {
    if (!sq1.enabled || !sq1.sweepEnabled) return;
    if (sq1.sweepPeriod <= 0) return;
    if (--sq1.sweepTimer > 0) return;
    sq1.sweepTimer = sq1.sweepPeriod;
    if (sq1.sweepShift == 0) return;
    int next = sweepComputeNextFreq(sq1.sweepShadow, sq1.sweepNegate, sq1.sweepShift);
    if (next < 0 || next > 2047) {
        sq1.enabled = false;
        return;
    }
    sq1.sweepShadow = next;
    sq1.frequency = next;
    // Overflow check for next prospective sweep step.
    int chk = sweepComputeNextFreq(sq1.sweepShadow, sq1.sweepNegate, sq1.sweepShift);
    if (chk < 0 || chk > 2047) sq1.enabled = false;
}

void APU::triggerSquare1() {
    sq1.duty = (nr11 >> 6) & 0x3;
    sq1.frequency = ((nr14 & 0x7) << 8) | nr13;
    sq1.lengthEnable = (nr14 & (1 << 6)) != 0;
    if (sq1.lengthCounter == 0) {
        sq1.lengthCounter = 64 - (nr11 & 0x3F);
        if (sq1.lengthCounter <= 0) sq1.lengthCounter = 64;
    }
    sq1.envVolume = (nr12 >> 4) & 0xF;
    sq1.envInc = (nr12 & (1 << 3)) != 0;
    sq1.envPeriod = nr12 & 0x7;
    sq1.envTimer = sq1.envPeriod ? sq1.envPeriod : 8;
    sq1.sweepPeriod = (nr10 >> 4) & 0x7;
    if (sq1.sweepPeriod == 0) sq1.sweepPeriod = 8;
    sq1.sweepNegate = (nr10 & (1 << 3)) != 0;
    sq1.sweepShift = nr10 & 0x7;
    sq1.sweepShadow = sq1.frequency;
    sq1.sweepTimer = sq1.sweepPeriod;
    sq1.sweepEnabled = (((nr10 >> 4) & 0x7) != 0) || (sq1.sweepShift != 0);
    sq1.phase = 0.0;
    sq1.enabled = (nr12 & 0xF8) != 0;
    if (sq1.sweepShift != 0) {
        int chk = sweepComputeNextFreq(sq1.sweepShadow, sq1.sweepNegate, sq1.sweepShift);
        if (chk < 0 || chk > 2047) sq1.enabled = false;
    }
}

void APU::triggerSquare2() {
    sq2.duty = (nr21 >> 6) & 0x3;
    sq2.frequency = ((nr24 & 0x7) << 8) | nr23;
    sq2.lengthEnable = (nr24 & (1 << 6)) != 0;
    if (sq2.lengthCounter == 0) {
        sq2.lengthCounter = 64 - (nr21 & 0x3F);
        if (sq2.lengthCounter <= 0) sq2.lengthCounter = 64;
    }
    sq2.envVolume = (nr22 >> 4) & 0xF;
    sq2.envInc = (nr22 & (1 << 3)) != 0;
    sq2.envPeriod = nr22 & 0x7;
    sq2.envTimer = sq2.envPeriod ? sq2.envPeriod : 8;
    sq2.phase = 0.0;
    sq2.enabled = (nr22 & 0xF8) != 0;
}

void APU::triggerWave() {
    wave.dacEnable = (nr30 & (1 << 7)) != 0;
    if (!wave.dacEnable) {
        wave.enabled = false;
        return;
    }
    wave.frequency = ((nr34 & 0x7) << 8) | nr33;
    wave.lengthEnable = (nr34 & (1 << 6)) != 0;
    if (wave.lengthCounter == 0) {
        wave.lengthCounter = 256 - nr31;
        if (wave.lengthCounter <= 0) wave.lengthCounter = 256;
    }
    wave.volumeCode = (nr32 >> 5) & 0x3;
    wave.forceVolume75 = (nr32 & (1 << 7)) != 0;
    wave.bankSelect = (nr30 & (1 << 5)) ? 1 : 0;
    wave.dimension64 = (nr30 & (1 << 6)) != 0;
    wave.sampleIndex = 0;
    wave.phase = 0.0;
    wave.enabled = true;
}

void APU::triggerNoise() {
    if ((nr42 & 0xF8) == 0) {
        noise.enabled = false;
        return;
    }
    noise.lengthEnable = (nr44 & (1 << 6)) != 0;
    if (noise.lengthCounter == 0) {
        noise.lengthCounter = 64 - (nr41 & 0x3F);
        if (noise.lengthCounter <= 0) noise.lengthCounter = 64;
    }
    noise.envVolume = (nr42 >> 4) & 0xF;
    noise.envInc = (nr42 & (1 << 3)) != 0;
    noise.envPeriod = nr42 & 0x7;
    noise.envTimer = noise.envPeriod ? noise.envPeriod : 8;
    noise.lfsr = 0x7FFF;
    noise.phase = 0.0;
    noise.enabled = true;
}

int APU::sampleSquare(SquareChan& ch) {
    if (!ch.enabled || ch.envVolume <= 0) return 0;
    if (ch.frequency >= 2048) return 0;
    static constexpr double dutyTable[4] = { 0.125, 0.25, 0.5, 0.75 };
    double hz = 131072.0 / static_cast<double>(2048 - ch.frequency);
    // A single instantaneous sample of a hard 0/1 square wave has harmonic
    // energy well above the internal 32768 Hz mixer's Nyquist frequency; that
    // energy aliases back down as harsh, inharmonic overtones, which is what
    // made PSG tones sound sharp/piercing. Supersampling within this output
    // period and averaging approximates a band-limited waveform (a cheap box
    // filter) and removes most of that aliasing, at negligible extra cost.
    constexpr int kSuper = 8;
    const double stepPhase = hz / (static_cast<double>(MIX_RATE) * kSuper);
    const double duty = dutyTable[ch.duty & 0x3];
    int acc = 0;
    for (int i = 0; i < kSuper; ++i) {
        ch.phase += stepPhase;
        while (ch.phase >= 1.0) ch.phase -= 1.0;
        acc += (ch.phase < duty) ? 1 : -1;
    }
    return (acc * ch.envVolume) / kSuper;
}

int APU::sampleWave() {
    if (!wave.enabled || !wave.dacEnable) return 0;
    if (wave.frequency >= 2048) return 0;

    int totalSamples = wave.dimension64 ? 64 : 32;
    int idx = wave.sampleIndex % totalSamples;
    int byteOff = 0;
    if (wave.dimension64) {
        byteOff = (idx >> 1); // full 64 samples over 32 bytes
    } else {
        int bankBase = wave.bankSelect ? 16 : 0;
        byteOff = bankBase + (idx >> 1); // selected 32-sample bank
    }

    // Sample first, then advance phase so trigger starts from sample 0 timing.
    u8 packed = waveRam[byteOff & 0x1F];
    int raw4 = (idx & 1) ? (packed & 0xF) : (packed >> 4);
    int centered = raw4 - 8;
    int out = 0;
    if (wave.forceVolume75) {
        out = (centered * 3) / 4;
    } else {
        switch (wave.volumeCode) {
            case 0: out = 0; break;
            case 1: out = centered; break;
            case 2: out = centered / 2; break;
            case 3: out = centered / 4; break;
            default: out = 0; break;
        }
    }

    double stepHz = 2097152.0 / static_cast<double>(2048 - wave.frequency);
    wave.phase += stepHz / static_cast<double>(MIX_RATE);
    while (wave.phase >= 1.0) {
        wave.phase -= 1.0;
        wave.sampleIndex = (wave.sampleIndex + 1) % totalSamples;
    }
    return out;
}

int APU::noiseFrequency() const {
    int r = nr43 & 0x7;
    int divisor = (r == 0) ? 8 : (r * 16);
    int shift = (nr43 >> 4) & 0xF;
    int hz = 524288 / divisor;
    hz >>= (shift + 1);
    if (hz < 1) hz = 1;
    return hz;
}

int APU::sampleNoise() {
    if (!noise.enabled || noise.envVolume <= 0) return 0;
    // Same aliasing problem as the square channels (see sampleSquare), and
    // noise is the worst offender since it toggles on every LFSR clock -- so
    // it gets the same supersample-and-average treatment.
    constexpr int kSuper = 8;
    const double stepPhase = static_cast<double>(noiseFrequency()) / (static_cast<double>(MIX_RATE) * kSuper);
    const bool width7 = (nr43 & (1 << 3)) != 0;
    int acc = 0;
    for (int i = 0; i < kSuper; ++i) {
        noise.phase += stepPhase;
        while (noise.phase >= 1.0) {
            noise.phase -= 1.0;
            u16 bit = static_cast<u16>((noise.lfsr ^ (noise.lfsr >> 1)) & 1);
            noise.lfsr = static_cast<u16>((noise.lfsr >> 1) | (bit << 14));
            if (width7) noise.lfsr = static_cast<u16>((noise.lfsr & ~(1 << 6)) | (bit << 6));
        }
        acc += ((~noise.lfsr) & 1) ? 1 : -1;
    }
    return (acc * noise.envVolume) / kSuper;
}

void APU::mixOneInternalSample() {
    if ((soundcnt_x & (1 << 7)) == 0) {
        mixQueue.push_back(0);
        mixQueue.push_back(0);
        return;
    }

    int directLeft = 0;
    int directRight = 0;

    int a = sampleA;
    int b = sampleB;
    if ((soundcnt_h & (1 << 2)) == 0) a >>= 1;
    if ((soundcnt_h & (1 << 3)) == 0) b >>= 1;

    if (soundcnt_h & (1 << 9)) directLeft += a;
    if (soundcnt_h & (1 << 8)) directRight += a;
    if (soundcnt_h & (1 << 13)) directLeft += b;
    if (soundcnt_h & (1 << 12)) directRight += b;

    // Smooth the direct-sound path only (see directFilterL/R comment in the
    // header) before it's combined with the PSG channels below.
    constexpr double kDirectFilterAlpha = 0.5;
    directFilterL += (static_cast<double>(directLeft) - directFilterL) * kDirectFilterAlpha;
    directFilterR += (static_cast<double>(directRight) - directFilterR) * kDirectFilterAlpha;
    directLeft = static_cast<int>(std::lround(directFilterL));
    directRight = static_cast<int>(std::lround(directFilterR));

    int ch1 = sampleSquare(sq1);
    int ch2 = sampleSquare(sq2);
    int ch3 = sampleWave();
    int ch4 = sampleNoise();

    int psgLeft = 0;
    int psgRight = 0;
    if (soundcnt_l & (1 << 12)) psgLeft += ch1;
    if (soundcnt_l & (1 << 13)) psgLeft += ch2;
    if (soundcnt_l & (1 << 14)) psgLeft += ch3;
    if (soundcnt_l & (1 << 15)) psgLeft += ch4;
    if (soundcnt_l & (1 << 8)) psgRight += ch1;
    if (soundcnt_l & (1 << 9)) psgRight += ch2;
    if (soundcnt_l & (1 << 10)) psgRight += ch3;
    if (soundcnt_l & (1 << 11)) psgRight += ch4;

    int leftMaster = (soundcnt_l >> 4) & 0x7;
    int rightMaster = soundcnt_l & 0x7;
    psgLeft *= leftMaster;
    psgRight *= rightMaster;

    // SOUNDCNT_H PSG master ratio: 00=25%, 01=50%, 10/11=100%.
    int psgRatioSel = soundcnt_h & 0x3;
    int psgNum = 1;
    int psgDen = 4;
    if (psgRatioSel == 1) {
        psgDen = 2;
    } else if (psgRatioSel >= 2) {
        psgDen = 1;
    }
    psgLeft = (psgLeft * psgNum) / psgDen;
    psgRight = (psgRight * psgNum) / psgDen;

    // Convert to GBA-like 10-bit DAC domain around SOUNDBIAS center.
    int mixRawL = directLeft * 2 + (psgLeft / 2);
    int mixRawR = directRight * 2 + (psgRight / 2);

    int bias = static_cast<int>(soundbias & 0x03FF);
    int dacL = std::clamp(bias + mixRawL, 0, 0x3FF);
    int dacR = std::clamp(bias + mixRawR, 0, 0x3FF);
    int outL = (dacL - 0x200) << 6;
    int outR = (dacR - 0x200) << 6;

    mixQueue.push_back(static_cast<int16_t>(std::clamp(outL, -32768, 32767)));
    mixQueue.push_back(static_cast<int16_t>(std::clamp(outR, -32768, 32767)));
}

void APU::runResampler() {
    if (mixQueue.size() < 4) return;
    const double step = static_cast<double>(MIX_RATE) / static_cast<double>(outputRate);
    while ((mixQueue.size() / 2) >= 2) {
        size_t i0 = static_cast<size_t>(resamplePos);
        size_t framesAvail = mixQueue.size() / 2;
        if (i0 + 1 >= framesAvail) break;
        double frac = resamplePos - static_cast<double>(i0);
        int l0 = mixQueue[i0 * 2];
        int r0 = mixQueue[i0 * 2 + 1];
        int l1 = mixQueue[(i0 + 1) * 2];
        int r1 = mixQueue[(i0 + 1) * 2 + 1];
        int outL = static_cast<int>(std::lround(l0 + (l1 - l0) * frac));
        int outR = static_cast<int>(std::lround(r0 + (r1 - r0) * frac));
        // Light smoothing to reduce zipper/noise when timer-fed FIFOs step.
        outL = (outL * 3 + lastOutL) / 4;
        outR = (outR * 3 + lastOutR) / 4;
        lastOutL = outL;
        lastOutR = outR;
        sampleQueue.push_back(static_cast<int16_t>(std::clamp(outL, -32768, 32767)));
        sampleQueue.push_back(static_cast<int16_t>(std::clamp(outR, -32768, 32767)));

        resamplePos += step;
        size_t dropFrames = static_cast<size_t>(resamplePos);
        if (dropFrames > 0) {
            size_t dropSamples = std::min(dropFrames * 2, mixQueue.size() >= 2 ? mixQueue.size() - 2 : size_t{0});
            for (size_t i = 0; i < dropSamples; ++i) mixQueue.pop_front();
            resamplePos -= static_cast<double>(dropFrames);
        }
    }
}

void APU::tick(int cycles) {
    tickFrameSequencer(cycles);
    mixAccum += static_cast<int64_t>(cycles) * MIX_RATE;
    while (mixAccum >= CPU_HZ) {
        mixAccum -= CPU_HZ;
        mixOneInternalSample();
    }
    runResampler();
}

std::vector<int16_t> APU::consumeSamples() {
    std::vector<int16_t> out;
    out.swap(sampleQueue);
    return out;
}

bool APU::saveState(std::ostream& os) const {
    using namespace stateio;
    if (!writePod(os, soundcnt_l)) return false;
    if (!writePod(os, soundcnt_h)) return false;
    if (!writePod(os, soundcnt_x)) return false;
    if (!writePod(os, soundbias)) return false;
    if (!writePod(os, nr10)) return false;
    if (!writePod(os, nr11)) return false;
    if (!writePod(os, nr12)) return false;
    if (!writePod(os, nr13)) return false;
    if (!writePod(os, nr14)) return false;
    if (!writePod(os, nr21)) return false;
    if (!writePod(os, nr22)) return false;
    if (!writePod(os, nr23)) return false;
    if (!writePod(os, nr24)) return false;
    if (!writePod(os, nr30)) return false;
    if (!writePod(os, nr31)) return false;
    if (!writePod(os, nr32)) return false;
    if (!writePod(os, nr33)) return false;
    if (!writePod(os, nr34)) return false;
    if (!writePod(os, nr41)) return false;
    if (!writePod(os, nr42)) return false;
    if (!writePod(os, nr43)) return false;
    if (!writePod(os, nr44)) return false;
    if (!writeArray(os, waveRam)) return false;

    auto writeSquare = [&](const SquareChan& c) -> bool {
        return writeBool(os, c.enabled) &&
               writeBool(os, c.lengthEnable) &&
               writePod(os, c.lengthCounter) &&
               writePod(os, c.duty) &&
               writePod(os, c.frequency) &&
               writePod(os, c.envVolume) &&
               writePod(os, c.envPeriod) &&
               writeBool(os, c.envInc) &&
               writePod(os, c.envTimer) &&
               writePod(os, c.phase) &&
               writePod(os, c.sweepPeriod) &&
               writeBool(os, c.sweepNegate) &&
               writePod(os, c.sweepShift) &&
               writePod(os, c.sweepTimer) &&
               writePod(os, c.sweepShadow) &&
               writeBool(os, c.sweepEnabled);
    };
    if (!writeSquare(sq1)) return false;
    if (!writeSquare(sq2)) return false;

    if (!writeBool(os, wave.enabled)) return false;
    if (!writeBool(os, wave.dacEnable)) return false;
    if (!writeBool(os, wave.lengthEnable)) return false;
    if (!writePod(os, wave.lengthCounter)) return false;
    if (!writePod(os, wave.volumeCode)) return false;
    if (!writeBool(os, wave.forceVolume75)) return false;
    if (!writePod(os, wave.frequency)) return false;
    if (!writePod(os, wave.sampleIndex)) return false;
    if (!writeBool(os, wave.dimension64)) return false;
    if (!writePod(os, wave.bankSelect)) return false;
    if (!writePod(os, wave.phase)) return false;

    if (!writeBool(os, noise.enabled)) return false;
    if (!writeBool(os, noise.lengthEnable)) return false;
    if (!writePod(os, noise.lengthCounter)) return false;
    if (!writePod(os, noise.envVolume)) return false;
    if (!writePod(os, noise.envPeriod)) return false;
    if (!writeBool(os, noise.envInc)) return false;
    if (!writePod(os, noise.envTimer)) return false;
    if (!writePod(os, noise.lfsr)) return false;
    if (!writePod(os, noise.phase)) return false;

    if (!writeDeque(os, fifoA)) return false;
    if (!writeDeque(os, fifoB)) return false;
    if (!writePod(os, sampleA)) return false;
    if (!writePod(os, sampleB)) return false;
    if (!writePod(os, outputRate)) return false;
    if (!writePod(os, mixAccum)) return false;
    if (!writePod(os, frameSeqAccum)) return false;
    if (!writePod(os, frameSeqStep)) return false;
    if (!writeDeque(os, mixQueue)) return false;
    if (!writePod(os, resamplePos)) return false;
    if (!writePod(os, lastOutL)) return false;
    if (!writePod(os, lastOutR)) return false;
    if (!writeVector(os, sampleQueue)) return false;
    return true;
}

bool APU::loadState(std::istream& is) {
    using namespace stateio;
    if (!readPod(is, soundcnt_l)) return false;
    if (!readPod(is, soundcnt_h)) return false;
    if (!readPod(is, soundcnt_x)) return false;
    if (!readPod(is, soundbias)) return false;
    if (!readPod(is, nr10)) return false;
    if (!readPod(is, nr11)) return false;
    if (!readPod(is, nr12)) return false;
    if (!readPod(is, nr13)) return false;
    if (!readPod(is, nr14)) return false;
    if (!readPod(is, nr21)) return false;
    if (!readPod(is, nr22)) return false;
    if (!readPod(is, nr23)) return false;
    if (!readPod(is, nr24)) return false;
    if (!readPod(is, nr30)) return false;
    if (!readPod(is, nr31)) return false;
    if (!readPod(is, nr32)) return false;
    if (!readPod(is, nr33)) return false;
    if (!readPod(is, nr34)) return false;
    if (!readPod(is, nr41)) return false;
    if (!readPod(is, nr42)) return false;
    if (!readPod(is, nr43)) return false;
    if (!readPod(is, nr44)) return false;
    if (!readArray(is, waveRam)) return false;

    auto readSquare = [&](SquareChan& c) -> bool {
        return readBool(is, c.enabled) &&
               readBool(is, c.lengthEnable) &&
               readPod(is, c.lengthCounter) &&
               readPod(is, c.duty) &&
               readPod(is, c.frequency) &&
               readPod(is, c.envVolume) &&
               readPod(is, c.envPeriod) &&
               readBool(is, c.envInc) &&
               readPod(is, c.envTimer) &&
               readPod(is, c.phase) &&
               readPod(is, c.sweepPeriod) &&
               readBool(is, c.sweepNegate) &&
               readPod(is, c.sweepShift) &&
               readPod(is, c.sweepTimer) &&
               readPod(is, c.sweepShadow) &&
               readBool(is, c.sweepEnabled);
    };
    if (!readSquare(sq1)) return false;
    if (!readSquare(sq2)) return false;

    if (!readBool(is, wave.enabled)) return false;
    if (!readBool(is, wave.dacEnable)) return false;
    if (!readBool(is, wave.lengthEnable)) return false;
    if (!readPod(is, wave.lengthCounter)) return false;
    if (!readPod(is, wave.volumeCode)) return false;
    if (!readBool(is, wave.forceVolume75)) return false;
    if (!readPod(is, wave.frequency)) return false;
    if (!readPod(is, wave.sampleIndex)) return false;
    if (!readBool(is, wave.dimension64)) return false;
    if (!readPod(is, wave.bankSelect)) return false;
    if (!readPod(is, wave.phase)) return false;

    if (!readBool(is, noise.enabled)) return false;
    if (!readBool(is, noise.lengthEnable)) return false;
    if (!readPod(is, noise.lengthCounter)) return false;
    if (!readPod(is, noise.envVolume)) return false;
    if (!readPod(is, noise.envPeriod)) return false;
    if (!readBool(is, noise.envInc)) return false;
    if (!readPod(is, noise.envTimer)) return false;
    if (!readPod(is, noise.lfsr)) return false;
    if (!readPod(is, noise.phase)) return false;

    if (!readDeque(is, fifoA)) return false;
    if (!readDeque(is, fifoB)) return false;
    if (!readPod(is, sampleA)) return false;
    if (!readPod(is, sampleB)) return false;
    if (!readPod(is, outputRate)) return false;
    if (!readPod(is, mixAccum)) return false;
    if (!readPod(is, frameSeqAccum)) return false;
    if (!readPod(is, frameSeqStep)) return false;
    if (!readDeque(is, mixQueue)) return false;
    if (!readPod(is, resamplePos)) return false;
    if (!readPod(is, lastOutL)) return false;
    if (!readPod(is, lastOutR)) return false;
    if (!readVector(is, sampleQueue)) return false;
    return true;
}

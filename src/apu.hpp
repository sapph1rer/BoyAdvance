#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <iosfwd>
#include <vector>
#include "types.hpp"

class Bus;

class APU {
public:
    void reset();
    void tick(int cycles);
    void setOutputRate(int hz);
    void onTimerOverflow(Bus& bus, int timerIndex, int count = 1);

    u16 readSOUNDCNT_L() const { return soundcnt_l; }
    u16 readSOUNDCNT_H() const { return soundcnt_h; }
    u16 readSOUNDCNT_X() const;

    void writeSOUNDCNT_L(u16 value) { soundcnt_l = value; }
    void writeSOUNDCNT_H(u16 value);
    void writeSOUNDCNT_X(u16 value);

    u16 readIO16(u32 addr) const;
    bool writeIO16(u32 addr, u16 value);

    void writeFIFOA16(u16 value);
    void writeFIFOB16(u16 value);

    std::vector<int16_t> consumeSamples();
    bool saveState(std::ostream& os) const;
    bool loadState(std::istream& is);

private:
    static constexpr int CPU_HZ = 16777216;
    static constexpr size_t FIFO_MAX_BYTES = 32;

    u16 soundcnt_l = 0;
    u16 soundcnt_h = 0;
    u16 soundcnt_x = 0;
    u16 soundbias = 0x0200;

    // PSG registers (GBA/GB-style channels)
    u8 nr10 = 0, nr11 = 0, nr12 = 0, nr13 = 0, nr14 = 0;
    u8 nr21 = 0, nr22 = 0, nr23 = 0, nr24 = 0;
    u8 nr30 = 0, nr31 = 0, nr32 = 0, nr33 = 0, nr34 = 0;
    u8 nr41 = 0, nr42 = 0, nr43 = 0, nr44 = 0;
    std::array<u8, 32> waveRam{};

    struct SquareChan {
        bool enabled = false;
        bool lengthEnable = false;
        int lengthCounter = 0;
        int duty = 0;
        int frequency = 0;
        int envVolume = 0;
        int envPeriod = 0;
        bool envInc = false;
        int envTimer = 0;
        double phase = 0.0;
        // CH1 sweep (used only for sq1)
        int sweepPeriod = 0;
        bool sweepNegate = false;
        int sweepShift = 0;
        int sweepTimer = 0;
        int sweepShadow = 0;
        bool sweepEnabled = false;
    };
    struct WaveChan {
        bool enabled = false;
        bool dacEnable = false;
        bool lengthEnable = false;
        int lengthCounter = 0;
        int volumeCode = 0;
        bool forceVolume75 = false;
        int frequency = 0;
        int sampleIndex = 0;
        bool dimension64 = false;
        int bankSelect = 0;
        double phase = 0.0;
    };
    struct NoiseChan {
        bool enabled = false;
        bool lengthEnable = false;
        int lengthCounter = 0;
        int envVolume = 0;
        int envPeriod = 0;
        bool envInc = false;
        int envTimer = 0;
        u16 lfsr = 0x7FFF;
        double phase = 0.0;
    };
    SquareChan sq1;
    SquareChan sq2;
    WaveChan wave;
    NoiseChan noise;

    std::deque<int8_t> fifoA;
    std::deque<int8_t> fifoB;
    int8_t sampleA = 0;
    int8_t sampleB = 0;

    int outputRate = 48000;
    int64_t mixAccum = 0;
    int64_t frameSeqAccum = 0;
    int frameSeqStep = 0;
    // Internal mixer runs at fixed rate, then resampled to host output rate.
    static constexpr int MIX_RATE = 32768;
    std::deque<int16_t> mixQueue;
    double resamplePos = 0.0;
    std::vector<int16_t> sampleQueue;
    int lastOutL = 0;
    int lastOutR = 0;
    // One-pole lowpass applied to the direct-sound (FIFO A/B) samples only.
    // Those are refilled by timer/DMA at a much lower rate than MIX_RATE, so
    // holding each byte for several internal ticks produces a stair-stepped
    // (zero-order-hold) waveform whose step edges alias into harsh, "crunchy"
    // high-frequency noise. Smoothing just this path (not the PSG channels,
    // which are separately band-limited in sampleSquare/sampleNoise) rounds
    // off those steps without dulling the rest of the mix.
    double directFilterL = 0.0;
    double directFilterR = 0.0;

    void mixOneInternalSample();
    void runResampler();
    void tickFrameSequencer(int cycles);
    void clockLength();
    void clockEnvelope();
    void clockSweep();
    void triggerSquare1();
    void triggerSquare2();
    void triggerWave();
    void triggerNoise();
    int sweepComputeNextFreq(int shadow, bool negate, int shift) const;
    int sampleSquare(SquareChan& ch);
    int sampleWave();
    int sampleNoise();
    int noiseFrequency() const;
    void pushFIFOBytes(std::deque<int8_t>& fifo, u16 value);
    void clockFIFO(Bus& bus, bool channelB);
};

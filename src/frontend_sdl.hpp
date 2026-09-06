#pragma once
#include <vector>
#include <SDL.h>
#include "ppu.hpp"
#include "keypad.hpp"

class FrontendSDL {
public:
    FrontendSDL();
    ~FrontendSDL();

    bool init(int scale = 3);
    bool processEvents(Keypad& keypad);
    void present(const std::array<u16, PPU::SCREEN_W * PPU::SCREEN_H>& fb);
    void queueAudio(const std::vector<int16_t>& samples);
    int audioSampleRate() const { return audioRate; }

private:
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioDeviceID audioDevice = 0;
    int audioRate = 48000;
    std::vector<u32> rgbaBuffer;

    static u32 gbaToRGBA(u16 color);
    static void setKeypadFromSDL(Keypad& keypad, SDL_Scancode sc, SDL_Keycode key, bool down);
};

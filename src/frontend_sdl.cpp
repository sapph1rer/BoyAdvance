#include "frontend_sdl.hpp"
#include <algorithm>
#include <iostream>

FrontendSDL::FrontendSDL() = default;

FrontendSDL::~FrontendSDL() {
    if (audioDevice) SDL_CloseAudioDevice(audioDevice);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

u32 FrontendSDL::gbaToRGBA(u16 color) {
    u32 r = (color & 0x1F) << 3;
    u32 g = ((color >> 5) & 0x1F) << 3;
    u32 b = ((color >> 10) & 0x1F) << 3;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

bool FrontendSDL::init(int scale) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }

    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::cerr << "Warning: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: " << SDL_GetError() << "\n";
        }
    }

    window = SDL_CreateWindow("GBA Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        PPU::SCREEN_W * scale, PPU::SCREEN_H * scale, 0);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return false;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    }
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        return false;
    }
    SDL_RenderSetLogicalSize(renderer, PPU::SCREEN_W, PPU::SCREEN_H);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        PPU::SCREEN_W, PPU::SCREEN_H);
    if (!texture) {
        std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        return false;
    }

    rgbaBuffer.resize(PPU::SCREEN_W * PPU::SCREEN_H);

    SDL_AudioSpec desired{};
    desired.freq = 48000;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 1024;
    SDL_AudioSpec obtained{};
    audioDevice = SDL_OpenAudioDevice(
        nullptr,
        0,
        &desired,
        &obtained,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE
    );
    if (!audioDevice && (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            audioDevice = SDL_OpenAudioDevice(
                nullptr,
                0,
                &desired,
                &obtained,
                SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE
            );
        }
    }
    if (!audioDevice) {
        std::cerr << "Warning: SDL_OpenAudioDevice failed: " << SDL_GetError() << "\n";
    } else {
        audioRate = obtained.freq > 0 ? obtained.freq : desired.freq;
        SDL_PauseAudioDevice(audioDevice, 0);
    }
    return true;
}

void FrontendSDL::setKeypadFromSDL(Keypad& keypad, SDL_Scancode sc, SDL_Keycode key, bool down) {
    // Prefer scancode (layout-independent); keep keycode fallback.
    switch (sc) {
        case SDL_SCANCODE_Z: keypad.set(Keypad::B, down); return;
        case SDL_SCANCODE_X: keypad.set(Keypad::A, down); return;
        case SDL_SCANCODE_BACKSPACE: keypad.set(Keypad::SELECT, down); return;
        case SDL_SCANCODE_RETURN: keypad.set(Keypad::START, down); return;
        case SDL_SCANCODE_RIGHT: keypad.set(Keypad::RIGHT, down); return;
        case SDL_SCANCODE_LEFT: keypad.set(Keypad::LEFT, down); return;
        case SDL_SCANCODE_UP: keypad.set(Keypad::UP, down); return;
        case SDL_SCANCODE_DOWN: keypad.set(Keypad::DOWN, down); return;
        case SDL_SCANCODE_A: keypad.set(Keypad::L, down); return;
        case SDL_SCANCODE_S: keypad.set(Keypad::R, down); return;
        default: break;
    }
    switch (key) {
        case SDLK_z: keypad.set(Keypad::B, down); break;
        case SDLK_x: keypad.set(Keypad::A, down); break;
        case SDLK_BACKSPACE: keypad.set(Keypad::SELECT, down); break;
        case SDLK_RETURN: keypad.set(Keypad::START, down); break;
        case SDLK_RIGHT: keypad.set(Keypad::RIGHT, down); break;
        case SDLK_LEFT: keypad.set(Keypad::LEFT, down); break;
        case SDLK_UP: keypad.set(Keypad::UP, down); break;
        case SDLK_DOWN: keypad.set(Keypad::DOWN, down); break;
        case SDLK_a: keypad.set(Keypad::L, down); break;
        case SDLK_s: keypad.set(Keypad::R, down); break;
        default: break;
    }
}

bool FrontendSDL::processEvents(Keypad& keypad) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            setKeypadFromSDL(keypad, e.key.keysym.scancode, e.key.keysym.sym, true);
        }
        if (e.type == SDL_KEYUP) {
            setKeypadFromSDL(keypad, e.key.keysym.scancode, e.key.keysym.sym, false);
        }
        if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST || e.window.event == SDL_WINDOWEVENT_LEAVE) {
                keypad.clear();
            }
        }
    }
    return true;
}

void FrontendSDL::present(const std::array<u16, PPU::SCREEN_W * PPU::SCREEN_H>& fb) {
    for (size_t i = 0; i < fb.size(); ++i) rgbaBuffer[i] = gbaToRGBA(fb[i]);
    SDL_UpdateTexture(texture, nullptr, rgbaBuffer.data(), PPU::SCREEN_W * sizeof(u32));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

void FrontendSDL::queueAudio(const std::vector<int16_t>& samples) {
    if (!audioDevice || samples.empty()) return;
    // Keep low queue for responsive audio (~40ms).
    const u32 maxQueuedBytes = static_cast<u32>(std::max(4096, (audioRate * 2 * static_cast<int>(sizeof(int16_t))) / 25));
    if (SDL_GetQueuedAudioSize(audioDevice) > maxQueuedBytes) {
        SDL_ClearQueuedAudio(audioDevice);
    }
    SDL_QueueAudio(audioDevice, samples.data(), static_cast<u32>(samples.size() * sizeof(int16_t)));
}

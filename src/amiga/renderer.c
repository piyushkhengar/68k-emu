/*
 * SDL2 renderer for the Amiga 500 emulator — Chapter 3.
 *
 * Creates a 640x512 window (2x native 320x256).  Each frame the full
 * ARGB8888 framebuffer is uploaded via SDL_UpdateTexture and presented
 * with SDL_RenderCopy + SDL_RenderPresent.
 *
 * Input handling (keyboard / mouse) is deferred to Chapter 5.
 */

#include "renderer.h"
#include <SDL.h>
#include <stdio.h>

static SDL_Window   *window;
static SDL_Renderer *sdl_renderer;
static SDL_Texture  *texture;

int renderer_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "amiga renderer: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    window = SDL_CreateWindow(
        "Amiga 500",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        AMIGA_WIDTH * 2, AMIGA_HEIGHT * 2,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "amiga renderer: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    sdl_renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer) {
        fprintf(stderr, "amiga renderer: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    /* Logical size: SDL scales the 320x256 texture to fill the window. */
    SDL_RenderSetLogicalSize(sdl_renderer, AMIGA_WIDTH, AMIGA_HEIGHT);

    texture = SDL_CreateTexture(
        sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        AMIGA_WIDTH, AMIGA_HEIGHT);
    if (!texture) {
        fprintf(stderr, "amiga renderer: SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(sdl_renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    return 0;
}

void renderer_shutdown(void)
{
    if (texture)      SDL_DestroyTexture(texture);
    if (sdl_renderer) SDL_DestroyRenderer(sdl_renderer);
    if (window)       SDL_DestroyWindow(window);
    SDL_Quit();
    texture      = NULL;
    sdl_renderer = NULL;
    window       = NULL;
}

void renderer_present(const uint32_t *framebuffer)
{
    SDL_UpdateTexture(texture, NULL, framebuffer,
                      AMIGA_WIDTH * (int)sizeof(uint32_t));
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, texture, NULL, NULL);
    SDL_RenderPresent(sdl_renderer);
}

int renderer_poll_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            return 1;
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
            return 1;
    }

    /* Ctrl + Left Amiga + Right Amiga = system reset.
     * Amiga keys are mapped to Alt/Option on the host because macOS and
     * Windows intercept GUI/Cmd/Win key combinations at the OS level.
     * This matches the convention used by FS-UAE and other Amiga emulators.
     * Accept either Alt or GUI keys so it works on all platforms. */
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int la = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_LGUI];
    int ra = keys[SDL_SCANCODE_RALT] || keys[SDL_SCANCODE_RGUI];
    if (keys[SDL_SCANCODE_LCTRL] && la && ra)
        return 2;

    return 0;
}

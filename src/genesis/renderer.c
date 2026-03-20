/*
 * SDL2 renderer for the Genesis emulator.
 *
 * Creates a 640x448 window (2x native 320x224) with an ARGB8888 texture.
 * Each frame the VDP framebuffer is uploaded via SDL_UpdateTexture and
 * presented with SDL_RenderCopy + SDL_RenderPresent.
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
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    window = SDL_CreateWindow(
        "Genesis",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        GEN_WIDTH * 2, GEN_HEIGHT * 2,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    sdl_renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    SDL_RenderSetLogicalSize(sdl_renderer, GEN_WIDTH, GEN_HEIGHT);

    texture = SDL_CreateTexture(
        sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        GEN_WIDTH, GEN_HEIGHT);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
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
                      GEN_WIDTH * (int)sizeof(uint32_t));
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
    return 0;
}

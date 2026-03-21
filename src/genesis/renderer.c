/*
 * SDL2 renderer for the Genesis emulator.
 *
 * Creates a 640x448 window (2x native 320x224) with an ARGB8888 texture.
 * Each frame the VDP framebuffer is uploaded via SDL_UpdateTexture and
 * presented with SDL_RenderCopy + SDL_RenderPresent.
 *
 * Keyboard input is captured during event polling and forwarded to the
 * I/O controller via io_set_pad() for controller port 1.
 */

#include "renderer.h"
#include "io.h"
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

/* Map an SDL key to a PAD_* bit, or 0 if unmapped.
 *
 * Default mapping (player 1):
 *   Arrow keys  → D-pad
 *   Z           → A
 *   X           → B
 *   C           → C
 *   Enter       → Start
 */
static uint8_t key_to_pad(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_UP:     return PAD_UP;
    case SDLK_DOWN:   return PAD_DOWN;
    case SDLK_LEFT:   return PAD_LEFT;
    case SDLK_RIGHT:  return PAD_RIGHT;
    case SDLK_z:      return PAD_A;
    case SDLK_x:      return PAD_B;
    case SDLK_c:      return PAD_C;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return PAD_START;
    default:          return 0;
    }
}

static uint8_t pad1_state;

int renderer_poll_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            return 1;
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
            return 1;

        if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
            uint8_t bit = key_to_pad(ev.key.keysym.sym);
            if (bit) {
                pad1_state |= bit;
                io_set_pad(0, pad1_state);
            }
        } else if (ev.type == SDL_KEYUP) {
            uint8_t bit = key_to_pad(ev.key.keysym.sym);
            if (bit) {
                pad1_state &= ~bit;
                io_set_pad(0, pad1_state);
            }
        }
    }
    return 0;
}

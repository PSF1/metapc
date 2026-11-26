#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sdl2-emu.h"
#include "ibm.h"
#include "video.h"
#include "sdl2-video.h"
#include "sdl2-display.h"

#include "sdl2-video-gl3.h"
#include "sdl2-video-renderer.h"

void video_blit_complete();

VIDEO_BITMAP *screen = NULL;
static VIDEO_BITMAP *screen_copy = NULL;
/* The colour filter is applied when the image goes to the renderer, into its own bitmap, so
   the emulated image stays untouched and a change of filter redraws it at once. */
static VIDEO_BITMAP *screen_filtered = NULL;
static int color_filter = VIDEO_FILTER_COLOR;
static int crt_effect = 0;
static uint32_t filter_lut[256];
flash_t flash = {0};
static SDL_Rect screen_rect;
static SDL_Rect updated_rect;
static SDL_Rect updated_rect_copy;
static SDL_Rect blit_rect;
static SDL_Rect texture_rect;
static int updated = 0;

static SDL_mutex *blitMutex = NULL;

static void sdl_blit_memtoscreen(int x, int y, int y1, int y2, int w, int h);

int video_scale_mode = 1;
int video_vsync = 0;
int video_focus_dim = 0;
int video_fullscreen_mode = 0;
int video_alternative_update_lock = 0;

static sdl_render_driver sdl_render_drivers[] = {
        {RENDERER_AUTO, "auto", "Auto", 0, sdl2_renderer_create, sdl2_renderer_close, sdl2_renderer_available},
        {RENDERER_DIRECT3D, "direct3d", "Direct3D", 0, sdl2_renderer_create, sdl2_renderer_close, sdl2_renderer_available},
        {RENDERER_OPENGL, "opengl", "OpenGL", 0, sdl2_renderer_create, sdl2_renderer_close, sdl2_renderer_available},
        {RENDERER_OPENGLES2, "opengles2", "OpenGL ES 2", 0, sdl2_renderer_create, sdl2_renderer_close, sdl2_renderer_available},
        {RENDERER_OPENGLES, "opengles", "OpenGL ES", 0, sdl2_renderer_create, sdl2_renderer_close, sdl2_renderer_available},
        {RENDERER_SOFTWARE, "software", "Software", 0, sdl2_renderer_create, sdl2_renderer_close, sdl2_renderer_available},
        {RENDERER_GL3, "gl3", "OpenGL 3.0", SDL_WINDOW_OPENGL, gl3_renderer_create, gl3_renderer_close, gl3_renderer_available}};

sdl_render_driver requested_render_driver;

char current_render_driver_name[50];
static sdl_renderer_t *renderer = NULL;

void hline(VIDEO_BITMAP *b, int x1, int y, int x2, int col) {
        if (y < 0 || y >= buffer32->h)
                return;

        for (; x1 < x2; x1++)
                ((uint32_t *)b->line[y])[x1] = col;
}

void destroy_bitmap(VIDEO_BITMAP *b) { free(b); }

VIDEO_BITMAP *create_bitmap(int x, int y) {
        VIDEO_BITMAP *b = malloc(sizeof(VIDEO_BITMAP) + (y * sizeof(uint8_t *)));
        int c;
        b->dat = malloc(x * y * 4);
        for (c = 0; c < y; c++) {
                b->line[c] = b->dat + (c * x * 4);
        }
        b->w = x;
        b->h = y;
        return b;
}

sdl_render_driver *sdl_get_render_drivers(int *num) {
        if (num)
                *num = SDL_arraysize(sdl_render_drivers);
        return sdl_render_drivers;
}

sdl_render_driver sdl_get_render_driver_by_name(const char *name, int def) {
        int i;
        for (i = 0; i < SDL_arraysize(sdl_render_drivers); ++i) {
                if (!strcmp(sdl_render_drivers[i].sdl_id, name)) {
                        return sdl_render_drivers[i];
                }
        }
        return sdl_render_drivers[def];
}

sdl_render_driver *sdl_get_render_driver_by_name_ptr(const char *name) {
        int i;
        for (i = 0; i < SDL_arraysize(sdl_render_drivers); ++i) {
                if (!strcmp(sdl_render_drivers[i].sdl_id, name)) {
                        return &sdl_render_drivers[i];
                }
        }
        return 0;
}

sdl_render_driver sdl_get_render_driver_by_id(int id, int def) {
        int i;
        for (i = 0; i < SDL_arraysize(sdl_render_drivers); ++i) {
                if (sdl_render_drivers[i].id == id) {
                        return sdl_render_drivers[i];
                }
        }
        return sdl_render_drivers[def];
}

void sdl_scale(int scale, SDL_Rect src, SDL_Rect *dst, int w, int h) {
        double t, b, l, r;
        int ratio_w, ratio_h;
        switch (scale) {
        case FULLSCR_SCALE_43:
                t = 0;
                b = src.h;
                l = (src.w / 2) - ((src.h * 4) / (3 * 2));
                r = (src.w / 2) + ((src.h * 4) / (3 * 2));
                if (l < 0) {
                        l = 0;
                        r = src.w;
                        t = (src.h / 2) - ((src.w * 3) / (4 * 2));
                        b = (src.h / 2) + ((src.w * 3) / (4 * 2));
                }
                break;
        case FULLSCR_SCALE_SQ:
                t = 0;
                b = src.h;
                l = (src.w / 2) - ((src.h * w) / (h * 2));
                r = (src.w / 2) + ((src.h * w) / (h * 2));
                if (l < 0) {
                        l = 0;
                        r = src.w;
                        t = (src.h / 2) - ((src.w * h) / (w * 2));
                        b = (src.h / 2) + ((src.w * h) / (w * 2));
                }
                break;
        case FULLSCR_SCALE_INT:
                ratio_w = src.w / w;
                ratio_h = src.h / h;
                if (ratio_h < ratio_w)
                        ratio_w = ratio_h;
                l = (src.w / 2) - ((w * ratio_w) / 2);
                r = (src.w / 2) + ((w * ratio_w) / 2);
                t = (src.h / 2) - ((h * ratio_w) / 2);
                b = (src.h / 2) + ((h * ratio_w) / 2);
                break;
        case FULLSCR_SCALE_FULL:
        default:
                l = 0;
                t = 0;
                r = src.w;
                b = src.h;
                break;
        }

        dst->x = l;
        dst->y = t;
        dst->w = r - l;
        dst->h = b - t;
}

static void set_updated_size(int x, int y, int w, int h) {
        if (updated) {
                updated_rect.x = x < updated_rect.x ? x : updated_rect.x;
                updated_rect.y = y < updated_rect.y ? y : updated_rect.y;
                updated_rect.w = w > updated_rect.w ? w : updated_rect.w;
                updated_rect.h = h > updated_rect.h ? h : updated_rect.h;
        } else {
                updated_rect.x = x;
                updated_rect.y = y;
                updated_rect.w = w;
                updated_rect.h = h;
                updated = 1;
        }
}

static void sdl_blit_memtoscreen(int x, int y, int y1, int y2, int w, int h) {
        if (y1 == y2) {
                video_blit_complete();
                return; /*Nothing to do*/
        }

        int yy;
        SDL_LockMutex(blitMutex);
        for (yy = y1; yy < y2; yy++) {
                if ((y + yy) >= 0 && (y + yy) < buffer32->h)
                        memcpy(screen->dat + (yy * screen->w * 4), &(((uint32_t *)buffer32->line[y + yy])[x]), w * 4);
        }
        set_updated_size(0, y1, w, y2 - y1);
        //        set_updated_size(0, 0, w, h);
        blit_rect.w = w;
        blit_rect.h = h;
        SDL_UnlockMutex(blitMutex);
        video_blit_complete();
}

static void build_filter_lut(int filter) {
        /* Phosphor tints, at full brightness. */
        int r = 255, g = 255, b = 255;
        if (filter == VIDEO_FILTER_GREEN) {
                r = 51;
                g = 255;
                b = 80;
        } else if (filter == VIDEO_FILTER_AMBER) {
                r = 255;
                g = 190;
                b = 0;
        }
        for (int i = 0; i < 256; i++)
                filter_lut[i] = ((r * i / 255) << 16) | ((g * i / 255) << 8) | (b * i / 255);
}

/* Sends the whole current image to the renderer again. Call with blitMutex held. */
static void refilter_screen() {
        if (blit_rect.w > 0 && blit_rect.h > 0) {
                updated_rect.x = 0;
                updated_rect.y = 0;
                updated_rect.w = blit_rect.w;
                updated_rect.h = blit_rect.h;
                updated = 1;
        }
}

void video_set_color_filter(int filter) {
        if (filter < VIDEO_FILTER_COLOR || filter > VIDEO_FILTER_AMBER)
                filter = VIDEO_FILTER_COLOR;
        if (blitMutex)
                SDL_LockMutex(blitMutex);
        color_filter = filter;
        build_filter_lut(filter);
        if (blitMutex) {
                refilter_screen();
                SDL_UnlockMutex(blitMutex);
        }
}

int video_get_color_filter() { return color_filter; }

void video_set_crt_effect(int on) {
        if (blitMutex)
                SDL_LockMutex(blitMutex);
        crt_effect = on ? 1 : 0;
        if (blitMutex) {
                refilter_screen();
                SDL_UnlockMutex(blitMutex);
        }
}

int video_get_crt_effect() { return crt_effect; }

/* CRT effect: brightness per column and per row, in 1/256ths. Rows alternate between a lit
   scanline and the darker gap between scanlines; both fall off towards the edges (vignette). */
#define CRT_GAP 150     /* brightness of the gap between scanlines */
#define CRT_EDGE 200    /* brightness at the edges of the screen */
#define CRT_BOOST 280   /* the lit lines, a little brighter to make up for the gaps */
static int crt_w = 0, crt_h = 0;
static uint16_t crt_col[2048];
static uint16_t crt_row[2048];

static int crt_falloff(int i, int n) {
        /* 256 in the middle, CRT_EDGE at the edges, quadratic */
        double d = n > 1 ? (2.0 * i / (n - 1) - 1.0) : 0.0;
        return 256 - (int)((256 - CRT_EDGE) * d * d);
}

static void build_crt_tables(int w, int h) {
        if (w == crt_w && h == crt_h)
                return;
        crt_w = w;
        crt_h = h;
        for (int x = 0; x < w && x < 2048; x++)
                crt_col[x] = crt_falloff(x, w);
        for (int y = 0; y < h && y < 2048; y++)
                crt_row[y] = crt_falloff(y, h) * ((y & 1) ? CRT_GAP : CRT_BOOST) / 256;
}

/* Scales each channel of p by f/256, saturating. */
static inline uint32_t scale_pixel(uint32_t p, int f) {
        int r = (((p >> 16) & 0xff) * f) >> 8;
        int g = (((p >> 8) & 0xff) * f) >> 8;
        int b = ((p & 0xff) * f) >> 8;
        if (r > 255)
                r = 255;
        if (g > 255)
                g = 255;
        if (b > 255)
                b = 255;
        return (p & 0xff000000) | (r << 16) | (g << 8) | b;
}

/* Returns the bitmap to give the renderer for rect: src itself, or a filtered copy. */
static VIDEO_BITMAP *apply_color_filter(VIDEO_BITMAP *src, SDL_Rect rect) {
        int crt = crt_effect;
        if (color_filter == VIDEO_FILTER_COLOR && !crt)
                return src;
        if (!screen_filtered)
                screen_filtered = create_bitmap(screen_rect.w, screen_rect.h);
        int x1 = rect.x < 0 ? 0 : rect.x;
        int y1 = rect.y < 0 ? 0 : rect.y;
        int x2 = rect.x + rect.w > src->w ? src->w : rect.x + rect.w;
        int y2 = rect.y + rect.h > src->h ? src->h : rect.y + rect.h;
        int w = blit_rect.w > 0 ? blit_rect.w : x2;
        int h = blit_rect.h > 0 ? blit_rect.h : y2;
        if (crt)
                build_crt_tables(w, h);
        for (int y = y1; y < y2; y++) {
                uint32_t *in = (uint32_t *)src->line[y];
                uint32_t *out = (uint32_t *)screen_filtered->line[y];
                int row = (crt && y < 2048) ? crt_row[y] : 256;
                for (int x = x1; x < x2; x++) {
                        uint32_t p = in[x];
                        if (crt) {
                                /* the beam spreads a little: soften horizontally */
                                uint32_t l = in[x > 0 ? x - 1 : x];
                                uint32_t r = in[x + 1 < w ? x + 1 : x];
                                p = (p & 0xff000000) + ((p & 0xfefefe) >> 1) + ((l & 0xfcfcfc) >> 2) + ((r & 0xfcfcfc) >> 2);
                        }
                        if (color_filter != VIDEO_FILTER_COLOR) {
                                /* Rec. 601 luma */
                                int lum = (((p >> 16) & 0xff) * 77 + ((p >> 8) & 0xff) * 150 + (p & 0xff) * 29) >> 8;
                                p = (p & 0xff000000) | filter_lut[lum];
                        }
                        if (crt)
                                p = scale_pixel(p, (row * (x < 2048 ? crt_col[x] : 256)) >> 8);
                        out[x] = p;
                }
        }
        return screen_filtered;
}

int sdl_is_fullscreen(SDL_Window *window) {
        int flags = SDL_GetWindowFlags(window);
        return (flags & SDL_WINDOW_FULLSCREEN) || (flags & SDL_WINDOW_FULLSCREEN_DESKTOP);
}

int sdl_video_init() {
        blitMutex = SDL_CreateMutex();
        updated = 0;

        video_blit_memtoscreen_func = sdl_blit_memtoscreen;
        requested_render_driver = sdl_get_render_driver_by_id(RENDERER_AUTO, RENDERER_AUTO);

        screen_rect.w = screen_rect.h = 2048;

        screen = create_bitmap(screen_rect.w, screen_rect.h);
        build_filter_lut(color_filter);

        return SDL_TRUE;
}

void sdl_video_close() {
        requested_render_driver.renderer_close(renderer);
        renderer = NULL;
        destroy_bitmap(screen);
        screen = NULL;
        if (screen_filtered)
                destroy_bitmap(screen_filtered);
        screen_filtered = NULL;
        SDL_DestroyMutex(blitMutex);
        blitMutex = NULL;
}

int sdl_renderer_init(SDL_Window *window) {
        if (video_alternative_update_lock)
                screen_copy = create_bitmap(screen_rect.w, screen_rect.h);
        else
                screen_copy = NULL;

        pclog("Renderer: initialising the %s renderer\n", requested_render_driver.name);
        renderer = requested_render_driver.renderer_create();
        int ret = renderer->init(window, requested_render_driver, screen_rect);
        pclog("Renderer: %s renderer %s\n", requested_render_driver.name, ret ? "ready" : "failed to initialise");
        return ret;
}

void sdl_renderer_close() {
        if (renderer)
                renderer->close();
        renderer = NULL;

        if (screen_copy)
                destroy_bitmap(screen_copy);
        screen_copy = NULL;
}

int sdl_renderer_update(SDL_Window *window) {
        // When using timer-based rendering, window might be NULL
        if ((window == NULL) || (renderer == NULL))
                return 0;
        int render = 0;
        SDL_LockMutex(blitMutex);
        if (updated) {
                updated = 0;
                if (screen_copy) {
                        memcpy(&updated_rect_copy, &updated_rect, sizeof(updated_rect));
                        memcpy(screen_copy->dat + (updated_rect.y * screen_copy->w * 4),
                               screen->dat + (updated_rect.y * screen->w * 4), updated_rect.h * screen->w * 4);
                } else
                        renderer->update(window, updated_rect, apply_color_filter(screen, updated_rect));
                texture_rect.w = blit_rect.w;
                texture_rect.h = blit_rect.h;
                render = 1;
        }
        SDL_UnlockMutex(blitMutex);
        if (screen_copy && render)
                renderer->update(window, updated_rect_copy, apply_color_filter(screen_copy, updated_rect_copy));
        return render || renderer->always_update;
}

void sdl_renderer_present(SDL_Window *window) {
        if (flash.enabled) {
                int elapsed = SDL_GetTicks() - flash.start;
                float a = flash.func(elapsed / (float)flash.time) * (flash.alpha & 0xff);
                flash.color[3] = (char)a;
                if (elapsed >= flash.time)
                        flash.enabled = 0;
        }

        SDL_Rect wr;
        display_get_size(window, &wr.w, &wr.h);
        sdl_scale(video_fullscreen_scale, wr, &wr, texture_rect.w, texture_rect.h);
        renderer->present(window, texture_rect, wr, screen_rect);
}

void color_flash(FLASH_FUNC func, int time_ms, char r, char g, char b, char a) {
        flash.func = func;
        flash.color[0] = r;
        flash.color[1] = g;
        flash.color[2] = b;
        flash.color[3] = 0;
        flash.alpha = a;
        flash.start = SDL_GetTicks();
        flash.time = time_ms;
        flash.enabled = 1;
}

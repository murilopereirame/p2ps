/* A minimal 2D framebuffer and bitmap-font text renderer for the PS4. */
#include <string.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>

#include "gfx.h"
#include "font8x8_basic.h"
#include "font8x8_ext_latin.h"

#define NUM_BUFFERS 2
#define FB_SIZE (GFX_W * GFX_H * 4)
#define MEM_ALIGN 0x200000
#define MEM_SIZE (((FB_SIZE * NUM_BUFFERS) + MEM_ALIGN - 1) / MEM_ALIGN * MEM_ALIGN)

static int video = -1;
static OrbisKernelEqueue flip_queue;
static uint32_t *buffers[NUM_BUFFERS];
static int active;
static int64_t frame_id;

int gfx_init(void)
{
    OrbisVideoOutBufferAttribute attr;
    off_t mem_off = 0;
    void *mem = NULL;
    int i;

    video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (video < 0)
        return -1;
    if (sceKernelCreateEqueue(&flip_queue, "p2f flip") < 0)
        return -2;
    sceVideoOutAddFlipEvent(flip_queue, video, 0);

    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                      MEM_SIZE, MEM_ALIGN, 3, &mem_off) < 0)
        return -3;
    if (sceKernelMapDirectMemory(&mem, MEM_SIZE, 0x33, 0, mem_off, MEM_ALIGN) < 0)
        return -4;

    for (i = 0; i < NUM_BUFFERS; i++)
        buffers[i] = (uint32_t *)((char *)mem + (size_t)i * FB_SIZE);

    /* 0x80000000 = A8R8G8B8 sRGB, tiling 1 = linear. */
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, GFX_W, GFX_H, GFX_W);
    if (sceVideoOutRegisterBuffers(video, 0, (void **)buffers, NUM_BUFFERS, &attr) != 0)
        return -5;
    sceVideoOutSetFlipRate(video, 0);
    active = 0;
    return 0;
}

static inline uint32_t px(uint32_t rgb)
{
    return 0x80000000u | (rgb & 0xffffff);
}

void gfx_clear(uint32_t rgb)
{
    uint32_t c = px(rgb), *p = buffers[active];
    size_t i;
    for (i = 0; i < (size_t)GFX_W * GFX_H; i++)
        p[i] = c;
}

void gfx_rect(int x, int y, int w, int h, uint32_t rgb)
{
    uint32_t c = px(rgb);
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    for (j = 0; j < h; j++) {
        uint32_t *row = buffers[active] + (size_t)(y + j) * GFX_W + x;
        for (i = 0; i < w; i++)
            row[i] = c;
    }
}

static const char *glyph_for(uint32_t cp)
{
    if (cp < 128)
        return font8x8_basic[cp];
    if (cp >= 0xA0 && cp <= 0xFF)
        return font8x8_ext_latin[cp - 0xA0];
    return font8x8_basic['?'];
}

static void draw_glyph(int x, int y, int scale, uint32_t c, const char *g)
{
    int row, col, sx, sy;
    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++) {
            if (!(g[row] & (1 << col)))
                continue;
            for (sy = 0; sy < scale; sy++) {
                int yy = y + row * scale + sy;
                uint32_t *line;
                if (yy < 0 || yy >= GFX_H)
                    continue;
                line = buffers[active] + (size_t)yy * GFX_W;
                for (sx = 0; sx < scale; sx++) {
                    int xx = x + col * scale + sx;
                    if (xx >= 0 && xx < GFX_W)
                        line[xx] = c;
                }
            }
        }
    }
}

/* Decode one UTF-8 code point. Invalid bytes become '?'. */
static uint32_t next_cp(const unsigned char **s)
{
    const unsigned char *p = *s;
    uint32_t cp;
    int extra, i;
    if (p[0] < 0x80) { *s = p + 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; extra = 1; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; extra = 2; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; extra = 3; }
    else { *s = p + 1; return '?'; }
    for (i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s = p + i; return '?'; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s = p + extra + 1;
    return cp;
}

void gfx_text(int x, int y, int scale, uint32_t rgb, const char *utf8,
              int max_chars)
{
    const unsigned char *s = (const unsigned char *)utf8;
    uint32_t c = px(rgb);
    int n = 0, total = 0, adv = 8 * scale;
    const unsigned char *t = s;

    while (*t) { next_cp(&t); total++; }

    while (*s) {
        uint32_t cp = next_cp(&s);
        if (max_chars > 0 && total > max_chars && n == max_chars - 3) {
            draw_glyph(x, y, scale, c, font8x8_basic['.']);
            draw_glyph(x + adv, y, scale, c, font8x8_basic['.']);
            draw_glyph(x + 2 * adv, y, scale, c, font8x8_basic['.']);
            return;
        }
        draw_glyph(x, y, scale, c, glyph_for(cp));
        x += adv;
        n++;
    }
}

void gfx_present(void)
{
    OrbisKernelEvent evt;
    int count;

    frame_id++;
    sceVideoOutSubmitFlip(video, active, ORBIS_VIDEO_OUT_FLIP_VSYNC, frame_id);
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(video, &st);
        if (st.flipArg >= frame_id)
            break;
        if (sceKernelWaitEqueue(flip_queue, &evt, 1, &count, 0) != 0)
            break;
    }
    active = (active + 1) % NUM_BUFFERS;
}

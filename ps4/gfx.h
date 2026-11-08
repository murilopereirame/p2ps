/* A minimal 2D framebuffer and bitmap-font text renderer for the PS4. */
#ifndef P2F_GFX_H
#define P2F_GFX_H

#include <stdint.h>

#define GFX_W 1920
#define GFX_H 1080

/* Colors are 0xRRGGBB. */
int gfx_init(void);
void gfx_clear(uint32_t rgb);
void gfx_rect(int x, int y, int w, int h, uint32_t rgb);
/* Draw UTF-8 text (Latin-1 range shown, others as '?'). scale 1 = 8 px.
 * max_chars > 0 cuts the text with "..." */
void gfx_text(int x, int y, int scale, uint32_t rgb, const char *utf8,
              int max_chars);
/* Show the frame that was drawn and wait for vsync. */
void gfx_present(void);

#endif

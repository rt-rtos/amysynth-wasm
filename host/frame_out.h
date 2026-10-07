#pragma once
/* Frame decode and writers for a u8g2 full-buffer frame in the
 * vertical_top_lsb layout (SSD13xx full-buffer setups): byte
 * (y & ~7) * tile_w + x, bit y & 7. Knows nothing about the screens. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const uint8_t *buf;   /* u8g2_GetBufferPtr()            */
    int tile_w;           /* u8g2_GetBufferTileWidth()      */
    int tile_h;           /* u8g2_GetBufferTileHeight()     */
} frame_t;

/* How lit pixels look in the PNG, sixel and terminal writers. band > 0 is a
 * dual-colour panel: rows 0..band-1 yellow, the rest blue. band == 0 is a
 * white monochrome panel. */
typedef struct {
    int scale;            /* PNG/sixel pixels per panel pixel           */
    bool grid;            /* PNG/sixel: 1-px gap per cell when scale>=3 */
    int band;             /* yellow rows at the top, 0 = monochrome     */
} frame_look_t;

int frame_width(const frame_t *f);
int frame_height(const frame_t *f);

/* Each returns 0 on success, -1 (errno set) if the file cannot be written. */
int frame_write_txt(const char *path, const char *name, const frame_t *f);
int frame_write_png(const char *path, const frame_t *f, const frame_look_t *look);

/* Terminal output uses 24-bit colour escapes unless look->band == 0. */
void frame_print_term(FILE *out, const char *name, const frame_t *f, const frame_look_t *look);
void frame_print_sixel(FILE *out, const char *name, const frame_t *f, const frame_look_t *look);

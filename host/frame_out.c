#include "frame_out.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Colour indices shared by the PNG palette and the sixel registers. */
enum { C_DARK, C_WHITE, C_YELLOW, C_BLUE, C_GAP, C_COUNT };

static const uint8_t palette[C_COUNT][3] = {
    [C_DARK]   = {   0,   0,   0 },
    [C_WHITE]  = { 255, 255, 255 },
    [C_YELLOW] = { 255, 214,   0 },
    [C_BLUE]   = {  40, 170, 255 },
    [C_GAP]    = {  48,  48,  48 },
};

static int px(const uint8_t *buf, int tile_w, int x, int y)
{
    return (buf[(y & ~7) * tile_w + x] >> (y & 7)) & 1;
}

static int lit_colour(const frame_look_t *look, int y)
{
    if (look->band <= 0) return C_WHITE;
    return y < look->band ? C_YELLOW : C_BLUE;
}

int frame_width(const frame_t *f)  { return f->tile_w * 8; }
int frame_height(const frame_t *f) { return f->tile_h * 8; }

/* Scaled colour-index raster shared by PNG and sixel. With the grid on, the
 * last column and row of every cell is C_GAP. NULL on OOM. */
static uint8_t *raster(const frame_t *f, const frame_look_t *look, int *w_out, int *h_out)
{
    const int scale = look->scale;
    const int w = frame_width(f), h = frame_height(f);
    const int sw = w * scale, sh = h * scale;
    const bool gap = look->grid && scale >= 3;
    uint8_t *img = malloc((size_t)sw * (size_t)sh);
    if (!img) return NULL;
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            uint8_t v;
            if (gap && (x % scale == scale - 1 || y % scale == scale - 1)) {
                v = C_GAP;
            } else {
                v = px(f->buf, f->tile_w, x / scale, y / scale)
                    ? (uint8_t)lit_colour(look, y / scale) : C_DARK;
            }
            img[(size_t)y * (size_t)sw + (size_t)x] = v;
        }
    }
    *w_out = sw;
    *h_out = sh;
    return img;
}

int frame_write_txt(const char *path, const char *name, const frame_t *f)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    const int w = frame_width(f), h = frame_height(f);
    fprintf(fp, "# %s %dx%d\n", name, w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) fputc(px(f->buf, f->tile_w, x, y) ? '#' : '.', fp);
        fputc('\n', fp);
    }
    return fclose(fp) == 0 ? 0 : -1;
}

/* ── PNG ── */

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int png_chunk(FILE *fp, const char type[4], const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc_update(0xFFFFFFFFu, hdr + 4, 4);
    crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
    uint8_t tail[4];
    put_be32(tail, crc);
    if (fwrite(hdr, 1, 8, fp) != 8) return -1;
    if (len && fwrite(data, 1, len, fp) != len) return -1;
    if (fwrite(tail, 1, 4, fp) != 4) return -1;
    return 0;
}

int frame_write_png(const char *path, const frame_t *f, const frame_look_t *look)
{
    static bool crc_ready;
    if (!crc_ready) { crc_init(); crc_ready = true; }

    int w, h;
    uint8_t *img = raster(f, look, &w, &h);
    if (!img) return -1;

    /* Filter byte 0 (None) in front of every scanline. */
    const size_t raw_len = (size_t)h * ((size_t)w + 1);
    uint8_t *raw = malloc(raw_len);
    uLongf z_len = compressBound((uLong)raw_len);
    uint8_t *z = malloc(z_len);
    int rc = -1;
    if (raw && z) {
        for (int y = 0; y < h; y++) {
            raw[(size_t)y * ((size_t)w + 1)] = 0;
            memcpy(raw + (size_t)y * ((size_t)w + 1) + 1, img + (size_t)y * (size_t)w, (size_t)w);
        }
        if (compress2(z, &z_len, raw, (uLong)raw_len, Z_BEST_COMPRESSION) == Z_OK) {
            FILE *fp = fopen(path, "wb");
            if (fp) {
                static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
                uint8_t ihdr[13];
                put_be32(ihdr, (uint32_t)w);
                put_be32(ihdr + 4, (uint32_t)h);
                ihdr[8]  = 8;   /* bit depth          */
                ihdr[9]  = 3;   /* indexed colour     */
                ihdr[10] = 0;   /* deflate            */
                ihdr[11] = 0;   /* adaptive filtering */
                ihdr[12] = 0;   /* no interlace       */
                rc = (fwrite(sig, 1, 8, fp) == 8
                      && png_chunk(fp, "IHDR", ihdr, 13) == 0
                      && png_chunk(fp, "PLTE", &palette[0][0], sizeof(palette)) == 0
                      && png_chunk(fp, "IDAT", z, (uint32_t)z_len) == 0
                      && png_chunk(fp, "IEND", NULL, 0) == 0) ? 0 : -1;
                if (fclose(fp) != 0) rc = -1;
            }
        }
    }
    free(z);
    free(raw);
    free(img);
    return rc;
}

/* ── Terminal ── */

static void term_colour(FILE *out, int layer, int c)
{
    fprintf(out, "\033[%d;2;%d;%d;%dm", layer, palette[c][0], palette[c][1], palette[c][2]);
}

/* One cell = two pixel rows. A cell straddling the band seam with both
 * pixels lit draws the upper half in the foreground and the lower half in
 * the background colour. */
void frame_print_term(FILE *out, const char *name, const frame_t *f, const frame_look_t *look)
{
    const int w = frame_width(f), h = frame_height(f);
    const bool colour = look->band > 0;
    fprintf(out, "== %s ==\n", name);
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x++) {
            const int top = px(f->buf, f->tile_w, x, y);
            const int bot = (y + 1 < h) ? px(f->buf, f->tile_w, x, y + 1) : 0;
            const int ct = lit_colour(look, y), cb = lit_colour(look, y + 1);
            const char *glyph = top ? (bot ? "█" : "▀") : (bot ? "▄" : " ");
            if (colour && top && bot && ct != cb) {
                term_colour(out, 38, ct);
                term_colour(out, 48, cb);
                glyph = "▀";
            } else if (colour && (top || bot)) {
                term_colour(out, 38, top ? ct : cb);
            }
            fputs(glyph, out);
            if (colour && (top || bot)) fputs("\033[0m", out);
        }
        fputc('\n', out);
    }
    fputc('\n', out);
}

/* ── Sixel ── */

static void sixel_run(FILE *out, int ch, int n)
{
    if (n > 3) {
        fprintf(out, "!%d%c", n, ch);
    } else {
        while (n-- > 0) fputc(ch, out);
    }
}

void frame_print_sixel(FILE *out, const char *name, const frame_t *f, const frame_look_t *look)
{
    fprintf(out, "== %s ==\n", name);
    int w, h;
    uint8_t *img = raster(f, look, &w, &h);
    if (!img) return;

    fprintf(out, "\033Pq\"1;1;%d;%d", w, h);
    for (int c = 0; c < C_COUNT; c++) {
        fprintf(out, "#%d;2;%d;%d;%d", c, palette[c][0] * 100 / 255,
                palette[c][1] * 100 / 255, palette[c][2] * 100 / 255);
    }
    for (int band = 0; band * 6 < h; band++) {
        for (int c = 0; c < C_COUNT; c++) {
            fprintf(out, "#%d", c);
            int run_ch = -1, run_n = 0;
            for (int x = 0; x < w; x++) {
                int bits = 0;
                for (int r = 0; r < 6; r++) {
                    int y = band * 6 + r;
                    if (y >= h) break;
                    if (img[(size_t)y * (size_t)w + (size_t)x] == c) bits |= 1 << r;
                }
                int ch = 63 + bits;
                if (ch == run_ch) {
                    run_n++;
                } else {
                    sixel_run(out, run_ch, run_n);
                    run_ch = ch;
                    run_n = 1;
                }
            }
            sixel_run(out, run_ch, run_n);
            fputc(c == C_COUNT - 1 ? '-' : '$', out);
        }
    }
    fputs("\033\\\n", out);
    free(img);
}

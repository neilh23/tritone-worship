/* Renders the pedal to a PNG for the README: screenshot out.png */
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

#include "tw_ui.h"

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char head[8];
    put32(head, len);
    for (int i = 0; i < 4; i++)
        head[4 + i] = type[i];
    uint32_t crc = crc32(crc32(0, head + 4, 4), data, len);
    unsigned char tail[4];
    put32(tail, crc);
    fwrite(head, 1, 8, f);
    fwrite(data, 1, len, f);
    fwrite(tail, 1, 4, f);
}

int main(int argc, char **argv)
{
    static uint32_t px[TW_UI_W * TW_UI_H];
    float value[TW_P_COUNT];
    for (int p = 0; p < TW_P_COUNT; p++)
        value[p] = tw_param_info[p].def;
    value[TW_P_DEATH] = 40;
    value[TW_P_BLACKEN] = 1;
    value[TW_P_TREM_AMOUNT] = 60;
    value[TW_P_TREM_SPEED] = 1000;
    tw_ui_draw(px, value, -1);

    /* RGB rows, each prefixed with filter type 0 */
    static unsigned char raw[TW_UI_H * (1 + 3 * TW_UI_W)];
    unsigned char *r = raw;
    for (int y = 0; y < TW_UI_H; y++) {
        *r++ = 0;
        for (int x = 0; x < TW_UI_W; x++) {
            uint32_t c = px[y * TW_UI_W + x];
            *r++ = c >> 16;
            *r++ = c >> 8;
            *r++ = c;
        }
    }
    uLongf zlen = compressBound(sizeof raw);
    unsigned char *z = malloc(zlen);
    compress2(z, &zlen, raw, sizeof raw, 9);

    FILE *f = fopen(argv[1], "wb");
    if (!f)
        return 1;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    unsigned char ihdr[13] = {0};
    put32(ihdr, TW_UI_W);
    put32(ihdr + 4, TW_UI_H);
    ihdr[8] = 8; /* bits per channel */
    ihdr[9] = 2; /* RGB */
    chunk(f, "IHDR", ihdr, sizeof ihdr);
    chunk(f, "IDAT", z, zlen);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    return 0;
}

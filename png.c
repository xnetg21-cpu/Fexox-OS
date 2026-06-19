/*
 * png.c — минимальный PNG-декодер для FEXOS
 *
 * Содержит собственную реализацию INFLATE (RFC1951: stored, fixed Huffman,
 * dynamic Huffman) и парсер PNG-чанков (RFC2083) для 8-bit RGB/RGBA,
 * non-interlaced изображений.
 *
 * Based on the well-known public-domain algorithm structure of
 * "puff.c" (Mark Adler), переписан без файлового I/O и libc.
 */
#include "png.h"

extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern void *memset(void *dst, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

/* =========================================================================
 * Битовый поток (LSB-first, как требует DEFLATE)
 * ========================================================================= */
typedef struct {
    const uint8_t *in;
    size_t          in_len;
    size_t          in_pos;   /* текущий байт */
    uint32_t        bitbuf;   /* накопленные биты */
    int             bitcnt;   /* сколько бит валидно в bitbuf */
} bitstream_t;

static int bs_getbit(bitstream_t *bs) {
    if (bs->bitcnt == 0) {
        if (bs->in_pos >= bs->in_len) return -1; /* EOF */
        bs->bitbuf = bs->in[bs->in_pos++];
        bs->bitcnt = 8;
    }
    int bit = bs->bitbuf & 1;
    bs->bitbuf >>= 1;
    bs->bitcnt--;
    return bit;
}

/* Читает n бит (n <= 16), LSB-first, возвращает -1 при ошибке/EOF */
static int32_t bs_getbits(bitstream_t *bs, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        int b = bs_getbit(bs);
        if (b < 0) return -1;
        val |= (uint32_t)b << i;
    }
    return (int32_t)val;
}

/* Сбросить остаток текущего байта (для перехода к stored block) */
static void bs_align(bitstream_t *bs) {
    bs->bitbuf = 0;
    bs->bitcnt = 0;
}

/* =========================================================================
 * Huffman-декодер (по алгоритму puff.c: канонические коды)
 * ========================================================================= */
#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES (MAXLCODES + MAXDCODES)

typedef struct {
    int16_t count[MAXBITS + 1]; /* кол-во кодов каждой длины */
    int16_t symbol[MAXCODES];   /* символы в порядке кодов */
} huffman_t;

/* Декодирует один символ по таблице h, читая биты из bs.
 * Возвращает символ >=0 или -1 при ошибке. */
static int huffman_decode(bitstream_t *bs, const huffman_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = bs_getbit(bs);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* Строит каноническую Huffman-таблицу из массива длин кодов length[0..n-1] */
static void huffman_build(huffman_t *h, const uint8_t *length, int n) {
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    h->count[0] = 0;

    int16_t offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];

    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0)
            h->symbol[offs[length[sym]]++] = (int16_t)sym;
}

/* =========================================================================
 * Таблицы длин/расстояний DEFLATE (RFC1951 §3.2.5)
 * ========================================================================= */
static const uint16_t LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* =========================================================================
 * Выходной буфер inflate — фиксированного размера (мы знаем его заранее)
 * ========================================================================= */
typedef struct {
    uint8_t *buf;
    size_t   size;
    size_t   pos;
} outbuf_t;

static int out_put(outbuf_t *o, uint8_t byte) {
    if (o->pos >= o->size) return -1;
    o->buf[o->pos++] = byte;
    return 0;
}

/* Копирует block из stored-блока */
static int inflate_stored(bitstream_t *bs, outbuf_t *out) {
    bs_align(bs);
    if (bs->in_pos + 4 > bs->in_len) return PNG_ERR_INFLATE;
    uint16_t len  = (uint16_t)(bs->in[bs->in_pos] | (bs->in[bs->in_pos+1] << 8));
    /* nlen (复~len) проверять не обязательно для нашего случая */
    bs->in_pos += 4;
    if (bs->in_pos + len > bs->in_len) return PNG_ERR_INFLATE;
    for (uint16_t i = 0; i < len; i++) {
        if (out_put(out, bs->in[bs->in_pos++]) < 0) return PNG_ERR_INFLATE;
    }
    return PNG_OK;
}

/* Декодирует один блок с заданными Huffman-таблицами (fixed или dynamic) */
static int inflate_block_data(bitstream_t *bs, outbuf_t *out,
                               const huffman_t *lc, const huffman_t *dc) {
    for (;;) {
        int sym = huffman_decode(bs, lc);
        if (sym < 0) return PNG_ERR_INFLATE;
        if (sym < 256) {
            if (out_put(out, (uint8_t)sym) < 0) return PNG_ERR_INFLATE;
        } else if (sym == 256) {
            return PNG_OK; /* конец блока */
        } else {
            sym -= 257;
            if (sym >= 29) return PNG_ERR_INFLATE;
            int32_t extra = LEN_EXTRA[sym] ? bs_getbits(bs, LEN_EXTRA[sym]) : 0;
            if (extra < 0) return PNG_ERR_INFLATE;
            uint32_t length = LEN_BASE[sym] + (uint32_t)extra;

            int dsym = huffman_decode(bs, dc);
            if (dsym < 0 || dsym >= 30) return PNG_ERR_INFLATE;
            int32_t dextra = DIST_EXTRA[dsym] ? bs_getbits(bs, DIST_EXTRA[dsym]) : 0;
            if (dextra < 0) return PNG_ERR_INFLATE;
            uint32_t dist = DIST_BASE[dsym] + (uint32_t)dextra;

            if (dist > out->pos) return PNG_ERR_INFLATE;
            size_t src = out->pos - dist;
            for (uint32_t i = 0; i < length; i++) {
                if (out_put(out, out->buf[src + i]) < 0) return PNG_ERR_INFLATE;
            }
        }
    }
}

static huffman_t g_fixed_lc, g_fixed_dc;
static int       g_fixed_built = 0;

static void build_fixed_tables(void) {
    uint8_t lengths[MAXLCODES];
    int sym = 0;
    for (; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < 288; sym++) lengths[sym] = 8;
    huffman_build(&g_fixed_lc, lengths, 288);

    uint8_t dlengths[MAXDCODES];
    for (sym = 0; sym < 30; sym++) dlengths[sym] = 5;
    huffman_build(&g_fixed_dc, dlengths, 30);

    g_fixed_built = 1;
}

/* Порядок индексов для таблицы длин кодов длин (RFC1951 §3.2.7) */
static const uint8_t CLEN_ORDER[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

static int inflate_dynamic(bitstream_t *bs, outbuf_t *out) {
    int32_t hlit  = bs_getbits(bs, 5);  /* +257 */
    int32_t hdist = bs_getbits(bs, 5);  /* +1   */
    int32_t hclen = bs_getbits(bs, 4);  /* +4   */
    if (hlit < 0 || hdist < 0 || hclen < 0) return PNG_ERR_INFLATE;
    hlit += 257; hdist += 1; hclen += 4;

    uint8_t cl_lengths[19];
    memset(cl_lengths, 0, sizeof(cl_lengths));
    for (int i = 0; i < hclen; i++) {
        int32_t v = bs_getbits(bs, 3);
        if (v < 0) return PNG_ERR_INFLATE;
        cl_lengths[CLEN_ORDER[i]] = (uint8_t)v;
    }
    huffman_t cl_table;
    huffman_build(&cl_table, cl_lengths, 19);

    uint8_t lengths[MAXCODES];
    memset(lengths, 0, sizeof(lengths));
    int idx = 0;
    int total = hlit + hdist;
    while (idx < total) {
        int sym = huffman_decode(bs, &cl_table);
        if (sym < 0) return PNG_ERR_INFLATE;
        if (sym < 16) {
            lengths[idx++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (idx == 0) return PNG_ERR_INFLATE;
            int32_t rep = bs_getbits(bs, 2);
            if (rep < 0) return PNG_ERR_INFLATE;
            rep += 3;
            uint8_t prev = lengths[idx - 1];
            while (rep-- && idx < total) lengths[idx++] = prev;
        } else if (sym == 17) {
            int32_t rep = bs_getbits(bs, 3);
            if (rep < 0) return PNG_ERR_INFLATE;
            rep += 3;
            while (rep-- && idx < total) lengths[idx++] = 0;
        } else { /* 18 */
            int32_t rep = bs_getbits(bs, 7);
            if (rep < 0) return PNG_ERR_INFLATE;
            rep += 11;
            while (rep-- && idx < total) lengths[idx++] = 0;
        }
    }

    huffman_t lc, dc;
    huffman_build(&lc, lengths, hlit);
    huffman_build(&dc, lengths + hlit, hdist);

    return inflate_block_data(bs, out, &lc, &dc);
}

/* zlib stream: 2-байтный заголовок, затем DEFLATE-блоки, затем 4-байтный adler32
 * (который мы не проверяем — нет необходимости для отрисовки). */
static int inflate_zlib(const uint8_t *in, size_t in_len, outbuf_t *out) {
    if (in_len < 2) return PNG_ERR_INFLATE;
    /* in[0..1] — zlib header (CMF/FLG), пропускаем */
    bitstream_t bs = { .in = in + 2, .in_len = in_len - 2, .in_pos = 0, .bitbuf = 0, .bitcnt = 0 };

    if (!g_fixed_built) build_fixed_tables();

    for (;;) {
        int32_t bfinal = bs_getbits(&bs, 1);
        if (bfinal < 0) return PNG_ERR_INFLATE;
        int32_t btype  = bs_getbits(&bs, 2);
        if (btype < 0) return PNG_ERR_INFLATE;

        int rc;
        switch (btype) {
            case 0: rc = inflate_stored(&bs, out); break;
            case 1: rc = inflate_block_data(&bs, out, &g_fixed_lc, &g_fixed_dc); break;
            case 2: rc = inflate_dynamic(&bs, out); break;
            default: return PNG_ERR_INFLATE;
        }
        if (rc != PNG_OK) return rc;

        if (bfinal) break;
        if (out->pos >= out->size) break; /* получили всё, что нужно */
    }
    return PNG_OK;
}

/* =========================================================================
 * PNG chunk parsing
 * ========================================================================= */
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

#define PNG_COLOR_GRAY     0
#define PNG_COLOR_RGB      2
#define PNG_COLOR_PALETTE  3
#define PNG_COLOR_GRAY_A   4
#define PNG_COLOR_RGBA     6

/* Paeth predictor (RFC2083 §6.6) */
static inline uint8_t paeth(int a, int b, int c) {
    int p  = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

int png_decode(const uint8_t *data, size_t size, png_image_t *out) {
    static const uint8_t PNG_SIG[8] = {0x89,'P','N','G','\r','\n',0x1A,'\n'};

    if (size < 8) return PNG_ERR_MAGIC;
    for (int i = 0; i < 8; i++)
        if (data[i] != PNG_SIG[i]) return PNG_ERR_MAGIC;

    size_t pos = 8;
    uint32_t width = 0, height = 0;
    uint8_t  bit_depth = 0, color_type = 0, interlace = 0;
    int have_ihdr = 0;

    /* PLTE (для color_type == PALETTE) — до 256 записей RGB */
    uint8_t palette[256 * 3];
    size_t  palette_count = 0;

    /* tRNS (для color_type == PALETTE) — альфа на запись палитры.
     * tRNS для GRAY/RGB (color-key transparency) не поддерживается —
     * это редкий случай для непрозрачных обоев/курсоров. */
    uint8_t trns_alpha[256];
    size_t  trns_count = 0;

    /* IDAT может быть разбит на несколько чанков — посчитаем общий размер,
     * затем соберём всё в один буфер. Для этого делаем два прохода.
     * Также вычитываем PLTE/tRNS на этом же проходе. */
    size_t idat_total = 0;
    size_t scan_pos = 8;
    while (scan_pos + 8 <= size) {
        uint32_t len = be32(data + scan_pos);
        const uint8_t *type = data + scan_pos + 4;
        if (scan_pos + 8 + (size_t)len + 4 > size) break;

        if (type[0]=='I' && type[1]=='H' && type[2]=='D' && type[3]=='R') {
            if (len < 13) return PNG_ERR_FORMAT;
            const uint8_t *p = data + scan_pos + 8;
            width      = be32(p);
            height     = be32(p + 4);
            bit_depth  = p[8];
            color_type = p[9];
            interlace  = p[12];
            have_ihdr = 1;
        } else if (type[0]=='P' && type[1]=='L' && type[2]=='T' && type[3]=='E') {
            const uint8_t *p = data + scan_pos + 8;
            palette_count = (size_t)len / 3;
            if (palette_count > 256) palette_count = 256;
            memcpy(palette, p, palette_count * 3);
        } else if (type[0]=='t' && type[1]=='R' && type[2]=='N' && type[3]=='S') {
            const uint8_t *p = data + scan_pos + 8;
            trns_count = (size_t)len;
            if (trns_count > 256) trns_count = 256;
            memcpy(trns_alpha, p, trns_count);
        } else if (type[0]=='I' && type[1]=='D' && type[2]=='A' && type[3]=='T') {
            idat_total += len;
        } else if (type[0]=='I' && type[1]=='E' && type[2]=='N' && type[3]=='D') {
            break;
        }
        scan_pos += 8 + (size_t)len + 4;
    }

    if (!have_ihdr) return PNG_ERR_FORMAT;
    if (interlace != 0) return PNG_ERR_UNSUPPORTED;
    if (width == 0 || height == 0 || idat_total == 0) return PNG_ERR_FORMAT;

    int channels;
    switch (color_type) {
        case PNG_COLOR_GRAY:    channels = 1; break;
        case PNG_COLOR_RGB:     channels = 3; break;
        case PNG_COLOR_PALETTE: channels = 1; break;
        case PNG_COLOR_GRAY_A:  channels = 2; break;
        case PNG_COLOR_RGBA:    channels = 4; break;
        default: return PNG_ERR_UNSUPPORTED;
    }

    /* RGB/RGBA/GRAY+ALPHA в этом декодере — только 8 бит/канал (без 16-bit).
     * GRAY и PALETTE — допускаем "узкие" битовые глубины 1/2/4/8,
     * это часто встречается в палитровых PNG, сохранённых оптимизаторами
     * (pngquant, tinypng и т.п.) или простыми редакторами для обоев/иконок. */
    if (color_type == PNG_COLOR_RGB || color_type == PNG_COLOR_GRAY_A ||
        color_type == PNG_COLOR_RGBA) {
        if (bit_depth != 8) return PNG_ERR_UNSUPPORTED;
    } else {
        if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8)
            return PNG_ERR_UNSUPPORTED;
    }
    if (color_type == PNG_COLOR_PALETTE && palette_count == 0) return PNG_ERR_FORMAT;

    /* "bpp" по терминологии спецификации PNG — байтовая дистанция,
     * используемая фильтрами Sub/Up/Average/Paeth. Для битовых глубин < 8
     * она всё равно округляется вверх до 1 байта (т.к. у нас всегда 1 канал
     * в этих случаях — GRAY или PALETTE). */
    int filt_bpp = (int)(((uint32_t)bit_depth * (uint32_t)channels + 7) / 8);
    if (filt_bpp < 1) filt_bpp = 1;

    /* Собираем все IDAT-чанки в один сплошной буфер */
    uint8_t *idat = (uint8_t *)kmalloc(idat_total);
    if (!idat) return PNG_ERR_NOMEM;
    {
        size_t off = 0;
        pos = 8;
        while (pos + 8 <= size) {
            uint32_t len = be32(data + pos);
            const uint8_t *type = data + pos + 4;
            if (pos + 8 + (size_t)len + 4 > size) break;
            if (type[0]=='I' && type[1]=='D' && type[2]=='A' && type[3]=='T') {
                memcpy(idat + off, data + pos + 8, len);
                off += len;
            } else if (type[0]=='I' && type[1]=='E' && type[2]=='N' && type[3]=='D') {
                break;
            }
            pos += 8 + (size_t)len + 4;
        }
    }

    /* Размер несжатых данных: каждая строка = 1 байт фильтра +
     * ceil(width*bit_depth*channels/8) байт упакованных сэмплов. */
    size_t bytes_per_row = ((size_t)width * (size_t)bit_depth * (size_t)channels + 7) / 8;
    size_t row_bytes = 1 + bytes_per_row;
    size_t raw_size  = row_bytes * (size_t)height;

    uint8_t *raw = (uint8_t *)kmalloc(raw_size);
    if (!raw) { kfree(idat); return PNG_ERR_NOMEM; }

    outbuf_t ob = { .buf = raw, .size = raw_size, .pos = 0 };
    int rc = inflate_zlib(idat, idat_total, &ob);
    kfree(idat);
    if (rc != PNG_OK || ob.pos < raw_size) {
        kfree(raw);
        return (rc != PNG_OK) ? rc : PNG_ERR_INFLATE;
    }

    /* Выделяем итоговый RGBA-буфер */
    uint8_t *rgba = (uint8_t *)kmalloc((size_t)width * (size_t)height * 4);
    if (!rgba) { kfree(raw); return PNG_ERR_NOMEM; }

    /* Применяем un-filter построчно (нужна предыдущая строка для Up/Avg/Paeth) */
    uint8_t *prev_line = (uint8_t *)kmalloc(bytes_per_row);
    uint8_t *cur_line  = (uint8_t *)kmalloc(bytes_per_row);
    if (!prev_line || !cur_line) {
        if (prev_line) kfree(prev_line);
        if (cur_line)  kfree(cur_line);
        kfree(rgba); kfree(raw);
        return PNG_ERR_NOMEM;
    }
    memset(prev_line, 0, bytes_per_row);

    uint32_t maxval = (1u << bit_depth) - 1; /* для масштабирования GRAY < 8 бит */

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src_row = raw + (size_t)y * row_bytes;
        uint8_t filter = src_row[0];
        const uint8_t *filt = src_row + 1;

        for (size_t x = 0; x < bytes_per_row; x++) {
            int raw_byte = filt[x];
            int a = (x >= (size_t)filt_bpp) ? cur_line[x - filt_bpp] : 0;
            int b = prev_line[x];
            int c = (x >= (size_t)filt_bpp) ? prev_line[x - filt_bpp] : 0;
            int val;
            switch (filter) {
                case 0: val = raw_byte; break;                       /* None */
                case 1: val = raw_byte + a; break;                   /* Sub */
                case 2: val = raw_byte + b; break;                   /* Up */
                case 3: val = raw_byte + ((a + b) >> 1); break;       /* Average */
                case 4: val = raw_byte + paeth(a, b, c); break;       /* Paeth */
                default:
                    kfree(prev_line); kfree(cur_line); kfree(rgba); kfree(raw);
                    return PNG_ERR_FORMAT;
            }
            cur_line[x] = (uint8_t)(val & 0xFF);
        }

        /* Конвертация строки в RGBA8888 */
        uint8_t *dst_row = rgba + (size_t)y * (size_t)width * 4;

        if (bit_depth == 8) {
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t *sp = cur_line + (size_t)x * (size_t)channels;
                uint8_t *dp = dst_row + (size_t)x * 4;
                switch (color_type) {
                    case PNG_COLOR_GRAY:
                        dp[0] = dp[1] = dp[2] = sp[0];
                        dp[3] = 0xFF;
                        break;
                    case PNG_COLOR_RGB:
                        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                        dp[3] = 0xFF;
                        break;
                    case PNG_COLOR_PALETTE: {
                        uint8_t idx = sp[0];
                        if ((size_t)idx < palette_count) {
                            dp[0] = palette[idx*3 + 0];
                            dp[1] = palette[idx*3 + 1];
                            dp[2] = palette[idx*3 + 2];
                        } else {
                            dp[0] = dp[1] = dp[2] = 0;
                        }
                        dp[3] = ((size_t)idx < trns_count) ? trns_alpha[idx] : 0xFF;
                        break;
                    }
                    case PNG_COLOR_GRAY_A:
                        dp[0] = dp[1] = dp[2] = sp[0];
                        dp[3] = sp[1];
                        break;
                    case PNG_COLOR_RGBA:
                        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
                        break;
                }
            }
        } else {
            /* bit_depth 1/2/4 — всегда 1 канал на пиксель (GRAY или PALETTE),
             * сэмплы упакованы по битам, MSB первым (RFC2083 §7). */
            for (uint32_t x = 0; x < width; x++) {
                size_t bit_off  = (size_t)x * bit_depth;
                size_t byte_off = bit_off / 8;
                int    shift    = 8 - bit_depth - (int)(bit_off % 8);
                uint8_t raw_val = (uint8_t)((cur_line[byte_off] >> shift) & maxval);
                uint8_t *dp = dst_row + (size_t)x * 4;

                if (color_type == PNG_COLOR_PALETTE) {
                    uint8_t idx = raw_val;
                    if ((size_t)idx < palette_count) {
                        dp[0] = palette[idx*3 + 0];
                        dp[1] = palette[idx*3 + 1];
                        dp[2] = palette[idx*3 + 2];
                    } else {
                        dp[0] = dp[1] = dp[2] = 0;
                    }
                    dp[3] = ((size_t)idx < trns_count) ? trns_alpha[idx] : 0xFF;
                } else { /* GRAY, масштабируем 0..maxval -> 0..255 */
                    uint32_t v = (uint32_t)raw_val * 255u / maxval;
                    dp[0] = dp[1] = dp[2] = (uint8_t)v;
                    dp[3] = 0xFF;
                }
            }
        }

        /* swap prev <-> cur */
        uint8_t *tmp = prev_line;
        prev_line = cur_line;
        cur_line  = tmp;
    }

    kfree(prev_line);
    kfree(cur_line);
    kfree(raw);

    out->width  = width;
    out->height = height;
    out->pixels = rgba;
    return PNG_OK;
}

#include "PalEngineCore.h"

#include <SDL3/SDL.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <errno.h>
#include <iconv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAL_TOC_OFFSET 0x1C
#define PAL_TOC_MAX_SLOTS 3000
#define PAL_TOC_REC_SIZE 44
#define PAL_MZ_HEADER_SIZE 0x200
#define PAL_TSW_KEY_TRANSPARENT 0x319F
#define PAL_TSW_KEY_SHADOW 0x026B

/* =========================================================================
 * Byte-order & Safe Buffer Utilities
 * ========================================================================= */

static inline uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline int16_t rd_i16(const uint8_t *p) {
    return (int16_t)rd_u16(p);
}

static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int join_path(char *dst, size_t cap, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] == '/') {
        return snprintf(dst, cap, "%s%s", dir, name) < (int)cap ? 0 : -1;
    }
    return snprintf(dst, cap, "%s/%s", dir, name) < (int)cap ? 0 : -1;
}

static uint8_t *read_entire_file(const char *path, size_t *out_size) {
    *out_size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = sz > 0 ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    buf[sz] = 0;
    *out_size = (size_t)sz;
    return buf;
}

static int read_file_slice(FILE *f, uint64_t offset, void *dst, size_t len) {
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

static uint64_t get_file_size_fp(FILE *f) {
    if (fseeko(f, 0, SEEK_END) != 0) return 0;
    off_t sz = ftello(f);
    return sz > 0 ? (uint64_t)sz : 0;
}

static void cp950_to_utf8(const uint8_t *src, size_t src_len, char *dst, size_t dst_cap) {
    if (!dst || dst_cap == 0) return;
    dst[0] = '\0';
    if (!src || src_len == 0) return;

    iconv_t cd = iconv_open("UTF-8", "CP950");
    if (cd == (iconv_t)-1) {
        size_t copy_n = src_len < dst_cap - 1 ? src_len : dst_cap - 1;
        for (size_t i = 0; i < copy_n; i++) {
            dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
        }
        dst[copy_n] = '\0';
        return;
    }

    char *inbuf = (char *)src;
    size_t inleft = src_len;
    char *outbuf = dst;
    size_t outleft = dst_cap - 1;

    while (inleft > 0 && outleft > 0) {
        size_t rc = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
        if (rc == (size_t)-1) {
            if (errno == EILSEQ || errno == EINVAL) {
                if (outleft < 1) break;
                *outbuf++ = '?';
                outleft--;
                inbuf++;
                inleft--;
            } else {
                break;
            }
        }
    }
    *outbuf = '\0';
    iconv_close(cd);
}

/* =========================================================================
 * LZO1X Decompressor (Clean-Room Bounds-Checked)
 * ========================================================================= */

static int pal_lzo1x_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len) {
    if (!in || !out || !out_len || in_len == 0) {
        if (out_len) *out_len = 0;
        return -1;
    }

    const uint8_t *ip = in;
    const uint8_t *const ip_end = in + in_len;
    uint8_t *op = out;
    const uint8_t *const out_end = out + *out_len;
    const uint8_t *m_pos;
    size_t t;

#define LZO_NEED_IN(n) do { if ((size_t)(ip_end - ip) < (size_t)(n)) { *out_len = (size_t)(op - out); return -1; } } while (0)
#define LZO_NEED_OUT(n) do { if ((size_t)(out_end - op) < (size_t)(n)) { *out_len = (size_t)(op - out); return -3; } } while (0)
#define LZO_CHECK_MPOS(p) do { if ((p) < out || (p) >= op) { *out_len = (size_t)(op - out); return -4; } } while (0)

    if (*ip > 17) {
        t = *ip++ - 17;
        if (t < 4) goto match_next;
        LZO_NEED_IN(t);
        LZO_NEED_OUT(t);
        do *op++ = *ip++; while (--t > 0);
        goto first_literal_run;
    }

    while (ip < ip_end) {
        t = *ip++;
        if (t >= 16) goto match;
        if (t == 0) {
            while (ip < ip_end && *ip == 0) {
                t += 255;
                ip++;
            }
            LZO_NEED_IN(1);
            t += 15 + *ip++;
        }
        LZO_NEED_IN(t + 3);
        LZO_NEED_OUT(t + 3);
        size_t lit = t + 3;
        do *op++ = *ip++; while (--lit > 0);

first_literal_run:
        LZO_NEED_IN(1);
        t = *ip++;
        if (t >= 16) goto match;
        LZO_NEED_IN(1);
        LZO_NEED_OUT(3);
        m_pos = op - (1 + 0x800);
        m_pos -= t >> 2;
        m_pos -= *ip++ << 2;
        LZO_CHECK_MPOS(m_pos);
        *op++ = *m_pos++;
        *op++ = *m_pos++;
        *op++ = *m_pos++;
        goto match_done;

        do {
match:
            if (t >= 64) {
                LZO_NEED_IN(1);
                m_pos = op - 1;
                m_pos -= (t >> 2) & 7;
                m_pos -= *ip++ << 3;
                t = (t >> 5) - 1;
            } else if (t >= 32) {
                t &= 31;
                if (t == 0) {
                    while (ip < ip_end && *ip == 0) {
                        t += 255;
                        ip++;
                    }
                    LZO_NEED_IN(1);
                    t += 31 + *ip++;
                }
                LZO_NEED_IN(2);
                m_pos = op - 1;
                m_pos -= ((uint16_t)ip[0] | ((uint16_t)ip[1] << 8)) >> 2;
                ip += 2;
            } else if (t >= 16) {
                m_pos = op;
                m_pos -= (t & 8) << 11;
                t &= 7;
                if (t == 0) {
                    while (ip < ip_end && *ip == 0) {
                        t += 255;
                        ip++;
                    }
                    LZO_NEED_IN(1);
                    t += 7 + *ip++;
                }
                LZO_NEED_IN(2);
                m_pos -= ((uint16_t)ip[0] | ((uint16_t)ip[1] << 8)) >> 2;
                ip += 2;
                if (m_pos == op) goto eof_found;
                m_pos -= 0x4000;
            } else {
                LZO_NEED_IN(1);
                LZO_NEED_OUT(2);
                m_pos = op - 1;
                m_pos -= t >> 2;
                m_pos -= *ip++ << 2;
                LZO_CHECK_MPOS(m_pos);
                *op++ = *m_pos++;
                *op++ = *m_pos++;
                goto match_done;
            }

            LZO_CHECK_MPOS(m_pos);
            LZO_NEED_OUT(t + 2);
            *op++ = *m_pos++;
            *op++ = *m_pos++;
            do *op++ = *m_pos++; while (--t > 0);

match_done:
            t = ip[-2] & 3;
            if (t == 0) break;
match_next:
            LZO_NEED_IN(t + 1);
            LZO_NEED_OUT(t);
            do *op++ = *ip++; while (--t > 0);
            t = *ip++;
        } while (ip < ip_end);
    }

eof_found:
    *out_len = (size_t)(op - out);
    return (ip == ip_end ? 0 : (ip < ip_end ? -1 : -2));
}

static uint8_t *lzo_alloc_decompress(const uint8_t *src, size_t src_len, size_t expected_len, int allow_trunc, size_t *actual_len) {
    *actual_len = 0;
    if (!src || src_len == 0) return NULL;

    size_t cap = expected_len > 0 ? expected_len : src_len * 10;
    if (cap < 65536) cap = 65536;
    const size_t max_cap = 64u * 1024u * 1024u;

    while (cap <= max_cap) {
        uint8_t *out = (uint8_t *)calloc(1, cap);
        if (!out) return NULL;
        size_t out_len = cap;
        int rc = pal_lzo1x_decompress(src, src_len, out, &out_len);
        if (rc == 0 || (rc == -1 && ((expected_len > 0 && out_len == expected_len) || allow_trunc))) {
            *actual_len = (allow_trunc && expected_len > out_len) ? expected_len : out_len;
            return out;
        }
        free(out);
        if (rc == -3) {
            cap *= 2;
            continue;
        }
        return NULL;
    }
    return NULL;
}

/* =========================================================================
 * Pixel & ARLE Decoders -> RGBA32 ([R, G, B, A])
 * ========================================================================= */

static inline void rgb555_write_rgba(uint8_t *dst, uint16_t c, uint8_t a) {
    uint8_t r5 = (uint8_t)((c >> 10) & 0x1F);
    uint8_t g5 = (uint8_t)((c >> 5) & 0x1F);
    uint8_t b5 = (uint8_t)(c & 0x1F);
    dst[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    dst[1] = (uint8_t)((g5 << 3) | (g5 >> 2));
    dst[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    dst[3] = a;
}

static inline void rgb555_write_keyed(uint8_t *dst, uint16_t c) {
    /* Verified in Palgame.exe (0x401d84, 0x438acc, 0x465e43): both 0x319F and 0x026B are transparent chromakeys. */
    if (c == PAL_TSW_KEY_TRANSPARENT || c == PAL_TSW_KEY_SHADOW) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
    } else {
        rgb555_write_rgba(dst, c, 255);
    }
}

static void decode_arle_to_rgba(
    const uint8_t *data, size_t data_len,
    uint16_t w, uint16_t h, uint16_t bpp,
    const uint16_t *palette,
    const uint8_t *mask, size_t mask_len,
    uint8_t *rgba_out
) {
    size_t total_px = (size_t)w * (size_t)h;
    memset(rgba_out, 0, total_px * 4);
    (void)mask;
    (void)mask_len;

    size_t bp = 8;
    for (uint16_t row = 0; row < h && bp + 2 <= data_len; row++) {
        uint16_t row_marker = rd_u16(data + bp);
        if (row_marker == 0) break;
        bp += 2;
        size_t col = 0;
        size_t row_base = (size_t)row * (size_t)w;
        while (bp + 2 <= data_len) {
            uint16_t token = rd_u16(data + bp);
            bp += 2;
            if (token == 0) break;

            uint16_t tag = token & 0xC000u;
            uint16_t count = token & 0x3FFFu;
            if (tag == 0) {
                for (uint16_t i = 0; i < count; i++) {
                    size_t dst_idx = row_base + col;
                    if (bpp == 16) {
                        if (bp + 2 <= data_len) {
                            uint16_t px = rd_u16(data + bp);
                            bp += 2;
                            if (col < w && dst_idx < total_px) {
                                rgb555_write_keyed(rgba_out + dst_idx * 4, px);
                            }
                        }
                    } else {
                        if (bp < data_len) {
                            uint8_t pidx = data[bp++];
                            uint16_t c = palette ? palette[pidx] : 0;
                            if (col < w && dst_idx < total_px) {
                                rgb555_write_keyed(rgba_out + dst_idx * 4, c);
                            }
                        }
                    }
                    col++;
                }
            } else {
                /* Both 0x8000 (0x319F) and 0xC000 (0x026B) are transparent skip runs (0x438acc). */
                col += count;
            }
        }
    }
}

/* =========================================================================
 * Container Parsers: 3000-slot TOC (.tsw, .act, .snd) & LMF & DB (.dat)
 * ========================================================================= */

typedef struct {
    uint32_t slot_index;
    uint32_t id;
    uint32_t offset;
    uint32_t size_raw;
    uint8_t flag;
    char name[24];
} PalTocEntry;

typedef struct {
    uint32_t index;
    uint32_t id;
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
} PalLmfEntry;

static int read_archive_toc(FILE *f, uint64_t fsize, PalTocEntry **out_entries, int *out_count) {
    *out_entries = NULL;
    *out_count = 0;
    if (fsize < PAL_TOC_OFFSET) return -1;

    uint64_t slots_avail = (fsize - PAL_TOC_OFFSET) / PAL_TOC_REC_SIZE;
    int max_possible = (int)(slots_avail < PAL_TOC_MAX_SLOTS ? slots_avail : PAL_TOC_MAX_SLOTS);
    if (max_possible <= 0) return 0;

    size_t toc_bytes = (size_t)max_possible * PAL_TOC_REC_SIZE;
    uint8_t *toc = (uint8_t *)malloc(toc_bytes);
    if (!toc) return -1;
    if (read_file_slice(f, PAL_TOC_OFFSET, toc, toc_bytes) != 0) {
        free(toc);
        return -1;
    }

    PalTocEntry *arr = (PalTocEntry *)calloc((size_t)max_possible, sizeof(PalTocEntry));
    if (!arr) {
        free(toc);
        return -1;
    }

    int cnt = 0;
    for (int i = 0; i < max_possible; i++) {
        const uint8_t *rec = toc + (size_t)i * PAL_TOC_REC_SIZE;
        uint32_t sz_raw = rd_u32(rec + 20);
        uint32_t off = rd_u32(rec + 24);
        uint32_t sid = rd_u32(rec + 28);
        if (off == 0 || sz_raw == 0 || off >= fsize) continue;

        PalTocEntry *e = &arr[cnt++];
        e->slot_index = (uint32_t)i;
        e->id = sid;
        e->offset = off;
        e->size_raw = sz_raw;
        e->flag = rec[0x20] & 3u;
        memcpy(e->name, rec, 20);
        e->name[20] = '\0';
    }
    free(toc);
    *out_entries = arr;
    *out_count = cnt;
    return 0;
}

static int read_lmf_directory(FILE *f, uint64_t fsize, PalLmfEntry **out_entries, int *out_count) {
    *out_entries = NULL;
    *out_count = 0;
    if (fsize < 20) return -1;

    uint8_t hdr4[4];
    if (read_file_slice(f, 0, hdr4, 4) != 0) return -1;
    uint32_t idx_off = rd_u32(hdr4);
    if (idx_off >= fsize || fsize - idx_off < 16) return -1;

    int raw_n = (int)((fsize - idx_off) / 16);
    uint8_t *dir = (uint8_t *)malloc((size_t)raw_n * 16);
    if (!dir) return -1;
    if (read_file_slice(f, idx_off, dir, (size_t)raw_n * 16) != 0) {
        free(dir);
        return -1;
    }

    PalLmfEntry *arr = (PalLmfEntry *)calloc((size_t)raw_n, sizeof(PalLmfEntry));
    if (!arr) {
        free(dir);
        return -1;
    }

    int cnt = 0;
    for (int i = 0; i < raw_n; i++) {
        const uint8_t *r = dir + (size_t)i * 16;
        uint32_t m_off = rd_u32(r);
        uint32_t m_sz = rd_u32(r + 4);
        uint32_t m_id = rd_u32(r + 8);
        uint32_t m_flg = rd_u32(r + 12);
        if (m_off >= fsize || m_sz == 0) continue;
        arr[cnt].index = (uint32_t)i;
        arr[cnt].id = m_id;
        arr[cnt].offset = m_off;
        arr[cnt].size = m_sz;
        arr[cnt].flags = m_flg;
        cnt++;
    }
    free(dir);
    *out_entries = arr;
    *out_count = cnt;
    return 0;
}

/* =========================================================================
 * .act 35-Opcode Bytecode Analyzer
 * ========================================================================= */

typedef struct {
    uint16_t code;
    uint8_t nargs;
} ActOpcodeSpec;

static const ActOpcodeSpec kActOpcodes[] = {
    {0x4544, 0}, {0x4F56, 0}, {0x4F32, 0}, {0x4E4F, 0}, {0x5649, 0},
    {0x534D, 0}, {0x434C, 0}, {0x4148, 0}, {0x414D, 0}, {0x414E, 0},
    {0x544E, 1}, {0x5246, 1}, {0x524F, 1}, {0x5344, 1}, {0x5041, 1},
    {0x5859, 2}, {0x5748, 2}, {0x4F58, 1}, {0x4F59, 1}, {0x4F41, 1},
    {0x5756, 1}, {0x5145, 1}, {0x4154, 1}, {0x4145, 1}, {0x4158, 1},
    {0x4159, 1}, {0x5457, 1}, {0x5343, 1}, {0x4753, 1}, {0x4C44, 1},
    {0x4C50, 3}, {0x4347, 2}, {0x4342, 2}, {0x4352, 2}, {0x464C, 7},
};

static int lookup_act_nargs(uint16_t code) {
    size_t n = sizeof(kActOpcodes) / sizeof(kActOpcodes[0]);
    for (size_t i = 0; i < n; i++) {
        if (kActOpcodes[i].code == code) return (int)kActOpcodes[i].nargs;
    }
    return -1;
}

typedef struct {
    int sub_actions;
    int decoded_insns;
    int clean_streams;
    int16_t linked_tsw_id;
    int16_t frame_delay;
} PalActInfo;

static int vfs_inspect_act_slot(const char *path, int target_idx, PalActInfo *out) {
    memset(out, 0, sizeof(*out));
    out->linked_tsw_id = -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t fsize = get_file_size_fp(f);

    PalTocEntry *entries = NULL;
    int cnt = 0;
    if (read_archive_toc(f, fsize, &entries, &cnt) != 0 || cnt == 0) {
        free(entries);
        fclose(f);
        return -1;
    }
    if (target_idx < 0 || target_idx >= cnt) target_idx = 0;
    PalTocEntry sel = entries[target_idx];
    free(entries);

    if (sel.size_raw < 2 || (uint64_t)sel.offset + sel.size_raw > fsize) {
        fclose(f);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc(sel.size_raw);
    if (!buf || read_file_slice(f, sel.offset, buf, sel.size_raw) != 0) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    uint16_t sub_cnt = rd_u16(buf);
    uint32_t max_cnt = (sel.size_raw - 2) / 4;
    if (sub_cnt > max_cnt) sub_cnt = (uint16_t)max_cnt;
    uint32_t hdr_end = 2 + (uint32_t)sub_cnt * 4;

    for (uint16_t a = 0; a < sub_cnt; a++) {
        uint32_t off = rd_u32(buf + 2 + (size_t)a * 4);
        if (off < hdr_end || off + 2 > sel.size_raw) continue;
        out->sub_actions++;

        uint32_t end = sel.size_raw;
        for (uint16_t b = 0; b < sub_cnt; b++) {
            uint32_t o2 = rd_u32(buf + 2 + (size_t)b * 4);
            if (o2 > off && o2 < end) end = o2;
        }

        size_t pos = off + 2; /* skip 2-byte stream header */
        int clean = 1;
        while (pos + 2 <= end) {
            uint16_t code = rd_u16(buf + pos);
            pos += 2;
            if (code == 0) continue;
            int nargs = lookup_act_nargs(code);
            if (nargs < 0 || pos + (size_t)nargs * 2 > end) {
                clean = 0;
                break;
            }
            if (code == 0x544E && out->linked_tsw_id < 0) {
                out->linked_tsw_id = rd_i16(buf + pos);
            } else if (code == 0x5246 && out->frame_delay == 0) {
                out->frame_delay = rd_i16(buf + pos);
            }
            out->decoded_insns++;
            pos += (size_t)nargs * 2;
        }
        if (clean && pos == end) out->clean_streams++;
    }

    free(buf);
    return 0;
}

/* =========================================================================
 * 0x7F Database + maps.dat + namex.dat + talk1.dat Validation & Metadata
 * ========================================================================= */

static int parse_tag7f_db(
    const uint8_t *buf, size_t len,
    int *out_records,
    PalHeroSummary *heroes_out, int *hero_cnt_out, int max_heroes
) {
    *out_records = 0;
    if (len < PAL_MZ_HEADER_SIZE + 4 || buf[0] != 'M' || buf[1] != 'Z') return -1;

    const uint8_t *p = buf + PAL_MZ_HEADER_SIZE;
    size_t plen = len - PAL_MZ_HEADER_SIZE;
    if ((rd_u32(p) >> 24) != 0x7F) return -1;

    size_t pos = 0;
    int records = 0;
    int in_rec = 0;
    PalHeroSummary cur_hero;
    memset(&cur_hero, 0, sizeof(cur_hero));

    while (pos + 4 <= plen) {
        uint32_t tag = rd_u32(p + pos);
        pos += 4;
        if (tag == 0x7F00FFFFu) {
            if (in_rec) {
                if (heroes_out && hero_cnt_out && *hero_cnt_out < max_heroes && cur_hero.id != 0) {
                    heroes_out[*hero_cnt_out] = cur_hero;
                    (*hero_cnt_out)++;
                }
                records++;
                in_rec = 0;
                memset(&cur_hero, 0, sizeof(cur_hero));
            }
            continue;
        }
        if ((tag >> 24) != 0x7F) return -1;

        in_rec = 1;
        int is_str = ((tag & 0xFFFFu) >= 0xF000u) && (((tag & 0xF0000u) >> 16) != 0);
        if (is_str) {
            const uint8_t *end = NULL;
            for (size_t k = pos; k + 1 < plen; k++) {
                if (p[k] == '%' && p[k + 1] == '%') {
                    end = p + k;
                    break;
                }
            }
            if (!end) return -1;
            if (tag == 0x7FF2F000u && heroes_out) {
                cp950_to_utf8(p + pos, (size_t)(end - (p + pos)), cur_hero.name, sizeof(cur_hero.name));
            }
            pos = (size_t)(end - p) + 2;
        } else {
            uint32_t scalars[4] = {0};
            int n_sc = 0;
            while (pos + 4 <= plen) {
                uint32_t u = rd_u32(p + pos);
                if ((u >> 24) == 0x7F) break;
                if (n_sc < 4) scalars[n_sc] = u;
                n_sc++;
                pos += 4;
            }
            if (heroes_out && tag == 0x7F101000u && n_sc >= 1) {
                cur_hero.id = (int32_t)scalars[0];
            } else if (heroes_out && tag == 0x7F201001u && n_sc == 2) {
                int32_t pid = (int32_t)scalars[0];
                int32_t val = (int32_t)scalars[1];
                switch (pid) {
                    case 5:  cur_hero.lv = val; break;
                    case 16: cur_hero.max_hp = val; break;
                    case 17: cur_hero.cur_hp = val; break;
                    case 22: cur_hero.max_mp = val; break;
                    case 23: cur_hero.cur_mp = val; break;
                    case 29: cur_hero.atk = val; break;
                    case 35: cur_hero.mag = val; break;
                    case 41: cur_hero.def = val; break;
                    case 47: cur_hero.spd = val; break;
                    case 53: cur_hero.luck = val; break;
                    default: break;
                }
            }
        }
    }
    if (in_rec || pos != plen) return -1;
    *out_records = records;
    return 0;
}

static int parse_maps_titles(const uint8_t *buf, size_t len, int *out_title_count, char *first_title, size_t first_cap) {
    *out_title_count = 0;
    if (first_title && first_cap > 0) first_title[0] = '\0';
    if (len < PAL_MZ_HEADER_SIZE + 104 || buf[0] != 'M' || buf[1] != 'Z') return -1;

    const uint8_t *p = buf + PAL_MZ_HEADER_SIZE;
    size_t plen = len - PAL_MZ_HEADER_SIZE;
    if (rd_u32(p) != 100u) return -1;

    uint32_t s19_off = rd_u32(p + 4 + 19 * 4);
    if (s19_off >= plen) return -1;

    size_t s19_end = plen;
    for (int i = 0; i < 25; i++) {
        uint32_t o = rd_u32(p + 4 + (size_t)i * 4);
        if (o > s19_off && o < s19_end) s19_end = o;
    }

    size_t pos = s19_off;
    int titles = 0;
    while (pos + 2 <= s19_end) {
        uint16_t mid = rd_u16(p + pos);
        if (mid == 0xFFFFu) break;
        pos += 2;
        const uint8_t *end = NULL;
        for (size_t k = pos; k + 1 < s19_end; k++) {
            if (p[k] == '%' && p[k + 1] == 'Q') {
                end = p + k;
                break;
            }
        }
        if (!end) break;
        if (titles == 0 && first_title && first_cap > 0) {
            cp950_to_utf8(p + pos, (size_t)(end - (p + pos)), first_title, first_cap);
        }
        titles++;
        pos = (size_t)(end - p) + 2;
    }
    *out_title_count = titles;
    return titles > 0 ? 0 : -1;
}

static int parse_namex_count(const uint8_t *buf, size_t len, int *out_non_empty) {
    *out_non_empty = 0;
    const size_t n_slots = 7100;
    if (len < PAL_MZ_HEADER_SIZE + n_slots * 4 || buf[0] != 'M' || buf[1] != 'Z') return -1;

    const uint8_t *p = buf + PAL_MZ_HEADER_SIZE;
    size_t plen = len - PAL_MZ_HEADER_SIZE;
    int cnt = 0;
    for (size_t i = 0; i < n_slots; i++) {
        uint32_t off = rd_u32(p + i * 4);
        if (off < n_slots * 4 || off >= plen) continue;
        const uint8_t *end = NULL;
        for (size_t k = off; k + 1 < plen; k++) {
            if (p[k] == '%' && p[k + 1] == 'Q') {
                end = p + k;
                break;
            }
        }
        if (end && end > p + off) cnt++;
    }
    *out_non_empty = cnt;
    return cnt > 0 ? 0 : -1;
}

static int validate_talk1_and_extract_sample(const uint8_t *buf, size_t len, int *out_scripts, char *sample_out, size_t sample_cap) {
    *out_scripts = 0;
    if (sample_out && sample_cap > 0) sample_out[0] = '\0';
    const size_t n_slots = 701;
    if (len < PAL_MZ_HEADER_SIZE + n_slots * 4 || buf[0] != 'M' || buf[1] != 'Z') return -1;

    const uint8_t *p = buf + PAL_MZ_HEADER_SIZE;
    size_t plen = len - PAL_MZ_HEADER_SIZE;
    if (rd_u32(p) != 100u && rd_u32(p) != 0xFFFFFFFFu) return -1;

    size_t data_start = n_slots * 4;
    int valid_scripts = 0;

    for (size_t i = 0; i < n_slots; i++) {
        uint32_t off = rd_u32(p + i * 4);
        if (off == 0xFFFFFFFFu || off < data_start) continue;
        if (off + 2 > plen) return -1;
        if (off + 4 <= plen && rd_u32(p + off) == 0xFFFF0402u) continue;
        valid_scripts++;

        if (sample_out && sample_out[0] == '\0') {
            uint16_t raw = rd_u16(p + off);
            uint16_t op = raw & 0x3FFFu;
            size_t hdr_bytes = 0;
            if (op == 1 || op == 2) hdr_bytes = 6;
            else if (op >= 3 && op <= 6) hdr_bytes = 14;
            else if (op == 0x59 || op == 0x5A) hdr_bytes = 10;

            if (hdr_bytes > 0 && off + hdr_bytes < plen) {
                for (size_t k = off + hdr_bytes; k + 1 < plen; k++) {
                    if (p[k] == '%' && p[k + 1] == 'Q') {
                        if (k > off + hdr_bytes) {
                            cp950_to_utf8(p + off + hdr_bytes, k - (off + hdr_bytes), sample_out, sample_cap);
                        }
                        break;
                    }
                }
            }
        }
    }
    *out_scripts = valid_scripts;
    return valid_scripts > 0 ? 0 : -1;
}

/* =========================================================================
 * Validation Helper Registry
 * ========================================================================= */

static void record_check(
    PalValidationReport *r,
    const char *filename,
    const char *category,
    int exists,
    int sig_ok,
    int count,
    const char *detail
) {
    if (r->check_count >= PAL_MAX_CHECKS) return;
    PalArchiveCheck *c = &r->checks[r->check_count++];
    snprintf(c->filename, sizeof(c->filename), "%s", filename);
    snprintf(c->category, sizeof(c->category), "%s", category);
    c->exists = exists;
    c->signature_ok = sig_ok;
    c->item_count = count;
    snprintf(c->detail, sizeof(c->detail), "%s", detail);
    if (exists && sig_ok) r->valid_archives_count++;
}

int32_t pal_sdl3_query_metal_backend(char *out_drivers, int32_t out_cap) {
    int n = SDL_GetNumRenderDrivers();
    int has_metal = 0;
    if (out_drivers && out_cap > 0) {
        out_drivers[0] = '\0';
        size_t used = 0;
        for (int i = 0; i < n; i++) {
            const char *name = SDL_GetRenderDriver(i);
            if (!name) continue;
            if (strcmp(name, "metal") == 0) has_metal = 1;
            int w = snprintf(out_drivers + used, (size_t)out_cap - used, "%s%s", i > 0 ? ", " : "", name);
            if (w > 0 && used + (size_t)w < (size_t)out_cap) used += (size_t)w;
        }
    } else {
        for (int i = 0; i < n; i++) {
            const char *name = SDL_GetRenderDriver(i);
            if (name && strcmp(name, "metal") == 0) has_metal = 1;
        }
    }
    return has_metal;
}

int32_t pal_vfs_validate_directory(const char *game_dir, PalValidationReport *report) {
    if (!report) return -1;
    memset(report, 0, sizeof(*report));
    snprintf(report->encoding_name, sizeof(report->encoding_name), "CP950 (Big5)");
    int has_metal = pal_sdl3_query_metal_backend(NULL, 0);
    snprintf(report->sdl_render_driver, sizeof(report->sdl_render_driver), "%s", has_metal ? "metal" : "unavailable");

    if (!game_dir || game_dir[0] == '\0') {
        snprintf(report->summary_message, sizeof(report->summary_message), "No game directory selected.");
        return -1;
    }

    static const char *const kTswFiles[] = {
        "all_char.tsw", "all_item.tsw", "all_magic.tsw", "all_map1.tsw", "all_map2.tsw", "all_sys.tsw"
    };
    static const char *const kActFiles[] = {
        "all_char.act", "all_item.act", "all_magic.act", "all_map1.act", "all_map2.act", "all_sys.act"
    };
    static const char *const kLmfFiles[] = {"huge.lmf", "huge2.lmf"};
    static const char *const k7fFiles[] = {"ghero.dat", "gmon.dat", "gitem.dat", "gmagic.dat", "GFight1.dat"};

    report->total_required_archives = 6 + 6 + 1 + 2 + 5 + 3; /* 23 core archives & DBs */

    char path[1200];

    /* 1. Validate all_*.tsw */
    for (int i = 0; i < 6; i++) {
        join_path(path, sizeof(path), game_dir, kTswFiles[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            record_check(report, kTswFiles[i], "tsw", 0, 0, 0, "Missing file");
            continue;
        }
        uint64_t fsize = get_file_size_fp(f);
        PalTocEntry *entries = NULL;
        int cnt = 0;
        int valid_tsw = 0;
        if (read_archive_toc(f, fsize, &entries, &cnt) == 0 && cnt > 0) {
            for (int s = 0; s < cnt; s++) {
                uint8_t chdr[12];
                if (entries[s].offset + 12 <= fsize && read_file_slice(f, entries[s].offset, chdr, 12) == 0) {
                    if (rd_u16(chdr + 4) == 0xABCDu) valid_tsw++;
                }
            }
        }
        free(entries);
        fclose(f);
        if (i == 0) report->char_sprite_slots = valid_tsw;
        char msg[128];
        snprintf(msg, sizeof(msg), valid_tsw > 0 ? "%d valid sprite slots (0xABCD)" : "Invalid TSW header", valid_tsw);
        record_check(report, kTswFiles[i], "tsw", 1, valid_tsw > 0, valid_tsw, msg);
    }

    /* 2. Validate all_*.act */
    for (int i = 0; i < 6; i++) {
        join_path(path, sizeof(path), game_dir, kActFiles[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            record_check(report, kActFiles[i], "act", 0, 0, 0, "Missing file");
            continue;
        }
        uint64_t fsize = get_file_size_fp(f);
        PalTocEntry *entries = NULL;
        int cnt = 0;
        int valid_act = 0;
        if (read_archive_toc(f, fsize, &entries, &cnt) == 0 && cnt > 0) {
            for (int s = 0; s < cnt; s++) {
                if (entries[s].size_raw >= 2 && (uint64_t)entries[s].offset + entries[s].size_raw <= fsize) {
                    valid_act++;
                }
            }
            if (valid_act > 0) {
                uint8_t hdr2[2];
                if (read_file_slice(f, entries[0].offset, hdr2, 2) == 0) {
                    uint16_t sub_cnt = rd_u16(hdr2);
                    if (sub_cnt > (entries[0].size_raw - 2) / 4) valid_act = 0;
                }
            }
        }
        free(entries);
        fclose(f);
        if (i == 0) report->char_act_slots = valid_act;
        char msg[128];
        snprintf(msg, sizeof(msg), valid_act > 0 ? "%d action script slots" : "Invalid ACT TOC", valid_act);
        record_check(report, kActFiles[i], "act", 1, valid_act > 0, valid_act, msg);
    }

    /* 3. Validate all.snd */
    {
        join_path(path, sizeof(path), game_dir, "all.snd");
        FILE *f = fopen(path, "rb");
        if (!f) {
            record_check(report, "all.snd", "snd", 0, 0, 0, "Missing file");
        } else {
            uint64_t fsize = get_file_size_fp(f);
            PalTocEntry *entries = NULL;
            int cnt = 0;
            int valid_snd = 0;
            if (read_archive_toc(f, fsize, &entries, &cnt) == 0 && cnt > 0) {
                for (int s = 0; s < cnt; s++) {
                    uint32_t sz = entries[s].size_raw & 0x03FFFFFFu;
                    if (sz > 0 && (uint64_t)entries[s].offset + sz <= fsize) valid_snd++;
                }
            }
            free(entries);
            fclose(f);
            report->snd_slots = valid_snd;
            char msg[128];
            snprintf(msg, sizeof(msg), valid_snd > 0 ? "%d sound slots (WAV/PCM)" : "Invalid SND TOC", valid_snd);
            record_check(report, "all.snd", "snd", 1, valid_snd > 0, valid_snd, msg);
        }
    }

    /* 4. Validate huge.lmf & huge2.lmf */
    for (int i = 0; i < 2; i++) {
        join_path(path, sizeof(path), game_dir, kLmfFiles[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            record_check(report, kLmfFiles[i], "lmf", 0, 0, 0, "Missing file");
            continue;
        }
        uint64_t fsize = get_file_size_fp(f);
        PalLmfEntry *maps = NULL;
        int cnt = 0;
        int msf3_ok = 0;
        if (read_lmf_directory(f, fsize, &maps, &cnt) == 0 && cnt > 0) {
            uint8_t magic[4];
            if (read_file_slice(f, maps[0].offset, magic, 4) == 0 && memcmp(magic, "MSF3", 4) == 0) {
                msf3_ok = 1;
            }
        }
        free(maps);
        fclose(f);
        if (i == 0) report->huge_map_count = msf3_ok ? cnt : 0;
        else report->huge2_map_count = msf3_ok ? cnt : 0;
        char msg[128];
        snprintf(msg, sizeof(msg), msf3_ok ? "%d maps (MSF3 verified)" : "Missing MSF3 signature", cnt);
        record_check(report, kLmfFiles[i], "lmf", 1, msf3_ok, cnt, msg);
    }

    /* 5. Validate 0x7F Databases */
    for (int i = 0; i < 5; i++) {
        join_path(path, sizeof(path), game_dir, k7fFiles[i]);
        size_t sz = 0;
        uint8_t *buf = read_entire_file(path, &sz);
        if (!buf) {
            record_check(report, k7fFiles[i], "dat", 0, 0, 0, "Missing file");
            continue;
        }
        int rec_cnt = 0;
        int rc = (i == 0)
            ? parse_tag7f_db(buf, sz, &rec_cnt, report->heroes, &report->hero_summary_count, PAL_MAX_HEROES)
            : parse_tag7f_db(buf, sz, &rec_cnt, NULL, NULL, 0);
        free(buf);
        if (rc == 0) {
            if (i == 0) report->hero_count = rec_cnt;
            else if (i == 1) report->monster_count = rec_cnt;
            else if (i == 2) report->item_count = rec_cnt;
            else if (i == 3) report->magic_count = rec_cnt;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), rc == 0 ? "%d 0x7F TLV records (MZ+0x200)" : "Corrupt 0x7F TLV stream", rec_cnt);
        record_check(report, k7fFiles[i], "dat", 1, rc == 0, rec_cnt, msg);
    }

    /* 6. Validate namex.dat, maps.dat, talk1.dat */
    {
        join_path(path, sizeof(path), game_dir, "namex.dat");
        size_t sz = 0;
        uint8_t *buf = read_entire_file(path, &sz);
        if (!buf) {
            record_check(report, "namex.dat", "dat", 0, 0, 0, "Missing file");
        } else {
            int cnt = 0;
            int rc = parse_namex_count(buf, sz, &cnt);
            free(buf);
            report->namex_count = cnt;
            char msg[128];
            snprintf(msg, sizeof(msg), rc == 0 ? "%d CP950 strings (%%Q)" : "Invalid namex table", cnt);
            record_check(report, "namex.dat", "dat", 1, rc == 0, cnt, msg);
        }
    }
    {
        join_path(path, sizeof(path), game_dir, "maps.dat");
        size_t sz = 0;
        uint8_t *buf = read_entire_file(path, &sz);
        if (!buf) {
            record_check(report, "maps.dat", "dat", 0, 0, 0, "Missing file");
        } else {
            int cnt = 0;
            int rc = parse_maps_titles(buf, sz, &cnt, report->sample_map_title, sizeof(report->sample_map_title));
            free(buf);
            report->map_title_count = cnt;
            char msg[128];
            snprintf(msg, sizeof(msg), rc == 0 ? "%d map titles (magic=100, slot 19)" : "Invalid maps.dat header", cnt);
            record_check(report, "maps.dat", "dat", 1, rc == 0, cnt, msg);
        }
    }
    {
        join_path(path, sizeof(path), game_dir, "talk1.dat");
        size_t sz = 0;
        uint8_t *buf = read_entire_file(path, &sz);
        if (!buf) {
            record_check(report, "talk1.dat", "dat", 0, 0, 0, "Missing file");
        } else {
            int cnt = 0;
            int rc = validate_talk1_and_extract_sample(buf, sz, &cnt, report->sample_dialogue, sizeof(report->sample_dialogue));
            free(buf);
            char msg[128];
            snprintf(msg, sizeof(msg), rc == 0 ? "%d Talk VM scripts" : "Invalid Talk offset table", cnt);
            record_check(report, "talk1.dat", "dat", 1, rc == 0, cnt, msg);
        }
    }

    report->is_valid = (report->valid_archives_count == report->total_required_archives && has_metal) ? 1 : 0;
    snprintf(
        report->summary_message, sizeof(report->summary_message),
        "%d/%d archives validated | %d char sprites, %d SFX, %d+%d maps, %d heroes, %d monsters, %d map titles.",
        report->valid_archives_count, report->total_required_archives,
        report->char_sprite_slots, report->snd_slots,
        report->huge_map_count, report->huge2_map_count,
        report->hero_count, report->monster_count, report->map_title_count
    );
    return report->is_valid ? 0 : -1;
}

/* =========================================================================
 * Live In-Memory Decoders for SDL3 Runtime (.lmf, .tsw, .snd)
 * ========================================================================= */

typedef struct {
    uint32_t map_id;
    int w_tiles;
    int h_tiles;
    int tile_sz;
    int w_px;
    int h_px;
    int total_maps;
    SDL_Texture *texture;
} PalLoadedMap;

typedef struct {
    int slot_index;
    uint32_t slot_id;
    int nframes;
    int w;
    int h;
    int total_slots;
    SDL_Texture **frames;
} PalLoadedSprite;

static int vfs_load_lmf_map(
    const char *game_dir, const char *lmf_name, int map_idx,
    SDL_Renderer *renderer, SDL_ScaleMode scale_mode, PalLoadedMap *out_map
) {
    memset(out_map, 0, sizeof(*out_map));
    char path[1200];
    join_path(path, sizeof(path), game_dir, lmf_name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t fsize = get_file_size_fp(f);

    PalLmfEntry *entries = NULL;
    int cnt = 0;
    if (read_lmf_directory(f, fsize, &entries, &cnt) != 0 || cnt == 0) {
        free(entries);
        fclose(f);
        return -1;
    }
    if (map_idx < 0) map_idx = (map_idx % cnt + cnt) % cnt;
    if (map_idx >= cnt) map_idx %= cnt;

    PalLmfEntry me = entries[map_idx];
    free(entries);
    out_map->total_maps = cnt;
    out_map->map_id = me.id;

    uint8_t msf_hdr[4 + 0x400];
    if (read_file_slice(f, me.offset, msf_hdr, sizeof(msf_hdr)) != 0 || memcmp(msf_hdr, "MSF3", 4) != 0) {
        fclose(f);
        return -1;
    }
    const uint8_t *hdr = msf_hdr + 4;
    uint16_t w_tiles = rd_u16(hdr + 0x100);
    uint16_t h_tiles = rd_u16(hdr + 0x102);
    uint16_t tile_sz = rd_u16(hdr + 0x104);
    uint16_t bg_type = rd_u16(hdr + 0x108);
    uint32_t bg_chunk_off = rd_u32(hdr + 0x38);
    (void)bg_type;

    if (bg_chunk_off == 0 || tile_sz == 0 || w_tiles == 0 || h_tiles == 0) {
        fclose(f);
        return -1;
    }

    size_t layer_tiles = (size_t)w_tiles * (size_t)h_tiles;
    uint8_t *tm_raw = (uint8_t *)malloc(layer_tiles * 4);
    uint8_t bg_hdr[0x400];
    if (!tm_raw ||
        read_file_slice(f, (uint64_t)me.offset + 0x118, tm_raw, layer_tiles * 4) != 0 ||
        read_file_slice(f, (uint64_t)me.offset + bg_chunk_off, bg_hdr, sizeof(bg_hdr)) != 0) {
        free(tm_raw);
        fclose(f);
        return -1;
    }

    uint32_t block_size = rd_u32(bg_hdr + 4);
    size_t pool_cap = 0, pool_used = 0;
    uint8_t *tile_pool = NULL;

    for (size_t k = 0; 0x10 + k * 8 + 8 <= sizeof(bg_hdr); k++) {
        uint32_t b_off = rd_u32(bg_hdr + 0x10 + k * 8);
        uint32_t b_sz = rd_u32(bg_hdr + 0x14 + k * 8);
        if (b_off == 0 && b_sz == 0) break;
        if ((uint64_t)me.offset + b_off + b_sz > fsize) break;

        uint8_t *comp = (uint8_t *)malloc(b_sz);
        if (!comp || read_file_slice(f, (uint64_t)me.offset + b_off, comp, b_sz) != 0) {
            free(comp);
            break;
        }
        size_t decomp_len = 0;
        uint8_t *decomp = lzo_alloc_decompress(comp, b_sz, block_size, 0, &decomp_len);
        free(comp);
        if (!decomp) break;

        if (pool_used + decomp_len > pool_cap) {
            size_t new_cap = (pool_cap == 0 ? 65536 : pool_cap * 2);
            while (new_cap < pool_used + decomp_len) new_cap *= 2;
            uint8_t *grown = (uint8_t *)realloc(tile_pool, new_cap);
            if (!grown) {
                free(decomp);
                break;
            }
            tile_pool = grown;
            pool_cap = new_cap;
        }
        memcpy(tile_pool + pool_used, decomp, decomp_len);
        pool_used += decomp_len;
        free(decomp);
    }
    fclose(f);

    size_t tile_bytes = (size_t)tile_sz * (size_t)tile_sz * 2;
    size_t avail_tiles = tile_bytes > 0 ? pool_used / tile_bytes : 0;
    int pw = (int)w_tiles * (int)tile_sz;
    int ph = (int)h_tiles * (int)tile_sz;
    uint8_t *rgba = (uint8_t *)calloc((size_t)pw * (size_t)ph, 4);

    if (rgba && avail_tiles > 0) {
        size_t row_stride = (size_t)pw * 4;
        for (int ty = 0; ty < (int)h_tiles; ty++) {
            for (int tx = 0; tx < (int)w_tiles; tx++) {
                uint16_t tid = (uint16_t)(rd_u32(tm_raw + ((size_t)ty * w_tiles + (size_t)tx) * 4) & 0xFFFFu);
                if (tid >= avail_tiles) continue;
                const uint8_t *src_tile = tile_pool + (size_t)tid * tile_bytes;
                for (int py = 0; py < (int)tile_sz; py++) {
                    uint8_t *dst_row = rgba + ((size_t)ty * tile_sz + (size_t)py) * row_stride + (size_t)tx * tile_sz * 4;
                    const uint8_t *src_row = src_tile + (size_t)py * tile_sz * 2;
                    for (int px = 0; px < (int)tile_sz; px++) {
                        rgb555_write_rgba(dst_row + (size_t)px * 4, rd_u16(src_row + (size_t)px * 2), 255);
                    }
                }
            }
        }
    }

    free(tile_pool);
    free(tm_raw);

    if (!rgba) return -1;

    SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, pw, ph);
    if (tex) {
        SDL_UpdateTexture(tex, NULL, rgba, pw * 4);
        SDL_SetTextureScaleMode(tex, scale_mode);
    }
    free(rgba);
    if (!tex) return -1;

    out_map->w_tiles = w_tiles;
    out_map->h_tiles = h_tiles;
    out_map->tile_sz = tile_sz;
    out_map->w_px = pw;
    out_map->h_px = ph;
    out_map->texture = tex;
    return 0;
}

static void free_loaded_map(PalLoadedMap *m) {
    if (m->texture) SDL_DestroyTexture(m->texture);
    memset(m, 0, sizeof(*m));
}

static int load_tsw_from_toc_index(
    const char *game_dir, const char *tsw_name, int query_val, int match_by_id,
    SDL_Renderer *renderer, SDL_ScaleMode scale_mode, PalLoadedSprite *out_sp
) {
    memset(out_sp, 0, sizeof(*out_sp));
    char path[1200];
    join_path(path, sizeof(path), game_dir, tsw_name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t fsize = get_file_size_fp(f);

    PalTocEntry *entries = NULL;
    int cnt = 0;
    if (read_archive_toc(f, fsize, &entries, &cnt) != 0 || cnt == 0) {
        free(entries);
        fclose(f);
        return -1;
    }
    int target_idx = -1;
    if (match_by_id) {
        for (int i = 0; i < cnt; i++) {
            if (entries[i].id == (uint32_t)query_val) {
                target_idx = i;
                break;
            }
        }
        if (target_idx < 0) {
            free(entries);
            fclose(f);
            return -1;
        }
    } else {
        target_idx = query_val;
        if (target_idx < 0) target_idx = (target_idx % cnt + cnt) % cnt;
        if (target_idx >= cnt) target_idx %= cnt;
    }

    PalTocEntry se = entries[target_idx];
    free(entries);

    uint8_t chdr[12];
    if (read_file_slice(f, se.offset, chdr, 12) != 0 || rd_u16(chdr + 4) != 0xABCDu) {
        fclose(f);
        return -1;
    }

    uint16_t nframes = rd_u16(chdr + 6);
    uint16_t bpp = rd_u16(chdr + 8);
    uint16_t off0 = rd_u16(chdr + 10);
    if (nframes == 0) {
        fclose(f);
        return -1;
    }
    if (nframes > 32) nframes = 32;

    uint16_t palette[256] = {0};
    uint64_t ft_base = (uint64_t)se.offset + off0;
    if (bpp == 8) {
        uint8_t pal_bytes[512];
        if (read_file_slice(f, (uint64_t)se.offset + 12, pal_bytes, 512) == 0) {
            for (int p = 0; p < 256; p++) palette[p] = rd_u16(pal_bytes + p * 2);
        }
        ft_base += 0x200;
    }

    out_sp->frames = (SDL_Texture **)calloc(nframes, sizeof(SDL_Texture *));
    if (!out_sp->frames) {
        fclose(f);
        return -1;
    }

    int loaded = 0;
    for (uint16_t fr = 0; fr < nframes; fr++) {
        uint8_t fhdr[36];
        if (read_file_slice(f, ft_base + (uint64_t)fr * 36, fhdr, 36) != 0) break;
        uint32_t f_off = rd_u32(fhdr);
        uint32_t c_sz1 = rd_u32(fhdr + 4);
        uint32_t u_sz1 = rd_u32(fhdr + 8);
        uint32_t c_sz2 = rd_u32(fhdr + 16);
        uint32_t u_sz2 = rd_u32(fhdr + 20);
        uint16_t w = rd_u16(fhdr + 0x20);
        uint16_t h = rd_u16(fhdr + 0x22);
        if (w == 0 || h == 0 || c_sz1 == 0) continue;

        uint8_t *comp1 = (uint8_t *)malloc(c_sz1);
        if (!comp1 || read_file_slice(f, (uint64_t)se.offset + f_off, comp1, c_sz1) != 0) {
            free(comp1);
            continue;
        }
        size_t decomp1_len = 0;
        uint8_t *decomp1 = lzo_alloc_decompress(comp1, c_sz1, u_sz1, 0, &decomp1_len);
        free(comp1);
        if (!decomp1) continue;

        uint8_t *decomp2 = NULL;
        size_t decomp2_len = 0;
        if (c_sz2 > 0) {
            uint8_t *comp2 = (uint8_t *)malloc(c_sz2);
            if (comp2 && read_file_slice(f, (uint64_t)se.offset + f_off + c_sz1, comp2, c_sz2) == 0) {
                decomp2 = lzo_alloc_decompress(comp2, c_sz2, u_sz2, 1, &decomp2_len);
            }
            free(comp2);
        }

        size_t total_px = (size_t)w * (size_t)h;
        uint8_t *rgba = (uint8_t *)calloc(total_px, 4);
        if (rgba) {
            uint16_t magic = decomp1_len >= 2 ? rd_u16(decomp1) : 0;
            if (magic == 0xFFFFu) {
                decode_arle_to_rgba(decomp1, decomp1_len, w, h, bpp, palette, decomp2, decomp2_len, rgba);
            } else {
                size_t words = decomp1_len / 2;
                if (words > total_px) words = total_px;
                for (size_t p = 0; p < words; p++) {
                    rgb555_write_rgba(rgba + p * 4, rd_u16(decomp1 + p * 2), 255);
                }
            }
            SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
            if (tex) {
                SDL_UpdateTexture(tex, NULL, rgba, w * 4);
                SDL_SetTextureScaleMode(tex, scale_mode);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                out_sp->frames[loaded++] = tex;
                if (out_sp->w == 0) {
                    out_sp->w = w;
                    out_sp->h = h;
                }
            }
            free(rgba);
        }
        free(decomp2);
        free(decomp1);
    }

    fclose(f);
    out_sp->slot_index = target_idx;
    out_sp->slot_id = se.id;
    out_sp->nframes = loaded;
    out_sp->total_slots = cnt;
    return loaded > 0 ? 0 : -1;
}

static int vfs_load_tsw_slot(
    const char *game_dir, const char *tsw_name, int target_idx,
    SDL_Renderer *renderer, SDL_ScaleMode scale_mode, PalLoadedSprite *out_sp
) {
    return load_tsw_from_toc_index(game_dir, tsw_name, target_idx, 0, renderer, scale_mode, out_sp);
}

static int vfs_load_tsw_by_id(
    const char *game_dir, const char *tsw_name, uint32_t slot_id,
    SDL_Renderer *renderer, SDL_ScaleMode scale_mode, PalLoadedSprite *out_sp
) {
    return load_tsw_from_toc_index(game_dir, tsw_name, (int)slot_id, 1, renderer, scale_mode, out_sp);
}

static void free_loaded_sprite(PalLoadedSprite *s) {
    if (s->frames) {
        for (int i = 0; i < s->nframes; i++) {
            if (s->frames[i]) SDL_DestroyTexture(s->frames[i]);
        }
        free(s->frames);
    }
    memset(s, 0, sizeof(*s));
}

static int load_snd_from_toc(
    const char *game_dir, int query_val, int match_by_id,
    SDL_AudioSpec *out_spec, uint8_t **out_pcm, Uint32 *out_pcm_len, uint32_t *out_sid
) {
    *out_pcm = NULL;
    *out_pcm_len = 0;
    *out_sid = 0;

    char path[1200];
    join_path(path, sizeof(path), game_dir, "all.snd");
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t fsize = get_file_size_fp(f);

    PalTocEntry *entries = NULL;
    int cnt = 0;
    if (read_archive_toc(f, fsize, &entries, &cnt) != 0 || cnt == 0) {
        free(entries);
        fclose(f);
        return -1;
    }
    int target_idx = -1;
    if (match_by_id) {
        for (int i = 0; i < cnt; i++) {
            if (entries[i].id == (uint32_t)query_val) {
                target_idx = i;
                break;
            }
        }
        if (target_idx < 0) {
            free(entries);
            fclose(f);
            return -1;
        }
    } else {
        target_idx = query_val;
        if (target_idx < 0) target_idx = 0;
        if (target_idx >= cnt) target_idx %= cnt;
    }

    PalTocEntry se = entries[target_idx];
    free(entries);

    uint32_t sz = se.size_raw & 0x03FFFFFFu;
    if (sz == 0 || (uint64_t)se.offset + sz > fsize) {
        fclose(f);
        return -1;
    }

    uint8_t *raw = (uint8_t *)malloc(sz);
    if (!raw || read_file_slice(f, se.offset, raw, sz) != 0) {
        free(raw);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out_sid = se.id;

    int ok = 0;
    if (se.flag == 1) {
        SDL_IOStream *io = SDL_IOFromConstMem(raw, sz);
        ok = io && SDL_LoadWAV_IO(io, true, out_spec, out_pcm, out_pcm_len);
    } else if (sz > 18) {
        uint32_t pcm_len = sz - 18;
        uint8_t *wav = (uint8_t *)malloc(44 + pcm_len);
        if (wav) {
            memcpy(wav, "RIFF", 4);
            wr_u32(wav + 4, pcm_len + 36);
            memcpy(wav + 8, "WAVEfmt ", 8);
            wr_u32(wav + 16, 16);
            memcpy(wav + 20, raw, 16);
            memcpy(wav + 36, "data", 4);
            wr_u32(wav + 40, pcm_len);
            memcpy(wav + 44, raw + 18, pcm_len);
            SDL_IOStream *io = SDL_IOFromConstMem(wav, 44 + pcm_len);
            ok = io && SDL_LoadWAV_IO(io, true, out_spec, out_pcm, out_pcm_len);
            free(wav);
        }
    }
    free(raw);
    return ok ? 0 : -1;
}

static int vfs_load_snd_wav_bytes(
    const char *game_dir, int target_idx,
    SDL_AudioSpec *out_spec, uint8_t **out_pcm, Uint32 *out_pcm_len, uint32_t *out_sid
) {
    return load_snd_from_toc(game_dir, target_idx, 0, out_spec, out_pcm, out_pcm_len, out_sid);
}

static int vfs_load_snd_by_id(
    const char *game_dir, uint32_t slot_id,
    SDL_AudioSpec *out_spec, uint8_t **out_pcm, Uint32 *out_pcm_len, uint32_t *out_sid
) {
    return load_snd_from_toc(game_dir, (int)slot_id, 1, out_spec, out_pcm, out_pcm_len, out_sid);
}

/* =========================================================================
 * M1 Case-Insensitive Path Resolver (for Video/, Music/, Save/)
 * ========================================================================= */

#include <dirent.h>
#include <strings.h>

static int resolve_rel_path_ci(const char *base_dir, const char *rel_path, char *out_path, size_t out_cap) {
    char direct[1200];
    join_path(direct, sizeof(direct), base_dir, rel_path);
    FILE *probe = fopen(direct, "rb");
    if (probe) {
        fclose(probe);
        snprintf(out_path, out_cap, "%s", direct);
        return 0;
    }

    char cur[1200];
    snprintf(cur, sizeof(cur), "%s", base_dir);
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", rel_path);

    char *saveptr = NULL;
    char *tok = strtok_r(copy, "/\\", &saveptr);
    while (tok) {
        DIR *d = opendir(cur);
        if (!d) return -1;
        struct dirent *de;
        char matched[256] = {0};
        while ((de = readdir(d)) != NULL) {
            if (strcasecmp(de->d_name, tok) == 0) {
                snprintf(matched, sizeof(matched), "%s", de->d_name);
                break;
            }
        }
        closedir(d);
        if (matched[0] == '\0') return -1;
        char next[1200];
        join_path(next, sizeof(next), cur, matched);
        snprintf(cur, sizeof(cur), "%s", next);
        tok = strtok_r(NULL, "/\\", &saveptr);
    }

    snprintf(out_path, out_cap, "%s", cur);
    FILE *f = fopen(out_path, "rb");
    if (!f) return -1;
    fclose(f);
    return 0;
}

/* =========================================================================
 * M1 Module 1: 35-Opcode .act Bytecode VM (myact.cpp / 0x45bae2)
 * ========================================================================= */

static int resolve_tsw_from_fr(int32_t fr_id, const char **out_tsw_file, uint32_t *out_slot_id) {
    if (fr_id <= 0) return -1;
    if (fr_id < 3000) {
        *out_tsw_file = "all_char.tsw";
        *out_slot_id = (uint32_t)fr_id;
    } else if (fr_id < 6000) {
        *out_tsw_file = "all_item.tsw";
        *out_slot_id = (uint32_t)(fr_id - 3000);
    } else if (fr_id < 9000) {
        *out_tsw_file = "all_magic.tsw";
        *out_slot_id = (uint32_t)(fr_id - 6000);
    } else if (fr_id < 13000) {
        *out_tsw_file = "all_sys.tsw";
        *out_slot_id = (uint32_t)(fr_id - 9000);
    } else if (fr_id < 15000) {
        *out_tsw_file = "all_map1.tsw";
        *out_slot_id = (uint32_t)(fr_id - 13000);
    } else {
        *out_tsw_file = "all_map2.tsw";
        *out_slot_id = (uint32_t)(fr_id - 15000);
    }
    return *out_slot_id > 0 ? 0 : -1;
}

static int apply_act_opcode(PalActVM *vm, const PalActInsn *in) {
    switch (in->op) {
        case 0x544Eu: /* NT: set track / sub-layer / count */
            vm->nt_value = in->args[0];
            return 0;
        case 0x5246u: /* FR: set .tsw sprite ID (e.g. 9054..9057 -> all_sys 54..57) */
            vm->fr_tsw_id = in->args[0];
            return 0;
        case 0x5344u: /* DS: set base duration ticks (35ms/tick) */
            vm->duration_ticks = in->args[0] > 0 ? in->args[0] : 1;
            return 0;
        case 0x4E4Fu: /* ON: visible on (clear hidden bit 0 at 0x45bae2:357) */
            vm->visible = 1;
            return 0;
        case 0x5649u: /* IV: visible off / hidden */
            vm->visible = 0;
            return 0;
        case 0x414Du: /* MA: additive glow blend (flag |= 4) */
        case 0x414Eu: /* AN: additive blend */
            vm->blend_mode = 4;
            return 0;
        case 0x4148u: /* AH: half-alpha blend (flag |= 8) */
            vm->blend_mode = 8;
            return 0;
        case 0x434Cu: /* CL: clear color tint / blend */
            vm->blend_mode = 0;
            return 0;
        case 0x5041u: /* AP: set frame index and increment internal AP step */
            vm->frame_index = in->args[0];
            return 0;
        case 0x4544u: /* DE: commit current frame for duration_ticks */
            vm->delay_rem = vm->duration_ticks > 0 ? (vm->duration_ticks - 1) : 0;
            return 1; /* end of tick */
        case 0x5859u: /* XY: set (x, y) anchor/offset */
            vm->offset_x = in->args[0];
            vm->offset_y = in->args[1];
            return 0;
        case 0x5748u: /* HW: set hitbox width/height */
            vm->hitbox_w = in->args[0];
            vm->hitbox_h = in->args[1];
            return 0;
        case 0x4F58u: /* OX */
            vm->offset_x = in->args[0];
            return 0;
        case 0x4F59u: /* OY */
            vm->offset_y = in->args[0];
            return 0;
        case 0x524Fu: /* OR: orientation */
            vm->orientation = in->args[0];
            return 0;
        case 0x5343u: /* CS: trigger sound ID in all.snd */
            vm->last_sfx_id = in->args[0];
            return 0;
        case 0x4C50u: /* LP: loop [start_frame, end_frame, repeat_count] */
            vm->loop_start_frame = in->args[0];
            vm->loop_end_frame = in->args[1];
            vm->loop_remaining = in->args[2];
            return 0;
        case 0x4F56u: /* VO: end sequence */
        case 0x4F32u: /* 2O: end sequence */
            if (vm->loop_remaining == -1) {
                vm->pc = 0;
                vm->delay_rem = 0;
            } else if (vm->loop_remaining > 1) {
                vm->loop_remaining--;
                vm->pc = 0;
                vm->delay_rem = 0;
            } else {
                vm->finished = 1;
            }
            return 1;
        default:
            return 0;
    }
}

int32_t pal_act_vm_step_tick(PalActVM *vm) {
    if (!vm || vm->insn_count <= 0) return -1;
    if (vm->finished) return 0;

    if (vm->delay_rem > 0) {
        vm->delay_rem--;
        return 1;
    }

    int guard = 0;
    while (guard++ < PAL_ACT_MAX_INSNS) {
        if (vm->pc < 0 || vm->pc >= vm->insn_count) {
            vm->finished = 1;
            return 1;
        }
        const PalActInsn *in = &vm->insns[vm->pc];
        vm->pc++;
        if (apply_act_opcode(vm, in) != 0) {
            return 1;
        }
    }
    return 1;
}

int32_t pal_act_vm_init(
    const char *game_dir, const char *act_file,
    uint32_t act_slot_id, int32_t sub_action_idx,
    PalActVM *out_vm
) {
    if (!game_dir || !act_file || !out_vm) return -1;
    memset(out_vm, 0, sizeof(*out_vm));
    out_vm->act_slot_id = act_slot_id;
    out_vm->sub_action_idx = sub_action_idx;
    out_vm->duration_ticks = 1;
    out_vm->loop_remaining = 0;

    char path[1200];
    join_path(path, sizeof(path), game_dir, act_file);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t fsize = get_file_size_fp(f);

    PalTocEntry *entries = NULL;
    int cnt = 0;
    if (read_archive_toc(f, fsize, &entries, &cnt) != 0 || cnt == 0) {
        free(entries);
        fclose(f);
        return -1;
    }

    int found = -1;
    for (int i = 0; i < cnt; i++) {
        if (entries[i].id == act_slot_id) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        free(entries);
        fclose(f);
        return -1;
    }
    PalTocEntry sel = entries[found];
    free(entries);

    if (sel.size_raw < 6 || (uint64_t)sel.offset + sel.size_raw > fsize) {
        fclose(f);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc(sel.size_raw);
    if (!buf || read_file_slice(f, sel.offset, buf, sel.size_raw) != 0) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    uint16_t sub_cnt = rd_u16(buf);
    uint32_t max_cnt = (sel.size_raw - 2) / 4;
    if (sub_cnt > max_cnt) sub_cnt = (uint16_t)max_cnt;
    out_vm->total_sub_actions = sub_cnt;

    if (sub_action_idx < 0 || sub_action_idx >= (int32_t)sub_cnt) {
        free(buf);
        return -1;
    }

    uint32_t hdr_end = 2 + (uint32_t)sub_cnt * 4;
    uint32_t off = rd_u32(buf + 2 + (size_t)sub_action_idx * 4);
    if (off < hdr_end || off + 2 > sel.size_raw) {
        free(buf);
        return -1;
    }
    uint32_t end = sel.size_raw;
    for (uint16_t b = 0; b < sub_cnt; b++) {
        uint32_t o2 = rd_u32(buf + 2 + (size_t)b * 4);
        if (o2 > off && o2 < end) end = o2;
    }

    size_t pos = off + 2; /* skip 2-byte sub-action header */
    while (pos + 2 <= end && out_vm->insn_count < PAL_ACT_MAX_INSNS) {
        uint16_t code = rd_u16(buf + pos);
        pos += 2;
        if (code == 0) continue;
        int nargs = lookup_act_nargs(code);
        if (nargs < 0 || pos + (size_t)nargs * 2 > end) break;
        PalActInsn *in = &out_vm->insns[out_vm->insn_count++];
        in->op = code;
        in->nargs = nargs;
        for (int a = 0; a < nargs && a < 7; a++) {
            in->args[a] = rd_i16(buf + pos + (size_t)a * 2);
        }
        pos += (size_t)nargs * 2;
    }
    free(buf);

    if (out_vm->insn_count == 0) return -1;
    pal_act_vm_step_tick(out_vm);
    return 0;
}

/* =========================================================================
 * M1 Module 2: CoreText Chinese Rich-Text Renderer (GrpFont.cpp / %C %N %S %K)
 * ========================================================================= */

/* Authentic 10-entry palette from Palgame.exe DAT_004edd0c (Win32 COLORREF 0x00BBGGRR) */
static const uint32_t kPalFontColorRef[10] = {
    0x00FFE581u, /* %C0 sky cyan */
    0x00B9DAFFu, /* %C1 warm peach */
    0x00A3FEFCu, /* %C2 pale yellow */
    0x00ACCFE9u, /* %C3 parchment tan */
    0x002C577Bu, /* %C4 deep bronze */
    0x00FFE6E6u, /* %C5 crisp white (dialogue default body) */
    0x000080FFu, /* %C6 golden orange (speaker name highlight) */
    0x00FFC8EDu, /* %C7 soft lavender */
    0x0085CFB5u, /* %C8 jade green */
    0x00800000u  /* %C9 navy accent */
};

static CTFontRef create_pal_ct_font(float pt_size) {
    const char *families[] = {"PingFang TC", "Heiti TC", "PingFang SC", "STHeiti"};
    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
        CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, families[i], kCFStringEncodingUTF8);
        CTFontRef font = name ? CTFontCreateWithName(name, (CGFloat)pt_size, NULL) : NULL;
        if (name) CFRelease(name);
        if (font) return font;
    }
    return CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, (CGFloat)pt_size, NULL);
}

static CGColorRef create_cg_colorref(uint32_t colorref, CGFloat alpha) {
    CGFloat r = (CGFloat)(colorref & 0xFFu) / 255.0;
    CGFloat g = (CGFloat)((colorref >> 8) & 0xFFu) / 255.0;
    CGFloat b = (CGFloat)((colorref >> 16) & 0xFFu) / 255.0;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[4] = {r, g, b, alpha};
    CGColorRef col = CGColorCreate(cs, comps);
    if (cs) CGColorSpaceRelease(cs);
    return col;
}

static void append_attributed_chunk(
    CFMutableAttributedStringRef astr,
    CFMutableAttributedStringRef shadow_astr,
    const char *utf8, size_t len,
    CTFontRef font, CGColorRef fg_color, CGColorRef shadow_color
) {
    if (!utf8 || len == 0) return;
    CFStringRef seg = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)utf8, (CFIndex)len, kCFStringEncodingUTF8, false);
    if (!seg) return;

    CFIndex start = CFAttributedStringGetLength(astr);
    CFAttributedStringReplaceString(astr, CFRangeMake(start, 0), seg);
    CFIndex end = CFAttributedStringGetLength(astr);
    CFRange r = CFRangeMake(start, end - start);
    if (font) CFAttributedStringSetAttribute(astr, r, kCTFontAttributeName, font);
    if (fg_color) CFAttributedStringSetAttribute(astr, r, kCTForegroundColorAttributeName, fg_color);

    if (shadow_astr) {
        CFAttributedStringReplaceString(shadow_astr, CFRangeMake(start, 0), seg);
        if (font) CFAttributedStringSetAttribute(shadow_astr, r, kCTFontAttributeName, font);
        if (shadow_color) CFAttributedStringSetAttribute(shadow_astr, r, kCTForegroundColorAttributeName, shadow_color);
    }
    CFRelease(seg);
}

static uint8_t *rasterize_rich_text_rgba(
    const char *utf8_rich_text,
    float base_font_size,
    int32_t max_width,
    PalRichTextMetrics *out_metrics
) {
    if (out_metrics) memset(out_metrics, 0, sizeof(*out_metrics));
    if (!utf8_rich_text || utf8_rich_text[0] == '\0') return NULL;
    if (base_font_size <= 0.0f) base_font_size = 18.0f;
    if (max_width <= 0) max_width = 560;

    CFMutableAttributedStringRef astr = CFAttributedStringCreateMutable(kCFAllocatorDefault, 0);
    CFMutableAttributedStringRef shadow_astr = CFAttributedStringCreateMutable(kCFAllocatorDefault, 0);
    if (!astr || !shadow_astr) {
        if (astr) CFRelease(astr);
        if (shadow_astr) CFRelease(shadow_astr);
        return NULL;
    }

    CTFontRef fonts[3] = {
        create_pal_ct_font(base_font_size * 0.88f), /* %S0 compact */
        create_pal_ct_font(base_font_size),         /* %S1 standard */
        create_pal_ct_font(base_font_size * 1.18f)  /* %S2 emphasis */
    };
    CGColorRef colors[10];
    for (int i = 0; i < 10; i++) {
        colors[i] = create_cg_colorref(kPalFontColorRef[i], 1.0);
    }
    CGColorRef shadow_col = create_cg_colorref(0x00100808u, 0.92);

    int cur_color = 5; /* %C5 default ice-white */
    int cur_size = 1;
    int line_count = 1;

    char run_buf[1024];
    size_t run_len = 0;

    const char *p = utf8_rich_text;
    while (*p != '\0') {
        if (*p == '%' && p[1] != '\0') {
            char tag = p[1];
            if (tag == 'N' || tag == 'n') {
                run_buf[run_len++] = '\n';
                line_count++;
                p += 2;
            } else if ((tag == 'C' || tag == 'c') && p[2] >= '0' && p[2] <= '9') {
                if (run_len > 0) {
                    append_attributed_chunk(astr, shadow_astr, run_buf, run_len, fonts[cur_size], colors[cur_color], shadow_col);
                    run_len = 0;
                }
                cur_color = p[2] - '0';
                p += 3;
            } else if ((tag == 'S' || tag == 's') && p[2] >= '0' && p[2] <= '9') {
                if (run_len > 0) {
                    append_attributed_chunk(astr, shadow_astr, run_buf, run_len, fonts[cur_size], colors[cur_color], shadow_col);
                    run_len = 0;
                }
                int sv = p[2] - '0';
                cur_size = (sv == 0) ? 0 : (sv >= 2 ? 2 : 1);
                p += 3;
            } else if ((tag == 'D' || tag == 'd') && p[2] >= '0' && p[2] <= '9') {
                p += 3;
            } else if (tag == 'K' || tag == 'k' || tag == 'L' || tag == 'l' || tag == 'P' || tag == 'p' || tag == 'B' || tag == 'b' || tag == 'A' || tag == 'a' || tag == 'Q' || tag == 'q') {
                p += 2;
            } else if (tag == '%') {
                run_buf[run_len++] = '%';
                p += 2;
            } else {
                run_buf[run_len++] = *p++;
            }
        } else {
            if (*p == '\n') line_count++;
            run_buf[run_len++] = *p++;
        }

        if (run_len >= sizeof(run_buf) - 8) {
            append_attributed_chunk(astr, shadow_astr, run_buf, run_len, fonts[cur_size], colors[cur_color], shadow_col);
            run_len = 0;
        }
    }
    if (run_len > 0) {
        append_attributed_chunk(astr, shadow_astr, run_buf, run_len, fonts[cur_size], colors[cur_color], shadow_col);
    }

    if (CFAttributedStringGetLength(astr) == 0) {
        append_attributed_chunk(astr, shadow_astr, " ", 1, fonts[1], colors[5], shadow_col);
    }

    CTFramesetterRef fs_fg = CTFramesetterCreateWithAttributedString(astr);
    CTFramesetterRef fs_sh = CTFramesetterCreateWithAttributedString(shadow_astr);

    CGSize sz = CTFramesetterSuggestFrameSizeWithConstraints(
        fs_fg, CFRangeMake(0, 0), NULL, CGSizeMake((CGFloat)max_width, 2000.0), NULL
    );

    int w = (int)ceil(sz.width) + 6;
    int h = (int)ceil(sz.height) + 6;
    if (w < 4) w = 4;
    if (h < 4) h = 4;
    if (w > 1280) w = 1280;
    if (h > 960) h = 960;

    uint8_t *rgba = (uint8_t *)calloc((size_t)w * (size_t)h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = (rgba && cs)
        ? CGBitmapContextCreate(rgba, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
                                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big)
        : NULL;

    if (ctx) {
        CGContextSetAllowsAntialiasing(ctx, true);
        CGContextSetShouldAntialias(ctx, true);
        CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);

        /* Shadow pass shifted (+1.5, -1.5) in CG coordinates = (+1.5 right, +1.5 down on screen) */
        CGMutablePathRef path_sh = CGPathCreateMutable();
        CGPathAddRect(path_sh, NULL, CGRectMake(2.5, 1.5, (CGFloat)(w - 4), (CGFloat)(h - 4)));
        CTFrameRef frame_sh = CTFramesetterCreateFrame(fs_sh, CFRangeMake(0, 0), path_sh, NULL);
        if (frame_sh) {
            CTFrameDraw(frame_sh, ctx);
            CFRelease(frame_sh);
        }
        CGPathRelease(path_sh);

        /* Foreground rich-text pass */
        CGMutablePathRef path_fg = CGPathCreateMutable();
        CGPathAddRect(path_fg, NULL, CGRectMake(1.0, 3.0, (CGFloat)(w - 4), (CGFloat)(h - 4)));
        CTFrameRef frame_fg = CTFramesetterCreateFrame(fs_fg, CFRangeMake(0, 0), path_fg, NULL);
        if (frame_fg) {
            CTFrameDraw(frame_fg, ctx);
            CFRelease(frame_fg);
        }
        CGPathRelease(path_fg);

        CGContextRelease(ctx);
    }

    int32_t non_trans = 0;
    if (rgba) {
        size_t total = (size_t)w * (size_t)h;
        for (size_t i = 0; i < total; i++) {
            uint8_t a = rgba[i * 4 + 3];
            if (a > 0) {
                non_trans++;
                if (a < 255) {
                    /* Un-premultiply RGBA for crisp SDL_BLENDMODE_BLEND compositing */
                    int r = ((int)rgba[i * 4 + 0] * 255) / (int)a;
                    int g = ((int)rgba[i * 4 + 1] * 255) / (int)a;
                    int b = ((int)rgba[i * 4 + 2] * 255) / (int)a;
                    rgba[i * 4 + 0] = (uint8_t)(r > 255 ? 255 : r);
                    rgba[i * 4 + 1] = (uint8_t)(g > 255 ? 255 : g);
                    rgba[i * 4 + 2] = (uint8_t)(b > 255 ? 255 : b);
                }
            }
        }
    }

    if (out_metrics) {
        out_metrics->width = w;
        out_metrics->height = h;
        out_metrics->line_count = line_count;
        out_metrics->non_transparent_pixels = non_trans;
    }

    if (cs) CGColorSpaceRelease(cs);
    if (fs_fg) CFRelease(fs_fg);
    if (fs_sh) CFRelease(fs_sh);
    if (shadow_col) CGColorRelease(shadow_col);
    for (int i = 0; i < 10; i++) {
        if (colors[i]) CGColorRelease(colors[i]);
    }
    for (int i = 0; i < 3; i++) {
        if (fonts[i]) CFRelease(fonts[i]);
    }
    CFRelease(astr);
    CFRelease(shadow_astr);
    return rgba;
}

int32_t pal_font_probe_rich_text(
    const char *utf8_rich_text, float font_size, int32_t max_width,
    PalRichTextMetrics *out_metrics
) {
    uint8_t *rgba = rasterize_rich_text_rgba(utf8_rich_text, font_size, max_width, out_metrics);
    if (!rgba) return -1;
    int ok = (out_metrics && out_metrics->width > 0 && out_metrics->height > 0 && out_metrics->non_transparent_pixels > 0);
    free(rgba);
    return ok ? 0 : -1;
}

static SDL_Texture *create_richtext_texture(
    SDL_Renderer *renderer, const char *utf8_rich_text, float font_size, int32_t max_width,
    int *out_w, int *out_h
) {
    PalRichTextMetrics m;
    uint8_t *rgba = rasterize_rich_text_rgba(utf8_rich_text, font_size, max_width, &m);
    if (!rgba) return NULL;
    SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, m.width, m.height);
    if (tex) {
        SDL_UpdateTexture(tex, NULL, rgba, m.width * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    free(rgba);
    if (out_w) *out_w = m.width;
    if (out_h) *out_h = m.height;
    return tex;
}

/* =========================================================================
 * M1 Module 3: FFmpeg MP3/WAV BGM Streamer (SGSound.cpp / 0x4983ac)
 * ========================================================================= */

static int ffmpeg_decode_audio_to_s16(
    const char *file_path,
    uint8_t **out_pcm,
    size_t *out_bytes,
    double *out_duration_sec,
    char *out_codec,
    size_t codec_cap
) {
    *out_pcm = NULL;
    *out_bytes = 0;
    if (out_duration_sec) *out_duration_sec = 0.0;
    if (out_codec && codec_cap > 0) out_codec[0] = '\0';

    av_log_set_level(AV_LOG_ERROR);

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, file_path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (aidx < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    AVStream *st = fmt->streams[aidx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return -1;
    }
    if (out_codec && codec_cap > 0) {
        snprintf(out_codec, codec_cap, "%s", codec->name ? codec->name : "audio");
    }

    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 || avcodec_open2(dec, codec, NULL) < 0) {
        if (dec) avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return -1;
    }

    SwrContext *swr = NULL;
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(
            &swr, &out_layout, AV_SAMPLE_FMT_S16, 44100,
            &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, NULL) < 0 ||
        !swr || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return -1;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    size_t cap = 44100 * 4 * 4;
    size_t used = 0;
    uint8_t *pcm = (uint8_t *)malloc(cap);

    if (pkt && frm && pcm) {
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == aidx) {
                if (avcodec_send_packet(dec, pkt) >= 0) {
                    while (avcodec_receive_frame(dec, frm) >= 0) {
                        int max_out = swr_get_out_samples(swr, frm->nb_samples);
                        if (max_out > 0) {
                            size_t need = (size_t)max_out * 4;
                            if (used + need > cap) {
                                size_t ncap = cap * 2;
                                while (ncap < used + need) ncap *= 2;
                                uint8_t *g = (uint8_t *)realloc(pcm, ncap);
                                if (!g) break;
                                pcm = g;
                                cap = ncap;
                            }
                            uint8_t *dst_ptr = pcm + used;
                            int conv = swr_convert(swr, &dst_ptr, max_out, (const uint8_t **)frm->extended_data, frm->nb_samples);
                            if (conv > 0) used += (size_t)conv * 4;
                        }
                        av_frame_unref(frm);
                    }
                }
            }
            av_packet_unref(pkt);
        }
        /* Flush delayed samples */
        int rem = swr_get_out_samples(swr, 0);
        if (rem > 0) {
            size_t need = (size_t)rem * 4;
            if (used + need <= cap) {
                uint8_t *dst_ptr = pcm + used;
                int conv = swr_convert(swr, &dst_ptr, rem, NULL, 0);
                if (conv > 0) used += (size_t)conv * 4;
            }
        }
    }

    av_frame_free(&frm);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);

    if (!pcm || used == 0) {
        free(pcm);
        return -1;
    }

    *out_pcm = pcm;
    *out_bytes = used;
    if (out_duration_sec) *out_duration_sec = (double)used / (44100.0 * 4.0);
    return 0;
}

int32_t pal_bgm_probe(const char *game_dir, const char *rel_path, PalBgmProbeInfo *out_info) {
    if (!game_dir || !rel_path || !out_info) return -1;
    memset(out_info, 0, sizeof(*out_info));

    char path[1200];
    if (resolve_rel_path_ci(game_dir, rel_path, path, sizeof(path)) != 0) return -1;

    uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    double dur = 0.0;
    if (ffmpeg_decode_audio_to_s16(path, &pcm, &pcm_len, &dur, out_info->codec_name, sizeof(out_info->codec_name)) != 0) {
        return -1;
    }
    free(pcm);

    out_info->sample_rate = 44100;
    out_info->channels = 2;
    out_info->pcm_bytes = (int32_t)(pcm_len > (size_t)INT32_MAX ? INT32_MAX : pcm_len);
    out_info->duration_sec = dur;
    return 0;
}

typedef struct {
    uint8_t *pcm;
    size_t pcm_len;
    size_t cursor;
    SDL_AudioStream *stream;
    int playing;
    float volume;
    int muted;
    char current_track[64];
} PalBgmPlayer;

static void pal_bgm_stop(PalBgmPlayer *bgm) {
    if (bgm->stream) {
        SDL_ClearAudioStream(bgm->stream);
        SDL_DestroyAudioStream(bgm->stream);
        bgm->stream = NULL;
    }
    free(bgm->pcm);
    bgm->pcm = NULL;
    bgm->pcm_len = 0;
    bgm->cursor = 0;
    bgm->playing = 0;
    bgm->current_track[0] = '\0';
}

static void pal_bgm_update(PalBgmPlayer *bgm) {
    if (!bgm->playing || !bgm->stream || !bgm->pcm || bgm->pcm_len == 0) return;
    int queued = SDL_GetAudioStreamQueued(bgm->stream);
    const int target_bytes = 44100 * 4 / 2; /* ~0.5s buffer ahead */
    while (queued < target_bytes) {
        size_t chunk = bgm->pcm_len - bgm->cursor;
        if (chunk > 32768) chunk = 32768;
        if (chunk > 0) {
            SDL_PutAudioStreamData(bgm->stream, bgm->pcm + bgm->cursor, (int)chunk);
            bgm->cursor = (bgm->cursor + chunk) % bgm->pcm_len;
            queued += (int)chunk;
        } else {
            break;
        }
    }
}

static int pal_bgm_play_track(PalBgmPlayer *bgm, const char *game_dir, const char *rel_path, float volume, int muted) {
    if (bgm->playing && strcasecmp(bgm->current_track, rel_path) == 0) return 0;
    pal_bgm_stop(bgm);

    char full_path[1200];
    if (resolve_rel_path_ci(game_dir, rel_path, full_path, sizeof(full_path)) != 0) return -1;

    uint8_t *pcm = NULL;
    size_t len = 0;
    if (ffmpeg_decode_audio_to_s16(full_path, &pcm, &len, NULL, NULL, 0) != 0) return -1;

    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!st) {
        free(pcm);
        return -1;
    }

    bgm->pcm = pcm;
    bgm->pcm_len = len;
    bgm->cursor = 0;
    bgm->stream = st;
    bgm->volume = volume;
    bgm->muted = muted;
    bgm->playing = 1;
    snprintf(bgm->current_track, sizeof(bgm->current_track), "%s", rel_path);

    SDL_SetAudioStreamGain(st, muted ? 0.0f : volume * 0.75f);
    SDL_ResumeAudioStreamDevice(st);
    pal_bgm_update(bgm);
    return 0;
}

/* =========================================================================
 * M1 Module 4: FFmpeg Bink (.bik) Video Decoder & Player (BinkPlayer.cpp)
 * ========================================================================= */

int32_t pal_bink_probe(const char *game_dir, const char *rel_path, PalBinkProbeInfo *out_info) {
    if (!game_dir || !rel_path || !out_info) return -1;
    memset(out_info, 0, sizeof(*out_info));

    char full_path[1200];
    if (resolve_rel_path_ci(game_dir, rel_path, full_path, sizeof(full_path)) != 0) return -1;

    av_log_set_level(AV_LOG_ERROR);

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, full_path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vidx < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    AVStream *vst = fmt->streams[vidx];
    const AVCodec *vcodec = avcodec_find_decoder(vst->codecpar->codec_id);
    AVCodecContext *vdec = vcodec ? avcodec_alloc_context3(vcodec) : NULL;
    if (!vdec || avcodec_parameters_to_context(vdec, vst->codecpar) < 0 || avcodec_open2(vdec, vcodec, NULL) < 0) {
        if (vdec) avcodec_free_context(&vdec);
        avformat_close_input(&fmt);
        return -1;
    }

    out_info->width = vdec->width;
    out_info->height = vdec->height;
    out_info->has_audio = (aidx >= 0) ? 1 : 0;
    snprintf(out_info->codec_name, sizeof(out_info->codec_name), "%s", vcodec->name ? vcodec->name : "bink");

    double fps = av_q2d(vst->avg_frame_rate);
    if (fps <= 0.1) fps = av_q2d(vst->r_frame_rate);
    if (fps <= 0.1) fps = 15.0;
    out_info->fps = fps;
    out_info->frame_count = vst->nb_frames > 0 ? (int32_t)vst->nb_frames
        : (fmt->duration > 0 ? (int32_t)((double)fmt->duration / AV_TIME_BASE * fps) : 0);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    int got_frame = 0;

    if (pkt && frm) {
        while (!got_frame && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == vidx && avcodec_send_packet(vdec, pkt) >= 0) {
                if (avcodec_receive_frame(vdec, frm) >= 0) {
                    struct SwsContext *sws = sws_getContext(
                        vdec->width, vdec->height, (enum AVPixelFormat)frm->format,
                        vdec->width, vdec->height, AV_PIX_FMT_RGBA,
                        SWS_BILINEAR, NULL, NULL, NULL
                    );
                    if (sws) {
                        uint8_t *rgba = (uint8_t *)malloc((size_t)vdec->width * (size_t)vdec->height * 4);
                        if (rgba) {
                            uint8_t *dst[4] = {rgba, NULL, NULL, NULL};
                            int linesize[4] = {vdec->width * 4, 0, 0, 0};
                            sws_scale(sws, (const uint8_t *const *)frm->data, frm->linesize, 0, vdec->height, dst, linesize);
                            out_info->first_frame_rgba_bytes = vdec->width * vdec->height * 4;
                            got_frame = 1;
                            free(rgba);
                        }
                        sws_freeContext(sws);
                    }
                    av_frame_unref(frm);
                }
            }
            av_packet_unref(pkt);
        }
    }

    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&vdec);
    avformat_close_input(&fmt);
    return got_frame ? 0 : -1;
}

/* Returns 0 on normal completion or skip, 1 if app quit/window closed, -1 on open failure */
static int pal_bink_play_file(
    SDL_Window *window, SDL_Renderer *renderer,
    const char *game_dir, const char *rel_path,
    float master_volume, int mute_audio
) {
    char full_path[1200];
    if (resolve_rel_path_ci(game_dir, rel_path, full_path, sizeof(full_path)) != 0) return -1;

    av_log_set_level(AV_LOG_ERROR);

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, full_path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vidx < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    AVStream *vst = fmt->streams[vidx];
    const AVCodec *vc = avcodec_find_decoder(vst->codecpar->codec_id);
    AVCodecContext *vdec = vc ? avcodec_alloc_context3(vc) : NULL;
    if (!vdec || avcodec_parameters_to_context(vdec, vst->codecpar) < 0 || avcodec_open2(vdec, vc, NULL) < 0) {
        if (vdec) avcodec_free_context(&vdec);
        avformat_close_input(&fmt);
        return -1;
    }

    AVCodecContext *adec = NULL;
    SwrContext *swr = NULL;
    SDL_AudioStream *astream = NULL;
    if (aidx >= 0) {
        AVStream *ast = fmt->streams[aidx];
        const AVCodec *ac = avcodec_find_decoder(ast->codecpar->codec_id);
        if (ac) {
            adec = avcodec_alloc_context3(ac);
            if (adec && avcodec_parameters_to_context(adec, ast->codecpar) >= 0 && avcodec_open2(adec, ac, NULL) >= 0) {
                AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&swr, &out_ch, AV_SAMPLE_FMT_S16, 44100,
                                        &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, NULL) >= 0 &&
                    swr && swr_init(swr) >= 0) {
                    SDL_AudioSpec aspec = {SDL_AUDIO_S16, 2, 44100};
                    astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &aspec, NULL, NULL);
                    if (astream) {
                        SDL_SetAudioStreamGain(astream, mute_audio ? 0.0f : master_volume);
                        SDL_ResumeAudioStreamDevice(astream);
                    }
                }
            } else if (adec) {
                avcodec_free_context(&adec);
            }
        }
    }

    int vw = vdec->width;
    int vh = vdec->height;
    SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, vw, vh);
    if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    uint8_t *rgba = (uint8_t *)malloc((size_t)vw * (size_t)vh * 4);
    struct SwsContext *sws = NULL;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    double fps = av_q2d(vst->avg_frame_rate);
    if (fps <= 0.1) fps = av_q2d(vst->r_frame_rate);
    if (fps <= 0.1) fps = 15.0;
    double tb = av_q2d(vst->time_base);

    Uint64 start_ns = SDL_GetTicksNS();
    int64_t frame_num = 0;
    int skip = 0;
    int app_quit = 0;

    Uint8 *aud_tmp = NULL;
    int aud_tmp_cap = 0;

    while (!skip && !app_quit && pkt && frm && tex && rgba && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == aidx && adec && swr && astream) {
            if (avcodec_send_packet(adec, pkt) >= 0) {
                while (avcodec_receive_frame(adec, frm) >= 0) {
                    int max_out = swr_get_out_samples(swr, frm->nb_samples);
                    if (max_out > 0) {
                        int need = max_out * 4;
                        if (need > aud_tmp_cap) {
                            Uint8 *g = (Uint8 *)realloc(aud_tmp, (size_t)need);
                            if (g) { aud_tmp = g; aud_tmp_cap = need; }
                        }
                        if (aud_tmp && need <= aud_tmp_cap) {
                            uint8_t *dst_p = aud_tmp;
                            int conv = swr_convert(swr, &dst_p, max_out, (const uint8_t **)frm->extended_data, frm->nb_samples);
                            if (conv > 0 && !mute_audio) {
                                SDL_PutAudioStreamData(astream, aud_tmp, conv * 4);
                            }
                        }
                    }
                    av_frame_unref(frm);
                }
            }
        } else if (pkt->stream_index == vidx) {
            if (avcodec_send_packet(vdec, pkt) >= 0) {
                while (!skip && !app_quit && avcodec_receive_frame(vdec, frm) >= 0) {
                    if (!sws) {
                        sws = sws_getContext(vw, vh, (enum AVPixelFormat)frm->format,
                                             vw, vh, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
                    }
                    if (sws) {
                        uint8_t *dst[4] = {rgba, NULL, NULL, NULL};
                        int ls[4] = {vw * 4, 0, 0, 0};
                        sws_scale(sws, (const uint8_t *const *)frm->data, frm->linesize, 0, vh, dst, ls);
                        SDL_UpdateTexture(tex, NULL, rgba, vw * 4);
                    }

                    double target_sec = (frm->pts != AV_NOPTS_VALUE && tb > 0.0)
                        ? (double)frm->pts * tb
                        : (double)frame_num / fps;
                    frame_num++;

                    for (;;) {
                        SDL_Event ev;
                        while (SDL_PollEvent(&ev)) {
                            if (ev.type == SDL_EVENT_QUIT ||
                                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(window))) {
                                app_quit = 1;
                                break;
                            }
                            if (ev.type == SDL_EVENT_KEY_DOWN) {
                                SDL_Keycode k = ev.key.key;
                                if (k == SDLK_ESCAPE || k == SDLK_SPACE || k == SDLK_RETURN) {
                                    skip = 1;
                                    break;
                                }
                            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                                skip = 1;
                                break;
                            }
                        }
                        if (skip || app_quit) break;

                        double elapsed_sec = (double)(SDL_GetTicksNS() - start_ns) / 1e9;
                        if (elapsed_sec >= target_sec) break;
                        SDL_Delay(4);
                    }

                    if (!skip && !app_quit) {
                        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                        SDL_RenderClear(renderer);
                        /* Preserve aspect ratio inside 640x480 logical viewport */
                        float dst_h = 480.0f;
                        float dst_w = (float)vw * (480.0f / (float)(vh > 0 ? vh : 480));
                        if (dst_w > 640.0f) {
                            dst_w = 640.0f;
                            dst_h = (float)vh * (640.0f / (float)(vw > 0 ? vw : 640));
                        }
                        SDL_FRect dst_rect = {(640.0f - dst_w) * 0.5f, (480.0f - dst_h) * 0.5f, dst_w, dst_h};
                        SDL_RenderTexture(renderer, tex, NULL, &dst_rect);
                        SDL_RenderPresent(renderer);
                    }
                    av_frame_unref(frm);
                }
            }
        }
        av_packet_unref(pkt);
    }

    free(aud_tmp);
    free(rgba);
    if (tex) SDL_DestroyTexture(tex);
    if (sws) sws_freeContext(sws);
    if (astream) {
        SDL_ClearAudioStream(astream);
        SDL_DestroyAudioStream(astream);
    }
    if (swr) swr_free(&swr);
    if (adec) avcodec_free_context(&adec);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&vdec);
    avformat_close_input(&fmt);
    return app_quit ? 1 : 0;
}

/* =========================================================================
 * M1 Module 5: Talk1 Script Extractor & UI Transition Helper
 * ========================================================================= */

typedef struct {
    char lines[32][256];
    int count;
} PalDialogueList;

static void extract_talk1_dialogues(const char *game_dir, PalDialogueList *out_list) {
    memset(out_list, 0, sizeof(*out_list));
    char path[1200];
    join_path(path, sizeof(path), game_dir, "talk1.dat");
    size_t len = 0;
    uint8_t *buf = read_entire_file(path, &len);
    if (!buf) return;

    const size_t n_slots = 701;
    if (len < PAL_MZ_HEADER_SIZE + n_slots * 4 || buf[0] != 'M' || buf[1] != 'Z') {
        free(buf);
        return;
    }
    const uint8_t *p = buf + PAL_MZ_HEADER_SIZE;
    size_t plen = len - PAL_MZ_HEADER_SIZE;
    size_t data_start = n_slots * 4;

    for (size_t i = 0; i < n_slots && out_list->count < 32; i++) {
        uint32_t off = rd_u32(p + i * 4);
        if (off == 0xFFFFFFFFu || off < data_start || off + 16 > plen) continue;
        /* Scan inside script segment for opcode 0x0004 (dialogue box) followed by ASCII '%C' */
        uint32_t end = (uint32_t)plen;
        for (size_t j = 0; j < n_slots; j++) {
            uint32_t o2 = rd_u32(p + j * 4);
            if (o2 > off && o2 < end) end = o2;
        }
        for (size_t k = off; k + 18 < end && out_list->count < 32; k++) {
            uint16_t w = rd_u16(p + k);
            if ((w == 0x0004u || w == 0x0005u) && k + 14 < end && p[k + 14] == '%' && p[k + 15] == 'C') {
                size_t q = k + 14;
                while (q + 1 < end && !(p[q] == '%' && p[q + 1] == 'Q')) q++;
                if (q + 1 < end && q > k + 16) {
                    char utf8[256];
                    cp950_to_utf8(p + k + 14, q - (k + 14), utf8, sizeof(utf8));
                    if (strlen(utf8) > 8) {
                        snprintf(out_list->lines[out_list->count++], 256, "%s", utf8);
                    }
                    k = q + 2;
                }
            }
        }
    }
    free(buf);

    if (out_list->count == 0) {
        snprintf(out_list->lines[0], 256, "%%C6李逍遙：%%N%%C5新仙劍奇俠傳 SDL3 Metal 原生引擎已啟動。");
        out_list->count = 1;
    }
}

static int run_fade_transition(SDL_Window *window, SDL_Renderer *renderer, int fade_in, Uint32 duration_ms, PalBgmPlayer *bgm) {
    Uint64 start = SDL_GetTicksNS();
    for (;;) {
        pal_bgm_update(bgm);
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(window))) {
                return 1;
            }
        }
        double el_ms = (double)(SDL_GetTicksNS() - start) / 1e6;
        double t = duration_ms > 0 ? el_ms / (double)duration_ms : 1.0;
        if (t >= 1.0) break;
        Uint8 alpha = fade_in ? (Uint8)((1.0 - t) * 255.0) : (Uint8)(t * 255.0);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
        SDL_FRect full = {0.0f, 0.0f, 640.0f, 480.0f};
        SDL_RenderFillRect(renderer, &full);
        SDL_RenderPresent(renderer);
        SDL_Delay(8);
    }
    return 0;
}

typedef struct {
    PalActVM vm;
    PalLoadedSprite sprite;
    uint32_t loaded_fr_id;
    float base_x;
    float base_y;
} PalActSpriteNode;

static void free_act_sprite_node(PalActSpriteNode *n) {
    free_loaded_sprite(&n->sprite);
    memset(n, 0, sizeof(*n));
}

static void sync_act_sprite_texture(
    const char *game_dir, SDL_Renderer *renderer, SDL_ScaleMode scale_mode, PalActSpriteNode *node
) {
    if (node->vm.fr_tsw_id <= 0 || (uint32_t)node->vm.fr_tsw_id == node->loaded_fr_id) return;
    const char *arch = NULL;
    uint32_t tid = 0;
    if (resolve_tsw_from_fr(node->vm.fr_tsw_id, &arch, &tid) != 0) return;
    PalLoadedSprite sp = {0};
    if (vfs_load_tsw_by_id(game_dir, arch, tid, renderer, scale_mode, &sp) == 0) {
        free_loaded_sprite(&node->sprite);
        node->sprite = sp;
        node->loaded_fr_id = (uint32_t)node->vm.fr_tsw_id;
    }
}

static void draw_act_sprite_node(SDL_Renderer *renderer, const PalActSpriteNode *node, int override_frame) {
    if (!node->vm.visible || node->sprite.nframes <= 0) return;
    int fi = override_frame >= 0 ? override_frame : node->vm.frame_index;
    if (fi < 0) fi = 0;
    fi %= node->sprite.nframes;
    SDL_Texture *tex = node->sprite.frames[fi];
    if (!tex) return;
    SDL_SetTextureBlendMode(tex, node->vm.blend_mode == 4 ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
    SDL_FRect dst = {
        node->base_x + (float)node->vm.offset_x,
        node->base_y + (float)node->vm.offset_y,
        (float)node->sprite.w,
        (float)node->sprite.h
    };
    SDL_RenderTexture(renderer, tex, NULL, &dst);
}

static void play_ui_sfx(const char *game_dir, uint32_t snd_id, SDL_AudioStream **sfx_stream, float vol, int muted) {
    if (muted) return;
    SDL_AudioSpec spec = {0};
    uint8_t *pcm = NULL;
    Uint32 len = 0;
    uint32_t sid = 0;
    if (vfs_load_snd_by_id(game_dir, snd_id, &spec, &pcm, &len, &sid) != 0) return;
    if (*sfx_stream) SDL_DestroyAudioStream(*sfx_stream);
    *sfx_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (*sfx_stream) {
        SDL_SetAudioStreamGain(*sfx_stream, vol);
        SDL_ResumeAudioStreamDevice(*sfx_stream);
        SDL_PutAudioStreamData(*sfx_stream, pcm, (int)len);
    }
    SDL_free(pcm);
}

static int count_existing_saves(const char *game_dir, char *out_summary, size_t cap) {
    int found = 0;
    char list[256] = {0};
    size_t used = 0;
    for (int i = 0; i < 20; i++) {
        char rel[64], full[1200];
        snprintf(rel, sizeof(rel), "Save/%d.sav", i);
        if (resolve_rel_path_ci(game_dir, rel, full, sizeof(full)) == 0) {
            found++;
            if (used + 16 < sizeof(list)) {
                used += (size_t)snprintf(list + used, sizeof(list) - used, "%sSave/%d.sav", found > 1 ? "  " : "", i);
            }
        }
    }
    if (found == 0) {
        snprintf(out_summary, cap, "%%C6舊的回憶 (Save/0..19.sav)%%N%%C5目前尚未偵測到存檔紀錄。%%N%%C0按 Esc 或點擊畫面返回主選單。");
    } else {
        snprintf(out_summary, cap, "%%C6舊的回憶 — 偵測到 %d 筆存檔%%N%%C5%s%%N%%C0(存檔讀取將於 M7 完整接線，按 Esc 或點擊返回)", found, list);
    }
    return found;
}

/* =========================================================================
 * SDL3 Metal Interactive Runtime & Self-Test (Boot -> Title -> Field/Talk)
 * ========================================================================= */

typedef enum {
    PAL_SCENE_TITLE = 0,
    PAL_SCENE_LOAD_OVERLAY = 1,
    PAL_SCENE_FIELD = 2
} PalSceneState;

static int run_session_internal(const PalLaunchConfig *cfg, int is_self_test, char *out_err, int32_t err_cap) {
    if (!cfg || cfg->game_dir[0] == '\0') {
        if (out_err) snprintf(out_err, (size_t)err_cap, "Missing game_dir");
        return -1;
    }

    PalValidationReport report;
    if (pal_vfs_validate_directory(cfg->game_dir, &report) != 0) {
        if (out_err) snprintf(out_err, (size_t)err_cap, "Validation failed: %s", report.summary_message);
        return -1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        if (out_err) snprintf(out_err, (size_t)err_cap, "SDL_Init failed: %s", SDL_GetError());
        return -1;
    }

    int win_w = cfg->window_scale == 2 ? 1280 : 640;
    int win_h = cfg->window_scale == 2 ? 960 : 480;
    SDL_WindowFlags wflags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE;
    if (is_self_test) wflags |= SDL_WINDOW_HIDDEN;
    else if (cfg->window_scale == 0) wflags |= SDL_WINDOW_FULLSCREEN;

    SDL_Window *window = SDL_CreateWindow("NewPal — 新仙劍奇俠傳 (SDL3 Metal)", win_w, win_h, wflags);
    if (!window) {
        if (out_err) snprintf(out_err, (size_t)err_cap, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
        return -1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, "metal");
    if (!renderer) {
        if (out_err) snprintf(out_err, (size_t)err_cap, "SDL_CreateRenderer(\"metal\") failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
        return -1;
    }

    const char *active_backend = SDL_GetRendererName(renderer);
    SDL_SetRenderLogicalPresentation(renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(renderer, cfg->vsync_enabled ? 1 : 0);
    SDL_ScaleMode scale_mode = cfg->nearest_filter ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR;

    /* Load Core Field + Title Assets in-memory */
    int map_idx = cfg->initial_map_index;
    int sprite_idx = cfg->initial_char_slot;

    PalLoadedMap map = {0};
    PalLoadedSprite hero_sprite = {0};
    if (vfs_load_lmf_map(cfg->game_dir, "huge.lmf", map_idx, renderer, scale_mode, &map) != 0 ||
        vfs_load_tsw_slot(cfg->game_dir, "all_char.tsw", sprite_idx, renderer, scale_mode, &hero_sprite) != 0) {
        if (out_err) snprintf(out_err, (size_t)err_cap, "Failed to decode initial MSF3 map or TSW sprite.");
        free_loaded_map(&map);
        free_loaded_sprite(&hero_sprite);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
        return -1;
    }

    /* Title Screen .act VM Nodes (GeMainMenu 0x4831f2: all_sys.act id=10000 sub 0x5a..0x5d) */
    PalActSpriteNode title_bg = {.base_x = 0.0f, .base_y = 0.0f};
    PalActSpriteNode title_btns[3] = {
        {.base_x = 270.0f, .base_y = 282.0f}, /* 0x10e, 0x11a: 新的故事 (9055) */
        {.base_x = 270.0f, .base_y = 330.0f}, /* 0x10e, 0x14a: 舊的回憶 (9056) */
        {.base_x = 270.0f, .base_y = 378.0f}  /* 0x10e, 0x17a: 離開遊戲 (9057) */
    };
    int act_bg_ok = (pal_act_vm_init(cfg->game_dir, "all_sys.act", 10000, 0x5a, &title_bg.vm) == 0);
    int act_btn_ok = 1;
    for (int i = 0; i < 3; i++) {
        if (pal_act_vm_init(cfg->game_dir, "all_sys.act", 10000, 0x5b + i, &title_btns[i].vm) != 0) {
            act_btn_ok = 0;
        } else {
            sync_act_sprite_texture(cfg->game_dir, renderer, scale_mode, &title_btns[i]);
        }
    }
    sync_act_sprite_texture(cfg->game_dir, renderer, scale_mode, &title_bg);

    /* Animated cursors & Dialogue box UI from all_sys.tsw */
    PalLoadedSprite menu_cursor = {0};
    PalLoadedSprite dlg_box = {0};
    PalLoadedSprite dlg_cursor = {0};
    vfs_load_tsw_by_id(cfg->game_dir, "all_sys.tsw", 36, renderer, scale_mode, &menu_cursor);
    vfs_load_tsw_by_id(cfg->game_dir, "all_sys.tsw", 82, renderer, scale_mode, &dlg_box);
    vfs_load_tsw_by_id(cfg->game_dir, "all_sys.tsw", 51, renderer, scale_mode, &dlg_cursor);

    if (is_self_test) {
        PalActInfo act_info;
        char act_path[1200];
        join_path(act_path, sizeof(act_path), cfg->game_dir, "all_char.act");
        int act_rc = vfs_inspect_act_slot(act_path, 0, &act_info);

        SDL_AudioSpec wav_spec = {0};
        uint8_t *wav_buf = NULL;
        Uint32 wav_len = 0;
        uint32_t wav_sid = 0;
        int snd_ok = (vfs_load_snd_wav_bytes(cfg->game_dir, 0, &wav_spec, &wav_buf, &wav_len, &wav_sid) == 0);
        if (wav_buf) SDL_free(wav_buf);

        PalBinkProbeInfo bik_notice = {0}, bik_begin = {0};
        int bik_ok = (pal_bink_probe(cfg->game_dir, "Video/begin.bik", &bik_begin) == 0);
        int notice_ok = (pal_bink_probe(cfg->game_dir, "Video/notice.bik", &bik_notice) == 0);

        PalBgmProbeInfo bgm_m001 = {0}, bgm_f001 = {0};
        int bgm_title_ok = (pal_bgm_probe(cfg->game_dir, "Music/M001.mp3", &bgm_m001) == 0);
        int bgm_field_ok = (pal_bgm_probe(cfg->game_dir, "Music/F001.mp3", &bgm_f001) == 0);

        const char *sample_rich = "%C6李逍遙：%N%C5這可不是普通客棧！%S0(M1 CoreText)";
        PalRichTextMetrics rt_metrics = {0};
        int font_ok = (pal_font_probe_rich_text(sample_rich, 18.0f, 520, &rt_metrics) == 0);
        int txt_w = 0, txt_h = 0;
        SDL_Texture *rt_tex = create_richtext_texture(renderer, sample_rich, 18.0f, 520, &txt_w, &txt_h);

        /* Step button 0 VM through a full 15-frame cycle to confirm all 35-opcode transitions */
        int stepped_frames = 0;
        for (int tick = 0; tick < 60; tick++) {
            int prev_fr = title_btns[0].vm.frame_index;
            pal_act_vm_step_tick(&title_btns[0].vm);
            if (title_btns[0].vm.frame_index != prev_fr) stepped_frames++;
        }

        /* Render both Title composition & Field composition onto Metal swapchain */
        for (int f = 0; f < 3; f++) {
            SDL_SetRenderDrawColor(renderer, 8, 10, 14, 255);
            SDL_RenderClear(renderer);
            if (f == 0) {
                draw_act_sprite_node(renderer, &title_bg, 0);
                draw_act_sprite_node(renderer, &title_btns[0], 0);
            } else {
                SDL_FRect dst = {0.0f, 0.0f, (float)map.w_px, (float)map.h_px};
                SDL_RenderTexture(renderer, map.texture, NULL, &dst);
                if (dlg_box.nframes > 0) {
                    SDL_FRect dbox = {0.0f, 336.0f, (float)dlg_box.w, (float)dlg_box.h};
                    SDL_RenderTexture(renderer, dlg_box.frames[0], NULL, &dbox);
                }
                if (rt_tex) {
                    SDL_FRect tdst = {44.0f, 356.0f, (float)txt_w, (float)txt_h};
                    SDL_RenderTexture(renderer, rt_tex, NULL, &tdst);
                }
            }
            SDL_RenderPresent(renderer);
        }
        if (rt_tex) SDL_DestroyTexture(rt_tex);

        int backend_ok = (active_backend && strcmp(active_backend, "metal") == 0);
        int title_assets_ok = (act_bg_ok && act_btn_ok && title_bg.sprite.nframes > 0 &&
                               title_btns[0].sprite.nframes == 15 && menu_cursor.nframes > 0 && dlg_box.nframes > 0);

        printf("=== NewPal SDL3 Metal + BYOD VFS Self-Test (M0+M1) ===\n");
        printf("GameDir: %s\n", cfg->game_dir);
        printf("Validation: %d/%d (is_valid=%d)\n", report.valid_archives_count, report.total_required_archives, report.is_valid);
        printf("Renderer: %s (expected: metal)\n", active_backend ? active_backend : "null");
        printf("Bink Video: Video/begin.bik rc=%d %dx%d fps=%.2f frames=%d codec=%s first_rgba=%d | notice.bik rc=%d\n",
               bik_ok ? 0 : -1, bik_begin.width, bik_begin.height, bik_begin.fps, bik_begin.frame_count,
               bik_begin.codec_name, bik_begin.first_frame_rgba_bytes, notice_ok ? 0 : -1);
        printf("BGM Stream: Music/M001.mp3 rc=%d %dHz ch=%d pcm=%d dur=%.2fs codec=%s | F001.mp3 rc=%d\n",
               bgm_title_ok ? 0 : -1, bgm_m001.sample_rate, bgm_m001.channels, bgm_m001.pcm_bytes,
               bgm_m001.duration_sec, bgm_m001.codec_name, bgm_field_ok ? 0 : -1);
        printf("ACT VM[10000]: bg_FR=%d (%dx%d) btn0_FR=%d btn1_FR=%d btn2_FR=%d stepped=%d | cursor=%u dlg_box=%u\n",
               title_bg.vm.fr_tsw_id, title_bg.sprite.w, title_bg.sprite.h,
               title_btns[0].vm.fr_tsw_id, title_btns[1].vm.fr_tsw_id, title_btns[2].vm.fr_tsw_id,
               stepped_frames, menu_cursor.slot_id, dlg_box.slot_id);
        printf("CoreText RichText: rc=%d tex=%dx%d lines=%d opaque_px=%d\n",
               font_ok ? 0 : -1, rt_metrics.width, rt_metrics.height, rt_metrics.line_count, rt_metrics.non_transparent_pixels);
        printf("MSF3 Map[0]: id=%u size=%dx%d px (%dx%d tiles @ %dpx), huge.lmf=%d\n",
               map.map_id, map.w_px, map.h_px, map.w_tiles, map.h_tiles, map.tile_sz, map.total_maps);
        printf("TSW Sprite[0]: slot_id=%u frames=%d size=%dx%d\n",
               hero_sprite.slot_id, hero_sprite.nframes, hero_sprite.w, hero_sprite.h);
        printf("ACT Slot[0]: rc=%d sub_actions=%d insns=%d clean=%d\n",
               act_rc, act_info.sub_actions, act_info.decoded_insns, act_info.clean_streams);
        printf("SND Slot[0]: rc=%d slot_id=%u rate=%d ch=%d bytes=%u\n",
               snd_ok ? 0 : -1, wav_sid, wav_spec.freq, wav_spec.channels, wav_len);
        if (report.hero_summary_count > 0) {
            printf("Hero[0]: id=%d name=%s HP=%d/%d MP=%d/%d\n",
                   report.heroes[0].id, report.heroes[0].name,
                   report.heroes[0].cur_hp, report.heroes[0].max_hp,
                   report.heroes[0].cur_mp, report.heroes[0].max_mp);
        }

        free_act_sprite_node(&title_bg);
        for (int i = 0; i < 3; i++) free_act_sprite_node(&title_btns[i]);
        free_loaded_sprite(&menu_cursor);
        free_loaded_sprite(&dlg_box);
        free_loaded_sprite(&dlg_cursor);
        free_loaded_map(&map);
        free_loaded_sprite(&hero_sprite);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);

        int all_ok = backend_ok && report.is_valid && act_rc == 0 && snd_ok &&
                     bik_ok && bgm_title_ok && title_assets_ok && font_ok && rt_tex != NULL;
        return all_ok ? 0 : -1;
    }

    /* =====================================================================
     * Interactive Boot Flow: notice.bik -> begin.bik -> Title Screen
     * ===================================================================== */
    PalBgmPlayer bgm = {0};
    SDL_AudioStream *sfx_stream = NULL;
    int app_quit = 0;

    if (pal_bink_play_file(window, renderer, cfg->game_dir, "Video/notice.bik", cfg->master_volume, cfg->mute_audio) == 1) {
        app_quit = 1;
    }
    if (!app_quit &&
        pal_bink_play_file(window, renderer, cfg->game_dir, "Video/begin.bik", cfg->master_volume, cfg->mute_audio) == 1) {
        app_quit = 1;
    }

    PalDialogueList dialogues;
    extract_talk1_dialogues(cfg->game_dir, &dialogues);

    int ver_w = 0, ver_h = 0, save_w = 0, save_h = 0, cur_dlg_w = 0, cur_dlg_h = 0;
    SDL_Texture *ver_tex = create_richtext_texture(renderer, "%C3VER 100C  %C0SDL3 Metal", 13.0f, 300, &ver_w, &ver_h);

    char save_msg[256] = {0};
    count_existing_saves(cfg->game_dir, save_msg, sizeof(save_msg));
    SDL_Texture *save_tex = create_richtext_texture(renderer, save_msg, 17.0f, 460, &save_w, &save_h);

    int dlg_idx = 0;
    int show_dialogue = 1;
    SDL_Texture *dlg_text_tex = create_richtext_texture(renderer, dialogues.lines[0], 18.0f, 520, &cur_dlg_w, &cur_dlg_h);

    PalSceneState scene = PAL_SCENE_TITLE;
    int selected_btn = 0;
    int show_hud = 1;
    float player_x = (float)map.w_px * 0.5f;
    float player_y = (float)map.h_px * 0.5f;
    int walk_frame = 0;
    float walk_acc = 0.0f;
    float mouse_lx = 320.0f, mouse_ly = 298.0f;

    if (!app_quit) {
        pal_bgm_play_track(&bgm, cfg->game_dir, "Music/M001.mp3", cfg->master_volume, cfg->mute_audio);
        SDL_HideCursor();
    }

    Uint64 last_ns = SDL_GetTicksNS();
    double tick35_acc = 0.0;
    int cursor_anim = 0;

    while (!app_quit) {
        pal_bgm_update(&bgm);

        float win_mx = 0.0f, win_my = 0.0f;
        SDL_GetMouseState(&win_mx, &win_my);
        SDL_RenderCoordinatesFromWindow(renderer, win_mx, win_my, &mouse_lx, &mouse_ly);

        if (scene == PAL_SCENE_TITLE) {
            for (int i = 0; i < 3; i++) {
                float bw = title_btns[i].sprite.w > 0 ? (float)title_btns[i].sprite.w : 104.0f;
                float bh = title_btns[i].sprite.h > 0 ? (float)title_btns[i].sprite.h : 34.0f;
                if (mouse_lx >= title_btns[i].base_x - 8.0f && mouse_lx <= title_btns[i].base_x + bw + 8.0f &&
                    mouse_ly >= title_btns[i].base_y - 4.0f && mouse_ly <= title_btns[i].base_y + bh + 4.0f) {
                    if (selected_btn != i) {
                        selected_btn = i;
                        play_ui_sfx(cfg->game_dir, 1, &sfx_stream, cfg->master_volume, cfg->mute_audio);
                    }
                    break;
                }
            }
        }

        int activate_btn = -1;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(window))) {
                app_quit = 1;
                break;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode key = ev.key.key;
                if (scene == PAL_SCENE_TITLE) {
                    if (key == SDLK_UP || key == SDLK_W) {
                        selected_btn = (selected_btn + 2) % 3;
                        play_ui_sfx(cfg->game_dir, 1, &sfx_stream, cfg->master_volume, cfg->mute_audio);
                    } else if (key == SDLK_DOWN || key == SDLK_S) {
                        selected_btn = (selected_btn + 1) % 3;
                        play_ui_sfx(cfg->game_dir, 1, &sfx_stream, cfg->master_volume, cfg->mute_audio);
                    } else if (key == SDLK_RETURN || key == SDLK_SPACE) {
                        activate_btn = selected_btn;
                    } else if (key == SDLK_ESCAPE) {
                        app_quit = 1;
                    }
                } else if (scene == PAL_SCENE_LOAD_OVERLAY) {
                    if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_SPACE) {
                        scene = PAL_SCENE_TITLE;
                    }
                } else if (scene == PAL_SCENE_FIELD) {
                    if (key == SDLK_ESCAPE) {
                        if (run_fade_transition(window, renderer, 0, 200, &bgm) == 1) { app_quit = 1; break; }
                        scene = PAL_SCENE_TITLE;
                        pal_bgm_play_track(&bgm, cfg->game_dir, "Music/M001.mp3", cfg->master_volume, cfg->mute_audio);
                    } else if (key == SDLK_TAB) {
                        show_hud = !show_hud;
                    } else if (key == SDLK_T) {
                        show_dialogue = !show_dialogue;
                    } else if (key == SDLK_B) {
                        SDL_PauseAudioStreamDevice(bgm.stream);
                        if (pal_bink_play_file(window, renderer, cfg->game_dir, "Video/begin.bik", cfg->master_volume, cfg->mute_audio) == 1) {
                            app_quit = 1;
                        }
                        if (bgm.stream) SDL_ResumeAudioStreamDevice(bgm.stream);
                    } else if (key == SDLK_SPACE || key == SDLK_RETURN) {
                        if (show_dialogue && dialogues.count > 0) {
                            dlg_idx = (dlg_idx + 1) % dialogues.count;
                            if (dlg_text_tex) SDL_DestroyTexture(dlg_text_tex);
                            dlg_text_tex = create_richtext_texture(renderer, dialogues.lines[dlg_idx], 18.0f, 520, &cur_dlg_w, &cur_dlg_h);
                            play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, cfg->mute_audio);
                        }
                    } else if (key == SDLK_RIGHTBRACKET || key == SDLK_LEFTBRACKET) {
                        int delta = (key == SDLK_RIGHTBRACKET) ? 1 : -1;
                        map_idx = (map_idx + delta + map.total_maps) % map.total_maps;
                        PalLoadedMap next_map = {0};
                        if (vfs_load_lmf_map(cfg->game_dir, "huge.lmf", map_idx, renderer, scale_mode, &next_map) == 0) {
                            free_loaded_map(&map);
                            map = next_map;
                            player_x = (float)map.w_px * 0.5f;
                            player_y = (float)map.h_px * 0.5f;
                        }
                    } else if (key == SDLK_N || key == SDLK_P) {
                        int delta = (key == SDLK_N) ? 1 : -1;
                        sprite_idx = (sprite_idx + delta + hero_sprite.total_slots) % hero_sprite.total_slots;
                        PalLoadedSprite next_sp = {0};
                        if (vfs_load_tsw_slot(cfg->game_dir, "all_char.tsw", sprite_idx, renderer, scale_mode, &next_sp) == 0) {
                            free_loaded_sprite(&hero_sprite);
                            hero_sprite = next_sp;
                            walk_frame = 0;
                        }
                    }
                }
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                if (scene == PAL_SCENE_TITLE) {
                    for (int i = 0; i < 3; i++) {
                        float bw = title_btns[i].sprite.w > 0 ? (float)title_btns[i].sprite.w : 104.0f;
                        float bh = title_btns[i].sprite.h > 0 ? (float)title_btns[i].sprite.h : 34.0f;
                        if (mouse_lx >= title_btns[i].base_x - 8.0f && mouse_lx <= title_btns[i].base_x + bw + 8.0f &&
                            mouse_ly >= title_btns[i].base_y - 4.0f && mouse_ly <= title_btns[i].base_y + bh + 4.0f) {
                            activate_btn = i;
                            break;
                        }
                    }
                } else if (scene == PAL_SCENE_LOAD_OVERLAY) {
                    scene = PAL_SCENE_TITLE;
                } else if (scene == PAL_SCENE_FIELD && show_dialogue && dialogues.count > 0) {
                    dlg_idx = (dlg_idx + 1) % dialogues.count;
                    if (dlg_text_tex) SDL_DestroyTexture(dlg_text_tex);
                    dlg_text_tex = create_richtext_texture(renderer, dialogues.lines[dlg_idx], 18.0f, 520, &cur_dlg_w, &cur_dlg_h);
                    play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, cfg->mute_audio);
                }
            }
        }

        if (app_quit) break;

        if (activate_btn == 0) {
            play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, cfg->mute_audio);
            if (run_fade_transition(window, renderer, 0, 220, &bgm) == 1) break;
            scene = PAL_SCENE_FIELD;
            pal_bgm_play_track(&bgm, cfg->game_dir, "Music/F001.mp3", cfg->master_volume, cfg->mute_audio);
        } else if (activate_btn == 1) {
            play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, cfg->mute_audio);
            scene = PAL_SCENE_LOAD_OVERLAY;
        } else if (activate_btn == 2) {
            play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, cfg->mute_audio);
            SDL_Delay(120);
            app_quit = 1;
            break;
        }

        Uint64 now_ns = SDL_GetTicksNS();
        float dt = (float)(now_ns - last_ns) / 1e9f;
        if (dt > 0.1f) dt = 0.1f;
        last_ns = now_ns;

        /* 35ms engine tick cadence for .act bytecode VMs & cursors */
        tick35_acc += (double)dt;
        while (tick35_acc >= 0.035) {
            tick35_acc -= 0.035;
            cursor_anim++;
            if (scene == PAL_SCENE_TITLE) {
                for (int i = 0; i < 3; i++) {
                    if (i == selected_btn) {
                        pal_act_vm_step_tick(&title_btns[i].vm);
                        if (title_btns[i].vm.finished) {
                            title_btns[i].vm.pc = 0;
                            title_btns[i].vm.finished = 0;
                            title_btns[i].vm.delay_rem = 0;
                            pal_act_vm_step_tick(&title_btns[i].vm);
                        }
                    } else {
                        title_btns[i].vm.frame_index = 0;
                    }
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (scene == PAL_SCENE_TITLE || scene == PAL_SCENE_LOAD_OVERLAY) {
            draw_act_sprite_node(renderer, &title_bg, 0);
            /* Focused button shimmers via additive .act animation; others stay subtle or unhighlighted */
            draw_act_sprite_node(renderer, &title_btns[selected_btn], -1);

            if (ver_tex) {
                SDL_FRect vdst = {475.0f, 444.0f, (float)ver_w, (float)ver_h};
                SDL_RenderTexture(renderer, ver_tex, NULL, &vdst);
            }

            if (scene == PAL_SCENE_LOAD_OVERLAY) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 12, 16, 28, 215);
                SDL_FRect ov = {64.0f, 120.0f, 512.0f, 220.0f};
                SDL_RenderFillRect(renderer, &ov);
                SDL_SetRenderDrawColor(renderer, 212, 178, 112, 230);
                SDL_RenderRect(renderer, &ov);
                if (save_tex) {
                    SDL_FRect sdst = {92.0f, 152.0f, (float)save_w, (float)save_h};
                    SDL_RenderTexture(renderer, save_tex, NULL, &sdst);
                }
            }

            if (menu_cursor.nframes > 0) {
                SDL_Texture *ctex = menu_cursor.frames[0];
                SDL_FRect cdst = {mouse_lx, mouse_ly, (float)menu_cursor.w, (float)menu_cursor.h};
                SDL_RenderTexture(renderer, ctex, NULL, &cdst);
            }
        } else {
            const bool *keys = SDL_GetKeyboardState(NULL);
            float vx = 0.0f, vy = 0.0f;
            if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])  vx -= 1.0f;
            if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) vx += 1.0f;
            if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])    vy -= 1.0f;
            if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])  vy += 1.0f;

            if (vx != 0.0f || vy != 0.0f) {
                player_x += vx * 180.0f * dt;
                player_y += vy * 180.0f * dt;
                if (player_x < 0.0f) player_x = 0.0f;
                if (player_y < 0.0f) player_y = 0.0f;
                if (player_x > (float)map.w_px) player_x = (float)map.w_px;
                if (player_y > (float)map.h_px) player_y = (float)map.h_px;
                walk_acc += dt;
                if (walk_acc >= 0.12f && hero_sprite.nframes > 0) {
                    walk_acc = 0.0f;
                    walk_frame = (walk_frame + 1) % hero_sprite.nframes;
                }
            }

            float cam_x = player_x - 320.0f;
            float cam_y = player_y - 240.0f;
            if (map.w_px > 640) {
                if (cam_x < 0.0f) cam_x = 0.0f;
                if (cam_x > (float)(map.w_px - 640)) cam_x = (float)(map.w_px - 640);
            } else cam_x = -0.5f * (float)(640 - map.w_px);
            if (map.h_px > 480) {
                if (cam_y < 0.0f) cam_y = 0.0f;
                if (cam_y > (float)(map.h_px - 480)) cam_y = (float)(map.h_px - 480);
            } else cam_y = -0.5f * (float)(480 - map.h_px);

            SDL_FRect map_dst = {-cam_x, -cam_y, (float)map.w_px, (float)map.h_px};
            SDL_RenderTexture(renderer, map.texture, NULL, &map_dst);

            if (hero_sprite.nframes > 0) {
                SDL_Texture *fr = hero_sprite.frames[walk_frame % hero_sprite.nframes];
                SDL_FRect sp_dst = {
                    player_x - cam_x - (float)hero_sprite.w * 0.5f,
                    player_y - cam_y - (float)hero_sprite.h,
                    (float)hero_sprite.w,
                    (float)hero_sprite.h
                };
                SDL_RenderTexture(renderer, fr, NULL, &sp_dst);
            }

            if (show_dialogue) {
                if (dlg_box.nframes > 0) {
                    SDL_FRect bd = {0.0f, 336.0f, (float)dlg_box.w, (float)dlg_box.h};
                    SDL_RenderTexture(renderer, dlg_box.frames[0], NULL, &bd);
                } else {
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(renderer, 16, 18, 36, 210);
                    SDL_FRect bd = {0.0f, 336.0f, 640.0f, 144.0f};
                    SDL_RenderFillRect(renderer, &bd);
                }
                if (dlg_text_tex) {
                    SDL_FRect td = {44.0f, 356.0f, (float)cur_dlg_w, (float)cur_dlg_h};
                    SDL_RenderTexture(renderer, dlg_text_tex, NULL, &td);
                }
                if (dlg_cursor.nframes > 0) {
                    SDL_Texture *dc = dlg_cursor.frames[(cursor_anim / 2) % dlg_cursor.nframes];
                    SDL_FRect cd = {596.0f, 444.0f, (float)dlg_cursor.w, (float)dlg_cursor.h};
                    SDL_RenderTexture(renderer, dc, NULL, &cd);
                }
            }

            if (show_hud) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 165);
                SDL_FRect hud = {8.0f, 8.0f, 624.0f, 46.0f};
                SDL_RenderFillRect(renderer, &hud);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                SDL_RenderDebugTextFormat(
                    renderer, 16.0f, 16.0f,
                    "SDL3 %s | Map %d/%d (ID %u) | Sprite %u | Talk %d/%d",
                    active_backend ? active_backend : "metal",
                    map_idx + 1, map.total_maps, map.map_id, hero_sprite.slot_id,
                    dlg_idx + 1, dialogues.count
                );
                SDL_RenderDebugText(
                    renderer, 16.0f, 32.0f,
                    "[WASD] Move  [[/]] Map  [N/P] Char  [Space/Click] Next Talk  [T] Box  [B] Intro  [Esc] Title"
                );
            }
        }

        SDL_RenderPresent(renderer);
    }

    SDL_ShowCursor();
    pal_bgm_stop(&bgm);
    if (sfx_stream) SDL_DestroyAudioStream(sfx_stream);
    if (ver_tex) SDL_DestroyTexture(ver_tex);
    if (save_tex) SDL_DestroyTexture(save_tex);
    if (dlg_text_tex) SDL_DestroyTexture(dlg_text_tex);
    free_act_sprite_node(&title_bg);
    for (int i = 0; i < 3; i++) free_act_sprite_node(&title_btns[i]);
    free_loaded_sprite(&menu_cursor);
    free_loaded_sprite(&dlg_box);
    free_loaded_sprite(&dlg_cursor);
    free_loaded_map(&map);
    free_loaded_sprite(&hero_sprite);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    return 0;
}

int32_t pal_sdl3_run_session(const PalLaunchConfig *cfg, char *out_err, int32_t err_cap) {
    return run_session_internal(cfg, 0, out_err, err_cap);
}

int32_t pal_sdl3_self_test(const char *game_dir) {
    PalLaunchConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.game_dir, sizeof(cfg.game_dir), "%s", game_dir ? game_dir : "");
    cfg.initial_map_index = 0;
    cfg.initial_char_slot = 0;
    cfg.window_scale = 1;
    cfg.vsync_enabled = 1;
    cfg.nearest_filter = 1;
    cfg.master_volume = 0.8f;
    cfg.mute_audio = 1;

    char err[256] = {0};
    int32_t rc = run_session_internal(&cfg, 1, err, sizeof(err));
    if (rc != 0 && err[0] != '\0') {
        fprintf(stderr, "Self-test error: %s\n", err);
    }
    return rc;
}

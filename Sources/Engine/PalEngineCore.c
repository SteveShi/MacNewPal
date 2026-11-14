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
    uint32_t id;
    uint32_t talk_id;
    uint32_t param;
    uint32_t flag_id;
} PalMapTrigger;

typedef struct {
    uint16_t sid;
    uint16_t flags;
    int w;
    int h;
    SDL_Texture *tex;
} PalMapFgDef;

typedef struct {
    uint16_t sid;
    int16_t px;
    int16_t py_top;
    int16_t sort_y;
    uint8_t layer_bucket; /* 2=below actors, 0=Y-sorted with actors, 1=above actors (0x443800) */
    uint8_t reserved;
} PalMapFgInst;

typedef struct {
    uint16_t obj_id;
    uint16_t act_id;
    uint16_t sub_state;
    uint16_t dir;
    uint16_t tx;
    uint16_t ty;
    uint16_t talk_id;
    uint16_t flags;
} PalMapNpcDef;

typedef struct {
    uint32_t map_id;
    uint32_t msf3_id;
    uint16_t music_idx;
    int map_idx;
    int w_tiles;
    int h_tiles;
    int tile_sz;
    int w_px;
    int h_px;
    int total_maps;
    int step_speed_px;
    SDL_Texture *texture;
    uint32_t *walk_grid;
    int walkable_count;
    int blocked_count;
    PalMapTrigger *triggers;
    int trigger_count;
    PalMapFgDef *fg_defs;
    int fg_def_count;
    PalMapFgInst *fg_insts;
    int fg_inst_count;
    PalMapNpcDef *npcs;
    int npc_count;
    char title_utf8[64];
    char bgm_track[64];
    char lmf_file[16];
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

static void free_loaded_map(PalLoadedMap *m) {
    if (!m) return;
    if (m->texture) SDL_DestroyTexture(m->texture);
    if (m->fg_defs) {
        for (int i = 0; i < m->fg_def_count; i++) {
            if (m->fg_defs[i].tex) SDL_DestroyTexture(m->fg_defs[i].tex);
        }
        free(m->fg_defs);
    }
    free(m->walk_grid);
    free(m->triggers);
    free(m->fg_insts);
    free(m->npcs);
    memset(m, 0, sizeof(*m));
}

static void vfs_lookup_map_metadata(
    const char *game_dir, uint32_t query_map_id,
    uint32_t *out_msf3_id, PalLoadedMap *out_map
) {
    if (out_msf3_id) *out_msf3_id = query_map_id;
    if (out_map) {
        if (out_map->title_utf8[0] == '\0') {
            snprintf(out_map->title_utf8, sizeof(out_map->title_utf8), "Map #%u", query_map_id);
        }
        if (out_map->bgm_track[0] == '\0') {
            snprintf(out_map->bgm_track, sizeof(out_map->bgm_track), "Music/F001.mp3");
        }
        if (out_map->step_speed_px <= 0) out_map->step_speed_px = 4;
    }

    char path[1200];
    join_path(path, sizeof(path), game_dir, "maps.dat");
    size_t len = 0;
    uint8_t *buf = read_entire_file(path, &len);
    if (!buf) return;

    if (len < PAL_MZ_HEADER_SIZE + 104 || buf[0] != 'M' || buf[1] != 'Z') {
        free(buf);
        return;
    }
    const uint8_t *p = buf + PAL_MZ_HEADER_SIZE;
    size_t plen = len - PAL_MZ_HEADER_SIZE;
    if (rd_u32(p) != 100u) {
        free(buf);
        return;
    }

    uint32_t slots[25] = {0};
    for (int i = 0; i < 25; i++) slots[i] = rd_u32(p + 4 + (size_t)i * 4);

    uint16_t slot2_bgm_idx = 0;

    /* Slot 2: 14-byte records [map_id, msf3_id, rec2(bgm_idx), rec3(step_speed), ...] */
    if (slots[2] > 0 && slots[2] + 14 <= plen) {
        size_t pos = slots[2];
        while (pos + 14 <= plen) {
            uint16_t mid = rd_u16(p + pos);
            if (mid == 0xFFFFu) break;
            if ((uint32_t)mid == query_map_id) {
                uint16_t msf_id = rd_u16(p + pos + 2);
                uint16_t w2 = rd_u16(p + pos + 4);
                uint16_t w3 = rd_u16(p + pos + 6);
                if (out_msf3_id && msf_id != 0xFFFFu && msf_id > 0) *out_msf3_id = msf_id;
                slot2_bgm_idx = w2 & 0x0Fu;
                if (out_map) {
                    uint16_t spd = w3 & 0x0Fu;
                    if (spd == 1 || spd == 2 || spd == 4 || spd == 8 || spd == 16) {
                        out_map->step_speed_px = (int)spd;
                    }
                }
                break;
            }
            pos += 14;
        }
    }

    if (!out_map) {
        free(buf);
        return;
    }

    /* Slot 19: packed (uint16 map_id, cp950 title, "%Q") entries (0x41d793) */
    if (slots[19] > 0 && slots[19] + 2 <= plen) {
        size_t pos = slots[19];
        while (pos + 2 <= plen) {
            uint16_t mid = rd_u16(p + pos);
            pos += 2;
            if (mid == 0xFFFFu) break;
            size_t start = pos;
            while (pos + 1 < plen && !(p[pos] == '%' && p[pos + 1] == 'Q')) pos++;
            if (pos + 1 >= plen) break;
            if ((uint32_t)mid == query_map_id) {
                char utf8[64] = {0};
                cp950_to_utf8(p + start, pos - start, utf8, sizeof(utf8));
                if (utf8[0] != '\0') {
                    snprintf(out_map->title_utf8, sizeof(out_map->title_utf8), "%s", utf8);
                }
                break;
            }
            pos += 2;
        }
    }

    /* Slot 1: double-indirection BGM string table (0x41d335) indexed by MSF3 hdr[0x10c] or slot2_bgm_idx */
    uint32_t eff_bgm_idx = out_map->music_idx > 0 ? (uint32_t)out_map->music_idx : (uint32_t)slot2_bgm_idx;
    if (eff_bgm_idx > 0 && slots[1] > 0 && slots[1] + 4 <= plen) {
        uint32_t tbl_off = rd_u32(p + slots[1]);
        if ((uint64_t)tbl_off + (uint64_t)eff_bgm_idx * 4 + 4 <= plen) {
            uint32_t str_off = rd_u32(p + tbl_off + (size_t)eff_bgm_idx * 4);
            if ((uint64_t)str_off + 6 <= plen) {
                char fname[48] = {0};
                size_t k = 0;
                while (k + 1 < sizeof(fname) && str_off + 4 + k < plen) {
                    uint8_t ch = p[str_off + 4 + k];
                    if (ch == 0) break;
                    fname[k++] = (char)ch;
                }
                fname[k] = '\0';
                if (strchr(fname, '.') != NULL) {
                    snprintf(out_map->bgm_track, sizeof(out_map->bgm_track), "Music/%s", fname);
                }
            }
        }
    }

    /* Slot 0: NPC & map object placement table (26 bytes per record, 0x4197a6) */
    if (!out_map->npcs && slots[0] > 0 && slots[0] + 26 <= plen) {
        size_t pos = slots[0];
        int match_cnt = 0;
        while (pos + 26 <= plen) {
            int16_t mid = rd_i16(p + pos);
            if (mid == -1) break;
            if ((uint32_t)(uint16_t)mid == query_map_id) {
                uint16_t obj_id = rd_u16(p + pos + 2);
                uint16_t act_id = rd_u16(p + pos + 4);
                if (obj_id != 10000u && obj_id != 10001u && act_id != 0) match_cnt++;
            }
            pos += 26;
        }
        if (match_cnt > 0) {
            out_map->npcs = (PalMapNpcDef *)calloc((size_t)match_cnt, sizeof(PalMapNpcDef));
            if (out_map->npcs) {
                pos = slots[0];
                int n = 0;
                while (pos + 26 <= plen && n < match_cnt) {
                    int16_t mid = rd_i16(p + pos);
                    if (mid == -1) break;
                    if ((uint32_t)(uint16_t)mid == query_map_id) {
                        uint16_t obj_id = rd_u16(p + pos + 2);
                        uint16_t act_id = rd_u16(p + pos + 4);
                        if (obj_id != 10000u && obj_id != 10001u && act_id != 0) {
                            PalMapNpcDef *nd = &out_map->npcs[n++];
                            nd->obj_id = obj_id;
                            nd->act_id = act_id;
                            nd->sub_state = rd_u16(p + pos + 6);
                            nd->dir = rd_u16(p + pos + 8);
                            nd->tx = rd_u16(p + pos + 10);
                            nd->ty = rd_u16(p + pos + 12);
                            nd->talk_id = rd_u16(p + pos + 14);
                            nd->flags = rd_u16(p + pos + 20);
                        }
                    }
                    pos += 26;
                }
                out_map->npc_count = n;
            }
        }
    }

    free(buf);
}

static int vfs_load_lmf_map(
    const char *game_dir, const char *lmf_name, int map_idx,
    SDL_Renderer *renderer, SDL_ScaleMode scale_mode, PalLoadedMap *out_map
) {
    memset(out_map, 0, sizeof(*out_map));
    snprintf(out_map->lmf_file, sizeof(out_map->lmf_file), "%s", lmf_name ? lmf_name : "huge.lmf");
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
    out_map->map_idx = map_idx;
    out_map->map_id = me.id;
    out_map->msf3_id = me.id;
    out_map->step_speed_px = 4;

    uint8_t msf_hdr[4 + 0x400];
    if (read_file_slice(f, me.offset, msf_hdr, sizeof(msf_hdr)) != 0 || memcmp(msf_hdr, "MSF3", 4) != 0) {
        fclose(f);
        return -1;
    }
    const uint8_t *hdr = msf_hdr + 4;
    uint16_t w_tiles = rd_u16(hdr + 0x100);
    uint16_t h_tiles = rd_u16(hdr + 0x102);
    uint16_t tile_sz = rd_u16(hdr + 0x104);
    uint16_t music_idx = rd_u16(hdr + 0x10c);
    uint32_t walk_off = rd_u32(hdr + 0x08);
    uint32_t walk_len = rd_u32(hdr + 0x0c);
    uint32_t trig_off = rd_u32(hdr + 0x10);
    uint32_t trig_len = rd_u32(hdr + 0x14);
    uint32_t c18_off  = rd_u32(hdr + 0x18);
    uint32_t c18_len  = rd_u32(hdr + 0x1c);
    uint32_t c20_off  = rd_u32(hdr + 0x20);
    uint32_t c20_len  = rd_u32(hdr + 0x24);
    uint32_t bg_chunk_off = rd_u32(hdr + 0x38);

    if (bg_chunk_off == 0 || tile_sz == 0 || w_tiles == 0 || h_tiles == 0) {
        fclose(f);
        return -1;
    }
    out_map->music_idx = music_idx;

    size_t layer_tiles = (size_t)w_tiles * (size_t)h_tiles;
    int pw = (int)w_tiles * (int)tile_sz;
    int ph = (int)h_tiles * (int)tile_sz;

    /* 1. Layer 0 Tilemap + LZO1X Tile Pool -> Background SDL_Texture (only when renderer != NULL) */
    if (renderer) {
        uint8_t *tm_raw = (uint8_t *)malloc(layer_tiles * 4);
        uint8_t bg_hdr[0x400];
        if (!tm_raw ||
            read_file_slice(f, (uint64_t)me.offset + 0x118, tm_raw, layer_tiles * 4) != 0 ||
            read_file_slice(f, (uint64_t)me.offset + bg_chunk_off, bg_hdr, sizeof(bg_hdr)) != 0) {
            free(tm_raw);
            fclose(f);
            free_loaded_map(out_map);
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

        size_t tile_bytes = (size_t)tile_sz * (size_t)tile_sz * 2;
        size_t avail_tiles = tile_bytes > 0 ? pool_used / tile_bytes : 0;
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
        if (!rgba) {
            fclose(f);
            free_loaded_map(out_map);
            return -1;
        }
        SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, pw, ph);
        if (tex) {
            SDL_UpdateTexture(tex, NULL, rgba, pw * 4);
            SDL_SetTextureScaleMode(tex, scale_mode);
        }
        free(rgba);
        if (!tex) {
            fclose(f);
            free_loaded_map(out_map);
            return -1;
        }
        out_map->texture = tex;
    }

    /* 2. Walkability & Trigger Grid (Chunk +0x08 -> w_tiles * h_tiles * 4 bytes) */
    if (walk_off > 0 && walk_len > 0 && (uint64_t)me.offset + walk_off + walk_len <= fsize) {
        uint8_t *wcomp = (uint8_t *)malloc(walk_len);
        if (wcomp && read_file_slice(f, (uint64_t)me.offset + walk_off, wcomp, walk_len) == 0) {
            size_t decomp_len = 0;
            uint8_t *wdec = lzo_alloc_decompress(wcomp, walk_len, layer_tiles * 4, 0, &decomp_len);
            if (wdec && decomp_len >= layer_tiles * 4) {
                out_map->walk_grid = (uint32_t *)malloc(layer_tiles * sizeof(uint32_t));
                if (out_map->walk_grid) {
                    int walk_c = 0, block_c = 0;
                    for (size_t i = 0; i < layer_tiles; i++) {
                        uint32_t cell = rd_u32(wdec + i * 4);
                        out_map->walk_grid[i] = cell;
                        if ((cell & 0x60000000u) != 0) block_c++;
                        else walk_c++;
                    }
                    out_map->walkable_count = walk_c;
                    out_map->blocked_count = block_c;
                }
            }
            free(wdec);
        }
        free(wcomp);
    }

    /* 3. Trigger Definitions (Chunk +0x10 -> count + count * 48 bytes) */
    if (trig_off > 0 && trig_len >= 4 && (uint64_t)me.offset + trig_off + trig_len <= fsize) {
        uint8_t *tbuf = (uint8_t *)malloc(trig_len);
        if (tbuf && read_file_slice(f, (uint64_t)me.offset + trig_off, tbuf, trig_len) == 0) {
            uint32_t ntrig = rd_u32(tbuf);
            if (ntrig > 0 && (uint64_t)ntrig * 48 + 4 <= trig_len) {
                out_map->triggers = (PalMapTrigger *)calloc(ntrig, sizeof(PalMapTrigger));
                if (out_map->triggers) {
                    for (uint32_t t = 0; t < ntrig; t++) {
                        const uint8_t *rec = tbuf + 4 + (size_t)t * 48;
                        out_map->triggers[t].id = rd_u32(rec + 0);
                        out_map->triggers[t].talk_id = rd_u32(rec + 4);
                        out_map->triggers[t].param = rd_u32(rec + 8);
                        out_map->triggers[t].flag_id = rd_u32(rec + 12);
                    }
                    out_map->trigger_count = (int)ntrig;
                }
            }
        }
        free(tbuf);
    }

    /* 4. Foreground Occlusion Sprite Definitions (Chunk +0x18 -> u16 n18 + recs @ +8) */
    if (c18_off > 0 && c18_len >= 8 && (uint64_t)me.offset + c18_off + c18_len <= fsize) {
        uint8_t *cbuf = (uint8_t *)malloc(c18_len);
        if (cbuf && read_file_slice(f, (uint64_t)me.offset + c18_off, cbuf, c18_len) == 0) {
            uint16_t n18 = rd_u16(cbuf);
            if (n18 > 0 && 8 + (size_t)n18 * 16 <= c18_len) {
                out_map->fg_defs = (PalMapFgDef *)calloc(n18, sizeof(PalMapFgDef));
                if (out_map->fg_defs) {
                    out_map->fg_def_count = (int)n18;
                    for (uint16_t k = 0; k < n18; k++) {
                        const uint8_t *rec = cbuf + 8 + (size_t)k * 16;
                        uint32_t soff = rd_u32(rec + 0);
                        uint32_t csz1 = rd_u32(rec + 4);
                        uint32_t csz2 = rd_u32(rec + 8);
                        uint16_t sid = rd_u16(rec + 12);
                        uint16_t sflg = rd_u16(rec + 14);
                        out_map->fg_defs[k].sid = sid;
                        out_map->fg_defs[k].flags = sflg;
                        if (renderer && soff > 0 && csz1 > 0 && csz2 >= 8 &&
                            (uint64_t)me.offset + soff + csz1 <= fsize) {
                            uint8_t *scomp = (uint8_t *)malloc(csz1);
                            if (scomp && read_file_slice(f, (uint64_t)me.offset + soff, scomp, csz1) == 0) {
                                size_t dec_len = 0;
                                uint8_t *sdec = lzo_alloc_decompress(scomp, csz1, csz2, 1, &dec_len);
                                if (sdec && dec_len >= 8 && rd_u16(sdec) == 0xFFFFu) {
                                    uint16_t sw = rd_u16(sdec + 2);
                                    uint16_t sh = rd_u16(sdec + 4);
                                    uint16_t sbpp = rd_u16(sdec + 6) & 0x3FFFu;
                                    if (sw > 0 && sh > 0 && (sbpp == 16 || sbpp == 8)) {
                                        uint8_t *srgba = (uint8_t *)calloc((size_t)sw * (size_t)sh, 4);
                                        if (srgba) {
                                            decode_arle_to_rgba(sdec, dec_len, sw, sh, sbpp, NULL, NULL, 0, srgba);
                                            SDL_Texture *stex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, sw, sh);
                                            if (stex) {
                                                SDL_UpdateTexture(stex, NULL, srgba, sw * 4);
                                                SDL_SetTextureScaleMode(stex, scale_mode);
                                                SDL_SetTextureBlendMode(stex, SDL_BLENDMODE_BLEND);
                                                out_map->fg_defs[k].w = sw;
                                                out_map->fg_defs[k].h = sh;
                                                out_map->fg_defs[k].tex = stex;
                                            }
                                            free(srgba);
                                        }
                                    }
                                }
                                free(sdec);
                            }
                            free(scomp);
                        }
                    }
                }
            }
        }
        free(cbuf);
    }

    /* 5. Foreground Occlusion Sprite Instances (Chunk +0x20 -> u16 n20 + recs @ +8) */
    if (c20_off > 0 && c20_len >= 8 && (uint64_t)me.offset + c20_off + c20_len <= fsize) {
        uint8_t *ibuf = (uint8_t *)malloc(c20_len);
        if (ibuf && read_file_slice(f, (uint64_t)me.offset + c20_off, ibuf, c20_len) == 0) {
            uint16_t n20 = rd_u16(ibuf);
            if (n20 > 0 && 8 + (size_t)n20 * 16 <= c20_len) {
                out_map->fg_insts = (PalMapFgInst *)calloc(n20, sizeof(PalMapFgInst));
                if (out_map->fg_insts) {
                    static const uint8_t kPrioMap[4] = {2, 0, 1, 0};
                    for (uint16_t k = 0; k < n20; k++) {
                        const uint8_t *rec = ibuf + 8 + (size_t)k * 16;
                        PalMapFgInst *inst = &out_map->fg_insts[k];
                        inst->sid = rd_u16(rec + 0);
                        inst->px = rd_i16(rec + 2);
                        inst->py_top = rd_i16(rec + 4);
                        inst->sort_y = rd_i16(rec + 8);
                        uint16_t lflg = rd_u16(rec + 10);
                        inst->layer_bucket = kPrioMap[(lflg & 0x300u) >> 8];
                    }
                    out_map->fg_inst_count = (int)n20;
                }
            }
        }
        free(ibuf);
    }

    fclose(f);

    out_map->w_tiles = w_tiles;
    out_map->h_tiles = h_tiles;
    out_map->tile_sz = tile_sz;
    out_map->w_px = pw;
    out_map->h_px = ph;

    vfs_lookup_map_metadata(game_dir, out_map->map_id, NULL, out_map);
    return 0;
}

static int find_lmf_entry_by_id(const char *game_dir, const char *lmf_name, uint32_t target_id) {
    char path[1200];
    join_path(path, sizeof(path), game_dir, lmf_name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t fsize = get_file_size_fp(f);
    PalLmfEntry *entries = NULL;
    int cnt = 0;
    int found = -1;
    if (read_lmf_directory(f, fsize, &entries, &cnt) == 0 && cnt > 0) {
        for (int i = 0; i < cnt; i++) {
            if (entries[i].id == target_id) {
                found = i;
                break;
            }
        }
    }
    free(entries);
    fclose(f);
    return found;
}

static int vfs_load_lmf_map_by_id(
    const char *game_dir, uint32_t map_id,
    SDL_Renderer *renderer, SDL_ScaleMode scale_mode, PalLoadedMap *out_map
) {
    uint32_t msf3_id = map_id;
    vfs_lookup_map_metadata(game_dir, map_id, &msf3_id, NULL);

    static const char *const kLmfArchives[2] = {"huge.lmf", "huge2.lmf"};
    for (int a = 0; a < 2; a++) {
        int idx = find_lmf_entry_by_id(game_dir, kLmfArchives[a], msf3_id);
        if (idx >= 0) {
            if (vfs_load_lmf_map(game_dir, kLmfArchives[a], idx, renderer, scale_mode, out_map) == 0) {
                if (out_map->map_id != map_id) {
                    out_map->map_id = map_id;
                    free(out_map->npcs);
                    out_map->npcs = NULL;
                    out_map->npc_count = 0;
                    vfs_lookup_map_metadata(game_dir, map_id, NULL, out_map);
                }
                return 0;
            }
        }
    }
    return -1;
}

/* =========================================================================
 * M2 Walkability, Collision, Diagonal Sliding & A* Grid Pathfinding
 * ========================================================================= */

static int pal_map_can_stand_tile(const PalLoadedMap *m, int tx, int ty, int foot_w) {
    if (!m || m->w_tiles <= 0 || m->h_tiles <= 0) return 0;
    if (foot_w < 1) foot_w = 2;
    if (tx < 0 || ty < 0 || tx + foot_w > m->w_tiles || ty >= m->h_tiles) return 0;
    if (!m->walk_grid) return 1;
    for (int dx = 0; dx < foot_w; dx++) {
        if ((m->walk_grid[(size_t)ty * (size_t)m->w_tiles + (size_t)(tx + dx)] & 0x60000000u) != 0) {
            return 0;
        }
    }
    return 1;
}

static int pal_map_can_stand_px(const PalLoadedMap *m, int px, int py, int foot_w, int noclip) {
    if (!m || m->w_px <= 0 || m->h_px <= 0) return 0;
    if (foot_w < 1) foot_w = 2;
    int fw_px = foot_w * 16;
    if (px < 0 || py < 0 || px + fw_px > m->w_px || py + 16 > m->h_px) return 0;
    if (noclip || !m->walk_grid) return 1;
    int tx0 = px >> 4;
    int tx1 = (px + fw_px - 1) >> 4;
    int ty0 = py >> 4;
    int ty1 = (py + 15) >> 4;
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            if (tx < 0 || ty < 0 || tx >= m->w_tiles || ty >= m->h_tiles) return 0;
            if ((m->walk_grid[(size_t)ty * (size_t)m->w_tiles + (size_t)tx] & 0x60000000u) != 0) {
                return 0;
            }
        }
    }
    return 1;
}

static uint8_t pal_map_trigger_at_px(const PalLoadedMap *m, int px, int py, int foot_w) {
    if (!m || !m->walk_grid || m->w_tiles <= 0 || m->h_tiles <= 0) return 0;
    if (foot_w < 1) foot_w = 2;
    int tx0 = px >> 4;
    int ty0 = py >> 4;
    for (int dx = 0; dx < foot_w; dx++) {
        int tx = tx0 + dx;
        if (tx >= 0 && tx < m->w_tiles && ty0 >= 0 && ty0 < m->h_tiles) {
            uint8_t trig = (uint8_t)(m->walk_grid[(size_t)ty0 * (size_t)m->w_tiles + (size_t)tx] & 0xFFu);
            if (trig != 0) return trig;
        }
    }
    return 0;
}

static int pal_map_nearest_walkable(const PalLoadedMap *m, int *io_tx, int *io_ty, int foot_w, int max_radius) {
    int cx = *io_tx, cy = *io_ty;
    if (pal_map_can_stand_tile(m, cx, cy, foot_w)) return 1;
    for (int r = 1; r <= max_radius; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r) continue;
                if (pal_map_can_stand_tile(m, cx + dx, cy + dy, foot_w)) {
                    *io_tx = cx + dx;
                    *io_ty = cy + dy;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int pal_map_find_path_astar(
    const PalLoadedMap *m,
    int start_tx, int start_ty,
    int goal_tx, int goal_ty,
    int foot_w,
    int *out_tx, int *out_ty, int max_steps
) {
    if (!m || !out_tx || !out_ty || max_steps <= 0) return 0;
    int W = m->w_tiles, H = m->h_tiles;
    if (W <= 0 || H <= 0) return 0;
    int N = W * H;

    if (!pal_map_nearest_walkable(m, &start_tx, &start_ty, foot_w, 6)) return 0;
    if (!pal_map_nearest_walkable(m, &goal_tx, &goal_ty, foot_w, 6)) return 0;
    if (start_tx == goal_tx && start_ty == goal_ty) return 0;

    int32_t *g_score = (int32_t *)malloc((size_t)N * sizeof(int32_t));
    int32_t *f_score = (int32_t *)malloc((size_t)N * sizeof(int32_t));
    int32_t *parent  = (int32_t *)malloc((size_t)N * sizeof(int32_t));
    uint8_t *state   = (uint8_t *)calloc((size_t)N, 1); /* 0=unseen, 1=open, 2=closed */
    int heap_cap = N * 4 + 1;
    int32_t *heap    = (int32_t *)malloc((size_t)heap_cap * sizeof(int32_t));
    int32_t *rev_x   = (int32_t *)malloc((size_t)max_steps * sizeof(int32_t));
    int32_t *rev_y   = (int32_t *)malloc((size_t)max_steps * sizeof(int32_t));

    if (!g_score || !f_score || !parent || !state || !heap || !rev_x || !rev_y) {
        free(g_score); free(f_score); free(parent); free(state); free(heap); free(rev_x); free(rev_y);
        return 0;
    }

    for (int i = 0; i < N; i++) {
        g_score[i] = 0x3FFFFFFF;
        f_score[i] = 0x3FFFFFFF;
        parent[i] = -1;
    }

    int start_idx = start_ty * W + start_tx;
    int goal_idx  = goal_ty * W + goal_tx;
    int dx0 = abs(goal_tx - start_tx), dy0 = abs(goal_ty - start_ty);
    int h0 = (dx0 < dy0 ? 14 * dx0 + 10 * (dy0 - dx0) : 14 * dy0 + 10 * (dx0 - dy0));

    g_score[start_idx] = 0;
    f_score[start_idx] = h0;
    state[start_idx] = 1;

    int heap_sz = 0;
    heap[heap_sz++] = start_idx;

    static const int kDx[8] = { 0,  0, -1, 1, -1,  1, -1,  1};
    static const int kDy[8] = {-1,  1,  0, 0, -1,  1,  1, -1};
    static const int kCost[8] = {10, 10, 10, 10, 14, 14, 14, 14};

    int found = 0;
    while (heap_sz > 0) {
        int cur = heap[0];
        heap[0] = heap[--heap_sz];
        int hi = 0;
        while (1) {
            int l = hi * 2 + 1, r = l + 1, sm = hi;
            if (l < heap_sz && f_score[heap[l]] < f_score[heap[sm]]) sm = l;
            if (r < heap_sz && f_score[heap[r]] < f_score[heap[sm]]) sm = r;
            if (sm == hi) break;
            int tmp = heap[hi]; heap[hi] = heap[sm]; heap[sm] = tmp;
            hi = sm;
        }

        if (state[cur] == 2) continue;
        state[cur] = 2;
        if (cur == goal_idx) {
            found = 1;
            break;
        }

        int cx = cur % W;
        int cy = cur / W;
        for (int d = 0; d < 8; d++) {
            int nx = cx + kDx[d];
            int ny = cy + kDy[d];
            if (!pal_map_can_stand_tile(m, nx, ny, foot_w)) continue;
            /* Prevent corner clipping across blocked diagonal corners */
            if (kDx[d] != 0 && kDy[d] != 0) {
                if (!pal_map_can_stand_tile(m, cx + kDx[d], cy, foot_w) ||
                    !pal_map_can_stand_tile(m, cx, cy + kDy[d], foot_w)) {
                    continue;
                }
            }
            int nidx = ny * W + nx;
            if (state[nidx] == 2) continue;
            int ng = g_score[cur] + kCost[d];
            if (ng < g_score[nidx]) {
                parent[nidx] = cur;
                g_score[nidx] = ng;
                int adx = abs(goal_tx - nx), ady = abs(goal_ty - ny);
                int nh = (adx < ady ? 14 * adx + 10 * (ady - adx) : 14 * ady + 10 * (adx - ady));
                f_score[nidx] = ng + nh;
                state[nidx] = 1;
                if (heap_sz < heap_cap) {
                    int ci = heap_sz++;
                    heap[ci] = nidx;
                    while (ci > 0) {
                        int pi = (ci - 1) / 2;
                        if (f_score[heap[pi]] <= f_score[heap[ci]]) break;
                        int tmp = heap[pi]; heap[pi] = heap[ci]; heap[ci] = tmp;
                        ci = pi;
                    }
                }
            }
        }
    }

    int step_cnt = 0;
    if (found) {
        int cur = goal_idx;
        while (cur != -1 && cur != start_idx && step_cnt < max_steps) {
            rev_x[step_cnt] = cur % W;
            rev_y[step_cnt] = cur / W;
            step_cnt++;
            cur = parent[cur];
        }
        for (int i = 0; i < step_cnt; i++) {
            out_tx[i] = rev_x[step_cnt - 1 - i];
            out_ty[i] = rev_y[step_cnt - 1 - i];
        }
    }

    free(g_score); free(f_score); free(parent); free(state); free(heap); free(rev_x); free(rev_y);
    return step_cnt;
}

int32_t pal_map_probe_m2(const char *game_dir, int32_t map_id, PalMapM2ProbeInfo *out_info) {
    if (!game_dir || !out_info || map_id <= 0) return -1;
    memset(out_info, 0, sizeof(*out_info));
    out_info->map_id = map_id;

    PalLoadedMap m = {0};
    if (vfs_load_lmf_map_by_id(game_dir, (uint32_t)map_id, NULL, SDL_SCALEMODE_NEAREST, &m) != 0) {
        return -1;
    }

    out_info->width_tiles = m.w_tiles;
    out_info->height_tiles = m.h_tiles;
    out_info->total_cells = m.w_tiles * m.h_tiles;
    out_info->walkable_cells = m.walkable_count;
    out_info->blocked_cells = m.blocked_count;
    out_info->trigger_count = m.trigger_count;
    out_info->fg_sprite_defs = m.fg_def_count;
    out_info->fg_sprite_instances = m.fg_inst_count;
    out_info->npc_count = m.npc_count;
    out_info->step_speed_px = m.step_speed_px;
    snprintf(out_info->map_title_utf8, sizeof(out_info->map_title_utf8), "%s", m.title_utf8);
    snprintf(out_info->bgm_track, sizeof(out_info->bgm_track), "%s", m.bgm_track);

    /* Anchor near playable center (inside isometric room bounds) and pick farthest reachable goal in the same component */
    int sx = m.w_tiles > 2 ? m.w_tiles / 2 : 1;
    int sy = m.h_tiles > 2 ? m.h_tiles / 2 : 1;
    int max_r = m.w_tiles > m.h_tiles ? m.w_tiles : m.h_tiles;
    if (pal_map_nearest_walkable(&m, &sx, &sy, 2, max_r) && m.w_tiles > 0 && m.h_tiles > 0) {
        int N = m.w_tiles * m.h_tiles;
        uint8_t *vis = (uint8_t *)calloc((size_t)N, 1);
        int32_t *q = (int32_t *)malloc((size_t)N * sizeof(int32_t));
        int gx = sx, gy = sy, best_dist = 0;
        if (vis && q) {
            int qh = 0, qt = 0;
            int sidx = sy * m.w_tiles + sx;
            vis[sidx] = 1;
            q[qt++] = sidx;
            static const int kBDx[4] = {0, 0, -1, 1};
            static const int kBDy[4] = {-1, 1, 0, 0};
            while (qh < qt) {
                int cur = q[qh++];
                int cx = cur % m.w_tiles, cy = cur / m.w_tiles;
                int dist = abs(cx - sx) + abs(cy - sy);
                if (dist > best_dist) {
                    best_dist = dist;
                    gx = cx;
                    gy = cy;
                }
                for (int d = 0; d < 4; d++) {
                    int nx = cx + kBDx[d], ny = cy + kBDy[d];
                    if (!pal_map_can_stand_tile(&m, nx, ny, 2)) continue;
                    int nidx = ny * m.w_tiles + nx;
                    if (!vis[nidx]) {
                        vis[nidx] = 1;
                        q[qt++] = nidx;
                    }
                }
            }
        }
        free(vis);
        free(q);
        if (best_dist >= 4) {
            int path_tx[512], path_ty[512];
            out_info->astar_path_steps = pal_map_find_path_astar(&m, sx, sy, gx, gy, 2, path_tx, path_ty, 512);
        }
    }

    free_loaded_map(&m);
    return 0;
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
    } else if (fr_id < 12000) {
        *out_tsw_file = "all_sys.tsw";
        *out_slot_id = (uint32_t)(fr_id - 9000);
    } else if (fr_id < 15000) {
        *out_tsw_file = "all_map1.tsw";
        *out_slot_id = (uint32_t)(fr_id - 12000);
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
            } else if ((tag == 'D' || tag == 'd' || tag == 'G' || tag == 'g') && p[2] >= '0' && p[2] <= '9') {
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
    if (!app_quit) {
        SDL_SetRenderTarget(renderer, NULL);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
    }
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

static int run_fade_transition(
    SDL_Window *window, SDL_Renderer *renderer, SDL_Texture *scene_rt,
    int fade_in, Uint32 duration_ms, PalBgmPlayer *bgm
) {
    SDL_SetRenderTarget(renderer, NULL);
    Uint64 start = SDL_GetTicksNS();
    for (;;) {
        if (bgm) pal_bgm_update(bgm);
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
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (scene_rt) {
            SDL_RenderTexture(renderer, scene_rt, NULL, NULL);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
        SDL_FRect full = {0.0f, 0.0f, 640.0f, 480.0f};
        SDL_RenderFillRect(renderer, &full);
        SDL_RenderPresent(renderer);
        SDL_Delay(8);
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (fade_in && scene_rt) {
        SDL_RenderTexture(renderer, scene_rt, NULL, NULL);
    }
    SDL_RenderPresent(renderer);
    return 0;
}

static int find_lmf_index_by_id(const char *game_dir, const char *lmf_name, uint32_t target_map_id) {
    char path[1200];
    join_path(path, sizeof(path), game_dir, lmf_name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t fsize = get_file_size_fp(f);

    PalLmfEntry *entries = NULL;
    int cnt = 0;
    int found = -1;
    if (read_lmf_directory(f, fsize, &entries, &cnt) == 0 && cnt > 0) {
        for (int i = 0; i < cnt; i++) {
            if (entries[i].id == target_map_id) {
                found = i;
                break;
            }
        }
    }
    free(entries);
    fclose(f);
    return found;
}

/* Returns 0 on completion or user skip, 1 if app quit / window closed */
static int play_lmf_splash_by_id(
    SDL_Window *window, SDL_Renderer *renderer, const char *game_dir,
    uint32_t target_map_id, SDL_ScaleMode scale_mode, Uint32 fade_ms, Uint32 hold_ms
) {
    int idx = find_lmf_index_by_id(game_dir, "huge.lmf", target_map_id);
    if (idx < 0) return 0;

    PalLoadedMap splash = {0};
    if (vfs_load_lmf_map(game_dir, "huge.lmf", idx, renderer, scale_mode, &splash) != 0 || !splash.texture) {
        free_loaded_map(&splash);
        return 0;
    }

    Uint64 start_ns = SDL_GetTicksNS();
    double total_ms = (double)fade_ms * 2.0 + (double)hold_ms;
    int app_quit = 0;
    int skip = 0;

    while (!app_quit && !skip) {
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
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                skip = 1;
                break;
            }
        }
        if (app_quit || skip) break;

        double el_ms = (double)(SDL_GetTicksNS() - start_ns) / 1e6;
        if (el_ms >= total_ms) break;

        Uint8 fade_alpha = 0;
        if (fade_ms > 0 && el_ms < (double)fade_ms) {
            double t = el_ms / (double)fade_ms;
            fade_alpha = (Uint8)((1.0 - t) * 255.0);
        } else if (fade_ms > 0 && el_ms > (double)fade_ms + (double)hold_ms) {
            double t = (el_ms - (double)fade_ms - (double)hold_ms) / (double)fade_ms;
            if (t > 1.0) t = 1.0;
            fade_alpha = (Uint8)(t * 255.0);
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_FRect dst = {0.0f, 0.0f, (float)splash.w_px, (float)splash.h_px};
        SDL_RenderTexture(renderer, splash.texture, NULL, &dst);
        if (fade_alpha > 0) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, fade_alpha);
            SDL_FRect full = {0.0f, 0.0f, 640.0f, 480.0f};
            SDL_RenderFillRect(renderer, &full);
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(8);
    }

    free_loaded_map(&splash);
    if (!app_quit) {
        SDL_SetRenderTarget(renderer, NULL);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
    }
    return app_quit ? 1 : 0;
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

#define PAL_TSW_CACHE_CAP 256

typedef struct {
    uint32_t fr_id;
    PalLoadedSprite sprite;
} PalTswCacheEntry;

typedef struct {
    PalTswCacheEntry items[PAL_TSW_CACHE_CAP];
    int count;
} PalTswCache;

static PalLoadedSprite *tsw_cache_get(
    PalTswCache *cache, const char *game_dir, SDL_Renderer *renderer,
    SDL_ScaleMode scale_mode, int32_t fr_id
) {
    if (!cache || fr_id <= 0) return NULL;
    for (int i = 0; i < cache->count; i++) {
        if (cache->items[i].fr_id == (uint32_t)fr_id) {
            return &cache->items[i].sprite;
        }
    }
    const char *arch = NULL;
    uint32_t tid = 0;
    if (resolve_tsw_from_fr(fr_id, &arch, &tid) != 0) return NULL;
    PalLoadedSprite sp = {0};
    if (vfs_load_tsw_by_id(game_dir, arch, tid, renderer, scale_mode, &sp) != 0) return NULL;
    if (cache->count >= PAL_TSW_CACHE_CAP) {
        free_loaded_sprite(&cache->items[0].sprite);
        memmove(&cache->items[0], &cache->items[1], sizeof(PalTswCacheEntry) * (PAL_TSW_CACHE_CAP - 1));
        cache->count--;
    }
    PalTswCacheEntry *e = &cache->items[cache->count++];
    e->fr_id = (uint32_t)fr_id;
    e->sprite = sp;
    return &e->sprite;
}

static void free_tsw_cache(PalTswCache *cache) {
    if (!cache) return;
    for (int i = 0; i < cache->count; i++) {
        free_loaded_sprite(&cache->items[i].sprite);
    }
    memset(cache, 0, sizeof(*cache));
}

static int resolve_act_file_for_id(int32_t act_id, const char **out_act_file, uint32_t *out_slot_id) {
    if (act_id <= 0) return -1;
    *out_slot_id = (uint32_t)act_id;
    if (act_id < 3000) *out_act_file = "all_char.act";
    else if (act_id < 6000) *out_act_file = "all_item.act";
    else if (act_id < 9000) *out_act_file = "all_magic.act";
    else if (act_id < 13000) *out_act_file = "all_sys.act";
    else if (act_id < 15000) *out_act_file = "all_map1.act";
    else *out_act_file = "all_map2.act";
    return 0;
}

typedef struct {
    int active;
    uint32_t act_id;
    uint16_t obj_id;
    uint16_t talk_id;
    uint16_t npc_flags;
    uint16_t orig_sub_state;
    int sub_action;
    int dir;          /* 0=U, 1=D, 2=L, 3=R, 4=UL, 5=DR, 6=DL, 7=UR */
    int action_base;  /* 0=stand, 8=walk, 16=run */
    int px;
    int py;
    int walk_target_px;
    int walk_target_py;
    int script_moving;
    PalActVM vm;
    int vm_ok;
} PalFieldActor;

static void field_actor_set_sub(PalFieldActor *a, const char *game_dir, uint32_t act_id, int sub_action, int loop) {
    if (!a) return;
    if (a->vm_ok && a->act_id == act_id && a->sub_action == sub_action) return;
    const char *act_file = NULL;
    uint32_t sid = 0;
    if (resolve_act_file_for_id((int32_t)act_id, &act_file, &sid) != 0) return;

    PalActVM next_vm;
    if (pal_act_vm_init(game_dir, act_file, sid, sub_action, &next_vm) != 0) {
        /* Fallback to sub 8+dir or 0+dir if 16+dir absent */
        int fb = sub_action >= 16 ? (8 + (sub_action - 16)) : 0;
        if (pal_act_vm_init(game_dir, act_file, sid, fb, &next_vm) != 0) return;
        sub_action = fb;
    }
    if (loop) next_vm.loop_remaining = -1;
    a->vm = next_vm;
    a->act_id = act_id;
    a->sub_action = sub_action;
    a->vm_ok = 1;
    a->active = 1;
}

static int dir_from_delta(int dx, int dy, int prev_dir) {
    if (dx == 0 && dy < 0) return 0; /* Up */
    if (dx == 0 && dy > 0) return 1; /* Down */
    if (dx < 0 && dy == 0) return 2; /* Left */
    if (dx > 0 && dy == 0) return 3; /* Right */
    if (dx < 0 && dy < 0) return 4;  /* Up-Left */
    if (dx > 0 && dy > 0) return 5;  /* Down-Right */
    if (dx < 0 && dy > 0) return 6;  /* Down-Left */
    if (dx > 0 && dy < 0) return 7;  /* Up-Right */
    return prev_dir;
}

/* =========================================================================
 * M3 Module: Multi-Bank talk1..8.dat Loader & Talk.cpp Bytecode VM (0x401eb5)
 * ========================================================================= */

#define PAL_STORY_FLAG_CAP  8192
#define PAL_SCRIPT_VAR_CAP  256
#define PAL_OBJ_CAP         8192
#define PAL_INVENTORY_CAP   256
#define PAL_MAX_PORTRAITS   4
#define PAL_MAX_CHOICES     8

static uint32_t talk_bank_slots(int bank) {
    return (bank == 1 || bank == 4) ? 701u : 1000u;
}

static int pal_load_talk_script_chunk(
    const char *game_dir, int32_t talk_id,
    uint8_t **out_chunk, uint32_t *out_len, uint32_t *out_payload_off
) {
    if (!game_dir || talk_id <= 0 || !out_chunk || !out_len || !out_payload_off) return -1;
    int bank = (talk_id / 1000) + 1;
    uint32_t slot = (uint32_t)(talk_id % 1000);
    if (bank < 1 || bank > 8) return -1;

    uint32_t n_slots = talk_bank_slots(bank);
    if (slot >= n_slots) return -1;

    char rel[32], full[1200];
    snprintf(rel, sizeof(rel), "talk%d.dat", bank);
    if (resolve_rel_path_ci(game_dir, rel, full, sizeof(full)) != 0) return -1;

    size_t flen = 0;
    uint8_t *buf = read_entire_file(full, &flen);
    if (!buf) return -1;

    if (flen < PAL_MZ_HEADER_SIZE + (size_t)n_slots * 4u || buf[0] != 'M' || buf[1] != 'Z') {
        free(buf);
        return -1;
    }
    const uint8_t *p = buf + PAL_MZ_HEADER_SIZE;
    uint32_t plen = (uint32_t)(flen - PAL_MZ_HEADER_SIZE);
    uint32_t data_start = n_slots * 4u;
    uint32_t off = rd_u32(p + (size_t)slot * 4u);
    if (off == 0xFFFFFFFFu || off < data_start || off >= plen) {
        free(buf);
        return -1;
    }

    uint32_t end = plen;
    for (uint32_t i = 0; i < n_slots; i++) {
        uint32_t o2 = rd_u32(p + (size_t)i * 4u);
        if (o2 != 0xFFFFFFFFu && o2 > off && o2 < end) {
            end = o2;
        }
    }
    if (end <= off) {
        free(buf);
        return -1;
    }

    uint32_t clen = end - off;
    uint8_t *chunk = (uint8_t *)malloc(clen);
    if (!chunk) {
        free(buf);
        return -1;
    }
    memcpy(chunk, p + off, clen);
    free(buf);

    *out_chunk = chunk;
    *out_len = clen;
    *out_payload_off = off;
    return 0;
}

static uint32_t talk_find_pct_q_end(const uint8_t *chunk, uint32_t chunk_len, uint32_t start_pos, uint32_t ip) {
    uint32_t p = start_pos;
    while (p + 1u < chunk_len) {
        if (chunk[p] == '%' && chunk[p + 1u] == 'Q') {
            return (p + 2u) - ip;
        }
        p++;
    }
    return 0u;
}

static uint32_t talk_fixed_insn_bytes(uint16_t op) {
    switch (op) {
        /* 2 bytes */
        case 0x07: case 0x09: case 0x13: case 0x25: case 0x2A: case 0x2B:
        case 0x33: case 0x35: case 0x3C: case 0x3D: case 0x40: case 0x4A:
        case 0x50: case 0x5E: case 0x5F: case 0x61: case 0x69: case 0x70:
        case 0x79: case 0x7A: case 0x7C: case 0x87: case 0x89: case 0x8F:
        case 0x93: case 0x94: case 0x95: case 0x98: case 0x9B: case 0x9C:
        case 0x9E: case 0x9F: case 0xA0: case 0xAF: case 0xB0: case 0xB1:
        case 0xB4: case 0xC0: case 0xC4: case 0xD2: case 0xD3: case 0xD4:
        case 0xD6: case 0xE9: case 0x312: case 0x400: case 0x401: case 0x402:
            return 2u;
        /* 4 bytes */
        case 0x08: case 0x0D: case 0x0E: case 0x12: case 0x19: case 0x1A:
        case 0x22: case 0x26: case 0x27: case 0x2E: case 0x2F: case 0x30:
        case 0x31: case 0x37: case 0x38: case 0x39: case 0x3B: case 0x41:
        case 0x43: case 0x44: case 0x45: case 0x48: case 0x4B: case 0x4E:
        case 0x58: case 0x5B: case 0x5C: case 0x5D: case 0x62: case 0x63:
        case 0x65: case 0x6A: case 0x73: case 0x76: case 0x77: case 0x8B:
        case 0x90: case 0x9A: case 0xA1: case 0xA2: case 0xB2: case 0xBF:
        case 0xCD: case 0xCE: case 0xD1: case 0xD7: case 0x309: case 0x30A:
        case 0x30D: case 0x311:
            return 4u;
        /* 6 bytes */
        case 0x0A: case 0x0B: case 0x0F: case 0x1C: case 0x1D: case 0x1E:
        case 0x1F: case 0x2C: case 0x2D: case 0x36: case 0x47: case 0x4C:
        case 0x4D: case 0x52: case 0x54: case 0x64: case 0x66: case 0x67:
        case 0x68: case 0x6B: case 0x6C: case 0x6E: case 0x6F: case 0x71:
        case 0x75: case 0x80: case 0x83: case 0x88: case 0x8C: case 0x8D:
        case 0x91: case 0x92: case 0x96: case 0xAE: case 0xCF: case 0xD0:
        case 0xD9: case 0xE1: case 0xE4: case 0xE5: case 0x30B: case 0x310:
            return 6u;
        /* 8 bytes */
        case 0x0C: case 0x10: case 0x11: case 0x15: case 0x16: case 0x23:
        case 0x24: case 0x28: case 0x49: case 0x72: case 0x81: case 0x82:
        case 0x84: case 0x8E: case 0x9D: case 0xA3: case 0xA4: case 0xA7:
        case 0xA8: case 0xB5: case 0xB6: case 0xB7: case 0xC3: case 0xC5:
        case 0xC6: case 0xC7: case 0xDF: case 0xE0: case 0xE3: case 0x30E:
        case 0x30F:
            return 8u;
        /* 10 bytes */
        case 0x20: case 0x21: case 0x32: case 0x3A: case 0x46: case 0x51:
        case 0x55: case 0x56: case 0x78: case 0x7B: case 0x7E: case 0x7F:
        case 0x86: case 0x97: case 0x99: case 0xA5: case 0xA6: case 0xB3:
        case 0xD8: case 0xDA: case 0xDC: case 0xE8:
            return 10u;
        /* 12 bytes */
        case 0xB8: case 0xB9: case 0xC1: case 0xC8: case 0xC9: case 0xCA:
        case 0xCC: case 0x30C:
            return 12u;
        /* 14 bytes */
        case 0x1B: case 0x8A: case 0xD5:
            return 14u;
        /* 16 bytes */
        case 0x34: case 0x42: case 0x4F: case 0x53: case 0xDD: case 0xDE:
        case 0xE2: case 0xE6: case 0xE7:
            return 16u;
        /* 18 bytes */
        case 0x3E:
            return 18u;
        default:
            return 0u;
    }
}

static uint32_t talk_dialogue_header_bytes(uint16_t op) {
    if (op == 0x01u || op == 0x02u) return 6u;
    if (op == 0x03u || op == 0x04u || op == 0x05u || op == 0x06u) return 14u;
    if (op == 0x59u || op == 0x5Au) return 10u;
    return 0u;
}

static uint32_t pal_talk_insn_length(const uint8_t *chunk, uint32_t chunk_len, uint32_t ip) {
    if (!chunk || ip + 2u > chunk_len) return 0u;
    uint16_t raw = rd_u16(chunk + ip);
    if (raw == 0xFFFFu) return 2u;

    uint16_t op = raw & 0x3FFFu;
    uint32_t dlg_hdr = talk_dialogue_header_bytes(op);
    if (dlg_hdr > 0u) {
        if (ip + dlg_hdr > chunk_len) return 0u;
        return talk_find_pct_q_end(chunk, chunk_len, ip + dlg_hdr, ip);
    }
    if (op == 0x7Du) {
        return talk_find_pct_q_end(chunk, chunk_len, ip + 2u, ip);
    }
    if (op == 0xAAu || op == 0xABu || op == 0xACu || op == 0xADu) {
        if ((raw & 1u) != 0u) {
            return talk_find_pct_q_end(chunk, chunk_len, ip + 2u, ip);
        }
        return 2u;
    }
    if (op == 0x60u || op == 0xC2u) {
        uint32_t p = ip + 2u;
        if (op == 0x60u) {
            if (p < chunk_len && chunk[p] == '%') p++;
            if (p < chunk_len && chunk[p] == '*') p++;
        }
        return talk_find_pct_q_end(chunk, chunk_len, p, ip);
    }
    if (op == 0x14u || op == 0xA9u) {
        if (ip + 4u > chunk_len) return 0u;
        uint32_t cnt = (uint32_t)(rd_u16(chunk + ip + 2u) & 0x3FFFu);
        uint32_t step = (op == 0x14u) ? 6u : 12u;
        uint32_t sz = 4u + cnt * step;
        return (ip + sz <= chunk_len) ? sz : 0u;
    }
    if (op == 0x17u || op == 0x18u) {
        uint32_t p = ip + 2u;
        int found = 0;
        while (p + 2u <= chunk_len) {
            uint16_t w = rd_u16(chunk + p);
            p += 2u;
            if (w == 0xFF00u) {
                found = 1;
                break;
            }
        }
        if (!found || p + 4u > chunk_len) return 0u;
        return (p + 4u) - ip;
    }
    if (op == 0x29u || op == 0x57u) {
        uint32_t p = ip + 2u;
        int found = 0;
        while (p + 4u <= chunk_len) {
            uint32_t t = rd_u32(chunk + p);
            p += 4u;
            if (t == 0xFF00FF00u) {
                found = 1;
                break;
            }
        }
        return found ? (p - ip) : 0u;
    }
    if (op == 0x3Fu) {
        if (ip + 4u > chunk_len) return 0u;
        uint32_t p = ip + 4u;
        int found = 0;
        while (p + 2u <= chunk_len) {
            uint16_t w = rd_u16(chunk + p);
            p += 2u;
            if (w == 0xFF00u) {
                found = 1;
                break;
            }
        }
        return found ? (p - ip) : 0u;
    }
    if (op == 0x6Du || op == 0x74u) {
        if (ip + 4u > chunk_len) return 0u;
        uint32_t cnt = (uint32_t)rd_u16(chunk + ip + 2u);
        uint32_t stride = (op == 0x6Du) ? 2u : 6u;
        uint32_t sz = 4u + cnt * stride;
        return (ip + sz <= chunk_len) ? sz : 0u;
    }
    if (op == 0x85u) {
        uint32_t p = ip + 2u;
        int found = 0;
        while (p + 2u <= chunk_len) {
            uint16_t w = rd_u16(chunk + p);
            p += 2u;
            if (w == 0u) {
                found = 1;
                break;
            }
        }
        return found ? (p - ip) : 0u;
    }
    if (op == 0xCBu) {
        if (ip + 6u > chunk_len) return 0u;
        uint32_t p = ip + 6u;
        int found = 0;
        while (p + 4u <= chunk_len) {
            uint32_t t = rd_u32(chunk + p);
            p += 4u;
            if (t == 0xFF00FF00u) {
                found = 1;
                break;
            }
        }
        return found ? (p - ip) : 0u;
    }

    uint32_t fixed = talk_fixed_insn_bytes(op);
    if (fixed == 0u || ip + fixed > chunk_len) return 0u;
    return fixed;
}

#define PAL_DB_MAX_HEROES    16
#define PAL_DB_MAX_ITEMS     384
#define PAL_DB_MAX_MAGICS    128
#define PAL_DB_MAX_LEVELS    512
#define PAL_DB_MAX_SCRIPTS   2048
#define PAL_SCRIPT_MAX_OPS   48
#define PAL_HERO_MAX_MAGICS  48
#define PAL_EQUIP_SLOT_COUNT 6

typedef struct {
    int32_t id;
    char name_utf8[48];
    int32_t start_lv;
    int32_t lv;
    int32_t next_exp;
    int32_t exp;
    int32_t max_hp;
    int32_t cur_hp;
    int32_t max_mp;
    int32_t cur_mp;
    int32_t atk;
    int32_t mag;
    int32_t def;
    int32_t spd;
    int32_t luck;
    int32_t res[6]; /* 59,65,71,77,83,89 */
    uint16_t init_equips[PAL_EQUIP_SLOT_COUNT];
    int32_t init_equip_count;
    uint16_t init_magics[PAL_HERO_MAX_MAGICS];
    int32_t init_magic_count;
} PalHeroRecord;

typedef struct {
    int32_t id;
    char name_utf8[64];
    char desc_utf8[256];
    int32_t field_script;  /* prop[1] */
    int32_t battle_script; /* prop[2] */
    int32_t equip_script;  /* prop[3] */
    int32_t target_flag;   /* prop[6] */
    int32_t hero_mask;     /* prop[8] */
    int32_t price;         /* prop[9] */
} PalItemRecord;

typedef struct {
    int32_t id;
    char name_utf8[64];
    char desc_utf8[256];
    int32_t elem_type;     /* prop[0] */
    int32_t field_script;  /* prop[1] */
    int32_t battle_script; /* prop[2] */
    int32_t mp_cost;       /* prop[5] */
    int32_t target_flag;   /* prop[6] */
    int32_t hero_mask;     /* prop[8] */
} PalMagicRecord;

typedef struct {
    int32_t id;            /* hero_id * 100 + target_lv */
    int32_t next_exp;      /* prop[10] */
    int32_t d_max_hp;      /* prop[16] */
    int32_t heal_hp;       /* prop[17] */
    int32_t d_max_mp;      /* prop[22] */
    int32_t heal_mp;       /* prop[23] */
    int32_t d_atk;         /* prop[29] */
    int32_t d_mag;         /* prop[35] */
    int32_t d_def;         /* prop[41] */
    int32_t d_spd;         /* prop[47] */
    int32_t d_luck;        /* prop[53] */
    uint16_t learn_magics[8];
    int32_t learn_magic_count;
} PalLevelTemplate;

typedef struct {
    uint32_t tag;          /* e.g. 0x7F30200B, 0x7F202041, 0x7F102042 */
    int32_t args[4];
    int32_t arg_count;
} PalScript7fOp;

typedef struct {
    int32_t id;
    PalScript7fOp ops[PAL_SCRIPT_MAX_OPS];
    int32_t op_count;
} PalScript7fRecord;

typedef struct {
    PalHeroRecord heroes[PAL_DB_MAX_HEROES];
    int32_t hero_count;
    PalItemRecord items[PAL_DB_MAX_ITEMS];
    int32_t item_count;
    PalMagicRecord magics[PAL_DB_MAX_MAGICS];
    int32_t magic_count;
    PalLevelTemplate levels[PAL_DB_MAX_LEVELS];
    int32_t level_count;
    PalScript7fRecord scripts[PAL_DB_MAX_SCRIPTS];
    int32_t script_count;
} PalGameDatabase;

typedef struct {
    int32_t active;
    int32_t hero_id;       /* 2000..2004, 2009 */
    char name_utf8[48];
    int32_t lv;
    int32_t exp;
    int32_t next_exp;
    int32_t cur_hp;
    int32_t cur_mp;
    /* Permanent base stats from ghero + glevel */
    int32_t base_max_hp;
    int32_t base_max_mp;
    int32_t base_atk;
    int32_t base_mag;
    int32_t base_def;
    int32_t base_spd;
    int32_t base_luck;
    int32_t base_res[6];
    /* Effective stats after equipment bonuses */
    int32_t eff_max_hp;
    int32_t eff_max_mp;
    int32_t eff_atk;
    int32_t eff_mag;
    int32_t eff_def;
    int32_t eff_spd;
    int32_t eff_luck;
    int32_t eff_res[6];
    uint16_t equips[PAL_EQUIP_SLOT_COUNT]; /* 0=Head, 1=Cloak, 2=Body, 3=Weapon, 4=Accessory, 5=Foot */
    uint16_t magics[PAL_HERO_MAX_MAGICS];
    int32_t magic_count;
} PalPartyMemberState;

typedef struct {
    uint8_t story_flags[PAL_STORY_FLAG_CAP];
    int32_t script_vars[PAL_SCRIPT_VAR_CAP];
    int16_t obj_rv[PAL_OBJ_CAP][4];
    uint8_t npc_visibility_override[PAL_OBJ_CAP]; /* 0=default, 1=shown, 2=hidden */
    uint16_t npc_talk_override[PAL_OBJ_CAP];      /* 0=default, >0=override talk_id */
    int32_t money;
    uint16_t inventory_ids[PAL_INVENTORY_CAP];
    int32_t inventory_qty[PAL_INVENTORY_CAP];
    int32_t inventory_count;
    PalGameDatabase *db;
    PalPartyMemberState party_members[4];
    int32_t party_member_count;
    int32_t active_shop_id;
    int32_t last_resolved_item_id;
} PalTalkWorldState;

static int pal_item_equip_slot(int32_t item_id) {
    if (item_id >= 3251 && item_id <= 3262) return 0; /* Headgear */
    if (item_id >= 3201 && item_id <= 3210) return 1; /* Cloak */
    if (item_id >= 3151 && item_id <= 3167) return 2; /* Body */
    if (item_id >= 3301 && item_id <= 3467) return 3; /* Weapon */
    if (item_id >= 3051 && item_id <= 3075) return 4; /* Accessory */
    if (item_id >= 3101 && item_id <= 3113) return 5; /* Footwear */
    return -1;
}

static int pal_hero_mask_bit(int32_t hero_id) {
    switch (hero_id) {
        case 2000: case 2009: return 0x80;
        case 2001: return 0x40;
        case 2002: return 0x20;
        case 2003: return 0x10;
        case 2004: return 0x08;
        default:   return 0xFF;
    }
}

static int pal_item_can_hero_equip(const PalItemRecord *it, int32_t hero_id) {
    if (!it || pal_item_equip_slot(it->id) < 0) return 0;
    if (it->hero_mask == 0 || it->hero_mask == 61439 || it->hero_mask == 65535) return 1;
    return (it->hero_mask & pal_hero_mask_bit(hero_id)) != 0 ? 1 : 0;
}

static const PalHeroRecord *pal_db_find_hero(const PalGameDatabase *db, int32_t hero_id) {
    if (!db || hero_id <= 0) return NULL;
    for (int32_t i = 0; i < db->hero_count; i++) {
        if (db->heroes[i].id == hero_id) return &db->heroes[i];
    }
    return NULL;
}

static const PalItemRecord *pal_db_find_item(const PalGameDatabase *db, int32_t item_id) {
    if (!db || item_id <= 0) return NULL;
    for (int32_t i = 0; i < db->item_count; i++) {
        if (db->items[i].id == item_id) return &db->items[i];
    }
    return NULL;
}

static const PalMagicRecord *pal_db_find_magic(const PalGameDatabase *db, int32_t magic_id) {
    if (!db || magic_id <= 0) return NULL;
    for (int32_t i = 0; i < db->magic_count; i++) {
        if (db->magics[i].id == magic_id) return &db->magics[i];
    }
    return NULL;
}

static const PalLevelTemplate *pal_db_find_level(const PalGameDatabase *db, int32_t level_id) {
    if (!db || level_id <= 0) return NULL;
    for (int32_t i = 0; i < db->level_count; i++) {
        if (db->levels[i].id == level_id) return &db->levels[i];
    }
    return NULL;
}

static const PalScript7fRecord *pal_db_find_script(const PalGameDatabase *db, int32_t script_id) {
    if (!db || script_id <= 0 || script_id == 61439 || script_id == 65535) return NULL;
    for (int32_t i = 0; i < db->script_count; i++) {
        if (db->scripts[i].id == script_id) return &db->scripts[i];
    }
    return NULL;
}

enum {
    PAL_DB_KIND_HERO  = 0,
    PAL_DB_KIND_ITEM  = 1,
    PAL_DB_KIND_MAGIC = 2,
    PAL_DB_KIND_LEVEL = 3
};

static void pal_db_apply_entity_prop(int kind, void *rec, int32_t pid, int32_t val) {
    if (kind == PAL_DB_KIND_HERO) {
        PalHeroRecord *h = (PalHeroRecord *)rec;
        switch (pid) {
            case 4:  h->start_lv = val; break;
            case 5:  h->lv = val; break;
            case 10: h->next_exp = val; break;
            case 11: h->exp = val; break;
            case 16: h->max_hp = val; break;
            case 17: h->cur_hp = val; break;
            case 22: h->max_mp = val; break;
            case 23: h->cur_mp = val; break;
            case 29: h->atk = val; break;
            case 35: h->mag = val; break;
            case 41: h->def = val; break;
            case 47: h->spd = val; break;
            case 53: h->luck = val; break;
            case 59: h->res[0] = val; break;
            case 65: h->res[1] = val; break;
            case 71: h->res[2] = val; break;
            case 77: h->res[3] = val; break;
            case 83: h->res[4] = val; break;
            case 89: h->res[5] = val; break;
            case 221:
                if (val > 0 && h->init_equip_count < PAL_EQUIP_SLOT_COUNT) {
                    h->init_equips[h->init_equip_count++] = (uint16_t)val;
                }
                break;
            case 220:
                if (val > 0 && h->init_magic_count < PAL_HERO_MAX_MAGICS) {
                    h->init_magics[h->init_magic_count++] = (uint16_t)val;
                }
                break;
            default: break;
        }
    } else if (kind == PAL_DB_KIND_ITEM) {
        PalItemRecord *it = (PalItemRecord *)rec;
        switch (pid) {
            case 1: it->field_script = val; break;
            case 2: it->battle_script = val; break;
            case 3: it->equip_script = val; break;
            case 6: it->target_flag = val; break;
            case 8: it->hero_mask = val; break;
            case 9: it->price = val; break;
            default: break;
        }
    } else if (kind == PAL_DB_KIND_MAGIC) {
        PalMagicRecord *mg = (PalMagicRecord *)rec;
        switch (pid) {
            case 0: mg->elem_type = val; break;
            case 1: mg->field_script = val; break;
            case 2: mg->battle_script = val; break;
            case 5: mg->mp_cost = val; break;
            case 6: mg->target_flag = val; break;
            case 8: mg->hero_mask = val; break;
            default: break;
        }
    } else if (kind == PAL_DB_KIND_LEVEL) {
        PalLevelTemplate *lv = (PalLevelTemplate *)rec;
        switch (pid) {
            case 10: lv->next_exp = val; break;
            case 16: lv->d_max_hp = val; break;
            case 17: lv->heal_hp = val; break;
            case 22: lv->d_max_mp = val; break;
            case 23: lv->heal_mp = val; break;
            case 29: lv->d_atk = val; break;
            case 35: lv->d_mag = val; break;
            case 41: lv->d_def = val; break;
            case 47: lv->d_spd = val; break;
            case 53: lv->d_luck = val; break;
            case 220:
                if (val > 0 && lv->learn_magic_count < 8) {
                    lv->learn_magics[lv->learn_magic_count++] = (uint16_t)val;
                }
                break;
            default: break;
        }
    }
}

static int pal_db_load_file(const char *game_dir, const char *filename, int kind, PalGameDatabase *db) {
    char path[1200];
    if (resolve_rel_path_ci(game_dir, filename, path, sizeof(path)) != 0) return -1;
    size_t sz = 0;
    uint8_t *buf = read_entire_file(path, &sz);
    if (!buf) return -1;
    if (sz < PAL_MZ_HEADER_SIZE + 4 || buf[0] != 'M' || buf[1] != 'Z') {
        free(buf);
        return -1;
    }
    const uint8_t *p = buf + PAL_MZ_HEADER_SIZE;
    size_t plen = sz - PAL_MZ_HEADER_SIZE;
    size_t pos = 0;

    typedef union {
        PalHeroRecord hero;
        PalItemRecord item;
        PalMagicRecord magic;
        PalLevelTemplate level;
    } CurEnt;
    CurEnt cur_ent;
    PalScript7fRecord cur_scr;
    memset(&cur_ent, 0, sizeof(cur_ent));
    memset(&cur_scr, 0, sizeof(cur_scr));
    int rec_kind = -1; /* -1=empty, 0=entity, 1=script */
    int32_t ent_id = 0;

    while (pos + 4 <= plen) {
        uint32_t tag = rd_u32(p + pos);
        pos += 4;
        if (tag == 0x7F00FFFFu) {
            if (rec_kind == 0 && ent_id != 0) {
                if (kind == PAL_DB_KIND_HERO && db->hero_count < PAL_DB_MAX_HEROES) {
                    cur_ent.hero.id = ent_id;
                    db->heroes[db->hero_count++] = cur_ent.hero;
                } else if (kind == PAL_DB_KIND_ITEM && db->item_count < PAL_DB_MAX_ITEMS) {
                    cur_ent.item.id = ent_id;
                    db->items[db->item_count++] = cur_ent.item;
                } else if (kind == PAL_DB_KIND_MAGIC && db->magic_count < PAL_DB_MAX_MAGICS) {
                    cur_ent.magic.id = ent_id;
                    db->magics[db->magic_count++] = cur_ent.magic;
                } else if (kind == PAL_DB_KIND_LEVEL && db->level_count < PAL_DB_MAX_LEVELS) {
                    cur_ent.level.id = ent_id;
                    db->levels[db->level_count++] = cur_ent.level;
                }
            } else if (rec_kind == 1 && cur_scr.id != 0 && db->script_count < PAL_DB_MAX_SCRIPTS) {
                if (!pal_db_find_script(db, cur_scr.id)) {
                    db->scripts[db->script_count++] = cur_scr;
                }
            }
            memset(&cur_ent, 0, sizeof(cur_ent));
            memset(&cur_scr, 0, sizeof(cur_scr));
            rec_kind = -1;
            ent_id = 0;
            continue;
        }
        if ((tag >> 24) != 0x7Fu) {
            free(buf);
            return -1;
        }

        int is_str = ((tag & 0xFFFFu) >= 0xF000u) && (((tag & 0xF0000u) >> 16) != 0);
        if (is_str) {
            const uint8_t *end = NULL;
            for (size_t k = pos; k + 1 < plen; k++) {
                if (p[k] == '%' && p[k + 1] == '%') {
                    end = p + k;
                    break;
                }
            }
            if (!end) {
                free(buf);
                return -1;
            }
            size_t slen = (size_t)(end - (p + pos));
            if (tag == 0x7FF2F000u) {
                if (kind == PAL_DB_KIND_HERO) cp950_to_utf8(p + pos, slen, cur_ent.hero.name_utf8, sizeof(cur_ent.hero.name_utf8));
                else if (kind == PAL_DB_KIND_ITEM) cp950_to_utf8(p + pos, slen, cur_ent.item.name_utf8, sizeof(cur_ent.item.name_utf8));
                else if (kind == PAL_DB_KIND_MAGIC) cp950_to_utf8(p + pos, slen, cur_ent.magic.name_utf8, sizeof(cur_ent.magic.name_utf8));
            } else if (tag == 0x7FF2F001u) {
                if (kind == PAL_DB_KIND_ITEM) cp950_to_utf8(p + pos, slen, cur_ent.item.desc_utf8, sizeof(cur_ent.item.desc_utf8));
                else if (kind == PAL_DB_KIND_MAGIC) cp950_to_utf8(p + pos, slen, cur_ent.magic.desc_utf8, sizeof(cur_ent.magic.desc_utf8));
            }
            pos = (size_t)(end - p) + 2;
        } else {
            int32_t scalars[8] = {0};
            int n_sc = 0;
            while (pos + 4 <= plen) {
                uint32_t u = rd_u32(p + pos);
                if ((u >> 24) == 0x7Fu) break;
                if (n_sc < 8) scalars[n_sc] = (int32_t)u;
                n_sc++;
                pos += 4;
            }
            if ((tag == 0x7F101000u || tag == 0x7F100000u) && n_sc >= 1) {
                rec_kind = 0;
                ent_id = scalars[0];
            } else if (tag == 0x7F102000u && n_sc >= 1) {
                rec_kind = 1;
                cur_scr.id = scalars[0];
            } else if ((tag == 0x7F201001u || tag == 0x7F200001u) && n_sc == 2) {
                if (rec_kind < 0) rec_kind = 0;
                pal_db_apply_entity_prop(kind, &cur_ent, scalars[0], scalars[1]);
            } else if ((tag == 0x7F30200Bu || tag == 0x7F202041u || tag == 0x7F102042u) &&
                       cur_scr.op_count < PAL_SCRIPT_MAX_OPS) {
                PalScript7fOp *op = &cur_scr.ops[cur_scr.op_count++];
                op->tag = tag;
                op->arg_count = n_sc < 4 ? n_sc : 4;
                for (int i = 0; i < op->arg_count; i++) op->args[i] = scalars[i];
            }
        }
    }
    free(buf);
    return 0;
}

static int pal_db_load(const char *game_dir, PalGameDatabase *db) {
    if (!game_dir || !db) return -1;
    memset(db, 0, sizeof(*db));
    if (pal_db_load_file(game_dir, "ghero.dat", PAL_DB_KIND_HERO, db) != 0) return -1;
    if (pal_db_load_file(game_dir, "gmagic.dat", PAL_DB_KIND_MAGIC, db) != 0) return -1;
    if (pal_db_load_file(game_dir, "gitem.dat", PAL_DB_KIND_ITEM, db) != 0) return -1;
    if (pal_db_load_file(game_dir, "glevel.dat", PAL_DB_KIND_LEVEL, db) != 0) return -1;
    return 0;
}

static void pal_db_free(PalGameDatabase *db) {
    if (!db) return;
    memset(db, 0, sizeof(*db));
}

static int pal_world_has_item(const PalTalkWorldState *ws, uint16_t item_id) {
    if (!ws || item_id == 0u) return 0;
    for (int32_t i = 0; i < ws->inventory_count; i++) {
        if (ws->inventory_ids[i] == item_id && ws->inventory_qty[i] > 0) return 1;
    }
    for (int32_t p = 0; p < ws->party_member_count && p < 4; p++) {
        if (!ws->party_members[p].active) continue;
        for (int s = 0; s < PAL_EQUIP_SLOT_COUNT; s++) {
            if (ws->party_members[p].equips[s] == item_id) return 1;
        }
    }
    return 0;
}

static void pal_world_add_item(PalTalkWorldState *ws, uint16_t item_id, int32_t delta) {
    if (!ws || item_id == 0u || delta == 0) return;
    for (int32_t i = 0; i < ws->inventory_count; i++) {
        if (ws->inventory_ids[i] == item_id) {
            ws->inventory_qty[i] += delta;
            if (ws->inventory_qty[i] <= 0) {
                memmove(&ws->inventory_ids[i], &ws->inventory_ids[i + 1],
                        sizeof(uint16_t) * (size_t)(ws->inventory_count - 1 - i));
                memmove(&ws->inventory_qty[i], &ws->inventory_qty[i + 1],
                        sizeof(int32_t) * (size_t)(ws->inventory_count - 1 - i));
                ws->inventory_count--;
            }
            return;
        }
    }
    if (delta > 0 && ws->inventory_count < PAL_INVENTORY_CAP) {
        ws->inventory_ids[ws->inventory_count] = item_id;
        ws->inventory_qty[ws->inventory_count] = delta;
        ws->inventory_count++;
    }
}

static void pal_party_recalc_member(const PalGameDatabase *db, PalPartyMemberState *m) {
    if (!m || !m->active) return;
    m->eff_max_hp = m->base_max_hp;
    m->eff_max_mp = m->base_max_mp;
    m->eff_atk    = m->base_atk;
    m->eff_mag    = m->base_mag;
    m->eff_def    = m->base_def;
    m->eff_spd    = m->base_spd;
    m->eff_luck   = m->base_luck;
    for (int i = 0; i < 6; i++) m->eff_res[i] = m->base_res[i];

    if (db) {
        for (int s = 0; s < PAL_EQUIP_SLOT_COUNT; s++) {
            if (m->equips[s] == 0u) continue;
            const PalItemRecord *it = pal_db_find_item(db, (int32_t)m->equips[s]);
            if (!it) continue;
            const PalScript7fRecord *sc = pal_db_find_script(db, it->equip_script);
            if (!sc) continue;
            for (int32_t oi = 0; oi < sc->op_count; oi++) {
                const PalScript7fOp *op = &sc->ops[oi];
                if (op->tag != 0x7F30200Bu || op->arg_count < 3) continue;
                int32_t pid = op->args[1];
                int32_t delta = op->args[2];
                switch (pid) {
                    case 16: m->eff_max_hp += delta; break;
                    case 22: m->eff_max_mp += delta; break;
                    case 26: case 29: m->eff_atk += delta; break;
                    case 32: case 35: m->eff_mag += delta; break;
                    case 38: case 41: m->eff_def += delta; break;
                    case 44: case 47: m->eff_spd += delta; break;
                    case 50: case 53: m->eff_luck += delta; break;
                    case 56: case 59: m->eff_res[0] += delta; break;
                    case 62: case 65: m->eff_res[1] += delta; break;
                    case 68: case 71: m->eff_res[2] += delta; break;
                    case 74: case 77: m->eff_res[3] += delta; break;
                    case 80: case 83: m->eff_res[4] += delta; break;
                    case 86: case 89: m->eff_res[5] += delta; break;
                    default: break;
                }
            }
        }
    }
    if (m->eff_max_hp < 1) m->eff_max_hp = 1;
    if (m->eff_max_mp < 0) m->eff_max_mp = 0;
    if (m->cur_hp > m->eff_max_hp) m->cur_hp = m->eff_max_hp;
    if (m->cur_mp > m->eff_max_mp) m->cur_mp = m->eff_max_mp;
}

static void pal_party_teach_magic(PalPartyMemberState *m, uint16_t magic_id) {
    if (!m || !m->active || magic_id == 0u) return;
    for (int32_t i = 0; i < m->magic_count; i++) {
        if (m->magics[i] == magic_id) return;
    }
    if (m->magic_count < PAL_HERO_MAX_MAGICS) {
        m->magics[m->magic_count++] = magic_id;
    }
}

static void pal_party_forget_magic(PalPartyMemberState *m, uint16_t magic_id) {
    if (!m || !m->active || magic_id == 0u) return;
    for (int32_t i = 0; i < m->magic_count; i++) {
        if (m->magics[i] == magic_id) {
            memmove(&m->magics[i], &m->magics[i + 1],
                    sizeof(uint16_t) * (size_t)(m->magic_count - 1 - i));
            m->magic_count--;
            return;
        }
    }
}

static int pal_world_find_party_member(const PalTalkWorldState *ws, int32_t hero_id) {
    if (!ws) return -1;
    for (int32_t i = 0; i < ws->party_member_count && i < 4; i++) {
        if (ws->party_members[i].active && ws->party_members[i].hero_id == hero_id) return i;
    }
    return -1;
}

static void pal_party_init_from_db(PalTalkWorldState *ws, PalGameDatabase *db, int preset) {
    if (!ws || !db) return;
    ws->db = db;
    memset(ws->party_members, 0, sizeof(ws->party_members));
    static const int32_t kChap1Heroes[4] = {2000, 2001, 2002, 2003};
    static const int32_t kDreamHeroes[4] = {2009, 2001, 2002, 2003};
    const int32_t *ids = (preset == 1) ? kDreamHeroes : kChap1Heroes;
    ws->party_member_count = 0;
    for (int i = 0; i < 4; i++) {
        const PalHeroRecord *hr = pal_db_find_hero(db, ids[i]);
        if (!hr) continue;
        PalPartyMemberState *m = &ws->party_members[ws->party_member_count++];
        memset(m, 0, sizeof(*m));
        m->active = 1;
        m->hero_id = hr->id;
        snprintf(m->name_utf8, sizeof(m->name_utf8), "%s", hr->name_utf8);
        m->lv = hr->lv > 0 ? hr->lv : (hr->start_lv > 0 ? hr->start_lv : 1);
        m->exp = hr->exp;
        m->next_exp = hr->next_exp > 0 ? hr->next_exp : 40;
        m->base_max_hp = hr->max_hp;
        m->base_max_mp = hr->max_mp;
        m->cur_hp = hr->cur_hp > 0 ? hr->cur_hp : hr->max_hp;
        m->cur_mp = hr->cur_mp >= 0 ? hr->cur_mp : hr->max_mp;
        m->base_atk = hr->atk;
        m->base_mag = hr->mag;
        m->base_def = hr->def;
        m->base_spd = hr->spd;
        m->base_luck = hr->luck;
        for (int r = 0; r < 6; r++) m->base_res[r] = hr->res[r];
        for (int32_t e = 0; e < hr->init_equip_count; e++) {
            uint16_t eq_id = hr->init_equips[e];
            int slot = pal_item_equip_slot((int32_t)eq_id);
            if (slot >= 0 && slot < PAL_EQUIP_SLOT_COUNT) {
                m->equips[slot] = eq_id;
            }
        }
        for (int32_t mg = 0; mg < hr->init_magic_count; mg++) {
            pal_party_teach_magic(m, hr->init_magics[mg]);
        }
        pal_party_recalc_member(db, m);
    }
    if (ws->inventory_count == 0) {
        ws->money = 500;
        pal_world_add_item(ws, 4401u, 3); /* 止血草 */
        pal_world_add_item(ws, 4402u, 2); /* 行軍丹 */
        pal_world_add_item(ws, 4301u, 2); /* 鼠兒果 */
        pal_world_add_item(ws, 3254u, 1); /* 皮帽 */
        pal_world_add_item(ws, 3452u, 1); /* 鐵劍 */
    }
}

static int pal_party_equip_item(PalTalkWorldState *ws, int member_idx, uint16_t new_item_id, char *out_msg, size_t msg_cap) {
    if (!ws || !ws->db || member_idx < 0 || member_idx >= ws->party_member_count) return -1;
    PalPartyMemberState *m = &ws->party_members[member_idx];
    if (!m->active) return -1;
    const PalItemRecord *it = pal_db_find_item(ws->db, (int32_t)new_item_id);
    int slot = pal_item_equip_slot((int32_t)new_item_id);
    if (!it || slot < 0) {
        if (out_msg) snprintf(out_msg, msg_cap, "此物品無法裝備。");
        return -1;
    }
    if (!pal_item_can_hero_equip(it, m->hero_id)) {
        if (out_msg) snprintf(out_msg, msg_cap, "%s 無法裝備「%s」。", m->name_utf8, it->name_utf8);
        return -1;
    }
    int in_bag = 0;
    for (int32_t i = 0; i < ws->inventory_count; i++) {
        if (ws->inventory_ids[i] == new_item_id && ws->inventory_qty[i] > 0) {
            in_bag = 1;
            break;
        }
    }
    if (!in_bag) {
        if (out_msg) snprintf(out_msg, msg_cap, "背包中沒有「%s」。", it->name_utf8);
        return -1;
    }
    uint16_t old_id = m->equips[slot];
    if (old_id == new_item_id) {
        if (out_msg) snprintf(out_msg, msg_cap, "%s 已裝備「%s」。", m->name_utf8, it->name_utf8);
        return 0;
    }
    pal_world_add_item(ws, new_item_id, -1);
    if (old_id != 0u) pal_world_add_item(ws, old_id, 1);
    m->equips[slot] = new_item_id;
    pal_party_recalc_member(ws->db, m);
    if (out_msg) snprintf(out_msg, msg_cap, "%s 已裝備「%s」。", m->name_utf8, it->name_utf8);
    return 0;
}

static int pal_party_unequip_item(PalTalkWorldState *ws, int member_idx, uint16_t item_id, int return_to_bag) {
    if (!ws || !ws->db || member_idx < 0 || member_idx >= ws->party_member_count) return -1;
    PalPartyMemberState *m = &ws->party_members[member_idx];
    if (!m->active) return -1;
    for (int s = 0; s < PAL_EQUIP_SLOT_COUNT; s++) {
        if (m->equips[s] != 0u && (item_id == 0u || m->equips[s] == item_id)) {
            uint16_t removed = m->equips[s];
            m->equips[s] = 0u;
            if (return_to_bag) pal_world_add_item(ws, removed, 1);
            pal_party_recalc_member(ws->db, m);
            return 0;
        }
    }
    return -1;
}

static void pal_party_apply_hp_mp_delta(PalPartyMemberState *m, int32_t d_hp, int32_t d_mp) {
    if (!m || !m->active) return;
    if (d_hp != 0) {
        m->cur_hp += d_hp;
        if (m->cur_hp < 0) m->cur_hp = 0;
        if (m->cur_hp > m->eff_max_hp) m->cur_hp = m->eff_max_hp;
    }
    if (d_mp != 0) {
        m->cur_mp += d_mp;
        if (m->cur_mp < 0) m->cur_mp = 0;
        if (m->cur_mp > m->eff_max_mp) m->cur_mp = m->eff_max_mp;
    }
}

static int pal_party_apply_field_script(PalTalkWorldState *ws, int target_idx, int32_t script_id) {
    if (!ws || !ws->db || target_idx < 0 || target_idx >= ws->party_member_count) return -1;
    const PalScript7fRecord *sc = pal_db_find_script(ws->db, script_id);
    if (!sc) return -1;
    int applied = 0;
    for (int32_t oi = 0; oi < sc->op_count; oi++) {
        const PalScript7fOp *op = &sc->ops[oi];
        if (op->tag != 0x7F30200Bu || op->arg_count < 3) continue;
        uint32_t tgt = (uint32_t)op->args[0];
        int32_t pid = op->args[1];
        int32_t delta = op->args[2];
        int is_all = (tgt == 0x7EFF1015u);
        int start = is_all ? 0 : target_idx;
        int end = is_all ? (ws->party_member_count - 1) : target_idx;
        for (int p = start; p <= end; p++) {
            PalPartyMemberState *m = &ws->party_members[p];
            if (!m->active) continue;
            if (pid == 17) {
                pal_party_apply_hp_mp_delta(m, delta, 0);
                applied = 1;
            } else if (pid == 23) {
                pal_party_apply_hp_mp_delta(m, 0, delta);
                applied = 1;
            } else if (pid == 16) {
                m->base_max_hp += delta;
                pal_party_recalc_member(ws->db, m);
                applied = 1;
            } else if (pid == 22) {
                m->base_max_mp += delta;
                pal_party_recalc_member(ws->db, m);
                applied = 1;
            }
        }
    }
    return applied ? 0 : -1;
}

static int pal_party_use_field_item(PalTalkWorldState *ws, int target_idx, uint16_t item_id, char *out_msg, size_t msg_cap) {
    if (!ws || !ws->db || target_idx < 0 || target_idx >= ws->party_member_count) return -1;
    const PalItemRecord *it = pal_db_find_item(ws->db, (int32_t)item_id);
    if (!it) return -1;
    if (pal_item_equip_slot((int32_t)item_id) >= 0) {
        return pal_party_equip_item(ws, target_idx, item_id, out_msg, msg_cap);
    }
    int qty = 0;
    for (int32_t i = 0; i < ws->inventory_count; i++) {
        if (ws->inventory_ids[i] == item_id) { qty = ws->inventory_qty[i]; break; }
    }
    if (qty <= 0) {
        if (out_msg) snprintf(out_msg, msg_cap, "「%s」數量不足。", it->name_utf8);
        return -1;
    }
    if (pal_party_apply_field_script(ws, target_idx, it->field_script) != 0) {
        if (out_msg) snprintf(out_msg, msg_cap, "「%s」無法在此時使用。", it->name_utf8);
        return -1;
    }
    pal_world_add_item(ws, item_id, -1);
    if (out_msg) snprintf(out_msg, msg_cap, "對 %s 使用了「%s」。", ws->party_members[target_idx].name_utf8, it->name_utf8);
    return 0;
}

static int pal_party_cast_field_magic(
    PalTalkWorldState *ws, int caster_idx, int target_idx, uint16_t magic_id, char *out_msg, size_t msg_cap
) {
    if (!ws || !ws->db || caster_idx < 0 || caster_idx >= ws->party_member_count ||
        target_idx < 0 || target_idx >= ws->party_member_count) return -1;
    PalPartyMemberState *caster = &ws->party_members[caster_idx];
    const PalMagicRecord *mg = pal_db_find_magic(ws->db, (int32_t)magic_id);
    if (!caster->active || !mg) return -1;
    int learned = 0;
    for (int32_t i = 0; i < caster->magic_count; i++) {
        if (caster->magics[i] == magic_id) { learned = 1; break; }
    }
    if (!learned) {
        if (out_msg) snprintf(out_msg, msg_cap, "%s 尚未習得「%s」。", caster->name_utf8, mg->name_utf8);
        return -1;
    }
    int32_t cost = mg->mp_cost > 0 ? mg->mp_cost : 0;
    if (caster->cur_mp < cost) {
        if (out_msg) snprintf(out_msg, msg_cap, "%s 的真氣不足 (%d/%d)。", caster->name_utf8, caster->cur_mp, cost);
        return -1;
    }
    if (pal_party_apply_field_script(ws, target_idx, mg->field_script) != 0) {
        if (out_msg) snprintf(out_msg, msg_cap, "「%s」僅限戰鬥中使用。", mg->name_utf8);
        return -1;
    }
    caster->cur_mp -= cost;
    if (out_msg) snprintf(out_msg, msg_cap, "%s 施展「%s」(消耗真氣 %d)。", caster->name_utf8, mg->name_utf8, cost);
    return 0;
}

static int pal_party_level_up_member(PalTalkWorldState *ws, int member_idx) {
    if (!ws || !ws->db || member_idx < 0 || member_idx >= ws->party_member_count) return -1;
    PalPartyMemberState *m = &ws->party_members[member_idx];
    if (!m->active) return -1;
    int32_t target_lv = m->lv + 1;
    int32_t tpl_id = m->hero_id * 100 + target_lv;
    const PalLevelTemplate *lt = pal_db_find_level(ws->db, tpl_id);
    if (!lt) return -1;
    m->lv = target_lv;
    if (lt->next_exp > 0) m->next_exp = lt->next_exp;
    m->base_max_hp += lt->d_max_hp;
    m->base_max_mp += lt->d_max_mp;
    m->base_atk    += lt->d_atk;
    m->base_mag    += lt->d_mag;
    m->base_def    += lt->d_def;
    m->base_spd    += lt->d_spd;
    m->base_luck   += lt->d_luck;
    for (int32_t i = 0; i < lt->learn_magic_count; i++) {
        pal_party_teach_magic(m, lt->learn_magics[i]);
    }
    pal_party_recalc_member(ws->db, m);
    if (lt->heal_hp >= 10000) m->cur_hp = m->eff_max_hp;
    else pal_party_apply_hp_mp_delta(m, lt->heal_hp, 0);
    if (lt->heal_mp >= 10000) m->cur_mp = m->eff_max_mp;
    else pal_party_apply_hp_mp_delta(m, 0, lt->heal_mp);
    return 0;
}

typedef struct {
    int active;
    uint16_t act_id;
    uint16_t sub_action;
    int anchor_x;
    int anchor_y;
    PalActVM vm;
    int vm_ok;
} PalTalkPortrait;

typedef struct {
    int active;
    int waiting_input;
    int wait_ticks;
    int32_t talk_id;
    uint16_t trigger_obj_id;
    uint16_t last_ref_obj_id;
    uint8_t *chunk;
    uint32_t chunk_len;
    uint32_t payload_off;
    uint32_t ip;

    int dlg_visible;
    int dlg_dirty;
    uint16_t dlg_mode;
    uint16_t dlg_obj_id;
    uint16_t dlg_color_id;
    int dlg_box_x;
    int dlg_box_y;
    char dlg_raw_utf8[1024];
    char dlg_clean_utf8[1024];
    int choice_count;
    int choice_selected;
    char choice_labels[PAL_MAX_CHOICES][128];

    PalTalkPortrait portraits[PAL_MAX_PORTRAITS];

    int32_t pending_map_id;
    int32_t pending_tx;
    int32_t pending_ty;
    int32_t pending_dir;
    int32_t pending_fade;
    int32_t pending_bgm;
    int32_t pending_sfx_id;
    int32_t pending_bink_id;
    char pending_bink_rel[128];

    int32_t exec_steps;
    int32_t dialogues_shown;
    int32_t portraits_shown;
    int32_t npc_talk_updates;
    int32_t last_item_given;
    char last_dialogue_utf8[128];
} PalTalkVM;

static void pal_talk_strip_rich_tags(const char *src, char *dst, size_t cap) {
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < cap; i++) {
        if (src[i] == '%') {
            if (src[i + 1] == 'N') {
                dst[out++] = '\n';
                i++;
                continue;
            }
            if ((src[i + 1] == 'C' || src[i + 1] == 'S' || src[i + 1] == 'D' || src[i + 1] == 'G') &&
                src[i + 2] >= '0' && src[i + 2] <= '9') {
                i += 2;
                continue;
            }
            if (src[i + 1] == 'K' || src[i + 1] == 'Q' || src[i + 1] == 'B') {
                i++;
                continue;
            }
        }
        dst[out++] = src[i];
    }
    dst[out] = '\0';
}

static void pal_talk_parse_choices(const char *raw_utf8, PalTalkVM *vm) {
    vm->choice_count = 0;
    vm->choice_selected = 0;
    memset(vm->choice_labels, 0, sizeof(vm->choice_labels));

    const char *first_b = strstr(raw_utf8, "%B");
    if (!first_b) {
        pal_talk_strip_rich_tags(raw_utf8, vm->dlg_clean_utf8, sizeof(vm->dlg_clean_utf8));
        snprintf(vm->dlg_raw_utf8, sizeof(vm->dlg_raw_utf8), "%s", raw_utf8);
        return;
    }

    size_t prompt_len = (size_t)(first_b - raw_utf8);
    if (prompt_len >= sizeof(vm->dlg_raw_utf8)) prompt_len = sizeof(vm->dlg_raw_utf8) - 1u;
    memcpy(vm->dlg_raw_utf8, raw_utf8, prompt_len);
    vm->dlg_raw_utf8[prompt_len] = '\0';
    pal_talk_strip_rich_tags(vm->dlg_raw_utf8, vm->dlg_clean_utf8, sizeof(vm->dlg_clean_utf8));

    const char *cur = first_b;
    while (cur && *cur && vm->choice_count < PAL_MAX_CHOICES) {
        if (cur[0] == '%' && cur[1] == 'B') cur += 2;
        const char *next_b = strstr(cur, "%B");
        size_t seg_len = next_b ? (size_t)(next_b - cur) : strlen(cur);
        char raw_opt[256];
        if (seg_len >= sizeof(raw_opt)) seg_len = sizeof(raw_opt) - 1u;
        memcpy(raw_opt, cur, seg_len);
        raw_opt[seg_len] = '\0';

        char clean_opt[128];
        pal_talk_strip_rich_tags(raw_opt, clean_opt, sizeof(clean_opt));
        /* Trim leading/trailing whitespace and full-width '／' separators */
        size_t s = 0, e = strlen(clean_opt);
        while (s < e && (clean_opt[s] == ' ' || clean_opt[s] == '\t' || clean_opt[s] == '\n' ||
                         (s + 3 <= e && (uint8_t)clean_opt[s] == 0xEF && (uint8_t)clean_opt[s + 1] == 0xBC && (uint8_t)clean_opt[s + 2] == 0x8F))) {
            s += (clean_opt[s] == ' ' || clean_opt[s] == '\t' || clean_opt[s] == '\n') ? 1u : 3u;
        }
        while (e > s) {
            if (clean_opt[e - 1] == ' ' || clean_opt[e - 1] == '\t' || clean_opt[e - 1] == '\n') {
                e--;
            } else if (e - s >= 3 && (uint8_t)clean_opt[e - 3] == 0xEF && (uint8_t)clean_opt[e - 2] == 0xBC && (uint8_t)clean_opt[e - 1] == 0x8F) {
                e -= 3;
            } else {
                break;
            }
        }
        clean_opt[e] = '\0';
        snprintf(vm->choice_labels[vm->choice_count++], 128, "%s", clean_opt + s);
        cur = next_b;
    }
}

static void pal_talk_vm_free(PalTalkVM *vm) {
    if (!vm) return;
    if (vm->chunk) free(vm->chunk);
    memset(vm, 0, sizeof(*vm));
    vm->pending_map_id = -1;
}

static int pal_talk_vm_start(PalTalkVM *vm, const char *game_dir, int32_t talk_id, uint16_t trigger_obj_id) {
    if (!vm || !game_dir || talk_id <= 0) return -1;
    pal_talk_vm_free(vm);
    if (pal_load_talk_script_chunk(game_dir, talk_id, &vm->chunk, &vm->chunk_len, &vm->payload_off) != 0) {
        return -1;
    }
    vm->active = 1;
    vm->talk_id = talk_id;
    vm->trigger_obj_id = trigger_obj_id;
    vm->last_ref_obj_id = trigger_obj_id;
    vm->ip = 0;
    vm->pending_map_id = -1;
    return 0;
}

static uint16_t talk_resolve_obj_id(const PalTalkVM *vm, uint16_t raw_obj) {
    if (raw_obj == 0xFFF0u) return vm->trigger_obj_id;
    if (raw_obj == 0xFFF1u) return vm->last_ref_obj_id;
    return raw_obj;
}

static PalFieldActor *talk_find_actor_by_obj(
    PalFieldActor *party, PalFieldActor *npcs, int npc_cnt, uint16_t obj_id, int *out_is_party
) {
    if (out_is_party) *out_is_party = 0;
    if (obj_id == 0u || obj_id == 1u) {
        if (party) {
            if (out_is_party) *out_is_party = 1;
            return &party[0];
        }
    }
    if (obj_id >= 1u && obj_id <= 4u && party) {
        if (out_is_party) *out_is_party = 1;
        return &party[obj_id - 1u];
    }
    if (npcs) {
        for (int i = 0; i < npc_cnt; i++) {
            if (npcs[i].obj_id == obj_id) return &npcs[i];
        }
    }
    return NULL;
}

static int talk_jump_to_target(PalTalkVM *vm, uint32_t target_u32) {
    if (target_u32 < vm->payload_off) {
        vm->active = 0;
        return 0;
    }
    uint32_t rel = target_u32 - vm->payload_off;
    if (rel >= vm->chunk_len) {
        vm->active = 0;
        return 0;
    }
    vm->ip = rel;
    return 1;
}

static void talk_set_portrait(PalTalkVM *vm, const char *game_dir, uint16_t act_id, uint16_t sub, int ax, int ay) {
    int slot = -1;
    for (int i = 0; i < PAL_MAX_PORTRAITS; i++) {
        if (vm->portraits[i].active && vm->portraits[i].anchor_x == ax) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < PAL_MAX_PORTRAITS; i++) {
            if (!vm->portraits[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) slot = PAL_MAX_PORTRAITS - 1;

    PalTalkPortrait *pt = &vm->portraits[slot];
    memset(pt, 0, sizeof(*pt));
    pt->active = 1;
    pt->act_id = act_id;
    pt->sub_action = sub;
    pt->anchor_x = ax;
    pt->anchor_y = ay;

    const char *act_file = NULL;
    uint32_t sid = 0;
    if (resolve_act_file_for_id((int32_t)act_id, &act_file, &sid) == 0) {
        if (pal_act_vm_init(game_dir, act_file, sid, (int)sub, &pt->vm) == 0) {
            pt->vm_ok = 1;
        }
    }
    vm->portraits_shown++;
}

static void talk_hide_portrait(PalTalkVM *vm, uint16_t act_id, uint16_t sub) {
    for (int i = 0; i < PAL_MAX_PORTRAITS; i++) {
        if (vm->portraits[i].active && vm->portraits[i].act_id == act_id &&
            (sub == 0xFFFFu || vm->portraits[i].sub_action == sub)) {
            vm->portraits[i].active = 0;
        }
    }
}

static void pal_talk_vm_step(
    PalTalkVM *vm, PalTalkWorldState *ws, const char *game_dir, uint32_t cur_map_id,
    PalFieldActor *party, PalFieldActor *npcs, int npc_cnt, int auto_advance_wait
) {
    if (!vm || !ws || !vm->active || !vm->chunk) return;
    if (!auto_advance_wait && (vm->waiting_input || vm->wait_ticks > 0 || vm->pending_map_id >= 0)) return;

    const int max_iter = 4096;
    for (int iter = 0; iter < max_iter && vm->active; iter++) {
        if (vm->ip + 2u > vm->chunk_len) {
            vm->active = 0;
            break;
        }

        uint16_t raw = rd_u16(vm->chunk + vm->ip);
        if (raw == 0xFFFFu) {
            vm->exec_steps++;
            vm->dlg_visible = 0;
            vm->active = 0;
            break;
        }

        uint32_t ilen = pal_talk_insn_length(vm->chunk, vm->chunk_len, vm->ip);
        if (ilen == 0u || vm->ip + ilen > vm->chunk_len) {
            vm->active = 0;
            break;
        }

        const uint8_t *ins = vm->chunk + vm->ip;
        uint16_t op = raw & 0x3FFFu;
        vm->exec_steps++;

        /* Dialogue & choice text opcodes */
        uint32_t dlg_hdr = talk_dialogue_header_bytes(op);
        if (dlg_hdr > 0u || op == 0x7Du || ((op == 0xAAu || op == 0xABu || op == 0xACu || op == 0xADu) && (raw & 1u))) {
            uint32_t text_off = (op == 0x7Du || op >= 0xAAu) ? 2u : dlg_hdr;
            uint32_t text_len = (ilen > text_off + 2u) ? (ilen - text_off - 2u) : 0u;
            vm->dlg_mode = op;
            vm->dlg_obj_id = (dlg_hdr >= 4u) ? talk_resolve_obj_id(vm, rd_u16(ins + 2u)) : 0u;
            vm->dlg_color_id = (dlg_hdr >= 6u) ? rd_u16(ins + 4u) : 9011u;
            vm->dlg_box_x = (dlg_hdr >= 10u) ? (int)(int16_t)rd_u16(ins + 6u) : 44;
            vm->dlg_box_y = (dlg_hdr >= 10u) ? (int)(int16_t)rd_u16(ins + 8u) : 345;

            char utf8[1024] = {0};
            if (text_len > 0u) {
                cp950_to_utf8(ins + text_off, text_len, utf8, sizeof(utf8));
            }
            pal_talk_parse_choices(utf8, vm);
            snprintf(vm->last_dialogue_utf8, sizeof(vm->last_dialogue_utf8), "%s", vm->dlg_clean_utf8);
            vm->dlg_visible = 1;
            vm->dlg_dirty = 1;
            vm->dialogues_shown++;
            vm->ip += ilen;
            continue;
        }

        switch (op) {
            case 0x0Eu: /* Wait dialogue dismiss / choice confirm */
                vm->ip += ilen;
                if (!auto_advance_wait) {
                    vm->waiting_input = 1;
                    return;
                }
                break;

            case 0x0Fu: /* Unconditional branch */
                if (!talk_jump_to_target(vm, rd_u32(ins + 2u))) return;
                break;

            case 0x15u: /* Branch if story flag SET */
            case 0x16u: { /* Branch if story flag CLEAR */
                uint16_t fid = rd_u16(ins + 2u);
                uint32_t tgt = rd_u32(ins + 4u);
                int is_set = (fid < PAL_STORY_FLAG_CAP) ? (ws->story_flags[fid] != 0) : 0;
                if ((op == 0x15u && is_set) || (op == 0x16u && !is_set)) {
                    if (!talk_jump_to_target(vm, tgt)) return;
                } else {
                    vm->ip += ilen;
                }
                break;
            }

            case 0x17u: /* Branch if ALL flags set */
            case 0x18u: { /* Branch if ANY flag set */
                uint32_t p = 2u;
                int all_set = 1, any_set = 0;
                while (p + 2u <= ilen - 4u) {
                    uint16_t w = rd_u16(ins + p);
                    p += 2u;
                    if (w == 0xFF00u) break;
                    int s = (w < PAL_STORY_FLAG_CAP) ? (ws->story_flags[w] != 0) : 0;
                    if (!s) all_set = 0;
                    if (s) any_set = 1;
                }
                uint32_t tgt = rd_u32(ins + ilen - 4u);
                if ((op == 0x17u && all_set) || (op == 0x18u && any_set)) {
                    if (!talk_jump_to_target(vm, tgt)) return;
                } else {
                    vm->ip += ilen;
                }
                break;
            }

            case 0x19u: { /* Set story flag */
                uint16_t fid = rd_u16(ins + 2u);
                if (fid < PAL_STORY_FLAG_CAP) ws->story_flags[fid] = 1u;
                vm->ip += ilen;
                break;
            }
            case 0x1Au: { /* Clear story flag */
                uint16_t fid = rd_u16(ins + 2u);
                if (fid < PAL_STORY_FLAG_CAP) ws->story_flags[fid] = 0u;
                vm->ip += ilen;
                break;
            }

            case 0x1Bu: /* Teleport map(id, tx, ty, fade, bgm, dir) */
                vm->pending_map_id = (int32_t)rd_u16(ins + 2u);
                vm->pending_tx = (int32_t)rd_u16(ins + 4u);
                vm->pending_ty = (int32_t)rd_u16(ins + 6u);
                vm->pending_fade = (int32_t)rd_u16(ins + 8u);
                vm->pending_bgm = (int32_t)rd_u16(ins + 10u);
                vm->pending_dir = (int32_t)(rd_u16(ins + 12u) & 7u);
                vm->ip += ilen;
                if (!auto_advance_wait) return;
                break;

            case 0x1Du: case 0x1Eu: case 0x1Fu: case 0x20u: case 0x21u:
            case 0xB5u: case 0xB6u: case 0xB7u: case 0xB8u: case 0xB9u:
            case 0x30Eu: case 0x30Fu: {
                uint16_t idx = rd_u16(ins + 2u);
                int is_32 = (op > 0x21u);
                int32_t val = is_32 ? (int32_t)rd_u32(ins + 4u) : (int32_t)(int16_t)rd_u16(ins + 4u);
                if (idx >= PAL_SCRIPT_VAR_CAP) {
                    vm->ip += ilen;
                    break;
                }
                if (op == 0x1Du || op == 0xB5u) {
                    ws->script_vars[idx] = val;
                    vm->ip += ilen;
                } else if (op == 0x1Eu || op == 0xB6u) {
                    ws->script_vars[idx] += val;
                    vm->ip += ilen;
                } else if (op == 0x1Fu || op == 0xB7u) {
                    ws->script_vars[idx] -= val;
                    if (ws->script_vars[idx] < 0) ws->script_vars[idx] = 0;
                    vm->ip += ilen;
                } else if (op == 0x30Eu) {
                    ws->script_vars[idx] *= val;
                    vm->ip += ilen;
                } else if (op == 0x30Fu) {
                    if (val != 0) ws->script_vars[idx] /= val;
                    vm->ip += ilen;
                } else {
                    uint32_t tgt = rd_u32(ins + (is_32 ? 8u : 6u));
                    int cond = ((op == 0x20u || op == 0xB8u) && ws->script_vars[idx] >= val) ||
                               ((op == 0x21u || op == 0xB9u) && ws->script_vars[idx] <= val);
                    if (cond) {
                        if (!talk_jump_to_target(vm, tgt)) return;
                    } else {
                        vm->ip += ilen;
                    }
                }
                break;
            }

            case 0x26u: /* Hide NPC / chest */
            case 0x27u: { /* Show NPC */
                uint16_t obj = talk_resolve_obj_id(vm, rd_u16(ins + 2u));
                int vis = (op == 0x27u) ? 1 : 0;
                if (obj < PAL_OBJ_CAP) ws->npc_visibility_override[obj] = vis ? 1u : 2u;
                PalFieldActor *a = talk_find_actor_by_obj(party, npcs, npc_cnt, obj, NULL);
                if (a) a->active = vis;
                vm->ip += ilen;
                break;
            }

            case 0x29u: /* Switch on dialogue choice */
            case 0x57u: { /* Random branch */
                uint32_t n_tgt = (ilen >= 6u) ? (ilen - 6u) / 4u : 0u;
                uint32_t pick = 0u;
                if (n_tgt > 0u) {
                    if (op == 0x29u) {
                        pick = (vm->choice_selected >= 0 && (uint32_t)vm->choice_selected < n_tgt)
                                   ? (uint32_t)vm->choice_selected
                                   : 0u;
                        vm->choice_count = 0;
                        vm->choice_selected = 0;
                    } else {
                        pick = (uint32_t)rand() % n_tgt;
                    }
                    uint32_t tgt = rd_u32(ins + 2u + pick * 4u);
                    if (!talk_jump_to_target(vm, tgt)) return;
                } else {
                    vm->ip += ilen;
                }
                break;
            }

            case 0x3Bu: /* Play SFX */
                vm->pending_sfx_id = (int32_t)rd_u16(ins + 2u);
                vm->ip += ilen;
                break;

            case 0x43u: { /* Wait timer (milliseconds in Talk.cpp, converted to 35ms ticks) */
                uint16_t ms = rd_u16(ins + 2u);
                vm->ip += ilen;
                if (!auto_advance_wait && ms > 0u) {
                    int ticks = ((int)ms + 34) / 35;
                    vm->wait_ticks = ticks > 0 ? ticks : 1;
                    return;
                }
                break;
            }

            case 0x4Cu: { /* Turn obj1 to face obj2 (dir toward obj2 or copy dir) */
                uint16_t o1 = talk_resolve_obj_id(vm, rd_u16(ins + 2u));
                uint16_t o2 = talk_resolve_obj_id(vm, rd_u16(ins + 4u));
                int p1 = 0;
                PalFieldActor *a1 = talk_find_actor_by_obj(party, npcs, npc_cnt, o1, &p1);
                PalFieldActor *a2 = talk_find_actor_by_obj(party, npcs, npc_cnt, o2, NULL);
                if (a1 && a2) {
                    int ndir = dir_from_delta(a2->px - a1->px, a2->py - a1->py, a1->dir);
                    a1->dir = ndir;
                    if (p1) {
                        field_actor_set_sub(a1, game_dir, a1->act_id, ndir, 0);
                    } else {
                        field_actor_set_sub(a1, game_dir, a1->act_id, (int)a1->orig_sub_state + ndir, 1);
                    }
                } else if (a1 && o2 < 8u) {
                    a1->dir = (int)(o2 & 7u);
                }
                vm->ip += ilen;
                break;
            }

            case 0x60u:
            case 0xC2u: { /* Play Bink Video */
                uint32_t p = 2u;
                if (op == 0x60u) {
                    if (p < ilen && ins[p] == '%') p++;
                    if (p < ilen && ins[p] == '*') p++;
                }
                uint32_t vlen = (ilen > p + 2u) ? (ilen - p - 2u) : 0u;
                char name[96] = {0};
                if (vlen > 0u) cp950_to_utf8(ins + p, vlen, name, sizeof(name));
                if (name[0]) {
                    if (strchr(name, '.')) {
                        snprintf(vm->pending_bink_rel, sizeof(vm->pending_bink_rel), "Video/%s", name);
                    } else {
                        snprintf(vm->pending_bink_rel, sizeof(vm->pending_bink_rel), "Video/%s.bik", name);
                    }
                }
                vm->ip += ilen;
                break;
            }

            case 0x64u: { /* Set NPC talk_id */
                uint16_t obj = talk_resolve_obj_id(vm, rd_u16(ins + 2u));
                uint16_t tid = rd_u16(ins + 4u);
                if (obj < PAL_OBJ_CAP) ws->npc_talk_override[obj] = tid;
                PalFieldActor *a = talk_find_actor_by_obj(party, npcs, npc_cnt, obj, NULL);
                if (a) a->talk_id = tid;
                vm->npc_talk_updates++;
                vm->ip += ilen;
                break;
            }

            case 0x78u: { /* Set actor sprite act_id / sub / dir */
                uint16_t obj = talk_resolve_obj_id(vm, rd_u16(ins + 2u));
                uint16_t aid = rd_u16(ins + 4u);
                uint16_t sub = rd_u16(ins + 6u);
                uint16_t dir = rd_u16(ins + 8u);
                PalFieldActor *a = talk_find_actor_by_obj(party, npcs, npc_cnt, obj, NULL);
                if (a) {
                    if (aid != 0xFFFFu) a->act_id = aid;
                    if (dir != 0xFFFFu) a->dir = (int)(dir & 7u);
                    if (sub != 0xFFFFu) {
                        field_actor_set_sub(a, game_dir, a->act_id, (int)sub, 1);
                    } else if (dir != 0xFFFFu) {
                        field_actor_set_sub(a, game_dir, a->act_id, (int)a->orig_sub_state + a->dir, 1);
                    }
                    a->active = 1;
                }
                vm->ip += ilen;
                break;
            }

            case 0x80u: { /* Add / remove inventory item (id==0 adjusts money) */
                uint16_t item_id = rd_u16(ins + 2u);
                int16_t delta = (int16_t)rd_u16(ins + 4u);
                if (item_id == 0u) {
                    ws->money += (int32_t)delta;
                    if (ws->money < 0) ws->money = 0;
                } else {
                    pal_world_add_item(ws, item_id, (int32_t)delta);
                }
                vm->ip += ilen;
                break;
            }

            case 0x81u: case 0x82u: case 0xA7u: case 0xA8u: {
                uint16_t item_id = rd_u16(ins + 2u);
                uint32_t tgt = rd_u32(ins + 4u);
                int has = pal_world_has_item(ws, item_id);
                int expect_has = (op == 0x82u || op == 0xA8u) ? 1 : 0;
                if (has == expect_has) {
                    vm->ip += ilen;
                } else {
                    if (!talk_jump_to_target(vm, tgt)) return;
                }
                break;
            }

            case 0x83u: { /* Teach (>0) or forget (<0) magic for hero */
                uint16_t hero_id = rd_u16(ins + 2u);
                int16_t magic_delta = (int16_t)rd_u16(ins + 4u);
                int pidx = pal_world_find_party_member(ws, (int32_t)hero_id);
                if (pidx >= 0) {
                    if (magic_delta > 0) pal_party_teach_magic(&ws->party_members[pidx], (uint16_t)magic_delta);
                    else if (magic_delta < 0) pal_party_forget_magic(&ws->party_members[pidx], (uint16_t)(-magic_delta));
                }
                vm->ip += ilen;
                break;
            }

            case 0x84u: { /* Equip (mode==0) or unequip (mode==1) item on hero */
                uint16_t hero_id = rd_u16(ins + 2u);
                uint16_t mode = rd_u16(ins + 4u);
                uint16_t item_id = rd_u16(ins + 6u);
                int pidx = pal_world_find_party_member(ws, (int32_t)hero_id);
                if (pidx >= 0) {
                    if (mode == 1u) pal_party_unequip_item(ws, pidx, item_id, 1);
                    else pal_party_equip_item(ws, pidx, item_id, NULL, 0);
                }
                vm->ip += ilen;
                break;
            }

            case 0x86u: { /* Adjust HP / MP for hero (or all party when hero_id == 0 / 0xFFFF) */
                uint16_t hero_id = rd_u16(ins + 2u);
                int16_t d_hp = (int16_t)rd_u16(ins + 4u);
                int16_t d_mp = (int16_t)rd_u16(ins + 6u);
                if (hero_id == 0u || hero_id == 0xFFFFu) {
                    for (int32_t i = 0; i < ws->party_member_count && i < 4; i++) {
                        pal_party_apply_hp_mp_delta(&ws->party_members[i], (int32_t)d_hp, (int32_t)d_mp);
                    }
                } else {
                    int pidx = pal_world_find_party_member(ws, (int32_t)hero_id);
                    if (pidx >= 0) {
                        pal_party_apply_hp_mp_delta(&ws->party_members[pidx], (int32_t)d_hp, (int32_t)d_mp);
                    }
                }
                vm->ip += ilen;
                break;
            }

            case 0xA3u: /* Branch if current map != arg0 */
            case 0xA4u: { /* Branch if current map == arg0 */
                uint16_t mid = rd_u16(ins + 2u);
                uint32_t tgt = rd_u32(ins + 4u);
                int eq = (cur_map_id == (uint32_t)mid);
                if ((op == 0xA3u && !eq) || (op == 0xA4u && eq)) {
                    if (!talk_jump_to_target(vm, tgt)) return;
                } else {
                    vm->ip += ilen;
                }
                break;
            }

            case 0xC5u: case 0xC6u: case 0xC7u:
            case 0xC8u: case 0xC9u: case 0xCAu: {
                uint16_t obj = talk_resolve_obj_id(vm, rd_u16(ins + 2u));
                uint16_t rv = rd_u16(ins + 4u);
                int16_t val = (int16_t)rd_u16(ins + 6u);
                if (obj >= PAL_OBJ_CAP || rv >= 4u) {
                    vm->ip += ilen;
                    break;
                }
                if (op == 0xC5u) {
                    ws->obj_rv[obj][rv] = val;
                    vm->ip += ilen;
                } else if (op == 0xC6u) {
                    ws->obj_rv[obj][rv] += val;
                    vm->ip += ilen;
                } else if (op == 0xC7u) {
                    ws->obj_rv[obj][rv] -= val;
                    vm->ip += ilen;
                } else {
                    uint32_t tgt = rd_u32(ins + 8u);
                    int16_t cur = ws->obj_rv[obj][rv];
                    int cond = (op == 0xC8u && cur >= val) || (op == 0xC9u && cur <= val) || (op == 0xCAu && cur == val);
                    if (cond) {
                        if (!talk_jump_to_target(vm, tgt)) return;
                    } else {
                        vm->ip += ilen;
                    }
                }
                break;
            }

            case 0xCBu: {
                uint16_t obj = talk_resolve_obj_id(vm, rd_u16(ins + 2u));
                uint16_t rv = rd_u16(ins + 4u);
                int16_t idx = (obj < PAL_OBJ_CAP && rv < 4u) ? ws->obj_rv[obj][rv] : -1;
                uint32_t n_tgt = (ilen >= 10u) ? (ilen - 10u) / 4u : 0u;
                if (idx >= 0 && (uint32_t)idx < n_tgt) {
                    uint32_t tgt = rd_u32(ins + 6u + (uint32_t)idx * 4u);
                    if (!talk_jump_to_target(vm, tgt)) return;
                } else {
                    vm->ip += ilen;
                }
                break;
            }

            case 0xCCu: { /* Branch if distance(obj1, obj2) > arg2 * 16px */
                uint16_t o1 = talk_resolve_obj_id(vm, rd_u16(ins + 2u));
                uint16_t o2 = talk_resolve_obj_id(vm, rd_u16(ins + 4u));
                int32_t limit_px = (int32_t)rd_u16(ins + 6u) * 16;
                uint32_t tgt = rd_u32(ins + 8u);
                PalFieldActor *a1 = talk_find_actor_by_obj(party, npcs, npc_cnt, o1, NULL);
                PalFieldActor *a2 = talk_find_actor_by_obj(party, npcs, npc_cnt, o2, NULL);
                if (a1 && a2) {
                    int64_t dx = (int64_t)a1->px - (int64_t)a2->px;
                    int64_t dy = (int64_t)a1->py - (int64_t)a2->py;
                    if (dx * dx + dy * dy > (int64_t)limit_px * (int64_t)limit_px) {
                        if (!talk_jump_to_target(vm, tgt)) return;
                        break;
                    }
                }
                vm->ip += ilen;
                break;
            }

            case 0xD8u: { /* Show bust portrait */
                uint16_t aid = rd_u16(ins + 2u);
                uint16_t sub = rd_u16(ins + 4u);
                int ax = (int)(int16_t)rd_u16(ins + 6u);
                int ay = (int)(rd_u16(ins + 8u) & 0x7FFFu);
                talk_set_portrait(vm, game_dir, aid, sub, ax, ay);
                vm->ip += ilen;
                break;
            }

            case 0xD9u: /* Hide bust portrait */
                talk_hide_portrait(vm, rd_u16(ins + 2u), rd_u16(ins + 4u));
                vm->ip += ilen;
                break;

            case 0x309u: { /* Give item / reward script 5000..5799 or shop trigger 58000+ */
                uint16_t raw_id = rd_u16(ins + 2u);
                vm->last_item_given = (int32_t)raw_id;
                if (ws->db) {
                    if (raw_id >= 58000u) {
                        ws->active_shop_id = (int32_t)raw_id;
                    } else {
                        const PalScript7fRecord *sc = pal_db_find_script(ws->db, (int32_t)raw_id);
                        int granted = 0;
                        if (sc) {
                            for (int32_t oi = 0; oi < sc->op_count; oi++) {
                                const PalScript7fOp *sop = &sc->ops[oi];
                                if (sop->tag == 0x7F202041u && sop->arg_count >= 1 &&
                                    sop->args[0] > 0 && sop->args[0] < 10000) {
                                    int32_t qty = (sop->arg_count >= 2 && sop->args[1] > 0) ? sop->args[1] : 1;
                                    pal_world_add_item(ws, (uint16_t)sop->args[0], qty);
                                    ws->last_resolved_item_id = sop->args[0];
                                    granted = 1;
                                }
                            }
                        }
                        if (!granted && raw_id > 0u) {
                            pal_world_add_item(ws, raw_id, 1);
                            ws->last_resolved_item_id = (int32_t)raw_id;
                        }
                    }
                } else {
                    pal_world_add_item(ws, raw_id, 1);
                }
                vm->ip += ilen;
                break;
            }

            case 0x30Bu: { /* Sync script_var <-> money */
                uint16_t idx = rd_u16(ins + 2u);
                uint16_t dir = rd_u16(ins + 4u);
                if (idx < PAL_SCRIPT_VAR_CAP) {
                    if (dir == 0u) ws->script_vars[idx] = ws->money;
                    else ws->money = ws->script_vars[idx];
                }
                vm->ip += ilen;
                break;
            }

            default:
                vm->ip += ilen;
                break;
        }
    }
}

static void pal_talk_vm_advance_input(
    PalTalkVM *vm, PalTalkWorldState *ws, const char *game_dir, uint32_t cur_map_id,
    PalFieldActor *party, PalFieldActor *npcs, int npc_cnt
) {
    if (!vm || !vm->active || !vm->waiting_input) return;
    vm->waiting_input = 0;
    vm->dlg_visible = 0;
    pal_talk_vm_step(vm, ws, game_dir, cur_map_id, party, npcs, npc_cnt, 0);
}

int32_t pal_talk_vm_probe_m3(const char *game_dir, PalTalkM3ProbeInfo *out_info) {
    if (!game_dir || !out_info) return -1;
    memset(out_info, 0, sizeof(*out_info));

    for (int bank = 1; bank <= 8; bank++) {
        char rel[32], full[1200];
        snprintf(rel, sizeof(rel), "talk%d.dat", bank);
        if (resolve_rel_path_ci(game_dir, rel, full, sizeof(full)) != 0) continue;

        size_t flen = 0;
        uint8_t *buf = read_entire_file(full, &flen);
        if (!buf) continue;

        uint32_t n_slots = talk_bank_slots(bank);
        if (flen >= PAL_MZ_HEADER_SIZE + (size_t)n_slots * 4u && buf[0] == 'M' && buf[1] == 'Z') {
            out_info->bank_count++;
            const uint8_t *p = buf + PAL_MZ_HEADER_SIZE;
            uint32_t plen = (uint32_t)(flen - PAL_MZ_HEADER_SIZE);
            uint32_t data_start = n_slots * 4u;

            for (uint32_t s = 0; s < n_slots; s++) {
                uint32_t off = rd_u32(p + (size_t)s * 4u);
                if (off == 0xFFFFFFFFu || off < data_start || off >= plen) continue;
                uint32_t end = plen;
                for (uint32_t j = 0; j < n_slots; j++) {
                    uint32_t o2 = rd_u32(p + (size_t)j * 4u);
                    if (o2 != 0xFFFFFFFFu && o2 > off && o2 < end) end = o2;
                }
                if (end <= off) continue;
                uint32_t clen = end - off;
                out_info->total_scripts++;

                uint32_t ip = 0;
                while (ip < clen) {
                    uint32_t ilen = pal_talk_insn_length(p + off, clen, ip);
                    if (ilen == 0u || ip + ilen > clen) break;
                    ip += ilen;
                }
                if (ip == clen) out_info->clean_scripts++;
            }
        }
        free(buf);
    }

    PalTalkWorldState *ws = (PalTalkWorldState *)calloc(1, sizeof(PalTalkWorldState));
    if (!ws) return -1;

    PalTalkVM vm;
    memset(&vm, 0, sizeof(vm));
    vm.pending_map_id = -1;

    int ok_6001 = 0, ok_6007 = 0, ok_7012 = 0, ok_104 = 0, ok_105 = 0;

    /* 1. Portal 6001 from Map 1 -> Map 2 (54, 17, dir 6) */
    if (pal_talk_vm_start(&vm, game_dir, 6001, 0) == 0) {
        pal_talk_vm_step(&vm, ws, game_dir, 1u, NULL, NULL, 0, 1);
        out_info->tp_map_id = vm.pending_map_id;
        out_info->tp_tx = vm.pending_tx;
        out_info->tp_ty = vm.pending_ty;
        out_info->tp_dir = vm.pending_dir;
        ok_6001 = (vm.pending_map_id == 2 && vm.pending_tx == 54 && vm.pending_ty == 17 && vm.pending_dir == 6);
    }

    /* 2. Portal 6007 from Map 2 -> Map 1 (11, 23, dir 7) */
    if (pal_talk_vm_start(&vm, game_dir, 6007, 0) == 0) {
        pal_talk_vm_step(&vm, ws, game_dir, 2u, NULL, NULL, 0, 1);
        ok_6007 = (vm.pending_map_id == 1 && vm.pending_tx == 11 && vm.pending_ty == 23 && vm.pending_dir == 7);
    }

    /* 3. Chest 7012: hides obj 7012, grants item 5150, shows "得到皮帽" */
    if (pal_talk_vm_start(&vm, game_dir, 7012, 7012) == 0) {
        pal_talk_vm_step(&vm, ws, game_dir, 1u, NULL, NULL, 0, 1);
        out_info->chest_hidden = (ws->npc_visibility_override[7012] == 2u) ? 1 : 0;
        out_info->chest_item_id = vm.last_item_given;
        snprintf(out_info->chest_text_utf8, sizeof(out_info->chest_text_utf8), "%s", vm.last_dialogue_utf8);
        ok_7012 = (out_info->chest_hidden == 1 && out_info->chest_item_id == 5150 &&
                   strstr(out_info->chest_text_utf8, "皮帽") != NULL);
    }

    /* 4. Opening cutscene 104: portraits + >=10 dialogues + sets story flag 1001 */
    if (pal_talk_vm_start(&vm, game_dir, 104, 104) == 0) {
        pal_talk_vm_step(&vm, ws, game_dir, 1u, NULL, NULL, 0, 1);
        out_info->cutscene_flag1001 = (ws->story_flags[1001] != 0) ? 1 : 0;
        out_info->cutscene_portrait_count = vm.portraits_shown;
        out_info->cutscene_dialogue_count = vm.dialogues_shown;
        snprintf(out_info->cutscene_sample_utf8, sizeof(out_info->cutscene_sample_utf8), "%s", vm.last_dialogue_utf8);
        ok_104 = (out_info->cutscene_flag1001 == 1 &&
                  out_info->cutscene_portrait_count >= 4 &&
                  out_info->cutscene_dialogue_count >= 10);
    }

    /* 5. Cutscene 105: updates obj 7 -> talk 106 & obj 201 -> talk 6296 */
    if (pal_talk_vm_start(&vm, game_dir, 105, 105) == 0) {
        pal_talk_vm_step(&vm, ws, game_dir, 1u, NULL, NULL, 0, 1);
        out_info->cutscene_npc_updates = vm.npc_talk_updates;
        out_info->npc_update_obj7_talk = (int32_t)ws->npc_talk_override[7];
        out_info->npc_update_obj201_talk = (int32_t)ws->npc_talk_override[201];
        ok_105 = (out_info->cutscene_npc_updates >= 2 &&
                  out_info->npc_update_obj7_talk == 106 &&
                  out_info->npc_update_obj201_talk == 6296);
    }

    pal_talk_vm_free(&vm);
    free(ws);

    out_info->core_scripts_ok = (ok_6001 && ok_6007 && ok_7012 && ok_104 && ok_105) ? 1 : 0;
    out_info->ok = (out_info->bank_count == 8 &&
                    out_info->total_scripts >= 5600 &&
                    out_info->clean_scripts == out_info->total_scripts &&
                    out_info->core_scripts_ok == 1) ? 1 : 0;
    return out_info->ok ? 0 : -1;
}

int32_t pal_db_menu_probe_m4(const char *game_dir, PalDbM4ProbeInfo *out_info) {
    if (!game_dir || !out_info) return -1;
    memset(out_info, 0, sizeof(*out_info));

    PalGameDatabase *db = (PalGameDatabase *)calloc(1, sizeof(PalGameDatabase));
    PalTalkWorldState *ws = (PalTalkWorldState *)calloc(1, sizeof(PalTalkWorldState));
    if (!db || !ws) {
        free(db);
        free(ws);
        return -1;
    }

    if (pal_db_load(game_dir, db) != 0) {
        pal_db_free(db);
        free(db);
        free(ws);
        return -1;
    }

    out_info->hero_count = db->hero_count;
    out_info->item_count = db->item_count;
    out_info->magic_count = db->magic_count;
    out_info->level_template_count = db->level_count;

    pal_party_init_from_db(ws, db, 0);
    PalPartyMemberState *p0 = &ws->party_members[0];
    int hero_init_ok = (ws->party_member_count >= 4 && p0->hero_id == 2000 && p0->lv == 1 && p0->base_max_hp == 150);
    out_info->hero2000_lv1_hp = p0->base_max_hp;

    /* Strip starter equips cleanly to verify bare stats (20/20) -> equip 木劍 (3451: +2 ATK) & 皮帽 (3254: +4 DEF) */
    for (int s = 0; s < PAL_EQUIP_SLOT_COUNT; s++) {
        if (p0->equips[s] != 0u) pal_party_unequip_item(ws, 0, p0->equips[s], 1);
    }
    int bare_ok = (p0->eff_atk == 20 && p0->eff_def == 20);
    int eq_atk_rc = pal_party_equip_item(ws, 0, 3451u, NULL, 0);
    out_info->hero2000_equip_atk = p0->eff_atk;
    int eq_def_rc = pal_party_equip_item(ws, 0, 3254u, NULL, 0);
    out_info->hero2000_equip_def = p0->eff_def;

    /* Field healing: 80 HP -> 止血草 (4401: +50 => 130) -> 氣療術 (6901: +75 clamped => 150, MP 100->94) */
    p0->cur_hp = 80;
    int item_rc = pal_party_use_field_item(ws, 0, 4401u, NULL, 0);
    out_info->item4401_healed_hp = p0->cur_hp;
    int magic_rc = pal_party_cast_field_magic(ws, 0, 0, 6901u, NULL, 0);
    out_info->magic6901_healed_hp = p0->cur_hp;
    int mp_cost_ok = (p0->cur_mp == 94);

    /* Level-up Lv1 -> Lv2 via glevel.dat (200002: +15 MaxHP -> 165) */
    int lv_rc = pal_party_level_up_member(ws, 0);
    out_info->hero2000_lv2_hp = p0->eff_max_hp;

    /* Chest 7012 Talk script 0x309 [5150] -> resolves script 5150 -> 0x7F202041 [3254, 1] -> 皮帽 */
    PalTalkVM vm;
    memset(&vm, 0, sizeof(vm));
    if (pal_talk_vm_start(&vm, game_dir, 7012, 7012) == 0) {
        pal_talk_vm_step(&vm, ws, game_dir, 1u, NULL, NULL, 0, 1);
        out_info->chest7012_granted_item_id = ws->last_resolved_item_id;
        const PalItemRecord *chest_it = pal_db_find_item(db, ws->last_resolved_item_id);
        if (chest_it) {
            snprintf(out_info->chest7012_item_name_utf8, sizeof(out_info->chest7012_item_name_utf8),
                     "%s", chest_it->name_utf8);
        }
    }
    pal_talk_vm_free(&vm);

    out_info->ok = (out_info->hero_count == 9 &&
                    out_info->item_count == 252 &&
                    out_info->magic_count == 66 &&
                    out_info->level_template_count == 410 &&
                    hero_init_ok && bare_ok &&
                    eq_atk_rc == 0 && out_info->hero2000_equip_atk == 22 &&
                    eq_def_rc == 0 && out_info->hero2000_equip_def == 24 &&
                    item_rc == 0 && out_info->item4401_healed_hp == 130 &&
                    magic_rc == 0 && out_info->magic6901_healed_hp == 150 && mp_cost_ok &&
                    lv_rc == 0 && p0->lv == 2 && out_info->hero2000_lv2_hp == 165 &&
                    out_info->chest7012_granted_item_id == 3254 &&
                    pal_world_has_item(ws, 3254u) &&
                    strstr(out_info->chest7012_item_name_utf8, "皮帽") != NULL) ? 1 : 0;

    pal_db_free(db);
    free(db);
    free(ws);
    return out_info->ok ? 0 : -1;
}

static int talk_find_interact_npc(const PalFieldActor *leader, const PalFieldActor *npcs, int npc_cnt) {
    if (!leader || !npcs) return -1;
    static const int kDirDx[8] = {0, 0, -1, 1, -1, 1, -1, 1};
    static const int kDirDy[8] = {-1, 1, 0, 0, -1, 1, 1, -1};
    int d = (leader->dir >= 0 && leader->dir < 8) ? leader->dir : 1;
    int probe_x = leader->px + kDirDx[d] * 18;
    int probe_y = leader->py + kDirDy[d] * 18;

    int best_idx = -1;
    int best_score = 46 * 46;
    for (int i = 0; i < npc_cnt; i++) {
        if (!npcs[i].active || npcs[i].talk_id == 0u) continue;
        int dx = npcs[i].px - probe_x;
        int dy = npcs[i].py - probe_y;
        int d2 = dx * dx + dy * dy;
        int ldx = npcs[i].px - leader->px;
        int ldy = npcs[i].py - leader->py;
        int ld2 = ldx * ldx + ldy * ldy;
        int score = d2 < ld2 ? d2 : ld2;
        if (ld2 <= 46 * 46 && score < best_score) {
            best_score = score;
            best_idx = i;
        }
    }
    return best_idx;
}

static int talk_find_npc_at_world_pt(const PalFieldActor *npcs, int npc_cnt, int wx, int wy) {
    if (!npcs) return -1;
    int best = -1;
    int best_d2 = 34 * 34;
    for (int i = 0; i < npc_cnt; i++) {
        if (!npcs[i].active || npcs[i].talk_id == 0u) continue;
        int cx = npcs[i].px + 16;
        int cy = npcs[i].py - 12;
        int dx = wx - cx;
        int dy = wy - cy;
        int d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

static uint32_t talk_lookup_trigger_script(const PalLoadedMap *m, uint32_t trig_id) {
    if (!m || !m->triggers || trig_id == 0u) return 0u;
    for (int i = 0; i < m->trigger_count; i++) {
        if (m->triggers[i].id == trig_id) return m->triggers[i].talk_id;
    }
    return 0u;
}

static void talk_begin_npc_interaction(
    PalTalkVM *vm, PalTalkWorldState *ws, const char *game_dir, uint32_t map_id,
    PalFieldActor *party, PalFieldActor *npcs, int npc_cnt, int npc_idx, int *show_dialogue
) {
    if (!vm || !ws || npc_idx < 0 || npc_idx >= npc_cnt) return;
    PalFieldActor *npc = &npcs[npc_idx];
    if (!npc->active || npc->talk_id == 0u) return;

    int ndir = dir_from_delta(party[0].px - npc->px, party[0].py - npc->py, npc->dir);
    npc->dir = ndir;
    field_actor_set_sub(npc, game_dir, npc->act_id, (int)npc->orig_sub_state + ndir, 1);

    int pdir = dir_from_delta(npc->px - party[0].px, npc->py - party[0].py, party[0].dir);
    party[0].dir = pdir;
    field_actor_set_sub(&party[0], game_dir, party[0].act_id, pdir, 0);

    if (pal_talk_vm_start(vm, game_dir, (int32_t)npc->talk_id, npc->obj_id) == 0) {
        if (show_dialogue) *show_dialogue = 0;
        pal_talk_vm_step(vm, ws, game_dir, map_id, party, npcs, npc_cnt, 0);
    }
}

static void talk_format_display_text(const PalTalkVM *vm, char *out_buf, size_t cap, int *out_x, int *out_max_w) {
    int has_left_pt = 0, has_right_pt = 0;
    for (int i = 0; i < PAL_MAX_PORTRAITS; i++) {
        if (!vm->portraits[i].active) continue;
        if (vm->portraits[i].anchor_x < 260) has_left_pt = 1;
        else if (vm->portraits[i].anchor_x >= 380) has_right_pt = 1;
    }
    int tx = 44;
    int mw = 540;
    if (has_left_pt && !has_right_pt) {
        tx = 216;
        mw = 380;
    } else if (has_right_pt && !has_left_pt) {
        tx = 44;
        mw = 376;
    } else if (has_left_pt && has_right_pt) {
        tx = 180;
        mw = 280;
    }
    if (out_x) *out_x = tx;
    if (out_max_w) *out_max_w = mw;

    if (vm->choice_count <= 0) {
        snprintf(out_buf, cap, "%s", vm->dlg_raw_utf8);
        return;
    }
    size_t used = (size_t)snprintf(out_buf, cap, "%s", vm->dlg_raw_utf8);
    for (int i = 0; i < vm->choice_count && used + 32u < cap; i++) {
        used += (size_t)snprintf(
            out_buf + used, cap - used,
            "%s%s%d. %s",
            (i == 0 && vm->dlg_raw_utf8[0]) ? "%N" : (i > 0 ? "  " : ""),
            (i == vm->choice_selected) ? "%C6> " : "%C0  ",
            i + 1, vm->choice_labels[i]
        );
    }
}

static void pick_map_spawn_tile(const PalLoadedMap *m, int *out_tx, int *out_ty) {
    int cx = m->w_tiles > 2 ? m->w_tiles / 2 : 1;
    int cy = m->h_tiles > 2 ? m->h_tiles / 2 : 1;
    if (pal_map_nearest_walkable(m, &cx, &cy, 2, m->w_tiles > m->h_tiles ? m->w_tiles : m->h_tiles)) {
        *out_tx = cx;
        *out_ty = cy;
        return;
    }
    *out_tx = 4;
    *out_ty = 4;
}

typedef enum {
    PAL_RENDER_FG_INST = 0,
    PAL_RENDER_ACTOR   = 1
} PalRenderItemKind;

typedef struct {
    PalRenderItemKind kind;
    int sort_y;
    int draw_x;
    int draw_y;
    int draw_w;
    int draw_h;
    SDL_Texture *tex;
    int is_leader;
    int is_party;
} PalRenderItem;

static int cmp_render_item(const void *a, const void *b) {
    const PalRenderItem *ra = (const PalRenderItem *)a;
    const PalRenderItem *rb = (const PalRenderItem *)b;
    if (ra->sort_y != rb->sort_y) return (ra->sort_y < rb->sort_y) ? -1 : 1;
    /* Tie-break slightly: map props before actors at exact same Y? Or kind order */
    return (int)ra->kind - (int)rb->kind;
}

static const uint32_t kM2QuickMaps[] = {501u, 1u, 2u, 3u, 502u};
static const uint32_t kPartyHeroActIds[4] = {1u, 2u, 3u, 4u}; /* 李逍遙, 趙靈兒, 林月如, 阿奴 */

typedef enum {
    PAL_SCENE_TITLE = 0,
    PAL_SCENE_LOAD_OVERLAY = 1,
    PAL_SCENE_FIELD = 2
} PalSceneState;

#define CAMP_VISIBLE_ROWS 8
#define CAMP_TAB_X0 32.0f
#define CAMP_TAB_Y0 26.0f
#define CAMP_TAB_STEP 96.0f
#define CAMP_TAB_W 88.0f
#define CAMP_TAB_H 30.0f
#define CAMP_HERO_X 34.0f
#define CAMP_HERO_Y0 220.0f
#define CAMP_HERO_STEP 36.0f
#define CAMP_HERO_W 168.0f
#define CAMP_HERO_H 32.0f
#define CAMP_ROW_X 224.0f
#define CAMP_ROW_Y0 114.0f
#define CAMP_ROW_STEP 28.0f
#define CAMP_ROW_W 380.0f
#define CAMP_ROW_H 26.0f

typedef struct {
    uint16_t item_id;
    int32_t fr_tsw_id;
    int32_t frame_idx;
    int valid;
} PalItemIconEntry;

typedef struct {
    PalItemIconEntry entries[256];
    int count;
} PalItemIconCache;

static PalLoadedSprite *camp_get_item_icon(
    PalItemIconCache *ic, PalTswCache *tsw_cache, const char *game_dir,
    SDL_Renderer *renderer, SDL_ScaleMode scale_mode, uint16_t item_id, int *out_frame
) {
    if (out_frame) *out_frame = 0;
    if (!ic || !tsw_cache || !game_dir || !renderer || item_id == 0u) return NULL;

    for (int i = 0; i < ic->count; i++) {
        if (ic->entries[i].item_id == item_id) {
            if (!ic->entries[i].valid || ic->entries[i].fr_tsw_id <= 0) return NULL;
            if (out_frame) *out_frame = ic->entries[i].frame_idx;
            return tsw_cache_get(tsw_cache, game_dir, renderer, scale_mode, ic->entries[i].fr_tsw_id);
        }
    }

    PalItemIconEntry e = {0};
    e.item_id = item_id;
    PalActVM vm = {0};
    if (pal_act_vm_init(game_dir, "all_item.act", (uint32_t)item_id, 0, &vm) == 0) {
        for (int t = 0; t < 6 && vm.fr_tsw_id <= 0 && !vm.finished; t++) {
            pal_act_vm_step_tick(&vm);
        }
        if (vm.fr_tsw_id > 0) {
            e.fr_tsw_id = vm.fr_tsw_id;
            e.frame_idx = vm.frame_index;
            e.valid = 1;
        }
    }
    if (ic->count < 256) ic->entries[ic->count++] = e;
    if (!e.valid) return NULL;
    if (out_frame) *out_frame = e.frame_idx;
    return tsw_cache_get(tsw_cache, game_dir, renderer, scale_mode, e.fr_tsw_id);
}

typedef struct {
    int open;
    int tab;            /* 0=狀態, 1=仙術, 2=物品, 3=系統 */
    int hero_idx;
    int cursor;
    int scroll;
    int dirty;
    char feedback[160];
    float feedback_timer;
    SDL_Texture *top_tex;      int top_w, top_h;
    SDL_Texture *header_tex;   int header_w, header_h;
    SDL_Texture *party_tex[4]; int party_w[4], party_h[4];
    SDL_Texture *row_tex[CAMP_VISIBLE_ROWS];
    int row_w[CAMP_VISIBLE_ROWS], row_h[CAMP_VISIBLE_ROWS];
    uint16_t row_item_ids[CAMP_VISIBLE_ROWS];
    int row_vis_count;
    SDL_Texture *desc_tex;     int desc_w, desc_h;
} PalCampMenuState;

static void camp_free_textures(PalCampMenuState *cm) {
    if (!cm) return;
    if (cm->top_tex) { SDL_DestroyTexture(cm->top_tex); cm->top_tex = NULL; }
    if (cm->header_tex) { SDL_DestroyTexture(cm->header_tex); cm->header_tex = NULL; }
    if (cm->desc_tex) { SDL_DestroyTexture(cm->desc_tex); cm->desc_tex = NULL; }
    for (int i = 0; i < 4; i++) {
        if (cm->party_tex[i]) { SDL_DestroyTexture(cm->party_tex[i]); cm->party_tex[i] = NULL; }
    }
    for (int i = 0; i < CAMP_VISIBLE_ROWS; i++) {
        if (cm->row_tex[i]) { SDL_DestroyTexture(cm->row_tex[i]); cm->row_tex[i] = NULL; }
        cm->row_item_ids[i] = 0u;
    }
    cm->row_vis_count = 0;
}

static int camp_row_count(const PalTalkWorldState *ws, const PalCampMenuState *cm) {
    if (!ws || !cm) return 0;
    int hidx = (cm->hero_idx >= 0 && cm->hero_idx < ws->party_member_count) ? cm->hero_idx : 0;
    switch (cm->tab) {
        case 0: return PAL_EQUIP_SLOT_COUNT;
        case 1: return ws->party_member_count > 0 ? ws->party_members[hidx].magic_count : 0;
        case 2: return ws->inventory_count;
        case 3: return 5;
        default: return 0;
    }
}

static void camp_clamp_cursor(const PalTalkWorldState *ws, PalCampMenuState *cm) {
    if (!ws || !cm) return;
    if (ws->party_member_count > 0) {
        if (cm->hero_idx < 0) cm->hero_idx = 0;
        if (cm->hero_idx >= ws->party_member_count) cm->hero_idx = ws->party_member_count - 1;
    } else {
        cm->hero_idx = 0;
    }
    int total = camp_row_count(ws, cm);
    if (total <= 0) {
        cm->cursor = 0;
        cm->scroll = 0;
        return;
    }
    if (cm->cursor < 0) cm->cursor = 0;
    if (cm->cursor >= total) cm->cursor = total - 1;
    if (cm->cursor < cm->scroll) cm->scroll = cm->cursor;
    if (cm->cursor >= cm->scroll + CAMP_VISIBLE_ROWS) cm->scroll = cm->cursor - CAMP_VISIBLE_ROWS + 1;
    if (cm->scroll < 0) cm->scroll = 0;
}

static void camp_activate_row(
    PalCampMenuState *cm, PalTalkWorldState *ws, const char *game_dir,
    int *party_follow_enabled, int *runtime_mute, int *show_collision_grid,
    PalBgmPlayer *bgm, SDL_AudioStream **sfx_stream, float master_volume,
    int *out_request_title
) {
    if (!cm || !ws) return;
    camp_clamp_cursor(ws, cm);
    int total = camp_row_count(ws, cm);
    if (total <= 0 || cm->cursor >= total) return;

    int rc = -1;
    if (cm->tab == 0) {
        PalPartyMemberState *m = &ws->party_members[cm->hero_idx];
        uint16_t eq_id = m->equips[cm->cursor];
        if (eq_id != 0u) {
            const PalItemRecord *it = pal_db_find_item(ws->db, (int32_t)eq_id);
            if (pal_party_unequip_item(ws, cm->hero_idx, eq_id, 1) == 0) {
                snprintf(cm->feedback, sizeof(cm->feedback), "已卸下 %s 的「%s」。",
                         m->name_utf8, it ? it->name_utf8 : "裝備");
                rc = 0;
            }
        } else {
            snprintf(cm->feedback, sizeof(cm->feedback), "此部位尚未裝備道具（可至「物品」分頁裝備）。");
        }
    } else if (cm->tab == 1) {
        PalPartyMemberState *m = &ws->party_members[cm->hero_idx];
        uint16_t mid = m->magics[cm->cursor];
        rc = pal_party_cast_field_magic(ws, cm->hero_idx, cm->hero_idx, mid, cm->feedback, sizeof(cm->feedback));
    } else if (cm->tab == 2) {
        uint16_t iid = ws->inventory_ids[cm->cursor];
        rc = pal_party_use_field_item(ws, cm->hero_idx, iid, cm->feedback, sizeof(cm->feedback));
        camp_clamp_cursor(ws, cm);
    } else if (cm->tab == 3) {
        rc = 0;
        switch (cm->cursor) {
            case 0:
                *party_follow_enabled = !(*party_follow_enabled);
                snprintf(cm->feedback, sizeof(cm->feedback), "隊伍跟隨：%s", *party_follow_enabled ? "開啟" : "關閉");
                break;
            case 1:
                *runtime_mute = !(*runtime_mute);
                bgm->muted = *runtime_mute;
                if (bgm->stream) SDL_SetAudioStreamGain(bgm->stream, *runtime_mute ? 0.0f : master_volume);
                if (*sfx_stream) SDL_SetAudioStreamGain(*sfx_stream, *runtime_mute ? 0.0f : master_volume);
                snprintf(cm->feedback, sizeof(cm->feedback), "音樂與音效：%s", *runtime_mute ? "靜音" : "開啟");
                break;
            case 2:
                *show_collision_grid = !(*show_collision_grid);
                snprintf(cm->feedback, sizeof(cm->feedback), "碰撞網格顯示：%s", *show_collision_grid ? "顯示" : "隱藏");
                break;
            case 3:
                cm->open = 0;
                if (out_request_title) *out_request_title = 1;
                break;
            case 4:
                cm->open = 0;
                break;
            default: break;
        }
    }

    play_ui_sfx(game_dir, rc == 0 ? 2u : 1u, sfx_stream, master_volume, *runtime_mute);
    cm->feedback_timer = 3.2f;
    cm->dirty = 1;
}

static void camp_rebuild_textures(
    SDL_Renderer *renderer, const PalTalkWorldState *ws, PalCampMenuState *cm,
    int party_follow_enabled, int runtime_mute, int show_collision_grid
) {
    if (!renderer || !ws || !cm || !cm->dirty) return;
    camp_free_textures(cm);
    camp_clamp_cursor(ws, cm);

    char buf[512];
    snprintf(buf, sizeof(buf), "%%C6金錢：%d 文", ws->money);
    cm->top_tex = create_richtext_texture(renderer, buf, 15.0f, 200, &cm->top_w, &cm->top_h);

    for (int i = 0; i < ws->party_member_count && i < 4; i++) {
        const PalPartyMemberState *pm = &ws->party_members[i];
        snprintf(buf, sizeof(buf), "%%C%c%s Lv.%d  %%C0HP %d/%d  %%C3MP %d/%d",
                 (i == cm->hero_idx) ? '6' : '5', pm->name_utf8, pm->lv,
                 pm->cur_hp, pm->eff_max_hp, pm->cur_mp, pm->eff_max_mp);
        cm->party_tex[i] = create_richtext_texture(renderer, buf, 12.5f, 162, &cm->party_w[i], &cm->party_h[i]);
    }

    const PalPartyMemberState *cur_h = (ws->party_member_count > 0) ? &ws->party_members[cm->hero_idx] : NULL;
    int total = camp_row_count(ws, cm);
    static const char *kSlotNames[PAL_EQUIP_SLOT_COUNT] = {"頭戴", "披掛", "身穿", "手持", "佩帶", "足穿"};
    static const char *kSysLabels[5] = {
        "隊伍跟隨開關",
        "音樂與音效靜音",
        "地圖碰撞網格顯示",
        "返回遊戲主選單",
        "關閉系統選單"
    };

    if (cm->tab == 0 && cur_h) {
        snprintf(buf, sizeof(buf),
                 "%%C6%s %%C5(Lv.%d)  %%C0武:%d(%+d) 靈:%d 防:%d(%+d) 速:%d 運:%d",
                 cur_h->name_utf8, cur_h->lv,
                 cur_h->eff_atk, cur_h->eff_atk - cur_h->base_atk,
                 cur_h->eff_mag,
                 cur_h->eff_def, cur_h->eff_def - cur_h->base_def,
                 cur_h->eff_spd, cur_h->eff_luck);
    } else if (cm->tab == 1 && cur_h) {
        snprintf(buf, sizeof(buf), "%%C6%s 的仙術 (%d) %%C5— 真氣 %d/%d",
                 cur_h->name_utf8, cur_h->magic_count, cur_h->cur_mp, cur_h->eff_max_mp);
    } else if (cm->tab == 2) {
        snprintf(buf, sizeof(buf), "%%C6行囊物品 (%d 種) %%C5— 對象：%s",
                 ws->inventory_count, cur_h ? cur_h->name_utf8 : "");
    } else {
        snprintf(buf, sizeof(buf), "%%C6系統設定與控制 %%C5— Enter/點擊套用");
    }
    cm->header_tex = create_richtext_texture(renderer, buf, 14.0f, 380, &cm->header_w, &cm->header_h);

    cm->row_vis_count = 0;
    for (int r = 0; r < CAMP_VISIBLE_ROWS && (cm->scroll + r) < total; r++) {
        int idx = cm->scroll + r;
        int sel = (idx == cm->cursor);
        const char *col = sel ? "%C6" : "%C5";
        if (cm->tab == 0 && cur_h) {
            uint16_t eq = cur_h->equips[idx];
            const PalItemRecord *it = eq ? pal_db_find_item(ws->db, (int32_t)eq) : NULL;
            snprintf(buf, sizeof(buf), "%s[%s] %s%s",
                     col, kSlotNames[idx], it ? it->name_utf8 : "— 未裝備 —",
                     (it && sel) ? " (Enter卸下)" : "");
        } else if (cm->tab == 1 && cur_h) {
            uint16_t mid = cur_h->magics[idx];
            const PalMagicRecord *mg = pal_db_find_magic(ws->db, (int32_t)mid);
            int can_field = (mg && mg->field_script > 0 && mg->field_script != 61439);
            snprintf(buf, sizeof(buf), "%s%s  %%C3MP:%d %s",
                     col, mg ? mg->name_utf8 : "??",
                     mg ? mg->mp_cost : 0, can_field ? "%C0[日常]" : "");
        } else if (cm->tab == 2) {
            uint16_t iid = ws->inventory_ids[idx];
            int32_t qty = ws->inventory_qty[idx];
            const PalItemRecord *it = pal_db_find_item(ws->db, (int32_t)iid);
            cm->row_item_ids[r] = iid;
            int slot = pal_item_equip_slot((int32_t)iid);
            const char *badge = (slot >= 0) ? ((cur_h && pal_item_can_hero_equip(it, cur_h->hero_id)) ? "%C0[可裝]" : "%C1[限角]")
                                            : ((it && it->field_script > 0 && it->field_script != 61439) ? "%C0[可用]" : "");
            snprintf(buf, sizeof(buf), "%s%s  x%d  %s",
                     col, it ? it->name_utf8 : "??", qty, badge);
        } else {
            const char *state_tag = "";
            if (idx == 0) state_tag = party_follow_enabled ? "%C0[開啟]" : "%C1[關閉]";
            else if (idx == 1) state_tag = runtime_mute ? "%C1[靜音]" : "%C0[開啟]";
            else if (idx == 2) state_tag = show_collision_grid ? "%C0[顯示]" : "%C5[隱藏]";
            snprintf(buf, sizeof(buf), "%s%s  %s", col, kSysLabels[idx], state_tag);
        }
        int row_max_w = (cm->tab == 2) ? 336 : 368;
        cm->row_tex[r] = create_richtext_texture(renderer, buf, 14.5f, row_max_w, &cm->row_w[r], &cm->row_h[r]);
        cm->row_vis_count++;
    }

    char detail[320] = {0};
    if (cm->tab == 0 && cur_h && cm->cursor < PAL_EQUIP_SLOT_COUNT) {
        const PalItemRecord *it = cur_h->equips[cm->cursor] ? pal_db_find_item(ws->db, (int32_t)cur_h->equips[cm->cursor]) : NULL;
        snprintf(detail, sizeof(detail), "%s", (it && it->desc_utf8[0]) ? it->desc_utf8 : "選擇裝備欄位按 Enter 可卸下裝備回行囊。");
    } else if (cm->tab == 1 && cur_h && cm->cursor < cur_h->magic_count) {
        const PalMagicRecord *mg = pal_db_find_magic(ws->db, (int32_t)cur_h->magics[cm->cursor]);
        snprintf(detail, sizeof(detail), "%s", (mg && mg->desc_utf8[0]) ? mg->desc_utf8 : "選擇日常恢復仙術按 Enter 施放。");
    } else if (cm->tab == 2 && cm->cursor < ws->inventory_count) {
        const PalItemRecord *it = pal_db_find_item(ws->db, (int32_t)ws->inventory_ids[cm->cursor]);
        snprintf(detail, sizeof(detail), "%s", (it && it->desc_utf8[0]) ? it->desc_utf8 : "按 Enter 使用消耗品或替當前主角換裝。");
    } else {
        snprintf(detail, sizeof(detail), "調整引擎功能與系統狀態；設定將即時生效。");
    }
    if (cm->feedback_timer > 0.0f && cm->feedback[0] != '\0') {
        snprintf(buf, sizeof(buf), "%%C6%s%%N%%C5%s", cm->feedback, detail);
    } else {
        snprintf(buf, sizeof(buf), "%%C5%s%%N%%C3[←/→]分頁  [↑/↓]選擇  [Q/Tab]換人  [Enter]執行  [Esc/I]關閉", detail);
    }
    cm->desc_tex = create_richtext_texture(renderer, buf, 13.5f, 560, &cm->desc_w, &cm->desc_h);
    cm->dirty = 0;
}

static void camp_draw_overlay(
    SDL_Renderer *renderer, const char *game_dir, SDL_ScaleMode scale_mode,
    PalTswCache *tsw_cache, PalItemIconCache *icon_cache, const PalLoadedSprite *menu_cursor,
    const PalTalkWorldState *ws, PalCampMenuState *cm,
    int party_follow_enabled, int runtime_mute, int show_collision_grid, int anim_tick
) {
    if (!renderer || !ws || !cm || !cm->open) return;
    camp_rebuild_textures(renderer, ws, cm, party_follow_enabled, runtime_mute, show_collision_grid);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 6, 9, 14, 185);
    SDL_FRect dim = {0.0f, 0.0f, 640.0f, 480.0f};
    SDL_RenderFillRect(renderer, &dim);

    /* Main lacquer panels */
    SDL_FRect top_bar = {24.0f, 18.0f, 592.0f, 46.0f};
    SDL_FRect left_box = {24.0f, 70.0f, 186.0f, 296.0f};
    SDL_FRect right_box = {216.0f, 70.0f, 400.0f, 296.0f};
    SDL_FRect desc_box = {24.0f, 372.0f, 592.0f, 88.0f};
    SDL_SetRenderDrawColor(renderer, 16, 26, 38, 228);
    SDL_RenderFillRect(renderer, &top_bar);
    SDL_RenderFillRect(renderer, &left_box);
    SDL_RenderFillRect(renderer, &right_box);
    SDL_RenderFillRect(renderer, &desc_box);
    SDL_SetRenderDrawColor(renderer, 212, 178, 112, 235);
    SDL_RenderRect(renderer, &top_bar);
    SDL_RenderRect(renderer, &left_box);
    SDL_RenderRect(renderer, &right_box);
    SDL_RenderRect(renderer, &desc_box);

    /* Authentic all_sys.tsw tab / portrait art (id 2,3,6,8,15,18,70..73) */
    static const int kTabSysTsw[4] = {9003, 9015, 9008, 9006};
    static const char *kTabLabels[4] = {"狀態 Status", "仙術 Magic", "物品 Items", "系統 System"};
    for (int t = 0; t < 4; t++) {
        SDL_FRect tb = {CAMP_TAB_X0 + (float)t * CAMP_TAB_STEP, CAMP_TAB_Y0, CAMP_TAB_W, CAMP_TAB_H};
        SDL_SetRenderDrawColor(renderer, t == cm->tab ? 64 : 24, t == cm->tab ? 84 : 34, t == cm->tab ? 104 : 52, 240);
        SDL_RenderFillRect(renderer, &tb);
        SDL_SetRenderDrawColor(renderer, t == cm->tab ? 248 : 140, t == cm->tab ? 210 : 120, t == cm->tab ? 120 : 88, 255);
        SDL_RenderRect(renderer, &tb);
        PalLoadedSprite *tsp = tsw_cache_get(tsw_cache, game_dir, renderer, scale_mode, kTabSysTsw[t]);
        if (tsp && tsp->nframes > 0 && tsp->frames[0]) {
            SDL_FRect icon_dst = {tb.x + 4.0f, tb.y + 4.0f, 22.0f, 22.0f};
            SDL_RenderTexture(renderer, tsp->frames[0], NULL, &icon_dst);
        }
        SDL_SetRenderDrawColor(renderer, t == cm->tab ? 255 : 210, t == cm->tab ? 235 : 210, t == cm->tab ? 150 : 210, 255);
        SDL_RenderDebugTextFormat(renderer, tb.x + 30.0f, tb.y + 11.0f, "%s",
                                  t == 0 ? "STATUS" : (t == 1 ? "MAGIC" : (t == 2 ? "ITEMS" : "SYSTEM")));
        (void)kTabLabels;
    }
    if (cm->top_tex) {
        SDL_FRect mpos = {432.0f, 30.0f, (float)cm->top_w, (float)cm->top_h};
        SDL_RenderTexture(renderer, cm->top_tex, NULL, &mpos);
    }

    /* Left Panel: Selected Hero Portrait + Party Members */
    PalLoadedSprite *bg_card = tsw_cache_get(tsw_cache, game_dir, renderer, scale_mode, 9018);
    if (bg_card && bg_card->nframes > 0 && bg_card->frames[0]) {
        SDL_FRect bdst = {38.0f, 76.0f, 158.0f, 138.0f};
        SDL_SetTextureAlphaMod(bg_card->frames[0], 110);
        SDL_RenderTexture(renderer, bg_card->frames[0], NULL, &bdst);
        SDL_SetTextureAlphaMod(bg_card->frames[0], 255);
    }
    if (ws->party_member_count > 0) {
        int hid = ws->party_members[cm->hero_idx].hero_id;
        int port_sid = 9070 + ((hid >= 2000 && hid <= 2003) ? (hid - 2000) : 0);
        PalLoadedSprite *port = tsw_cache_get(tsw_cache, game_dir, renderer, scale_mode, port_sid);
        if (port && port->nframes > 0 && port->frames[0] && port->w > 0 && port->h > 0) {
            float scale = 132.0f / (float)(port->h > port->w ? port->h : port->w);
            if (scale > 1.0f) scale = 1.0f;
            float pw = (float)port->w * scale;
            float ph = (float)port->h * scale;
            SDL_FRect pdst = {38.0f + (158.0f - pw) * 0.5f, 76.0f + (136.0f - ph) * 0.5f, pw, ph};
            SDL_RenderTexture(renderer, port->frames[0], NULL, &pdst);
        }
    }
    for (int i = 0; i < ws->party_member_count && i < 4; i++) {
        SDL_FRect hb = {CAMP_HERO_X, CAMP_HERO_Y0 + (float)i * CAMP_HERO_STEP, CAMP_HERO_W, CAMP_HERO_H};
        SDL_SetRenderDrawColor(renderer, i == cm->hero_idx ? 54 : 22, i == cm->hero_idx ? 72 : 30, i == cm->hero_idx ? 92 : 44, 230);
        SDL_RenderFillRect(renderer, &hb);
        SDL_SetRenderDrawColor(renderer, i == cm->hero_idx ? 245 : 100, i == cm->hero_idx ? 205 : 120, i == cm->hero_idx ? 105 : 135, 255);
        SDL_RenderRect(renderer, &hb);
        if (cm->party_tex[i]) {
            SDL_FRect td = {hb.x + 6.0f, hb.y + 4.0f, (float)cm->party_w[i], (float)cm->party_h[i]};
            SDL_RenderTexture(renderer, cm->party_tex[i], NULL, &td);
        }
    }

    /* Right Panel: Header & Interactive Rows */
    if (cm->header_tex) {
        SDL_FRect hd = {226.0f, 78.0f, (float)cm->header_w, (float)cm->header_h};
        SDL_RenderTexture(renderer, cm->header_tex, NULL, &hd);
    }
    SDL_SetRenderDrawColor(renderer, 160, 140, 95, 160);
    SDL_RenderLine(renderer, 224.0f, 108.0f, 608.0f, 108.0f);

    for (int r = 0; r < cm->row_vis_count; r++) {
        int idx = cm->scroll + r;
        float ry = CAMP_ROW_Y0 + (float)r * CAMP_ROW_STEP;
        SDL_FRect rb = {CAMP_ROW_X, ry, CAMP_ROW_W, CAMP_ROW_H};
        if (idx == cm->cursor) {
            SDL_SetRenderDrawColor(renderer, 58, 78, 102, 220);
            SDL_RenderFillRect(renderer, &rb);
            SDL_SetRenderDrawColor(renderer, 232, 198, 118, 255);
            SDL_RenderRect(renderer, &rb);
            if (menu_cursor && menu_cursor->nframes > 0) {
                int cfi = (anim_tick / 3) % menu_cursor->nframes;
                SDL_FRect cd = {CAMP_ROW_X - 10.0f, ry + 4.0f, (float)menu_cursor->w * 0.7f, (float)menu_cursor->h * 0.7f};
                SDL_RenderTexture(renderer, menu_cursor->frames[cfi], NULL, &cd);
            }
        }
        float tx = CAMP_ROW_X + 8.0f;
        if (cm->tab == 2 && cm->row_item_ids[r] != 0u) {
            int ifi = 0;
            PalLoadedSprite *isp = camp_get_item_icon(icon_cache, tsw_cache, game_dir, renderer, scale_mode, cm->row_item_ids[r], &ifi);
            if (isp && isp->nframes > 0) {
                int ff = (ifi >= 0) ? (ifi % isp->nframes) : 0;
                SDL_FRect idst = {CAMP_ROW_X + 6.0f, ry + 1.0f, 24.0f, 24.0f};
                SDL_RenderTexture(renderer, isp->frames[ff], NULL, &idst);
            }
            tx = CAMP_ROW_X + 36.0f;
        }
        if (cm->row_tex[r]) {
            SDL_FRect td = {tx, ry + 4.0f, (float)cm->row_w[r], (float)cm->row_h[r]};
            SDL_RenderTexture(renderer, cm->row_tex[r], NULL, &td);
        }
    }

    /* Bottom Panel: Description / Feedback */
    if (cm->desc_tex) {
        SDL_FRect dd = {36.0f, 380.0f, (float)cm->desc_w, (float)cm->desc_h};
        SDL_RenderTexture(renderer, cm->desc_tex, NULL, &dd);
    }
}

static void compute_field_camera(const PalLoadedMap *m, int px, int py, float *out_cx, float *out_cy) {
    int cx = px - 304, cy = py - 240;
    if (m->w_px > 640) {
        if (cx < 0) cx = 0;
        if (cx > m->w_px - 640) cx = m->w_px - 640;
    } else {
        cx = -(640 - m->w_px) / 2;
    }
    if (m->h_px > 480) {
        if (cy < 0) cy = 0;
        if (cy > m->h_px - 480) cy = m->h_px - 480;
    } else {
        cy = -(480 - m->h_px) / 2;
    }
    if (out_cx) *out_cx = (float)cx;
    if (out_cy) *out_cy = (float)cy;
}

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

        /* M2 Automated Probes: Map 1 (盛漁村), Map 2 (盛漁村市場), Map 501 (羅剎峰) + Char .act VMs */
        PalMapM2ProbeInfo m2_1 = {0}, m2_2 = {0}, m2_501 = {0};
        int m2_1_ok = (pal_map_probe_m2(cfg->game_dir, 1, &m2_1) == 0 &&
                       m2_1.width_tiles == 40 && m2_1.height_tiles == 30 &&
                       m2_1.walkable_cells > 0 && m2_1.blocked_cells > 0 &&
                       m2_1.trigger_count == 2 && m2_1.fg_sprite_defs == 14 &&
                       m2_1.fg_sprite_instances == 14 && m2_1.astar_path_steps > 0);
        int m2_2_ok = (pal_map_probe_m2(cfg->game_dir, 2, &m2_2) == 0 &&
                       m2_2.width_tiles == 60 && m2_2.height_tiles == 45 &&
                       m2_2.trigger_count == 10 && m2_2.astar_path_steps > 0);
        int m2_501_ok = (pal_map_probe_m2(cfg->game_dir, 501, &m2_501) == 0 &&
                         m2_501.width_tiles == 40 && m2_501.height_tiles == 30 &&
                         m2_501.walkable_cells > 0);

        PalLoadedMap m2_render_map = {0};
        int m2_render_ok = (vfs_load_lmf_map_by_id(cfg->game_dir, 1, renderer, scale_mode, &m2_render_map) == 0 &&
                            m2_render_map.texture != NULL && m2_render_map.fg_def_count == 14);

        PalActVM char_stand_vm = {0}, char_walk_vm = {0}, char_h2_vm = {0}, char_h3_vm = {0}, char_h4_vm = {0};
        int char_act_ok = (pal_act_vm_init(cfg->game_dir, "all_char.act", 1, 0, &char_stand_vm) == 0 &&
                           pal_act_vm_init(cfg->game_dir, "all_char.act", 1, 8, &char_walk_vm) == 0 &&
                           pal_act_vm_init(cfg->game_dir, "all_char.act", 2, 8, &char_h2_vm) == 0 &&
                           pal_act_vm_init(cfg->game_dir, "all_char.act", 3, 8, &char_h3_vm) == 0 &&
                           pal_act_vm_init(cfg->game_dir, "all_char.act", 4, 8, &char_h4_vm) == 0);
        char_walk_vm.loop_remaining = -1;
        int walk_frames_changed = 0;
        for (int t = 0; t < 24; t++) {
            int pf = char_walk_vm.frame_index;
            pal_act_vm_step_tick(&char_walk_vm);
            if (char_walk_vm.frame_index != pf) walk_frames_changed++;
        }
        char_act_ok = char_act_ok && (char_stand_vm.fr_tsw_id == 187 && char_walk_vm.fr_tsw_id == 208 && walk_frames_changed >= 4);

        /* Render Title composition, M1 Field, & M2 Full Y-sorted Occlusion onto Metal swapchain */
        for (int f = 0; f < 3; f++) {
            SDL_SetRenderDrawColor(renderer, 8, 10, 14, 255);
            SDL_RenderClear(renderer);
            if (f == 0) {
                draw_act_sprite_node(renderer, &title_bg, 0);
                draw_act_sprite_node(renderer, &title_btns[0], 0);
            } else if (f == 1 && m2_render_ok) {
                SDL_FRect dst = {0.0f, 0.0f, 640.0f, 480.0f};
                SDL_RenderTexture(renderer, m2_render_map.texture, NULL, &dst);
                for (int k = 0; k < m2_render_map.fg_inst_count; k++) {
                    const PalMapFgInst *inst = &m2_render_map.fg_insts[k];
                    for (int d = 0; d < m2_render_map.fg_def_count; d++) {
                        if (m2_render_map.fg_defs[d].sid == inst->sid && m2_render_map.fg_defs[d].tex) {
                            SDL_FRect fd = {(float)inst->px, (float)inst->py_top, (float)m2_render_map.fg_defs[d].w, (float)m2_render_map.fg_defs[d].h};
                            SDL_RenderTexture(renderer, m2_render_map.fg_defs[d].tex, NULL, &fd);
                            break;
                        }
                    }
                }
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
        free_loaded_map(&m2_render_map);

        int backend_ok = (active_backend && strcmp(active_backend, "metal") == 0);
        int title_assets_ok = (act_bg_ok && act_btn_ok && title_bg.sprite.nframes > 0 &&
                               title_btns[0].sprite.nframes == 15 && menu_cursor.nframes > 0 && dlg_box.nframes > 0);
        int m2_ok = m2_1_ok && m2_2_ok && m2_501_ok && m2_render_ok && char_act_ok;
        PalTalkM3ProbeInfo m3_info = {0};
        int m3_ok = (pal_talk_vm_probe_m3(cfg->game_dir, &m3_info) == 0 && m3_info.ok == 1);
        PalDbM4ProbeInfo m4_info = {0};
        int m4_ok = (pal_db_menu_probe_m4(cfg->game_dir, &m4_info) == 0 && m4_info.ok == 1);

        printf("=== NewPal SDL3 Metal + BYOD VFS Self-Test (M0+M1+M2+M3+M4) ===\n");
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

        printf("MapM2[1]: ok=%d title=%s size=%dx%d walkable=%d blocked=%d triggers=%d fg=%d/%d npcs=%d astar_steps=%d bgm=%s\n",
               m2_1_ok, m2_1.map_title_utf8, m2_1.width_tiles, m2_1.height_tiles,
               m2_1.walkable_cells, m2_1.blocked_cells, m2_1.trigger_count,
               m2_1.fg_sprite_defs, m2_1.fg_sprite_instances, m2_1.npc_count,
               m2_1.astar_path_steps, m2_1.bgm_track);
        printf("MapM2[2]: ok=%d title=%s size=%dx%d walkable=%d blocked=%d triggers=%d astar_steps=%d bgm=%s\n",
               m2_2_ok, m2_2.map_title_utf8, m2_2.width_tiles, m2_2.height_tiles,
               m2_2.walkable_cells, m2_2.blocked_cells, m2_2.trigger_count,
               m2_2.astar_path_steps, m2_2.bgm_track);
        printf("MapM2[501]: ok=%d title=%s size=%dx%d walkable=%d blocked=%d npcs=%d bgm=%s\n",
               m2_501_ok, m2_501.map_title_utf8, m2_501.width_tiles, m2_501.height_tiles,
               m2_501.walkable_cells, m2_501.blocked_cells, m2_501.npc_count, m2_501.bgm_track);
        printf("CharActVM[1..4]: ok=%d stand_fr=%d(anchor=%d,%d) walk_fr=%d(delay=%d,changes=%d) party_walk_frs=(%d,%d,%d)\n",
               char_act_ok, char_stand_vm.fr_tsw_id, char_stand_vm.offset_x, char_stand_vm.offset_y,
               char_walk_vm.fr_tsw_id, char_walk_vm.duration_ticks, walk_frames_changed,
               char_h2_vm.fr_tsw_id, char_h3_vm.fr_tsw_id, char_h4_vm.fr_tsw_id);
        printf("TalkM3: ok=%d banks=%d scripts=%d/%d tp=(map:%d,%d,%d,dir:%d) chest=(hidden:%d,item:%d,txt:%s) cut104=(flag1001:%d,portraits:%d,dlgs:%d) cut105=(updates:%d,obj7:%d,obj201:%d)\n",
               m3_ok, m3_info.bank_count, m3_info.clean_scripts, m3_info.total_scripts,
               m3_info.tp_map_id, m3_info.tp_tx, m3_info.tp_ty, m3_info.tp_dir,
               m3_info.chest_hidden, m3_info.chest_item_id, m3_info.chest_text_utf8,
               m3_info.cutscene_flag1001, m3_info.cutscene_portrait_count, m3_info.cutscene_dialogue_count,
               m3_info.cutscene_npc_updates, m3_info.npc_update_obj7_talk, m3_info.npc_update_obj201_talk);
        printf("DbM4: ok=%d heroes=%d items=%d magics=%d levels=%d h2000=(lv1_hp:%d,eq_atk:%d,eq_def:%d,item_hp:%d,mag_hp:%d,lv2_hp:%d) chest309=(id:%d,name:%s)\n",
               m4_ok, m4_info.hero_count, m4_info.item_count, m4_info.magic_count, m4_info.level_template_count,
               m4_info.hero2000_lv1_hp, m4_info.hero2000_equip_atk, m4_info.hero2000_equip_def,
               m4_info.item4401_healed_hp, m4_info.magic6901_healed_hp, m4_info.hero2000_lv2_hp,
               m4_info.chest7012_granted_item_id, m4_info.chest7012_item_name_utf8);

        int all_ok = backend_ok && report.is_valid && act_rc == 0 && snd_ok &&
                     bik_ok && bgm_title_ok && title_assets_ok && font_ok && rt_tex != NULL && m2_ok && m3_ok && m4_ok;
        printf("[SelfTest] ALL CHECKS %s.\n", all_ok ? "PASSED" : "FAILED");
        return all_ok ? 0 : -1;
    }

    /* =====================================================================
     * Interactive Boot Flow: Title Screen directly (0x440348 -> 0x4983ac(-9))
     * ===================================================================== */
    SDL_Texture *scene_rt = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 640, 480);
    if (scene_rt) {
        SDL_SetTextureScaleMode(scene_rt, scale_mode);
        SDL_SetTextureBlendMode(scene_rt, SDL_BLENDMODE_NONE);
        SDL_SetRenderTarget(renderer, scene_rt);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderTarget(renderer, NULL);
    }

    PalBgmPlayer bgm = {0};
    SDL_AudioStream *sfx_stream = NULL;
    int app_quit = 0;
    int runtime_mute = cfg->mute_audio;

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
    int show_collision_grid = 0;
    int party_follow_enabled = 1;
    int noclip_mode = 0;
    int run_mode_enabled = 0;
    int pending_fade_in_ms = 0;
    int quick_map_pos = 0;
    uint32_t leader_hero_idx = 0; /* 0..3 -> 李逍遙, 趙靈兒, 林月如, 阿奴 */

    show_dialogue = 0; /* Unobstructed M2 field view by default; press T to toggle dialogue sample */

    PalTswCache tsw_cache = {0};
    PalItemIconCache item_icon_cache = {0};
    PalCampMenuState camp_menu = {0};
    camp_menu.dirty = 1;
    PalFieldActor party[4] = {{0}};
    PalFieldActor map_npcs[64] = {{0}};
    int map_npc_actor_cnt = 0;

    PalGameDatabase *game_db = (PalGameDatabase *)calloc(1, sizeof(PalGameDatabase));
    PalTalkWorldState *world_state = (PalTalkWorldState *)calloc(1, sizeof(PalTalkWorldState));
    if (game_db && world_state && pal_db_load(cfg->game_dir, game_db) == 0) {
        pal_party_init_from_db(world_state, game_db, 0);
    }
    PalTalkVM talk_vm = {0};
    talk_vm.pending_map_id = -1;
    int pending_spawn_override = 0, spawn_override_tx = 0, spawn_override_ty = 0, spawn_override_dir = 1;
    uint8_t last_stepped_trig_id = 0;
    int interact_target_npc_idx = -1;
    int dlg_text_x = 44;

    int trail_x[64] = {0}, trail_y[64] = {0}, trail_dir[64] = {0};
    int astar_tx[512] = {0}, astar_ty[512] = {0};
    int astar_count = 0, astar_cursor = 0;

    SDL_Texture *map_banner_tex = NULL;
    int map_banner_w = 0, map_banner_h = 0;
    float map_banner_timer = 0.0f;
    uint8_t active_trig_id = 0;
    char cur_bgm_playing[64] = {0};

    /* Initial map placement helper */
    {
        int stx = 0, sty = 0;
        pick_map_spawn_tile(&map, &stx, &sty);
        for (int p = 0; p < 4; p++) {
            party[p].px = stx << 4;
            party[p].py = sty << 4;
            party[p].dir = 1;
            field_actor_set_sub(&party[p], cfg->game_dir, kPartyHeroActIds[(leader_hero_idx + (uint32_t)p) % 4u], 1, 0);
        }
        for (int t = 0; t < 64; t++) {
            trail_x[t] = party[0].px;
            trail_y[t] = party[0].py;
            trail_dir[t] = 1;
        }
    }

    /* Populate initial map NPCs + banner */
    map_npc_actor_cnt = map.npc_count < 64 ? map.npc_count : 64;
    for (int i = 0; i < map_npc_actor_cnt; i++) {
        const PalMapNpcDef *nd = &map.npcs[i];
        map_npcs[i].obj_id = nd->obj_id;
        map_npcs[i].talk_id = (world_state && nd->obj_id < PAL_OBJ_CAP && world_state->npc_talk_override[nd->obj_id] != 0u)
                                  ? world_state->npc_talk_override[nd->obj_id]
                                  : nd->talk_id;
        map_npcs[i].npc_flags = nd->flags;
        map_npcs[i].orig_sub_state = nd->sub_state;
        map_npcs[i].active = (nd->flags & 0x8000u) != 0 ? 1 : 0;
        if (world_state && nd->obj_id < PAL_OBJ_CAP) {
            if (world_state->npc_visibility_override[nd->obj_id] == 1u) map_npcs[i].active = 1;
            else if (world_state->npc_visibility_override[nd->obj_id] == 2u) map_npcs[i].active = 0;
        }
        map_npcs[i].px = (int)nd->tx << 4;
        map_npcs[i].py = (int)nd->ty << 4;
        map_npcs[i].dir = (int)(nd->dir & 7u);
        if (map_npcs[i].active) {
            field_actor_set_sub(&map_npcs[i], cfg->game_dir, nd->act_id, (int)nd->sub_state + (int)nd->dir, 1);
        }
    }

    float cam_x = 0.0f, cam_y = 0.0f;
    compute_field_camera(&map, party[0].px, party[0].py, &cam_x, &cam_y);

    float mouse_lx = 320.0f, mouse_ly = 298.0f;

    if (!app_quit) {
        pal_bgm_play_track(&bgm, cfg->game_dir, "Music/M001.mp3", cfg->master_volume, runtime_mute);
        snprintf(cur_bgm_playing, sizeof(cur_bgm_playing), "Music/M001.mp3");
        SDL_HideCursor();
    }

    /* Inline macro-like state reset block via helper lambda pattern avoided -> simple local helper flag */
    int need_setup_loaded_map = 0;

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
                        play_ui_sfx(cfg->game_dir, 1, &sfx_stream, cfg->master_volume, runtime_mute);
                    }
                    break;
                }
            }
        }

        int activate_btn = -1;
        int request_return_title = 0;
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
                        play_ui_sfx(cfg->game_dir, 1, &sfx_stream, cfg->master_volume, runtime_mute);
                    } else if (key == SDLK_DOWN || key == SDLK_S) {
                        selected_btn = (selected_btn + 1) % 3;
                        play_ui_sfx(cfg->game_dir, 1, &sfx_stream, cfg->master_volume, runtime_mute);
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
                    if ((key == SDLK_I || key == SDLK_U || key == SDLK_F1) && !talk_vm.active) {
                        camp_menu.open = !camp_menu.open;
                        camp_menu.dirty = 1;
                        astar_count = 0; astar_cursor = 0; interact_target_npc_idx = -1;
                        play_ui_sfx(cfg->game_dir, camp_menu.open ? 2u : 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                    } else if (camp_menu.open) {
                        int nparty = (world_state && world_state->party_member_count > 0) ? world_state->party_member_count : 1;
                        if (key == SDLK_ESCAPE) {
                            camp_menu.open = 0;
                            play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                        } else if (key == SDLK_LEFT || key == SDLK_A) {
                            camp_menu.tab = (camp_menu.tab + 3) % 4;
                            camp_menu.cursor = 0; camp_menu.scroll = 0; camp_menu.dirty = 1;
                            play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                        } else if (key == SDLK_RIGHT || key == SDLK_D) {
                            camp_menu.tab = (camp_menu.tab + 1) % 4;
                            camp_menu.cursor = 0; camp_menu.scroll = 0; camp_menu.dirty = 1;
                            play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                        } else if (key == SDLK_Q) {
                            camp_menu.hero_idx = (camp_menu.hero_idx + nparty - 1) % nparty;
                            camp_menu.cursor = 0; camp_menu.scroll = 0; camp_menu.dirty = 1;
                            play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                        } else if (key == SDLK_R || key == SDLK_TAB) {
                            camp_menu.hero_idx = (camp_menu.hero_idx + 1) % nparty;
                            camp_menu.cursor = 0; camp_menu.scroll = 0; camp_menu.dirty = 1;
                            play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                        } else if (key == SDLK_UP || key == SDLK_W) {
                            camp_menu.cursor--;
                            camp_clamp_cursor(world_state, &camp_menu);
                            camp_menu.dirty = 1;
                            play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                        } else if (key == SDLK_DOWN || key == SDLK_S) {
                            camp_menu.cursor++;
                            camp_clamp_cursor(world_state, &camp_menu);
                            camp_menu.dirty = 1;
                            play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                        } else if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_E) {
                            camp_activate_row(&camp_menu, world_state, cfg->game_dir,
                                              &party_follow_enabled, &runtime_mute, &show_collision_grid,
                                              &bgm, &sfx_stream, cfg->master_volume, &request_return_title);
                        }
                    } else if (key == SDLK_ESCAPE) {
                        request_return_title = 1;
                    } else if (key == SDLK_TAB) {
                        show_hud = !show_hud;
                    } else if (key == SDLK_G) {
                        show_collision_grid = !show_collision_grid;
                    } else if (key == SDLK_P) {
                        party_follow_enabled = !party_follow_enabled;
                    } else if (key == SDLK_C) {
                        noclip_mode = !noclip_mode;
                    } else if ((key == SDLK_LSHIFT || key == SDLK_RSHIFT ||
                                ev.key.scancode == SDL_SCANCODE_LSHIFT || ev.key.scancode == SDL_SCANCODE_RSHIFT) &&
                               !ev.key.repeat) {
                        run_mode_enabled = !run_mode_enabled;
                    } else if (key == SDLK_V) {
                        leader_hero_idx = (leader_hero_idx + 1u) % 4u;
                        for (int p = 0; p < 4; p++) {
                            field_actor_set_sub(&party[p], cfg->game_dir,
                                                kPartyHeroActIds[(leader_hero_idx + (uint32_t)p) % 4u],
                                                party[p].dir, 0);
                        }
                    } else if (key == SDLK_T && !talk_vm.active) {
                        show_dialogue = !show_dialogue;
                        dlg_text_x = 44;
                    } else if (key == SDLK_B && !talk_vm.active) {
                        SDL_PauseAudioStreamDevice(bgm.stream);
                        if (pal_bink_play_file(window, renderer, cfg->game_dir, "Video/begin.bik", cfg->master_volume, runtime_mute) == 1) {
                            app_quit = 1;
                        }
                        if (bgm.stream) SDL_ResumeAudioStreamDevice(bgm.stream);
                        pending_fade_in_ms = 140;
                        last_ns = SDL_GetTicksNS();
                    } else if (talk_vm.active && talk_vm.waiting_input) {
                        if (talk_vm.choice_count > 0 &&
                            (key == SDLK_UP || key == SDLK_W || key == SDLK_LEFT || key == SDLK_A)) {
                            talk_vm.choice_selected = (talk_vm.choice_selected + talk_vm.choice_count - 1) % talk_vm.choice_count;
                            talk_vm.dlg_dirty = 1;
                            play_ui_sfx(cfg->game_dir, 1, &sfx_stream, cfg->master_volume, runtime_mute);
                        } else if (talk_vm.choice_count > 0 &&
                                   (key == SDLK_DOWN || key == SDLK_S || key == SDLK_RIGHT || key == SDLK_D)) {
                            talk_vm.choice_selected = (talk_vm.choice_selected + 1) % talk_vm.choice_count;
                            talk_vm.dlg_dirty = 1;
                            play_ui_sfx(cfg->game_dir, 1, &sfx_stream, cfg->master_volume, runtime_mute);
                        } else if (talk_vm.choice_count > 0 && key >= SDLK_1 && key <= SDLK_8) {
                            int idx = (int)(key - SDLK_1);
                            if (idx < talk_vm.choice_count) {
                                talk_vm.choice_selected = idx;
                                play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, runtime_mute);
                                if (world_state) {
                                    pal_talk_vm_advance_input(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt);
                                }
                            }
                        } else if (key == SDLK_SPACE || key == SDLK_RETURN || key == SDLK_E) {
                            play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, runtime_mute);
                            if (world_state) {
                                pal_talk_vm_advance_input(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt);
                            }
                        }
                    } else if ((key == SDLK_SPACE || key == SDLK_RETURN || key == SDLK_E) && !talk_vm.active) {
                        int nidx = talk_find_interact_npc(&party[0], map_npcs, map_npc_actor_cnt);
                        if (nidx >= 0) {
                            astar_count = 0; astar_cursor = 0; interact_target_npc_idx = -1;
                            talk_begin_npc_interaction(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt, nidx, &show_dialogue);
                            camp_menu.dirty = 1;
                        } else if (show_dialogue && dialogues.count > 0) {
                            dlg_idx = (dlg_idx + 1) % dialogues.count;
                            if (dlg_text_tex) SDL_DestroyTexture(dlg_text_tex);
                            dlg_text_tex = create_richtext_texture(renderer, dialogues.lines[dlg_idx], 18.0f, 520, &cur_dlg_w, &cur_dlg_h);
                            dlg_text_x = 44;
                            play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, runtime_mute);
                        }
                    } else if (key == SDLK_M && !talk_vm.active) {
                        quick_map_pos = (quick_map_pos + 1) % (int)(sizeof(kM2QuickMaps) / sizeof(kM2QuickMaps[0]));
                        PalLoadedMap next_map = {0};
                        if (vfs_load_lmf_map_by_id(cfg->game_dir, kM2QuickMaps[quick_map_pos], renderer, scale_mode, &next_map) == 0) {
                            free_loaded_map(&map);
                            map = next_map;
                            need_setup_loaded_map = 1;
                            last_ns = SDL_GetTicksNS();
                        }
                    } else if ((key == SDLK_RIGHTBRACKET || key == SDLK_LEFTBRACKET) && !talk_vm.active) {
                        int delta = (key == SDLK_RIGHTBRACKET) ? 1 : -1;
                        int total_cur = map.total_maps > 0 ? map.total_maps : 1;
                        map_idx = (map.map_idx + delta + total_cur) % total_cur;
                        PalLoadedMap next_map = {0};
                        if (vfs_load_lmf_map(cfg->game_dir, map.lmf_file[0] ? map.lmf_file : "huge.lmf", map_idx, renderer, scale_mode, &next_map) == 0) {
                            free_loaded_map(&map);
                            map = next_map;
                            need_setup_loaded_map = 1;
                            last_ns = SDL_GetTicksNS();
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
                } else if (scene == PAL_SCENE_FIELD && camp_menu.open) {
                    int handled = 0;
                    if (mouse_ly >= CAMP_TAB_Y0 && mouse_ly <= CAMP_TAB_Y0 + CAMP_TAB_H) {
                        for (int t = 0; t < 4; t++) {
                            float tx = CAMP_TAB_X0 + (float)t * CAMP_TAB_STEP;
                            if (mouse_lx >= tx && mouse_lx <= tx + CAMP_TAB_W) {
                                camp_menu.tab = t; camp_menu.cursor = 0; camp_menu.scroll = 0; camp_menu.dirty = 1;
                                play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                                handled = 1;
                                break;
                            }
                        }
                    }
                    if (!handled && mouse_lx >= CAMP_HERO_X && mouse_lx <= CAMP_HERO_X + CAMP_HERO_W && world_state) {
                        for (int i = 0; i < world_state->party_member_count && i < 4; i++) {
                            float hy = CAMP_HERO_Y0 + (float)i * CAMP_HERO_STEP;
                            if (mouse_ly >= hy && mouse_ly <= hy + CAMP_HERO_H) {
                                camp_menu.hero_idx = i; camp_menu.cursor = 0; camp_menu.scroll = 0; camp_menu.dirty = 1;
                                play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                                handled = 1;
                                break;
                            }
                        }
                    }
                    if (!handled && mouse_lx >= CAMP_ROW_X && mouse_lx <= CAMP_ROW_X + CAMP_ROW_W) {
                        for (int r = 0; r < camp_menu.row_vis_count; r++) {
                            float ry = CAMP_ROW_Y0 + (float)r * CAMP_ROW_STEP;
                            if (mouse_ly >= ry && mouse_ly <= ry + CAMP_ROW_H) {
                                int idx = camp_menu.scroll + r;
                                if (camp_menu.cursor == idx) {
                                    camp_activate_row(&camp_menu, world_state, cfg->game_dir,
                                                      &party_follow_enabled, &runtime_mute, &show_collision_grid,
                                                      &bgm, &sfx_stream, cfg->master_volume, &request_return_title);
                                } else {
                                    camp_menu.cursor = idx;
                                    camp_menu.dirty = 1;
                                    play_ui_sfx(cfg->game_dir, 1u, &sfx_stream, cfg->master_volume, runtime_mute);
                                }
                                break;
                            }
                        }
                    }
                } else if (scene == PAL_SCENE_FIELD) {
                    if (talk_vm.active && talk_vm.waiting_input) {
                        play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, runtime_mute);
                        if (world_state) {
                            pal_talk_vm_advance_input(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt);
                        }
                    } else if (show_dialogue && mouse_ly >= 336.0f && dialogues.count > 0 && !talk_vm.active) {
                        dlg_idx = (dlg_idx + 1) % dialogues.count;
                        if (dlg_text_tex) SDL_DestroyTexture(dlg_text_tex);
                        dlg_text_tex = create_richtext_texture(renderer, dialogues.lines[dlg_idx], 18.0f, 520, &cur_dlg_w, &cur_dlg_h);
                        dlg_text_x = 44;
                        play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, runtime_mute);
                    } else if (!talk_vm.active) {
                        int world_mx = (int)(mouse_lx + cam_x);
                        int world_my = (int)(mouse_ly + cam_y);
                        int hit_npc = talk_find_npc_at_world_pt(map_npcs, map_npc_actor_cnt, world_mx, world_my);
                        if (hit_npc >= 0) {
                            int dx = map_npcs[hit_npc].px - party[0].px;
                            int dy = map_npcs[hit_npc].py - party[0].py;
                            if (dx * dx + dy * dy <= 46 * 46) {
                                interact_target_npc_idx = -1;
                                astar_count = 0; astar_cursor = 0;
                                talk_begin_npc_interaction(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt, hit_npc, &show_dialogue);
                            } else {
                                interact_target_npc_idx = hit_npc;
                                int goal_tx = map_npcs[hit_npc].px >> 4;
                                int goal_ty = map_npcs[hit_npc].py >> 4;
                                astar_count = pal_map_find_path_astar(
                                    &map, party[0].px >> 4, party[0].py >> 4,
                                    goal_tx, goal_ty, 2, astar_tx, astar_ty, 512
                                );
                                astar_cursor = 0;
                            }
                        } else {
                            interact_target_npc_idx = -1;
                            int goal_tx = (world_mx >> 4) - 1;
                            int goal_ty = world_my >> 4;
                            if (goal_tx < 0) goal_tx = 0;
                            if (goal_ty < 0) goal_ty = 0;
                            astar_count = pal_map_find_path_astar(
                                &map, party[0].px >> 4, party[0].py >> 4,
                                goal_tx, goal_ty, 2, astar_tx, astar_ty, 512
                            );
                            astar_cursor = 0;
                        }
                    }
                }
            }
        }

        if (app_quit) break;

        if (request_return_title) {
            camp_menu.open = 0;
            camp_menu.dirty = 1;
            pal_talk_vm_free(&talk_vm);
            interact_target_npc_idx = -1;
            show_dialogue = 0;
            if (run_fade_transition(window, renderer, scene_rt, 0, 200, &bgm) == 1) { app_quit = 1; break; }
            scene = PAL_SCENE_TITLE;
            pending_fade_in_ms = 180;
            SDL_HideCursor();
            pal_bgm_play_track(&bgm, cfg->game_dir, "Music/M001.mp3", cfg->master_volume, runtime_mute);
            snprintf(cur_bgm_playing, sizeof(cur_bgm_playing), "Music/M001.mp3");
            last_ns = SDL_GetTicksNS();
        }

        if (activate_btn == 0) {
            play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, runtime_mute);
            if (run_fade_transition(window, renderer, scene_rt, 0, 220, &bgm) == 1) { app_quit = 1; break; }
            pal_bgm_stop(&bgm);
            /* Authentic talk1.dat talk_id=100 New Game sequence: map 532 -> map 533 -> begin.bik -> map 501 */
            if (play_lmf_splash_by_id(window, renderer, cfg->game_dir, 532, scale_mode, 350, 1400) == 1) {
                app_quit = 1;
                break;
            }
            if (play_lmf_splash_by_id(window, renderer, cfg->game_dir, 533, scale_mode, 350, 1400) == 1) {
                app_quit = 1;
                break;
            }
            if (pal_bink_play_file(window, renderer, cfg->game_dir, "Video/begin.bik",
                                   cfg->master_volume, runtime_mute) == 1) {
                app_quit = 1;
                break;
            }
            if (cfg->initial_map_index == 0) {
                PalLoadedMap next_map = {0};
                if (vfs_load_lmf_map_by_id(cfg->game_dir, 501u, renderer, scale_mode, &next_map) == 0) {
                    free_loaded_map(&map);
                    map = next_map;
                }
            }
            camp_menu.open = 0;
            camp_menu.dirty = 1;
            need_setup_loaded_map = 1;
            pending_fade_in_ms = 180;
            scene = PAL_SCENE_FIELD;
            SDL_ShowCursor();
            last_ns = SDL_GetTicksNS();
        } else if (activate_btn == 1) {
            play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, runtime_mute);
            scene = PAL_SCENE_LOAD_OVERLAY;
        } else if (activate_btn == 2) {
            play_ui_sfx(cfg->game_dir, 2, &sfx_stream, cfg->master_volume, runtime_mute);
            SDL_Delay(120);
            app_quit = 1;
            break;
        }

        Uint64 now_ns = SDL_GetTicksNS();
        float dt = (float)(now_ns - last_ns) / 1e9f;
        if (dt > 0.1f) dt = 0.1f;
        last_ns = now_ns;
        if (map_banner_timer > 0.0f) {
            map_banner_timer -= dt;
            if (map_banner_timer < 0.0f) map_banner_timer = 0.0f;
        }
        if (camp_menu.feedback_timer > 0.0f) {
            camp_menu.feedback_timer -= dt;
            if (camp_menu.feedback_timer <= 0.0f) {
                camp_menu.feedback_timer = 0.0f;
                camp_menu.feedback[0] = '\0';
                camp_menu.dirty = 1;
            }
        }

        /* 35ms authentic SWD2k engine tick (character walking, collision, party trail, .act VMs) */
        if (need_setup_loaded_map) {
            tick35_acc = 0.0;
        } else {
            tick35_acc += (double)dt;
        }
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
            } else if (scene == PAL_SCENE_FIELD) {
                if (talk_vm.active) {
                    for (int p = 0; p < PAL_MAX_PORTRAITS; p++) {
                        if (talk_vm.portraits[p].active && talk_vm.portraits[p].vm_ok && !talk_vm.portraits[p].vm.finished) {
                            pal_act_vm_step_tick(&talk_vm.portraits[p].vm);
                        }
                    }
                    if (talk_vm.wait_ticks > 0) {
                        talk_vm.wait_ticks--;
                        if (talk_vm.wait_ticks == 0 && world_state) {
                            pal_talk_vm_step(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt, 0);
                        }
                    }
                }

                const bool *keys = SDL_GetKeyboardState(NULL);
                int kx = 0, ky = 0;
                if (!talk_vm.active && !camp_menu.open) {
                    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])  kx -= 1;
                    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) kx += 1;
                    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])    ky -= 1;
                    if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])  ky += 1;
                }
                int running = run_mode_enabled ? 1 : 0;
                int step_px = running ? (map.step_speed_px * 2) : map.step_speed_px;
                if (step_px <= 0) step_px = running ? 8 : 4;

                int move_dx = 0, move_dy = 0;
                if (kx != 0 || ky != 0) {
                    /* Manual keyboard cancels click A* path */
                    astar_count = 0;
                    astar_cursor = 0;
                    interact_target_npc_idx = -1;
                    move_dx = kx * step_px;
                    move_dy = ky * step_px;
                } else if (!talk_vm.active && !camp_menu.open && astar_cursor < astar_count) {
                    int wtx = astar_tx[astar_cursor] << 4;
                    int wty = astar_ty[astar_cursor] << 4;
                    int ddx = wtx - party[0].px;
                    int ddy = wty - party[0].py;
                    if (abs(ddx) <= step_px && abs(ddy) <= step_px) {
                        party[0].px = wtx;
                        party[0].py = wty;
                        astar_cursor++;
                        if (astar_cursor < astar_count) {
                            wtx = astar_tx[astar_cursor] << 4;
                            wty = astar_ty[astar_cursor] << 4;
                            ddx = wtx - party[0].px;
                            ddy = wty - party[0].py;
                        } else {
                            ddx = 0; ddy = 0;
                        }
                    }
                    if (ddx != 0) move_dx = ddx > 0 ? (ddx < step_px ? ddx : step_px) : (-ddx < step_px ? ddx : -step_px);
                    if (ddy != 0) move_dy = ddy > 0 ? (ddy < step_px ? ddy : step_px) : (-ddy < step_px ? ddy : -step_px);
                }

                int moved = 0;
                if (move_dx != 0 || move_dy != 0) {
                    int want_dir = dir_from_delta(move_dx, move_dy, party[0].dir);
                    party[0].dir = want_dir;

                    int foot_w = party[0].vm.hitbox_w > 0 ? party[0].vm.hitbox_w : 2;
                    int nx = party[0].px + move_dx;
                    int ny = party[0].py + move_dy;
                    if (pal_map_can_stand_px(&map, nx, ny, foot_w, noclip_mode)) {
                        party[0].px = nx;
                        party[0].py = ny;
                        moved = 1;
                    } else if (move_dx != 0 && pal_map_can_stand_px(&map, party[0].px + move_dx, party[0].py, foot_w, noclip_mode)) {
                        party[0].px += move_dx;
                        moved = 1;
                    } else if (move_dy != 0 && pal_map_can_stand_px(&map, party[0].px, party[0].py + move_dy, foot_w, noclip_mode)) {
                        party[0].py += move_dy;
                        moved = 1;
                    } else {
                        /* Cancel stuck A* path on obstacle */
                        astar_count = 0;
                        astar_cursor = 0;
                    }
                }

                if (moved) {
                    memmove(&trail_x[1], &trail_x[0], sizeof(int) * 63);
                    memmove(&trail_y[1], &trail_y[0], sizeof(int) * 63);
                    memmove(&trail_dir[1], &trail_dir[0], sizeof(int) * 63);
                    trail_x[0] = party[0].px;
                    trail_y[0] = party[0].py;
                    trail_dir[0] = party[0].dir;
                    active_trig_id = pal_map_trigger_at_px(&map, party[0].px, party[0].py, 2);
                    if (active_trig_id == 0u) {
                        last_stepped_trig_id = 0u;
                    } else if (active_trig_id != last_stepped_trig_id && !talk_vm.active && world_state) {
                        last_stepped_trig_id = active_trig_id;
                        uint32_t trig_talk = talk_lookup_trigger_script(&map, active_trig_id);
                        if (trig_talk > 0u && pal_talk_vm_start(&talk_vm, cfg->game_dir, (int32_t)trig_talk, (uint16_t)active_trig_id) == 0) {
                            astar_count = 0;
                            astar_cursor = 0;
                            interact_target_npc_idx = -1;
                            show_dialogue = 0;
                            pal_talk_vm_step(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt, 0);
                        }
                    }
                }

                if (!talk_vm.active && interact_target_npc_idx >= 0 && interact_target_npc_idx < map_npc_actor_cnt) {
                    PalFieldActor *tgt = &map_npcs[interact_target_npc_idx];
                    if (tgt->active && tgt->talk_id > 0u) {
                        int dx = tgt->px - party[0].px;
                        int dy = tgt->py - party[0].py;
                        if (dx * dx + dy * dy <= 44 * 44 || astar_cursor >= astar_count) {
                            int hit_idx = interact_target_npc_idx;
                            interact_target_npc_idx = -1;
                            astar_count = 0;
                            astar_cursor = 0;
                            if (dx * dx + dy * dy <= 52 * 52) {
                                talk_begin_npc_interaction(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt, hit_idx, &show_dialogue);
                            }
                        }
                    } else {
                        interact_target_npc_idx = -1;
                    }
                }

                int lbase = moved ? (running ? 16 : 8) : 0;
                party[0].action_base = lbase;
                field_actor_set_sub(&party[0], cfg->game_dir,
                                    kPartyHeroActIds[leader_hero_idx % 4u],
                                    lbase + party[0].dir, moved ? 1 : 0);

                static const int kLagTicks[4] = {0, 8, 16, 24};
                for (int p = 1; p < 4; p++) {
                    if (party_follow_enabled) {
                        int lag = kLagTicks[p];
                        int fdx = trail_x[lag] - party[p].px;
                        int fdy = trail_y[lag] - party[p].py;
                        int fmoved = (fdx != 0 || fdy != 0);
                        if (fmoved) {
                            party[p].dir = trail_dir[lag];
                            party[p].px = trail_x[lag];
                            party[p].py = trail_y[lag];
                        }
                        int fbase = fmoved ? (running ? 16 : 8) : 0;
                        field_actor_set_sub(&party[p], cfg->game_dir,
                                            kPartyHeroActIds[(leader_hero_idx + (uint32_t)p) % 4u],
                                            fbase + party[p].dir, fmoved ? 1 : 0);
                    }
                }

                for (int p = 0; p < (party_follow_enabled ? 4 : 1); p++) {
                    if (party[p].vm_ok && !party[p].vm.finished) {
                        pal_act_vm_step_tick(&party[p].vm);
                    }
                }
                for (int n = 0; n < map_npc_actor_cnt; n++) {
                    if (map_npcs[n].active && map_npcs[n].vm_ok && !map_npcs[n].vm.finished) {
                        pal_act_vm_step_tick(&map_npcs[n].vm);
                    }
                }
            }
        }

        if (scene == PAL_SCENE_FIELD) {
            for (int map_chain = 0; map_chain < 4; map_chain++) {
                if (talk_vm.pending_sfx_id > 0) {
                    play_ui_sfx(cfg->game_dir, (uint32_t)talk_vm.pending_sfx_id, &sfx_stream, cfg->master_volume, runtime_mute);
                    talk_vm.pending_sfx_id = 0;
                }
                if (talk_vm.pending_bink_rel[0] != '\0') {
                    SDL_PauseAudioStreamDevice(bgm.stream);
                    if (pal_bink_play_file(window, renderer, cfg->game_dir, talk_vm.pending_bink_rel, cfg->master_volume, runtime_mute) == 1) {
                        app_quit = 1;
                    }
                    talk_vm.pending_bink_rel[0] = '\0';
                    if (bgm.stream) SDL_ResumeAudioStreamDevice(bgm.stream);
                    if (app_quit) break;
                    pending_fade_in_ms = 140;
                    last_ns = SDL_GetTicksNS();
                }
                if (talk_vm.pending_map_id >= 0) {
                    uint32_t dst_mid = (uint32_t)talk_vm.pending_map_id;
                    int dtx = talk_vm.pending_tx;
                    int dty = talk_vm.pending_ty;
                    int ddir = talk_vm.pending_dir;
                    talk_vm.pending_map_id = -1;
                    if (pending_fade_in_ms == 0) {
                        if (run_fade_transition(window, renderer, scene_rt, 0, 140, &bgm) == 1) {
                            app_quit = 1;
                            break;
                        }
                    }
                    PalLoadedMap next_map = {0};
                    if (vfs_load_lmf_map_by_id(cfg->game_dir, dst_mid, renderer, scale_mode, &next_map) == 0) {
                        free_loaded_map(&map);
                        map = next_map;
                        pending_spawn_override = 1;
                        spawn_override_tx = dtx;
                        spawn_override_ty = dty;
                        spawn_override_dir = ddir;
                        need_setup_loaded_map = 1;
                        pending_fade_in_ms = 140;
                    }
                    last_ns = SDL_GetTicksNS();
                }
                if (!need_setup_loaded_map) break;
                need_setup_loaded_map = 0;
                astar_count = 0;
                astar_cursor = 0;
                interact_target_npc_idx = -1;
                int stx = 0, sty = 0, sdir = 1;
                if (pending_spawn_override) {
                    stx = spawn_override_tx;
                    sty = spawn_override_ty;
                    sdir = spawn_override_dir & 7;
                    pending_spawn_override = 0;
                } else {
                    pick_map_spawn_tile(&map, &stx, &sty);
                }
                for (int p = 0; p < 4; p++) {
                    party[p].px = stx << 4;
                    party[p].py = sty << 4;
                    party[p].dir = sdir;
                    field_actor_set_sub(&party[p], cfg->game_dir,
                                        kPartyHeroActIds[(leader_hero_idx + (uint32_t)p) % 4u], sdir, 0);
                }
                for (int t = 0; t < 64; t++) {
                    trail_x[t] = party[0].px;
                    trail_y[t] = party[0].py;
                    trail_dir[t] = sdir;
                }
                compute_field_camera(&map, party[0].px, party[0].py, &cam_x, &cam_y);
                map_npc_actor_cnt = map.npc_count < 64 ? map.npc_count : 64;
                memset(map_npcs, 0, sizeof(map_npcs));
                for (int i = 0; i < map_npc_actor_cnt; i++) {
                    const PalMapNpcDef *nd = &map.npcs[i];
                    map_npcs[i].obj_id = nd->obj_id;
                    map_npcs[i].talk_id = (world_state && nd->obj_id < PAL_OBJ_CAP && world_state->npc_talk_override[nd->obj_id] != 0u)
                                              ? world_state->npc_talk_override[nd->obj_id]
                                              : nd->talk_id;
                    map_npcs[i].npc_flags = nd->flags;
                    map_npcs[i].orig_sub_state = nd->sub_state;
                    map_npcs[i].active = (nd->flags & 0x8000u) != 0 ? 1 : 0;
                    if (world_state && nd->obj_id < PAL_OBJ_CAP) {
                        if (world_state->npc_visibility_override[nd->obj_id] == 1u) map_npcs[i].active = 1;
                        else if (world_state->npc_visibility_override[nd->obj_id] == 2u) map_npcs[i].active = 0;
                    }
                    map_npcs[i].px = (int)nd->tx << 4;
                    map_npcs[i].py = (int)nd->ty << 4;
                    map_npcs[i].dir = (int)(nd->dir & 7u);
                    if (map_npcs[i].active) {
                        field_actor_set_sub(&map_npcs[i], cfg->game_dir, nd->act_id, (int)nd->sub_state + (int)nd->dir, 1);
                    }
                }
                if (strcmp(cur_bgm_playing, map.bgm_track) != 0) {
                    pal_bgm_play_track(&bgm, cfg->game_dir, map.bgm_track, cfg->master_volume, runtime_mute);
                    snprintf(cur_bgm_playing, sizeof(cur_bgm_playing), "%s", map.bgm_track);
                }
                if (map_banner_tex) SDL_DestroyTexture(map_banner_tex);
                char banner_msg[128];
                snprintf(banner_msg, sizeof(banner_msg), "%%C6%s %%C3(Map #%u)", map.title_utf8, map.map_id);
                map_banner_tex = create_richtext_texture(renderer, banner_msg, 20.0f, 360, &map_banner_w, &map_banner_h);
                map_banner_timer = 3.0f;
                active_trig_id = pal_map_trigger_at_px(&map, party[0].px, party[0].py, 2);
                last_stepped_trig_id = active_trig_id;

                if (talk_vm.active) {
                    pal_talk_vm_step(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt, 0);
                } else if (world_state) {
                    for (int i = 0; i < map_npc_actor_cnt; i++) {
                        if (map_npcs[i].active && (map_npcs[i].npc_flags & 0x20u) != 0u && map_npcs[i].talk_id > 0u) {
                            if (pal_talk_vm_start(&talk_vm, cfg->game_dir, (int32_t)map_npcs[i].talk_id, map_npcs[i].obj_id) == 0) {
                                show_dialogue = 0;
                                pal_talk_vm_step(&talk_vm, world_state, cfg->game_dir, map.map_id, party, map_npcs, map_npc_actor_cnt, 0);
                                break;
                            }
                        }
                    }
                }
            }
            if (app_quit) break;
            if (talk_vm.active && talk_vm.dlg_visible && talk_vm.dlg_dirty) {
                char rich_fmt[1536];
                int mw = 540;
                talk_format_display_text(&talk_vm, rich_fmt, sizeof(rich_fmt), &dlg_text_x, &mw);
                if (dlg_text_tex) SDL_DestroyTexture(dlg_text_tex);
                dlg_text_tex = create_richtext_texture(renderer, rich_fmt, 18.0f, mw, &cur_dlg_w, &cur_dlg_h);
                talk_vm.dlg_dirty = 0;
            }
        }

        if (scene_rt) SDL_SetRenderTarget(renderer, scene_rt);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (scene == PAL_SCENE_TITLE || scene == PAL_SCENE_LOAD_OVERLAY) {
            draw_act_sprite_node(renderer, &title_bg, 0);
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
            /* Authentic SWD2k camera deadzone (304, 240) clamped to map pixels (0x41aab1) */
            compute_field_camera(&map, party[0].px, party[0].py, &cam_x, &cam_y);

            /* Pass 0: Map Background Layer */
            SDL_FRect map_dst = {-cam_x, -cam_y, (float)map.w_px, (float)map.h_px};
            if (map.texture) SDL_RenderTexture(renderer, map.texture, NULL, &map_dst);

            /* Pass 1: Bucket 2 (below actors: ground shadows / decals) */
            for (int k = 0; k < map.fg_inst_count; k++) {
                const PalMapFgInst *inst = &map.fg_insts[k];
                if (inst->layer_bucket != 2) continue;
                for (int d = 0; d < map.fg_def_count; d++) {
                    if (map.fg_defs[d].sid == inst->sid && map.fg_defs[d].tex) {
                        SDL_FRect fd = {(float)inst->px - cam_x, (float)inst->py_top - cam_y,
                                        (float)map.fg_defs[d].w, (float)map.fg_defs[d].h};
                        SDL_RenderTexture(renderer, map.fg_defs[d].tex, NULL, &fd);
                        break;
                    }
                }
            }

            /* Pass 2: Bucket 0 (Y-sorted Map Props + NPCs + Party + Leader) */
            PalRenderItem items[1024];
            int item_cnt = 0;
            for (int k = 0; k < map.fg_inst_count && item_cnt < 1024; k++) {
                const PalMapFgInst *inst = &map.fg_insts[k];
                if (inst->layer_bucket != 0) continue;
                for (int d = 0; d < map.fg_def_count; d++) {
                    if (map.fg_defs[d].sid == inst->sid && map.fg_defs[d].tex) {
                        PalRenderItem *it = &items[item_cnt++];
                        it->kind = PAL_RENDER_FG_INST;
                        it->sort_y = inst->sort_y;
                        it->draw_x = inst->px;
                        it->draw_y = inst->py_top;
                        it->draw_w = map.fg_defs[d].w;
                        it->draw_h = map.fg_defs[d].h;
                        it->tex = map.fg_defs[d].tex;
                        it->is_leader = 0;
                        it->is_party = 0;
                        break;
                    }
                }
            }

            int pmax = party_follow_enabled ? 4 : 1;
            for (int p = 0; p < pmax && item_cnt < 1024; p++) {
                if (!party[p].vm_ok || !party[p].vm.visible) continue;
                PalLoadedSprite *sp = tsw_cache_get(&tsw_cache, cfg->game_dir, renderer, scale_mode, party[p].vm.fr_tsw_id);
                if (!sp || sp->nframes <= 0) continue;
                int fi = party[p].vm.frame_index;
                if (fi < 0) fi = 0;
                fi %= sp->nframes;
                PalRenderItem *it = &items[item_cnt++];
                it->kind = PAL_RENDER_ACTOR;
                it->sort_y = party[p].py + 15;
                it->draw_x = party[p].px - party[p].vm.offset_x;
                it->draw_y = party[p].py - party[p].vm.offset_y;
                it->draw_w = sp->w;
                it->draw_h = sp->h;
                it->tex = sp->frames[fi];
                it->is_leader = (p == 0);
                it->is_party = 1;
            }

            for (int n = 0; n < map_npc_actor_cnt && item_cnt < 1024; n++) {
                if (!map_npcs[n].active || !map_npcs[n].vm_ok || !map_npcs[n].vm.visible) continue;
                PalLoadedSprite *sp = tsw_cache_get(&tsw_cache, cfg->game_dir, renderer, scale_mode, map_npcs[n].vm.fr_tsw_id);
                if (!sp || sp->nframes <= 0) continue;
                int fi = map_npcs[n].vm.frame_index;
                if (fi < 0) fi = 0;
                fi %= sp->nframes;
                PalRenderItem *it = &items[item_cnt++];
                it->kind = PAL_RENDER_ACTOR;
                it->sort_y = map_npcs[n].py + 15;
                it->draw_x = map_npcs[n].px - map_npcs[n].vm.offset_x;
                it->draw_y = map_npcs[n].py - map_npcs[n].vm.offset_y;
                it->draw_w = sp->w;
                it->draw_h = sp->h;
                it->tex = sp->frames[fi];
                it->is_leader = 0;
                it->is_party = 0;
            }

            if (item_cnt > 1) qsort(items, (size_t)item_cnt, sizeof(PalRenderItem), cmp_render_item);
            for (int i = 0; i < item_cnt; i++) {
                SDL_FRect dst = {
                    (float)items[i].draw_x - cam_x,
                    (float)items[i].draw_y - cam_y,
                    (float)items[i].draw_w,
                    (float)items[i].draw_h
                };
                if (items[i].tex) SDL_RenderTexture(renderer, items[i].tex, NULL, &dst);
            }

            /* Pass 3: Bucket 1 (above actors: overhead roofs / tree canopies) */
            for (int k = 0; k < map.fg_inst_count; k++) {
                const PalMapFgInst *inst = &map.fg_insts[k];
                if (inst->layer_bucket != 1) continue;
                for (int d = 0; d < map.fg_def_count; d++) {
                    if (map.fg_defs[d].sid == inst->sid && map.fg_defs[d].tex) {
                        SDL_FRect fd = {(float)inst->px - cam_x, (float)inst->py_top - cam_y,
                                        (float)map.fg_defs[d].w, (float)map.fg_defs[d].h};
                        SDL_RenderTexture(renderer, map.fg_defs[d].tex, NULL, &fd);
                        break;
                    }
                }
            }

            /* Optional Collision + Trigger + A* Path Debug Overlay (G key) */
            if (show_collision_grid && map.walk_grid) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                int tx_min = (int)(cam_x / 16.0f) - 1; if (tx_min < 0) tx_min = 0;
                int tx_max = (int)((cam_x + 640.0f) / 16.0f) + 1; if (tx_max > map.w_tiles) tx_max = map.w_tiles;
                int ty_min = (int)(cam_y / 16.0f) - 1; if (ty_min < 0) ty_min = 0;
                int ty_max = (int)((cam_y + 480.0f) / 16.0f) + 1; if (ty_max > map.h_tiles) ty_max = map.h_tiles;
                for (int ty = ty_min; ty < ty_max; ty++) {
                    for (int tx = tx_min; tx < tx_max; tx++) {
                        uint32_t cell = map.walk_grid[(size_t)ty * (size_t)map.w_tiles + (size_t)tx];
                        if ((cell & 0x60000000u) != 0) {
                            SDL_SetRenderDrawColor(renderer, 220, 40, 40, 95);
                            SDL_FRect r = {(float)(tx * 16) - cam_x, (float)(ty * 16) - cam_y, 16.0f, 16.0f};
                            SDL_RenderFillRect(renderer, &r);
                        } else if ((cell & 0xFFu) != 0) {
                            SDL_SetRenderDrawColor(renderer, 250, 200, 40, 125);
                            SDL_FRect r = {(float)(tx * 16) - cam_x, (float)(ty * 16) - cam_y, 16.0f, 16.0f};
                            SDL_RenderFillRect(renderer, &r);
                        }
                    }
                }
                /* Leader footprint hitbox */
                SDL_SetRenderDrawColor(renderer, 40, 240, 120, 220);
                SDL_FRect foot_r = {(float)party[0].px - cam_x, (float)party[0].py - cam_y, 32.0f, 16.0f};
                SDL_RenderRect(renderer, &foot_r);
            }

            /* Render active A* target & path breadcrumbs */
            if (astar_cursor < astar_count) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                for (int s = astar_cursor; s < astar_count; s++) {
                    SDL_SetRenderDrawColor(renderer, 60, 225, 255, s == astar_count - 1 ? 220 : 120);
                    SDL_FRect pt = {(float)(astar_tx[s] * 16 + 12) - cam_x, (float)(astar_ty[s] * 16 + 4) - cam_y, 8.0f, 8.0f};
                    SDL_RenderFillRect(renderer, &pt);
                }
            }

            if ((talk_vm.active && talk_vm.dlg_visible) || (!talk_vm.active && show_dialogue)) {
                if (dlg_box.nframes > 0) {
                    SDL_FRect bd = {0.0f, 336.0f, (float)dlg_box.w, (float)dlg_box.h};
                    SDL_RenderTexture(renderer, dlg_box.frames[0], NULL, &bd);
                } else {
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(renderer, 16, 18, 36, 210);
                    SDL_FRect bd = {0.0f, 336.0f, 640.0f, 144.0f};
                    SDL_RenderFillRect(renderer, &bd);
                }
                if (talk_vm.active) {
                    for (int p = 0; p < PAL_MAX_PORTRAITS; p++) {
                        PalTalkPortrait *pt = &talk_vm.portraits[p];
                        if (!pt->active || !pt->vm_ok || !pt->vm.visible) continue;
                        PalLoadedSprite *psp = tsw_cache_get(&tsw_cache, cfg->game_dir, renderer, scale_mode, pt->vm.fr_tsw_id);
                        if (!psp || psp->nframes <= 0) continue;
                        int fi = pt->vm.frame_index;
                        if (fi < 0) fi = 0;
                        fi %= psp->nframes;
                        SDL_FRect pd = {
                            (float)(pt->anchor_x - pt->vm.offset_x),
                            (float)(pt->anchor_y - pt->vm.offset_y),
                            (float)psp->w,
                            (float)psp->h
                        };
                        SDL_RenderTexture(renderer, psp->frames[fi], NULL, &pd);
                    }
                }
                if (dlg_text_tex) {
                    float tx = talk_vm.active ? (float)dlg_text_x : 44.0f;
                    SDL_FRect td = {tx, 356.0f, (float)cur_dlg_w, (float)cur_dlg_h};
                    SDL_RenderTexture(renderer, dlg_text_tex, NULL, &td);
                }
                if (dlg_cursor.nframes > 0 && (!talk_vm.active || talk_vm.waiting_input)) {
                    SDL_Texture *dc = dlg_cursor.frames[(cursor_anim / 2) % dlg_cursor.nframes];
                    SDL_FRect cd = {596.0f, 444.0f, (float)dlg_cursor.w, (float)dlg_cursor.h};
                    SDL_RenderTexture(renderer, dc, NULL, &cd);
                }
            }

            if (map_banner_timer > 0.0f && map_banner_tex) {
                float alpha_f = map_banner_timer < 0.6f ? (map_banner_timer / 0.6f) : 1.0f;
                Uint8 ba = (Uint8)(alpha_f * 255.0f);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 10, 14, 24, (Uint8)(alpha_f * 185.0f));
                float bx = (640.0f - (float)(map_banner_w + 36)) * 0.5f;
                SDL_FRect br = {bx, 66.0f, (float)(map_banner_w + 36), (float)(map_banner_h + 16)};
                SDL_RenderFillRect(renderer, &br);
                SDL_SetRenderDrawColor(renderer, 212, 178, 112, ba);
                SDL_RenderRect(renderer, &br);
                SDL_SetTextureAlphaMod(map_banner_tex, ba);
                SDL_FRect bt = {bx + 18.0f, 74.0f, (float)map_banner_w, (float)map_banner_h};
                SDL_RenderTexture(renderer, map_banner_tex, NULL, &bt);
            }

            if (camp_menu.open && world_state) {
                camp_draw_overlay(renderer, cfg->game_dir, scale_mode, &tsw_cache, &item_icon_cache,
                                  &menu_cursor, world_state, &camp_menu,
                                  party_follow_enabled, runtime_mute, show_collision_grid, cursor_anim);
            }

            if (show_hud && !camp_menu.open) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 165);
                SDL_FRect hud = {8.0f, 8.0f, 624.0f, 52.0f};
                SDL_RenderFillRect(renderer, &hud);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                SDL_RenderDebugTextFormat(
                    renderer, 16.0f, 15.0f,
                    "SDL3 %s | Map #%u (%dx%d) | Tile(%d,%d) Dir:%d | NPC:%d Trig:%u Talk:%d | %s",
                    active_backend ? active_backend : "metal",
                    map.map_id, map.w_tiles, map.h_tiles,
                    party[0].px >> 4, party[0].py >> 4, party[0].dir,
                    map_npc_actor_cnt, (unsigned)active_trig_id,
                    talk_vm.active ? (int)talk_vm.talk_id : 0,
                    map.bgm_track
                );
                SDL_RenderDebugTextFormat(
                    renderer, 16.0f, 30.0f,
                    "[WASD/Click] Move  [Shift] %s  [E/Space] Talk  [I] Menu  [P] Party:%s",
                    run_mode_enabled ? "RUN" : "WALK",
                    party_follow_enabled ? "ON" : "OFF"
                );
                SDL_RenderDebugTextFormat(
                    renderer, 16.0f, 44.0f,
                    "[M/[/]] Map [G] Grid:%s [V] Hero [C] Clip [T] Dlg [B] Bik [Tab] HUD [Esc]",
                    show_collision_grid ? "ON" : "OFF"
                );
            }
        }

        if (scene_rt) {
            SDL_SetRenderTarget(renderer, NULL);
        }
        if (pending_fade_in_ms > 0) {
            int fade_ms = pending_fade_in_ms;
            pending_fade_in_ms = 0;
            if (run_fade_transition(window, renderer, scene_rt, 1, (Uint32)fade_ms, &bgm) == 1) {
                app_quit = 1;
                break;
            }
            last_ns = SDL_GetTicksNS();
        } else {
            if (scene_rt) {
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, scene_rt, NULL, NULL);
            }
            SDL_RenderPresent(renderer);
        }
    }

    SDL_ShowCursor();
    pal_bgm_stop(&bgm);
    pal_talk_vm_free(&talk_vm);
    camp_free_textures(&camp_menu);
    if (game_db) {
        pal_db_free(game_db);
        free(game_db);
    }
    free(world_state);
    if (scene_rt) SDL_DestroyTexture(scene_rt);
    if (sfx_stream) SDL_DestroyAudioStream(sfx_stream);
    if (ver_tex) SDL_DestroyTexture(ver_tex);
    if (save_tex) SDL_DestroyTexture(save_tex);
    if (dlg_text_tex) SDL_DestroyTexture(dlg_text_tex);
    if (map_banner_tex) SDL_DestroyTexture(map_banner_tex);
    free_tsw_cache(&tsw_cache);
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

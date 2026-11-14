#include "PalEngineCore.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <iconv.h>
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
    if (c == PAL_TSW_KEY_TRANSPARENT) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
    } else if (c == PAL_TSW_KEY_SHADOW) {
        dst[0] = dst[1] = dst[2] = 0;
        dst[3] = 128;
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

    size_t bp = 8;
    size_t px_idx = 0;

    while (bp + 2 <= data_len && px_idx < total_px) {
        uint16_t row_marker = rd_u16(data + bp);
        if (row_marker == 0) break;
        bp += 2;
        while (bp + 2 <= data_len && px_idx < total_px) {
            uint16_t token = rd_u16(data + bp);
            bp += 2;
            if (token == 0) break;

            uint16_t tag = token & 0xC000u;
            uint16_t count = token & 0x3FFFu;
            if (tag == 0) {
                for (uint16_t i = 0; i < count && px_idx < total_px; i++) {
                    if (bpp == 16) {
                        if (bp + 2 <= data_len) {
                            rgb555_write_keyed(rgba_out + px_idx * 4, rd_u16(data + bp));
                            bp += 2;
                        }
                    } else {
                        if (bp < data_len) {
                            uint8_t pidx = data[bp++];
                            uint16_t c = palette ? palette[pidx] : 0;
                            rgb555_write_keyed(rgba_out + px_idx * 4, c);
                        }
                    }
                    px_idx++;
                }
            } else if (tag == 0xC000u) {
                for (uint16_t i = 0; i < count && px_idx < total_px; i++) {
                    uint8_t *d = rgba_out + px_idx * 4;
                    d[0] = d[1] = d[2] = 0;
                    d[3] = 128;
                    px_idx++;
                }
            } else {
                px_idx += count;
                if (px_idx > total_px) px_idx = total_px;
            }
        }
    }

    if (mask && mask_len >= total_px) {
        for (size_t i = 0; i < total_px; i++) {
            if (mask[i] == 1 && rgba_out[i * 4 + 3] < 200) {
                rgba_out[i * 4 + 3] = 200;
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

static int vfs_load_tsw_slot(
    const char *game_dir, const char *tsw_name, int target_idx,
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
    if (target_idx < 0) target_idx = (target_idx % cnt + cnt) % cnt;
    if (target_idx >= cnt) target_idx %= cnt;

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

static void free_loaded_sprite(PalLoadedSprite *s) {
    if (s->frames) {
        for (int i = 0; i < s->nframes; i++) {
            if (s->frames[i]) SDL_DestroyTexture(s->frames[i]);
        }
        free(s->frames);
    }
    memset(s, 0, sizeof(*s));
}

static int vfs_load_snd_wav_bytes(
    const char *game_dir, int target_idx,
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
    if (target_idx < 0) target_idx = 0;
    if (target_idx >= cnt) target_idx %= cnt;

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

/* =========================================================================
 * SDL3 Metal Interactive Runtime & Self-Test
 * ========================================================================= */

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

    SDL_Window *window = SDL_CreateWindow("NewPal — SDL3 Metal Engine", win_w, win_h, wflags);
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

    int map_idx = cfg->initial_map_index;
    int sprite_idx = cfg->initial_char_slot;
    int snd_idx = 0;

    PalLoadedMap map = {0};
    PalLoadedSprite sprite = {0};
    if (vfs_load_lmf_map(cfg->game_dir, "huge.lmf", map_idx, renderer, scale_mode, &map) != 0 ||
        vfs_load_tsw_slot(cfg->game_dir, "all_char.tsw", sprite_idx, renderer, scale_mode, &sprite) != 0) {
        if (out_err) snprintf(out_err, (size_t)err_cap, "Failed to decode initial MSF3 map or TSW sprite.");
        free_loaded_map(&map);
        free_loaded_sprite(&sprite);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
        return -1;
    }

    SDL_AudioSpec wav_spec = {0};
    uint8_t *wav_buf = NULL;
    Uint32 wav_len = 0;
    uint32_t wav_sid = 0;
    int snd_ok = (vfs_load_snd_wav_bytes(cfg->game_dir, snd_idx, &wav_spec, &wav_buf, &wav_len, &wav_sid) == 0);
    SDL_AudioStream *audio_stream = NULL;
    if (snd_ok) {
        audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &wav_spec, NULL, NULL);
        if (audio_stream) {
            SDL_SetAudioStreamGain(audio_stream, cfg->mute_audio ? 0.0f : cfg->master_volume);
            SDL_ResumeAudioStreamDevice(audio_stream);
        }
    }

    if (is_self_test) {
        PalActInfo act_info;
        char act_path[1200];
        join_path(act_path, sizeof(act_path), cfg->game_dir, "all_char.act");
        int act_rc = vfs_inspect_act_slot(act_path, 0, &act_info);

        for (int f = 0; f < 3; f++) {
            SDL_SetRenderDrawColor(renderer, 16, 20, 28, 255);
            SDL_RenderClear(renderer);
            SDL_FRect dst = {0.0f, 0.0f, (float)map.w_px, (float)map.h_px};
            SDL_RenderTexture(renderer, map.texture, NULL, &dst);
            if (sprite.nframes > 0) {
                SDL_FRect sdst = {300.0f, 200.0f, (float)sprite.w, (float)sprite.h};
                SDL_RenderTexture(renderer, sprite.frames[f % sprite.nframes], NULL, &sdst);
            }
            SDL_RenderDebugTextFormat(renderer, 10.0f, 10.0f, "Renderer: %s | Map %u (%dx%d)", active_backend, map.map_id, map.w_px, map.h_px);
            SDL_RenderPresent(renderer);
        }

        int backend_ok = (active_backend && strcmp(active_backend, "metal") == 0);
        printf("=== NewPal SDL3 Metal + BYOD VFS Self-Test ===\n");
        printf("GameDir: %s\n", cfg->game_dir);
        printf("Validation: %d/%d (is_valid=%d)\n", report.valid_archives_count, report.total_required_archives, report.is_valid);
        printf("Renderer: %s (expected: metal)\n", active_backend ? active_backend : "null");
        printf("MSF3 Map[0]: id=%u size=%dx%d px (%dx%d tiles @ %dpx), huge.lmf=%d\n",
               map.map_id, map.w_px, map.h_px, map.w_tiles, map.h_tiles, map.tile_sz, map.total_maps);
        printf("TSW Sprite[0]: slot_id=%u frames=%d size=%dx%d\n",
               sprite.slot_id, sprite.nframes, sprite.w, sprite.h);
        printf("ACT Slot[0]: rc=%d sub_actions=%d insns=%d clean=%d linked_tsw=%d delay=%d\n",
               act_rc, act_info.sub_actions, act_info.decoded_insns, act_info.clean_streams,
               act_info.linked_tsw_id, act_info.frame_delay);
        printf("SND Slot[0]: rc=%d slot_id=%u rate=%d ch=%d bytes=%u\n",
               snd_ok ? 0 : -1, wav_sid, wav_spec.freq, wav_spec.channels, wav_len);
        printf("DB Counts: heroes=%d monsters=%d item_recs=%d magic_recs=%d map_titles=%d namex=%d\n",
               report.hero_count, report.monster_count, report.item_count, report.magic_count,
               report.map_title_count, report.namex_count);
        if (report.hero_summary_count > 0) {
            printf("Hero[0]: id=%d name=%s HP=%d/%d MP=%d/%d ATK=%d MAG=%d DEF=%d SPD=%d LUCK=%d\n",
                   report.heroes[0].id, report.heroes[0].name,
                   report.heroes[0].cur_hp, report.heroes[0].max_hp,
                   report.heroes[0].cur_mp, report.heroes[0].max_mp,
                   report.heroes[0].atk, report.heroes[0].mag, report.heroes[0].def,
                   report.heroes[0].spd, report.heroes[0].luck);
        }
        printf("First Map Title: %s\n", report.sample_map_title);

        if (wav_buf) SDL_free(wav_buf);
        if (audio_stream) SDL_DestroyAudioStream(audio_stream);
        free_loaded_map(&map);
        free_loaded_sprite(&sprite);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);

        return (backend_ok && report.is_valid && act_rc == 0 && snd_ok) ? 0 : -1;
    }

    /* Interactive SDL3 Metal Runtime Loop */
    float player_x = (float)map.w_px * 0.5f;
    float player_y = (float)map.h_px * 0.5f;
    int show_hud = 1;
    int running = 1;
    Uint64 last_ns = SDL_GetTicksNS();
    float anim_acc = 0.0f;
    int frame_idx = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(window))) {
                running = 0;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode key = ev.key.key;
                if (key == SDLK_ESCAPE) {
                    running = 0;
                } else if (key == SDLK_TAB) {
                    show_hud = !show_hud;
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
                    sprite_idx = (sprite_idx + delta + sprite.total_slots) % sprite.total_slots;
                    PalLoadedSprite next_sp = {0};
                    if (vfs_load_tsw_slot(cfg->game_dir, "all_char.tsw", sprite_idx, renderer, scale_mode, &next_sp) == 0) {
                        free_loaded_sprite(&sprite);
                        sprite = next_sp;
                        frame_idx = 0;
                    }
                } else if (key == SDLK_SPACE) {
                    if (wav_buf && audio_stream && !cfg->mute_audio) {
                        SDL_PutAudioStreamData(audio_stream, wav_buf, (int)wav_len);
                    }
                    snd_idx = (snd_idx + 1) % (report.snd_slots > 0 ? report.snd_slots : 1);
                    uint8_t *nbuf = NULL;
                    Uint32 nlen = 0;
                    uint32_t nsid = 0;
                    SDL_AudioSpec nspec = {0};
                    if (vfs_load_snd_wav_bytes(cfg->game_dir, snd_idx, &nspec, &nbuf, &nlen, &nsid) == 0) {
                        if (wav_buf) SDL_free(wav_buf);
                        wav_buf = nbuf;
                        wav_len = nlen;
                        wav_sid = nsid;
                        if (audio_stream) SDL_DestroyAudioStream(audio_stream);
                        audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &nspec, NULL, NULL);
                        if (audio_stream) {
                            SDL_SetAudioStreamGain(audio_stream, cfg->mute_audio ? 0.0f : cfg->master_volume);
                            SDL_ResumeAudioStreamDevice(audio_stream);
                        }
                    }
                }
            }
        }

        Uint64 now_ns = SDL_GetTicksNS();
        float dt = (float)(now_ns - last_ns) / 1e9f;
        if (dt > 0.1f) dt = 0.1f;
        last_ns = now_ns;

        const bool *keys = SDL_GetKeyboardState(NULL);
        float vx = 0.0f, vy = 0.0f;
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])  vx -= 1.0f;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) vx += 1.0f;
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])    vy -= 1.0f;
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])  vy += 1.0f;

        if (vx != 0.0f || vy != 0.0f) {
            float speed = 180.0f;
            player_x += vx * speed * dt;
            player_y += vy * speed * dt;
            if (player_x < 0.0f) player_x = 0.0f;
            if (player_y < 0.0f) player_y = 0.0f;
            if (player_x > (float)map.w_px) player_x = (float)map.w_px;
            if (player_y > (float)map.h_px) player_y = (float)map.h_px;
            anim_acc += dt;
            if (anim_acc >= 0.12f && sprite.nframes > 0) {
                anim_acc = 0.0f;
                frame_idx = (frame_idx + 1) % sprite.nframes;
            }
        }

        float cam_x = player_x - 320.0f;
        float cam_y = player_y - 240.0f;
        if (map.w_px > 640) {
            if (cam_x < 0.0f) cam_x = 0.0f;
            if (cam_x > (float)(map.w_px - 640)) cam_x = (float)(map.w_px - 640);
        } else {
            cam_x = -0.5f * (float)(640 - map.w_px);
        }
        if (map.h_px > 480) {
            if (cam_y < 0.0f) cam_y = 0.0f;
            if (cam_y > (float)(map.h_px - 480)) cam_y = (float)(map.h_px - 480);
        } else {
            cam_y = -0.5f * (float)(480 - map.h_px);
        }

        SDL_SetRenderDrawColor(renderer, 12, 14, 20, 255);
        SDL_RenderClear(renderer);

        SDL_FRect map_dst = {-cam_x, -cam_y, (float)map.w_px, (float)map.h_px};
        SDL_RenderTexture(renderer, map.texture, NULL, &map_dst);

        if (sprite.nframes > 0) {
            SDL_Texture *fr = sprite.frames[frame_idx % sprite.nframes];
            SDL_FRect sp_dst = {
                player_x - cam_x - (float)sprite.w * 0.5f,
                player_y - cam_y - (float)sprite.h,
                (float)sprite.w,
                (float)sprite.h
            };
            SDL_RenderTexture(renderer, fr, NULL, &sp_dst);
        }

        if (show_hud) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
            SDL_FRect hud = {8.0f, 8.0f, 624.0f, 68.0f};
            SDL_RenderFillRect(renderer, &hud);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugTextFormat(
                renderer, 16.0f, 16.0f,
                "SDL3 Renderer: %s | VSync: %d | Filter: %s",
                active_backend ? active_backend : "unknown",
                cfg->vsync_enabled,
                cfg->nearest_filter ? "NEAREST" : "LINEAR"
            );
            SDL_RenderDebugTextFormat(
                renderer, 16.0f, 30.0f,
                "Map %d/%d (ID %u, %dx%d) | Pos: (%.0f, %.0f) | Sprite Slot %u (%d fr)",
                map_idx + 1, map.total_maps, map.map_id, map.w_px, map.h_px,
                player_x, player_y, sprite.slot_id, sprite.nframes
            );
            if (report.hero_summary_count > 0) {
                const PalHeroSummary *h = &report.heroes[0];
                SDL_RenderDebugTextFormat(
                    renderer, 16.0f, 44.0f,
                    "Hero[%d] HP:%d/%d MP:%d/%d ATK:%d MAG:%d DEF:%d SPD:%d | SND:%u",
                    h->id, h->cur_hp, h->max_hp, h->cur_mp, h->max_mp,
                    h->atk, h->mag, h->def, h->spd, wav_sid
                );
            }
            SDL_RenderDebugText(
                renderer, 16.0f, 58.0f,
                "[WASD/Arrows] Move  [ / ] Map  N/P Sprite  [Space] SFX  [Tab] HUD  [Esc] Exit"
            );
        }

        SDL_RenderPresent(renderer);
    }

    if (wav_buf) SDL_free(wav_buf);
    if (audio_stream) SDL_DestroyAudioStream(audio_stream);
    free_loaded_map(&map);
    free_loaded_sprite(&sprite);
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

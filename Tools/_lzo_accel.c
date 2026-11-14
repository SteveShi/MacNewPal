#include <stdint.h>
#include <stddef.h>

/*
 * Standard LZO1X decompressor in C for PalGame asset unpacking.
 * On entry, *out_len holds the capacity of `out` in bytes.
 * On return (0), *out_len holds the actual decompressed byte count.
 * Returns:
 *   0  on clean EOF marker
 *  -1  if trailing input bytes remain (or truncated input)
 *  -2  if input overran
 *  -3  if output buffer capacity is insufficient
 *  -4  if match back-reference points before `out`
 */
int lzo1x_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len) {
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

    #define NEED_IN(n)  do { if ((size_t)(ip_end - ip) < (size_t)(n)) { *out_len = (size_t)(op - out); return -1; } } while (0)
    #define NEED_OUT(n) do { if ((size_t)(out_end - op) < (size_t)(n)) { *out_len = (size_t)(op - out); return -3; } } while (0)
    #define CHECK_MPOS(p) do { if ((p) < out || (p) >= op) { *out_len = (size_t)(op - out); return -4; } } while (0)

    if (*ip > 17) {
        t = *ip++ - 17;
        if (t < 4)
            goto match_next;
        NEED_IN(t);
        NEED_OUT(t);
        do *op++ = *ip++; while (--t > 0);
        goto first_literal_run;
    }

    while (ip < ip_end) {
        t = *ip++;
        if (t >= 16) goto match;
        /* literal run */
        if (t == 0) {
            while (ip < ip_end && *ip == 0) {
                t += 255;
                ip++;
            }
            NEED_IN(1);
            t += 15 + *ip++;
        }
        NEED_IN(t + 3);
        NEED_OUT(t + 3);
        size_t lit = t + 3;
        do *op++ = *ip++; while (--lit > 0);

first_literal_run:
        NEED_IN(1);
        t = *ip++;
        if (t >= 16) goto match;
        NEED_IN(1);
        NEED_OUT(3);
        m_pos = op - (1 + 0x800);
        m_pos -= t >> 2;
        m_pos -= *ip++ << 2;
        CHECK_MPOS(m_pos);
        *op++ = *m_pos++;
        *op++ = *m_pos++;
        *op++ = *m_pos++;
        goto match_done;

        /* handle matches */
        do {
match:
            if (t >= 64) {
                NEED_IN(1);
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
                    NEED_IN(1);
                    t += 31 + *ip++;
                }
                NEED_IN(2);
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
                    NEED_IN(1);
                    t += 7 + *ip++;
                }
                NEED_IN(2);
                m_pos -= ((uint16_t)ip[0] | ((uint16_t)ip[1] << 8)) >> 2;
                ip += 2;
                if (m_pos == op)
                    goto eof_found;
                m_pos -= 0x4000;
            } else {
                NEED_IN(1);
                NEED_OUT(2);
                m_pos = op - 1;
                m_pos -= t >> 2;
                m_pos -= *ip++ << 2;
                CHECK_MPOS(m_pos);
                *op++ = *m_pos++;
                *op++ = *m_pos++;
                goto match_done;
            }

            CHECK_MPOS(m_pos);
            NEED_OUT(t + 2);
            *op++ = *m_pos++;
            *op++ = *m_pos++;
            do *op++ = *m_pos++; while (--t > 0);

match_done:
            t = ip[-2] & 3;
            if (t == 0) break;
match_next:
            NEED_IN(t + 1);
            NEED_OUT(t);
            do *op++ = *ip++; while (--t > 0);
            t = *ip++;
        } while (ip < ip_end);
    }

eof_found:
    *out_len = (size_t)(op - out);
    return (ip == ip_end ? 0 : (ip < ip_end ? -1 : -2));
}

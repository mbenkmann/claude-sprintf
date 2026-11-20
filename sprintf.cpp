/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This code was generated entirely by AI without human creative contribution.
 * It is dedicated to the public domain under CC0 1.0 Universal.
 * https://creativecommons.org/publicdomain/zero/1.0/
 * 
 * sprintf.cpp - snprintf()/vsnprintf() for logging on ARM Cortex-M4, written for speed.
 *
 * Supported conversions (everything the current log strings use, plus a few free extras):
 *
 *     %d %i %u %x %X %s %c %%
 *     flag '0', decimal field width (%02X, %04x, %08llX, %5d, %5s ...)
 *     length modifiers: 'l' and 'z' (accepted, 32 bit like int on this target)
 *                       'll' (64 bit, only for %llx / %llX)
 *
 * Anything else (precision, '-', '+', ' ', '#', '*', %f, %p, %llu, %hhx ...) is NOT supported.
 * It ends in __builtin_trap() or garbage output - there is no input validation at all.
 * A NULL for %s, a NULL format or a bad va_list will crash.
 *
 * Behaviour:
 *  - C99 semantics: never writes more than 'size' bytes, always NUL terminates (size > 0),
 *    returns the length the full output would have had.  size == 0 is allowed.
 *  - No malloc, no static/global RAM, no tables, no library calls, no float.  Stack use is a
 *    small frame (a 20 byte scratch buffer plus spilled registers).  Reentrant.
 *  - Bytes behind the terminating NUL (but inside 'size') may be overwritten with garbage.
 *  - Format strings and %s arguments are read in aligned 32 bit words.  That can touch up to
 *    3 bytes behind the terminating NUL (always inside the same aligned word).
 *  - Needs a little endian core with unaligned LDR/STR enabled (Cortex-M4 default,
 *    SCB->CCR.UNALIGN_TRP must be 0).
 *
 * Build: arm-none-eabi-g++ -c sprintf.cpp (any -O level; the file raises itself to -O2 below).
 * Link order/duplicates: if the linker complains about newlib's snprintf/vsnprintf, define
 * SPRINTF_NAME / VSPRINTF_NAME (e.g. -DSPRINTF_NAME=log_snprintf).
 *
 * Host test (compares against the C library, checks bounds): sprintf_test.cpp, see its header.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------------------------
 * Configuration (override with -D...)
 * ---------------------------------------------------------------------------------------- */

/* Names of the two exported functions. */
#ifndef SPRINTF_NAME
#define SPRINTF_NAME snprintf
#endif
#ifndef VSPRINTF_NAME
#define VSPRINTF_NAME vsnprintf
#endif

/* 1: support %llx / %llX (needed for "MCU ID: 0x%08llX"). */
#ifndef SPRINTF_SUPPORT_LL
#define SPRINTF_SUPPORT_LL 1
#endif

/* 1: support %c. */
#ifndef SPRINTF_SUPPORT_C
#define SPRINTF_SUPPORT_C 1
#endif

/* Attribute for the two exported functions, e.g. __attribute__((section(".ramfunc"))) to run
 * them from RAM (no flash wait states, but then the code occupies RAM). */
#ifndef SPRINTF_ATTR
#define SPRINTF_ATTR
#endif

/* 1: raise this file to -O2 (the project builds with -Os) and stop GCC from turning copy loops
 * into memcpy()/memset() calls.  Set to 0 to compile with the command line options only. */
#ifndef SPRINTF_RAISE_OPTIMIZATION
#define SPRINTF_RAISE_OPTIMIZATION 1
#endif

#if SPRINTF_RAISE_OPTIMIZATION && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2", "no-tree-loop-distribute-patterns")
#endif

/* ------------------------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------------------- */

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little endian only");
static_assert(sizeof(int) == 4, "32 bit int expected");

#define SPF_INLINE   inline __attribute__((always_inline))
#define SPF_NOINLINE __attribute__((noinline, cold))
#define SPF_LIKELY(x)   __builtin_expect(!!(x), 1)
#define SPF_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* The most output a single number may touch (incl. the junk bytes of the 4-byte stores). */
#define SPF_NUM_MAX 16

typedef uint32_t __attribute__((may_alias)) spf_word;   /* aligned word access to char data */

/* Unaligned accesses (single LDR/STR/LDRH/STRH on Cortex-M4). */
static SPF_INLINE uint32_t ld32(const void* p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static SPF_INLINE uint32_t ld16(const void* p) { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
static SPF_INLINE void st32(void* p, uint32_t v) { __builtin_memcpy(p, &v, 4); }
static SPF_INLINE void st16(void* p, uint32_t v) { uint16_t h = (uint16_t)v; __builtin_memcpy(p, &h, 2); }

/* Non-zero if one of the 4 bytes is 0x00 or '%'.  The lowest set bit belongs to the FIRST such
 * byte (the classic "has zero byte" trick gives false positives only above a real hit). */
static SPF_INLINE uint32_t special_mask(uint32_t w)
{
    uint32_t y = w ^ 0x25252525u;
    return (((w - 0x01010101u) & ~w) | ((y - 0x01010101u) & ~y)) & 0x80808080u;
}

static SPF_INLINE uint32_t zero_mask(uint32_t w)
{
    return (w - 0x01010101u) & ~w & 0x80808080u;
}

/* Word at a, but with the bytes in front of s (which belong to somebody else) set to 0xFF so
 * they can't match. */
static SPF_INLINE uint32_t first_word(const spf_word* a, uintptr_t s)
{
    return *a | ((1u << ((s & 3u) << 3)) - 1u);
}

/* Reads only aligned words, so it never leaves the memory region the string lives in. */
static SPF_INLINE const char* find_special(const char* s)
{
    const spf_word* a = (const spf_word*)((uintptr_t)s & ~(uintptr_t)3);
    uint32_t w = first_word(a, (uintptr_t)s);
    uint32_t m;
    while (!(m = special_mask(w))) w = *++a;
    return (const char*)a + (__builtin_ctz(m) >> 3);
}

static SPF_INLINE size_t string_length(const char* s)
{
    const spf_word* a = (const spf_word*)((uintptr_t)s & ~(uintptr_t)3);
    uint32_t w = first_word(a, (uintptr_t)s);
    uint32_t m;
    while (!(m = zero_mask(w))) w = *++a;
    return (size_t)((const char*)a + (__builtin_ctz(m) >> 3) - s);
}

/* Copy n bytes, no loop for n < 8, no byte loop at all (overlapping head/tail accesses). */
static SPF_INLINE void copy_bytes(char* d, const char* s, size_t n)
{
    if (n >= 4) {
        uint32_t last = ld32(s + n - 4);
        char* dl = d + n - 4;
        while (d < dl) {
            st32(d, ld32(s));
            d += 4;
            s += 4;
        }
        st32(dl, last);
    } else if (n >= 2) {
        uint32_t a = ld16(s), b = ld16(s + n - 2);
        st16(d, a);
        st16(d + n - 2, b);
    } else if (n) {
        *d = *s;
    }
}

/* Copies the literal text starting at s up to the next '%' or NUL: reads aligned words, stores
 * unaligned words (the last store spills up to 3 junk bytes which the next output overwrites).
 * Returns true and advances d / sets e (pointing at the '%' or NUL) when done.  Returns false,
 * leaving d alone, if a 4 byte store would go beyond 'dlim' = last position a 4 byte store
 * may start at. */
static SPF_INLINE bool copy_run(const char* s, char*& d, uintptr_t dlim, const char*& e)
{
    const spf_word* a = (const spf_word*)((uintptr_t)s & ~(uintptr_t)3);
    const unsigned off = (unsigned)((uintptr_t)s & 3u);
    const unsigned sh = off << 3;
    uint32_t w = first_word(a, (uintptr_t)s);
    uint32_t m = special_mask(w);
    char* q = d;
    if (SPF_UNLIKELY((uintptr_t)q > dlim)) return false;
    st32(q, w >> sh);
    if (m) {                                        /* the run ends inside the first word */
        unsigned k = (unsigned)__builtin_ctz(m) >> 3;
        d = q + (k - off);
        e = (const char*)a + k;
        return true;
    }
    q += 4 - off;
    while (SPF_LIKELY((uintptr_t)q <= dlim)) {      /* room check doubles as the loop condition */
        w = *++a;
        st32(q, w);                                 /* store first: it overlaps with the scan below */
        m = special_mask(w);
        if (m) {
            unsigned k = (unsigned)__builtin_ctz(m) >> 3;
            d = q + k;
            e = (const char*)a + k;
            return true;
        }
        q += 4;
    }
    return false;
}

/* ------------------------------------------------------------------------------------------
 * Numbers.  All of these write with unaligned 4/2 byte stores, so the caller must guarantee
 * SPF_NUM_MAX bytes of room; the stores spill junk behind the digits.
 * ---------------------------------------------------------------------------------------- */

/* Two decimal digits of v (0..99) as ASCII, first digit in the low byte. */
static SPF_INLINE uint32_t digits2(uint32_t v)
{
    uint32_t t = (v * 103u) >> 10;                              /* v / 10 */
    return (t | ((v - t * 10u) << 8)) + 0x3030u;
}

/* Four decimal digits of v (0..9999) as ASCII, most significant digit in the low byte. */
static SPF_INLINE uint32_t digits4(uint32_t v)
{
    uint32_t a = (v * 5243u) >> 19;                             /* v / 100 */
    uint32_t x = a | ((v - a * 100u) << 16);                    /* two 16 bit lanes <100 */
    uint32_t t = ((x * 103u) >> 10) & 0x000F000Fu;              /* tens of both lanes */
    return (t | ((x - t * 10u) << 8)) + 0x30303030u;
}

/* Bits to shift a digits4() word right to drop its leading '0's (always keeps one digit). */
static SPF_INLINE unsigned lead_zero_bits(uint32_t w)
{
    return (unsigned)__builtin_ctz((w ^ 0x30303030u) | 0x80000000u) & ~7u;
}

/* v / 10000 as one UMULL + shift (UDIV is 2..12 cycles, and -Os would pick it). */
static SPF_INLINE uint32_t div10000(uint32_t v)
{
    return (uint32_t)(((uint64_t)v * 0xD1B71759u) >> 45);
}

/* Decimal v at q (<= 10 bytes), returns the end. */
static SPF_INLINE char* put_u32(char* q, uint32_t v)
{
    if (v < 100u) {
        unsigned one = v < 10u;
        st16(q, digits2(v) >> (one << 3));
        return q + 2 - one;
    }
    if (v < 10000u) {
        uint32_t w = digits4(v);
        unsigned s = lead_zero_bits(w);
        st32(q, w >> s);
        return q + 4 - (s >> 3);
    }
    uint32_t hi = div10000(v);
    uint32_t lo = v - hi * 10000u;
    if (hi < 10000u) {                                          /* v < 10^8 */
        uint32_t w = digits4(hi);
        unsigned s = lead_zero_bits(w);
        st32(q, w >> s);
        q += 4 - (s >> 3);
    } else {                                                    /* 10^8 <= v: hi = h2*10^4 + mid */
        uint32_t h2 = div10000(hi);                             /* 1..42 */
        uint32_t mid = hi - h2 * 10000u;
        unsigned one = h2 < 10u;
        st16(q, digits2(h2) >> (one << 3));
        q += 2 - one;
        st32(q, digits4(mid));
        q += 4;
    }
    st32(q, digits4(lo));
    return q + 4;
}

/* Number of hex digits of x (1 for x == 0). */
static SPF_INLINE unsigned hex_len(uint32_t x)
{
    return (35u - (unsigned)__builtin_clz(x | 1u)) >> 2;
}

/* adj is 39 for lower case and 7 for upper case ('a'-'9'-1, 'A'-'9'-1). */
static SPF_INLINE uint32_t hex2(uint32_t v, uint32_t adj)       /* v <= 0xFF */
{
    uint32_t s = (v >> 4) | ((v & 15u) << 8);
    uint32_t m = ((s + 0x0606u) >> 4) & 0x0101u;                /* 1 where nibble >= 10 */
    return s + 0x3030u + m * adj;
}

static SPF_INLINE uint32_t hex4(uint32_t v, uint32_t adj)       /* v <= 0xFFFF */
{
    uint32_t s = (v >> 12) | (((v >> 8) & 15u) << 8) | (((v >> 4) & 15u) << 16) | ((v & 15u) << 24);
    uint32_t m = ((s + 0x06060606u) >> 4) & 0x01010101u;
    return s + 0x30303030u + m * adj;
}

/* Exactly n hex digits (hex_len(x) <= n <= 8) of x at q, returns the end. */
static SPF_INLINE char* put_hex(char* q, uint32_t x, unsigned n, uint32_t adj)
{
    if (n <= 2) {
        st16(q, hex2(x, adj) >> ((2u - n) << 3));
        return q + n;
    }
    uint32_t lo = hex4(x & 0xFFFFu, adj);
    if (n <= 4) {
        st32(q, lo >> ((4u - n) << 3));
        return q + n;
    }
    st32(q, hex4(x >> 16, adj) >> ((8u - n) << 3));
    q += n - 4;
    st32(q, lo);
    return q + 4;
}

#if SPRINTF_SUPPORT_LL
static SPF_INLINE unsigned hex_len64(uint64_t x)
{
    uint32_t hi = (uint32_t)(x >> 32);
    return hi ? 8 + hex_len(hi) : hex_len((uint32_t)x);
}

/* Exactly n hex digits (hex_len64(x) <= n <= 16). */
static SPF_INLINE char* put_hex64(char* q, uint64_t x, unsigned n, uint32_t adj)
{
    if (n <= 8) return put_hex(q, (uint32_t)x, n, adj);
    q = put_hex(q, (uint32_t)(x >> 32), n - 8, adj);
    return put_hex(q, (uint32_t)x, 8, adj);
}
#endif

/* ------------------------------------------------------------------------------------------
 * Slow paths: buffer (nearly) full, or unusual field widths.  Always safe for any size/width.
 * ---------------------------------------------------------------------------------------- */

/* Append len bytes, clipped to the room left; what doesn't fit is only counted. */
static SPF_NOINLINE char* put_slow(char* p, char* end, const char* s, size_t len, size_t* extra)
{
    size_t room = (size_t)(end - p);
    size_t n = len < room ? len : room;
    copy_bytes(p, s, n);
    *extra += len - n;
    return p + n;
}

static SPF_NOINLINE char* put_fill(char* p, char* end, char c, size_t count, size_t* extra)
{
    size_t room = (size_t)(end - p);
    size_t n = count < room ? count : room;
    for (size_t i = 0; i < n; i++) p[i] = c;
    *extra += count - n;
    return p + n;
}

/* [sign] s[len], padded to 'width' with '0' (after the sign) or ' ' (before the sign). */
static SPF_NOINLINE char* put_padded(char* p, char* end, const char* s, size_t len, unsigned width,
                                     char padc, char sign, size_t* extra)
{
    size_t total = len + (sign != 0);
    size_t pad = width > total ? width - total : 0;
    if (padc != '0') {
        p = put_fill(p, end, ' ', pad, extra);
        pad = 0;
    }
    if (sign) p = put_slow(p, end, &sign, 1, extra);
    p = put_fill(p, end, '0', pad, extra);
    return put_slow(p, end, s, len, extra);
}

/* ------------------------------------------------------------------------------------------
 * vsnprintf / snprintf
 * ---------------------------------------------------------------------------------------- */

extern "C" SPRINTF_ATTR int VSPRINTF_NAME(char* buf, size_t size, const char* fmt, va_list ap)
{
    char dummy;
    char tmp[SPF_NUM_MAX + 4];          /* only used when the buffer is nearly full */
    size_t extra = 0;                   /* characters that did not fit */

    if (SPF_UNLIKELY(size == 0)) {
        buf = &dummy;
        size = 1;
    }
    char* p = buf;
    char* const end = buf + (size - 1);                 /* the slot of the terminating NUL */
    const uintptr_t dlim = (uintptr_t)end - 3;          /* last place where a 4 byte store fits */

    uint32_t v;
    uint32_t adj;
    bool neg;

    for (;;) {
        /* ---- literal text up to the next '%' ---- */
        const char* e = fmt;
        unsigned c = (unsigned char)*e;
        if (c != '%') {
            if (c == 0) break;
            char* d = p;
            if (SPF_LIKELY(copy_run(fmt, d, dlim, e))) {
                p = d;
            } else {
                e = find_special(fmt);
                p = put_slow(p, end, fmt, (size_t)(e - fmt), &extra);
            }
            if (*e == 0) break;
        }
        fmt = e + 1;

        /* ---- conversion ---- */
        unsigned width = 0;
        unsigned lcnt = 0;
        char padc = ' ';
    again:
        c = (unsigned char)*fmt++;
        switch (c) {
        case '0':
            if (width == 0) {
                padc = '0';
                goto again;
            }
            [[fallthrough]];
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            width = width * 10 + (c - '0');
            goto again;

        case 'l':
        case 'z':
            lcnt++;
            goto again;

        case 'd':
        case 'i':
            v = (uint32_t)va_arg(ap, int);
            neg = (int32_t)v < 0;
            if (neg) v = 0u - v;
            goto decimal;
        case 'u':
            v = va_arg(ap, unsigned);
            neg = false;
        decimal:
            if (lcnt > 1) __builtin_trap();             /* 64 bit decimal is not supported */
            {
                /* In place if there is room and no padding is needed, else via tmp[]. */
                const bool plain = width == 0;
                const bool inplace = plain && (size_t)(end - p) >= SPF_NUM_MAX;
                char* q = inplace ? p : tmp;
                char* s = q;
                if (neg && plain) *s++ = '-';
                s = put_u32(s, v);
                if (inplace)
                    p = s;
                else if (plain)
                    p = put_slow(p, end, tmp, (size_t)(s - tmp), &extra);
                else
                    p = put_padded(p, end, tmp, (size_t)(s - tmp), width, padc, neg ? '-' : 0, &extra);
            }
            continue;

        case 'x':
            adj = 39;
            goto hex;
        case 'X':
            adj = 7;
        hex:
            {
#if SPRINTF_SUPPORT_LL
                const bool ll = lcnt > 1;
                const unsigned maxw = ll ? 16 : 8;
                uint64_t x = ll ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
                unsigned n = ll ? hex_len64(x) : hex_len((uint32_t)x);
#else
                const unsigned maxw = 8;
                uint32_t x = va_arg(ap, unsigned);
                unsigned n = hex_len(x);
#endif
                /* Zero padding is free: just ask for more digits. */
                const bool zp = width == 0 || (padc == '0' && width <= maxw);
                if (zp && n < width) n = width;
                const bool inplace = zp && (size_t)(end - p) >= SPF_NUM_MAX;
                char* q = inplace ? p : tmp;
#if SPRINTF_SUPPORT_LL
                char* s = ll ? put_hex64(q, x, n, adj) : put_hex(q, (uint32_t)x, n, adj);
#else
                char* s = put_hex(q, x, n, adj);
#endif
                if (inplace)
                    p = s;
                else if (zp)
                    p = put_slow(p, end, tmp, (size_t)(s - tmp), &extra);
                else
                    p = put_padded(p, end, tmp, (size_t)(s - tmp), width, padc, 0, &extra);
            }
            continue;

        case 's': {
            const char* s = va_arg(ap, const char*);
            if (width == 0 && *s == 0) continue;        /* empty strings are common */
            size_t len = string_length(s);
            if (width != 0) {
                p = put_padded(p, end, s, len, width, ' ', 0, &extra);
            } else if (SPF_LIKELY(len <= (size_t)(end - p))) {
                copy_bytes(p, s, len);
                p += len;
            } else {
                p = put_slow(p, end, s, len, &extra);
            }
            continue;
        }

#if SPRINTF_SUPPORT_C
        case 'c':
            if (width == 0) {
                if (SPF_LIKELY(p < end)) *p++ = (char)va_arg(ap, int); else { (void)va_arg(ap, int); extra++; }
            } else {
                tmp[0] = (char)va_arg(ap, int);
                p = put_padded(p, end, tmp, 1, width, ' ', 0, &extra);
            }
            continue;
#endif

        case '%':
            if (SPF_LIKELY(p < end)) *p++ = '%'; else extra++;
            continue;

        default:
            __builtin_trap();                           /* unsupported conversion */
        }
    }

    *p = 0;
    return (int)((size_t)(p - buf) + extra);
}

extern "C" SPRINTF_ATTR int SPRINTF_NAME(char* buf, size_t size, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = VSPRINTF_NAME(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

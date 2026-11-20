/*
 * sprintf_test.cpp - host side test of sprintf.cpp against the C library's snprintf().
 * Not part of the firmware.  Needs a 64 bit Linux/POSIX host (uses mmap guard pages).
 *
 *   g++ -std=gnu++2a -O2 -Wall -Wextra sprintf_test.cpp -o sprintf_test
 *   ./sprintf_test fmt.txt                 # the format strings of the file + fuzzing
 *   ./sprintf_test fmt.txt --exhaustive    # + all 2^32 values through the number routines
 *                                          #   (add --part=K/N to run only slice K of N)
 *
 * The file fmt.txt is a list of format strings whose rendering of this snprintf() will be check
 * against the rendering of the system libc's snprintf().
 *
 * What is checked: output, return value and NUL termination for every buffer size from 0 up,
 * no write outside the buffer, no read outside the format string / %s arguments (both are
 * placed right in front of an unmapped page), all digit counts, all field widths.
 */
#define SPRINTF_NAME test_snprintf
#define VSPRINTF_NAME test_vsnprintf
#include "sprintf.cpp"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string>
#include <vector>

/* ---------------------------------------------------------------- random numbers */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd()
{
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
}
static uint32_t rnd_bits32()                      /* uniform number of significant bits */
{
    unsigned bits = (unsigned)(rnd() % 33);
    return bits == 0 ? 0u : (uint32_t)rnd() >> (32 - bits);
}
static uint64_t rnd_bits64()
{
    unsigned bits = (unsigned)(rnd() % 65);
    return bits == 0 ? 0ull : rnd() >> (64 - bits);
}

/* ---------------------------------------------------------------- guard page memory */
static const size_t PG = 4096;
struct Slot {                                       /* data placed right before an unmapped page */
    char* base;
    char* place(size_t n)                           /* n <= PG */
    {
        return base + PG - n;
    }
};
static Slot make_slot()
{
    char* m = (char*)mmap(nullptr, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(1); }
    mprotect(m + PG, PG, PROT_NONE);
    return Slot{m};
}

static const char* g_what = "";
static void on_segv(int)
{
    const char msg[] = "\nFAIL: SIGSEGV - out of bounds access while testing:\n";
    (void)!write(2, msg, sizeof msg - 1);
    (void)!write(2, g_what, strlen(g_what));
    (void)!write(2, "\n", 1);
    _exit(2);
}

/* ---------------------------------------------------------------- calling both implementations */
#define A20(a) a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],a[16],a[17],a[18],a[19]
typedef unsigned long long ull;
static int call_ref(char* b, size_t n, const char* f, const ull* a) { return snprintf(b, n, f, A20(a)); }
static int call_tst(char* b, size_t n, const char* f, const ull* a) { return test_snprintf(b, n, f, A20(a)); }

/* ---------------------------------------------------------------- one comparison */
static unsigned long g_checks = 0;
static bool g_guard = false;
static Slot g_fmt_slot, g_out_slot, g_str_slot[20];

static bool fail(const char* fmt, const char* why, size_t n)
{
    fprintf(stderr, "FAIL: %s (size %zu)\n  format: \"%s\"\n", why, n, fmt);
    exit(1);
}

/* Compare for one buffer size. */
static void check_size(const char* fmt, const ull* a, const char* ref, int rl, size_t n)
{
    unsigned char canary[48];
    memset(canary, 0xA5, sizeof canary);
    char* b;
    unsigned char* mem = nullptr;
    if (g_guard) {
        b = g_out_slot.place(n ? n : 1);
        if (n == 0) { b = g_out_slot.place(1); }
    } else {
        mem = (unsigned char*)malloc(n + 32);
        memset(mem, 0xA5, n + 32);
        b = (char*)mem + 16;
    }
    g_what = fmt;
    int r = call_tst(b, n, fmt, a);
    g_what = "";
    if (r != rl) {
        fprintf(stderr, "got return %d, expected %d\n", r, rl);
        fail(fmt, "return value", n);
    }
    if (n) {
        size_t len = (size_t)rl < n - 1 ? (size_t)rl : n - 1;
        if (memcmp(b, ref, len) != 0) {
            fprintf(stderr, "got      \"%.*s\"\nexpected \"%.*s\"\n", (int)len, b, (int)len, ref);
            fail(fmt, "output differs", n);
        }
        if (b[len] != 0) fail(fmt, "not NUL terminated", n);
    }
    if (mem) {
        for (int i = 0; i < 16; i++) if (mem[i] != 0xA5) fail(fmt, "wrote before the buffer", n);
        size_t from = 16 + n;
        if (n == 0) from = 16;                      /* size 0 must not touch the buffer at all */
        for (size_t i = from; i < n + 32; i++)
            if (mem[i] != 0xA5) fail(fmt, "wrote beyond the buffer", n);
        free(mem);
    }
    g_checks++;
}

/* Check a format with the given args for a spread of buffer sizes. */
static void check(const char* fmt, const ull* a)
{
    static char ref[4096];
    int rl = call_ref(ref, sizeof ref, fmt, a);
    if (rl < 0 || rl >= (int)sizeof ref - 1) fail(fmt, "test case too long", 0);
    size_t total = (size_t)rl;
    if (total <= 90) {
        for (size_t n = 0; n <= total + 3; n++) check_size(fmt, a, ref, rl, n);
    } else {
        for (size_t n = 0; n <= 20; n++) check_size(fmt, a, ref, rl, n);
        for (size_t n = total - 20; n <= total + 3; n++) check_size(fmt, a, ref, rl, n);
        for (int i = 0; i < 6; i++) check_size(fmt, a, ref, rl, 21 + rnd() % (total - 40));
    }
    check_size(fmt, a, ref, rl, total + 1);
    check_size(fmt, a, ref, rl, total + 64);
}

/* ---------------------------------------------------------------- argument generation */
static std::string rnd_string()
{
    static const char pool[] = "abcXYZ019 _-.,:;=<>()[]{}/\\\"'!?#$&*+~^|@`\x01\x02\x1f\x7f\x80\xa5\xc3\xe2\xfe\xff%%%";
    static const char pool2[] = "abcdefghijklmnopqrstuvwxyz0123456789 ";
    size_t len = rnd() % 4 == 0 ? rnd() % 4 : rnd() % 48;
    const char* p = rnd() % 2 ? pool : pool2;
    size_t pl = strlen(p);
    std::string s;
    for (size_t i = 0; i < len; i++) s += p[rnd() % pl];
    return s;
}

static const uint32_t specials[] = {0, 1, 9, 10, 11, 99, 100, 101, 999, 1000, 9999, 10000, 99999, 100000, 999999,
    1000000, 9999999, 10000000, 99999999, 100000000, 100000001, 999999999, 1000000000, 1000000001,
    4294967295u, 4294967294u, 2147483647u, 2147483648u, 2147483649u, 0xFF, 0x100, 0xFFFF, 0x10000,
    0xFFFFFF, 0x1000000, 0xFFFFFFF, 0x10000000, 0xF, 0x10, 0x7F, 0x80};

/* Argument kinds: d int32, u uint32, L uint64, s string, c char */
static void make_args(const std::string& kinds, ull* a, std::vector<std::string>& keep)
{
    keep.clear();
    keep.reserve(20);
    int si = 0;
    for (size_t i = 0; i < kinds.size(); i++) {
        switch (kinds[i]) {
        case 'd': {
            uint32_t v = rnd() % 6 == 0 ? specials[rnd() % (sizeof specials / sizeof *specials)] : rnd_bits32();
            if (rnd() & 1) v = 0u - v;
            a[i] = (ull)(long long)(int32_t)v;
            break;
        }
        case 'u':
            a[i] = rnd() % 6 == 0 ? specials[rnd() % (sizeof specials / sizeof *specials)] : rnd_bits32();
            break;
        case 'L':
            a[i] = rnd() % 6 == 0 ? (ull)specials[rnd() % (sizeof specials / sizeof *specials)] << (rnd() % 2 ? 32 : 0)
                                  : rnd_bits64();
            break;
        case 'c':
            a[i] = 1 + rnd() % 255;
            break;
        case 's': {
            keep.push_back(rnd_string());
            const std::string& s = keep.back();
            char* dst;
            if (g_guard) {
                dst = g_str_slot[si++].place(s.size() + 1);
            } else {                                /* random alignment */
                static char heap[20][128];
                dst = heap[si++] + rnd() % 4;
            }
            memcpy(dst, s.c_str(), s.size() + 1);
            a[i] = (ull)(uintptr_t)dst;
            break;
        }
        }
    }
}

/* Kinds string for a format (same grammar as the implementation). */
static std::string kinds_of(const char* f)
{
    std::string k;
    for (; *f; f++) {
        if (*f != '%') continue;
        f++;
        int ll = 0;
        while (*f == '0' || (*f >= '1' && *f <= '9') || *f == 'l' || *f == 'z') ll += *f++ == 'l';
        switch (*f) {
        case 'd': case 'i': k += 'd'; break;
        case 'u': k += 'u'; break;
        case 'x': case 'X': k += ll > 1 ? 'L' : 'u'; break;
        case 's': k += 's'; break;
        case 'c': k += 'c'; break;
        case '%': break;
        default: fprintf(stderr, "test: unsupported conversion '%c' in \"%s\"\n", *f, f); exit(1);
        }
    }
    return k;
}

/* Run a format a number of times with random arguments. */
static void run_format(const std::string& fmt_str, int iterations)
{
    std::string kinds = kinds_of(fmt_str.c_str());
    ull a[20] = {0};
    std::vector<std::string> keep;
    for (int it = 0; it < iterations; it++) {
        make_args(kinds, a, keep);
        const char* f;
        if (g_guard) {
            f = g_fmt_slot.place(fmt_str.size() + 1);
            memcpy((char*)f, fmt_str.c_str(), fmt_str.size() + 1);
        } else {
            static char buf[4096 + 8];
            char* d = buf + rnd() % 4;              /* random alignment of the format */
            memcpy(d, fmt_str.c_str(), fmt_str.size() + 1);
            f = d;
        }
        check(f, a);
    }
}

/* ---------------------------------------------------------------- tests */
static std::vector<std::string> load_formats(const char* file)
{
    std::vector<std::string> out;
    FILE* fp = fopen(file, "rb");
    if (!fp) { perror(file); exit(1); }
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        if (line[0] != '"') continue;
        std::string s;
        const char* p = line + 1;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                s += *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                p++;
            } else {
                s += *p++;
            }
        }
        out.push_back(s);
    }
    fclose(fp);
    return out;
}

static std::string rnd_format()
{
    static const char lit[] = "abcdefghijklmnopqrstuvwxyz ABC0123456789 :,.=()[]<>-_/\\!&$\x01\x80\xc2\xb5\xff";
    std::string f;
    int items = (int)(rnd() % 7);
    int nargs = 0;
    for (int i = 0; i <= items && nargs < 16; i++) {
        size_t len = rnd() % 3 == 0 ? rnd() % 3 : rnd() % 30;
        for (size_t j = 0; j < len; j++) f += lit[rnd() % (sizeof lit - 1)];
        if (i == items && rnd() % 2) break;
        f += '%';
        int pick = (int)(rnd() % 12);
        bool numeric = pick < 9;
        if (numeric) {
            if (rnd() % 2) f += '0';
            if (rnd() % 3 == 0) f += std::to_string(rnd() % 14);
            if (pick >= 4 && pick < 7 && rnd() % 3 == 0) f += "ll";
            else if (rnd() % 6 == 0) f += rnd() % 2 ? "l" : "z";
        }
        static const char convs[] = "diuuxXxXXsscc%";
        char c = convs[pick];
        if (!numeric && c != '%' && rnd() % 3 == 0) f += std::to_string(rnd() % 20);    /* %5s %3c */
        if (!numeric && c == '%') { f += '%'; continue; }
        f += c;
        nargs++;
    }
    /* "ll" is only supported for x/X, "l"/"z" modifiers before d/u/x/X only */
    std::string g;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] == '%' && i + 1 < f.size() && f[i + 1] != '%') {
            size_t j = i + 1;
            while (j < f.size() && strchr("0123456789lz", f[j])) j++;
            std::string mods = f.substr(i + 1, j - i - 1);
            char c = f[j];
            bool has_ll = mods.find("ll") != std::string::npos;
            if (has_ll && c != 'x' && c != 'X') mods.erase(mods.find("ll"), 2);
            if ((c == 's' || c == 'c') && (mods.find('l') != std::string::npos || mods.find('z') != std::string::npos))
                mods.erase(mods.find_first_of("lz"), 1);
            if ((c == 's' || c == 'c') && !mods.empty() && mods[0] == '0') mods[0] = '1';
            g += '%' + mods + c;
            i = j;
        } else if (f[i] == '%') {
            g += "%%";
            i++;
        } else {
            g += f[i];
        }
    }
    return g;
}

static void test_directed()
{
    ull a[20] = {0};
    const char* lits[] = {"", "a", "ab", "abc", "abcd", "abcde", "%%", "%%&x", "&%%&", "%%%%%%", "\x01%%\x26\x01",
                          "0123456789012345678901234567890123456789", "%c", "\xff\xff\xff\xff\xff\xff\xff\xff"};
    for (const char* l : lits) {
        for (int pass = 0; pass < 2; pass++) {
            g_guard = pass;
            run_format(l, 4);
        }
    }
    /* signs and padding */
    const char* fm[] = {"%d|%5d|%05d|%2d", "%05d|%5d|%2d|%02d", "%u|%5u|%05u|%2u", "%x|%5x|%05x|%2x|%02x",
                        "%X|%5X|%05X|%2X|%02X", "%s|%5s|%2s|%12s", "[%c|%3c|%1c]"};
    for (int pass = 0; pass < 2; pass++) {
        g_guard = pass;
        for (const char* f : fm) run_format(f, 300);
    }
    (void)a;
}

static void test_runs()
{
    /* literal runs of every length 0..75, with and without an embedded "%%", every start alignment
       (heap: random alignment, guard page: alignment follows from the length) */
    for (int pass = 0; pass < 2; pass++) {
        g_guard = pass;
        for (int len = 0; len <= 75; len++) {
            std::string s;
            for (int i = 0; i < len; i++) s += (char)('a' + i % 26);
            run_format(s, pass ? 2 : 6);
            for (int at = 0; at <= len; at += 1 + len / 9) {
                std::string t = s.substr(0, at) + "%%" + s.substr(at);
                run_format(t, pass ? 1 : 3);
                run_format(t + "%d" + s, pass ? 1 : 3);
                run_format("%u" + t + "%s", pass ? 1 : 3);
            }
        }
    }
#if SPRINTF_SUPPORT_LL
    /* 64 bit hex: every digit count and field width */
    for (int i = 0; i < 300000; i++) {
        uint64_t x = rnd_bits64();
        unsigned n = hex_len64(x);
        unsigned w = n + (unsigned)(rnd() % (17 - n));
        char out[40], ref[40];
        bool upper = rnd() & 1;
        char* e = put_hex64(out, x, w, upper ? 7 : 39);
        snprintf(ref, sizeof ref, upper ? "%0*llX" : "%0*llx", (int)w, (unsigned long long)x);
        if ((size_t)(e - out) != strlen(ref) || memcmp(out, ref, strlen(ref)) != 0) {
            fprintf(stderr, "put_hex64 wrong: %llx width %u -> %.*s, expected %s\n", (unsigned long long)x, w, (int)(e - out), out, ref);
            exit(1);
        }
    }
#endif
}

static void test_exhaustive(unsigned part, unsigned parts)
{
    const uint64_t lo_i = (0x100000000ull * part) / parts, hi_i = (0x100000000ull * (part + 1)) / parts;
    printf("exhaustive part %u/%u: values %llu..%llu\n", part, parts, (unsigned long long)lo_i, (unsigned long long)hi_i - 1);
    printf("exhaustive: %%u ...\n");
    fflush(stdout);
    char out[32];
    for (uint64_t i = lo_i; i < hi_i; i++) {
        uint32_t v = (uint32_t)i;
        char* e = put_u32(out, v);
        char ref[12];
        int len = 0;
        uint32_t x = v;
        do { ref[len++] = (char)('0' + x % 10); x /= 10; } while (x);
        if (e - out != len) { fprintf(stderr, "length wrong for %u\n", v); exit(1); }
        for (int k = 0; k < len; k++)
            if (out[k] != ref[len - 1 - k]) { fprintf(stderr, "digits wrong for %u\n", v); exit(1); }
    }
    printf("exhaustive: %%x/%%X (several widths) ...\n");
    fflush(stdout);
    static const char up[] = "0123456789ABCDEF", lo[] = "0123456789abcdef";
    for (uint64_t i = lo_i; i < hi_i; i++) {
        uint32_t v = (uint32_t)i;
        unsigned hl = hex_len(v);
        unsigned want = 1;
        for (uint32_t t = v >> 4; t; t >>= 4) want++;
        if (hl != want) { fprintf(stderr, "hex_len wrong for %x\n", v); exit(1); }
        unsigned ns[3] = {hl, 8, hl + (unsigned)(rnd() % (9 - hl))};
        for (unsigned n : ns) {
            bool upper = (i & 1) != 0;
            char* e = put_hex(out, v, n, upper ? 7 : 39);
            if (e - out != (ptrdiff_t)n) { fprintf(stderr, "hex length wrong for %x n=%u\n", v, n); exit(1); }
            for (unsigned k = 0; k < n; k++) {
                char c = (upper ? up : lo)[(v >> (4 * (n - 1 - k))) & 15];
                if (out[k] != c) { fprintf(stderr, "hex digits wrong for %x n=%u\n", v, n); exit(1); }
            }
        }
    }
    printf("exhaustive: div10000 ...\n");
    for (uint64_t i = lo_i; i < hi_i; i++)
        if (div10000((uint32_t)i) != (uint32_t)i / 10000u) { fprintf(stderr, "div10000 wrong for %llu\n", (unsigned long long)i); exit(1); }
}

int main(int argc, char** argv)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_segv;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);

    g_fmt_slot = make_slot();
    g_out_slot = make_slot();
    for (auto& s : g_str_slot) s = make_slot();

    const char* file = argc > 1 && argv[1][0] != '-' ? argv[1] : nullptr;
    bool exhaustive = false;
    unsigned ex_part = 0, ex_parts = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exhaustive")) exhaustive = true;
        if (!strncmp(argv[i], "--part=", 7)) sscanf(argv[i] + 7, "%u/%u", &ex_part, &ex_parts);   /* e.g. --part=3/16 */
    }

    test_directed();
    test_runs();
    printf("directed tests ok (%lu comparisons)\n", g_checks);

    if (file) {
        std::vector<std::string> fm = load_formats(file);
        for (int pass = 0; pass < 2; pass++) {
            g_guard = pass;
            for (const std::string& f : fm) run_format(f, pass ? 8 : 60);
            printf("%zu formats from %s, %s memory: ok (%lu comparisons)\n", fm.size(), file,
                   pass ? "guard page" : "heap", g_checks);
        }
    }

    for (int pass = 0; pass < 2; pass++) {
        g_guard = pass;
        int n = pass ? 20000 : 150000;
        for (int i = 0; i < n; i++) {
            std::string f = rnd_format();
            run_format(f, 1);
        }
        printf("random formats, %s memory: ok (%lu comparisons)\n", pass ? "guard page" : "heap", g_checks);
    }

    if (exhaustive) test_exhaustive(ex_part, ex_parts);
    printf("ALL TESTS PASSED\n");
    return 0;
}

// Diablo III: the game's zlib (1.2.3, built into main) decodes its compressed data with a fast inner loop at
// main+0x12590 (zlib's "inflate_fast": the decoder's hot path, used while at least 6 input bytes and 258 output bytes
// are free). Lifted to C it took about half a second of the ~1.2 s freeze after pressing Start Game (one loader thread,
// sampled with LARECOMP_SAMPLE; the tester 2026-11-15). This is the same step written for the host: same inputs (the
// stream and the decoder state the game's zlib keeps, at 1.2.3's offsets), same output bytes and the same state left
// behind, so the game's own inflate() carries on from it exactly as before. Written here from the deflate format
// (RFC 1951) and zlib's documented table layout; no zlib source is copied.
//
// src/analysis/d3_gen_patch.py puts "if (d3_inflate_fast(c)) return;" at the generated function's start (and the
// check below at its end). D3_NATIVE_INFLATE=0: the lifted code as before. D3_INFLATE_CHECK=1 (test): both run; the
// native result is compared with the lifted one (output bytes and every state field) and differences are logged.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu.h"
#include "../hle.h"

// z_stream (64-bit): next_in 0, avail_in 8, next_out 0x18, avail_out 0x20, msg 0x30, state 0x38
// inflate_state (1.2.3): mode 0, wsize 52, whave 56, write 60, window 64, hold 72, bits 80, lencode 96, distcode 104,
// lenbits 112, distbits 116
typedef struct { uint8_t op, bits; uint16_t val; } Code;
enum { MODE_TYPE = 11, MODE_BAD = 27 };
// the game's own messages for the two errors (its rodata, as the lifted code points at them)
#define MSG_BAD_DIST 0x7000e18b6eull   // "invalid distance too far back" (strm->msg, as the lifted code sets it)
#define MSG_BAD_DCODE 0x7000e32ee1ull  // "invalid distance code"
#define MSG_BAD_LCODE 0x7000e73cbaull  // "invalid literal/length code"

#define U8(a) (*(uint8_t *)(uintptr_t)(a))
#define U32(a) (*(uint32_t *)(uintptr_t)(a))
#define U64(a) (*(uint64_t *)(uintptr_t)(a))

static void run(uint64_t strm, uint32_t start) {
    uint64_t st = U64(strm + 0x38);
    const uint8_t *in = (const uint8_t *)(uintptr_t)U64(strm + 0);
    const uint8_t *last = in + U32(strm + 8) - 5;  // (loop while in < last: 6+ bytes left)
    uint8_t *out = (uint8_t *)(uintptr_t)U64(strm + 0x18);
    uint32_t avail_out = U32(strm + 0x20);
    uint8_t *beg = out - (start - avail_out);       // (the output of this inflate() call so far starts here)
    uint8_t *end = out + avail_out - 257;           // (loop while out < end: 258+ bytes of room)
    uint32_t wsize = U32(st + 52), whave = U32(st + 56), wnext = U32(st + 60);
    const uint8_t *window = (const uint8_t *)(uintptr_t)U64(st + 64);
    uint64_t hold = U64(st + 72);
    uint32_t bits = U32(st + 80);
    const Code *lcode = (const Code *)(uintptr_t)U64(st + 96), *dcode = (const Code *)(uintptr_t)U64(st + 104);
    uint32_t lmask = (1u << U32(st + 112)) - 1, dmask = (1u << U32(st + 116)) - 1;
    uint64_t msg = 0;
    int mode = -1;
#define NEED_BYTE() (hold += (uint64_t)*in++ << bits, bits += 8)
    do {
        if (bits < 15) { NEED_BYTE(); NEED_BYTE(); }
        Code h = lcode[hold & lmask];
        for (;;) {  // (a length / literal code, through any second-level tables)
            hold >>= h.bits;
            bits -= h.bits;
            uint32_t op = h.op;
            if (op == 0) { *out++ = (uint8_t)h.val; break; }  // literal
            if (op & 16) {  // length base
                uint32_t len = h.val;
                op &= 15;
                if (op) {
                    if (bits < op) NEED_BYTE();
                    len += (uint32_t)hold & ((1u << op) - 1);
                    hold >>= op;
                    bits -= op;
                }
                if (bits < 15) { NEED_BYTE(); NEED_BYTE(); }
                Code d = dcode[hold & dmask];
                for (;;) {  // (its distance code)
                    hold >>= d.bits;
                    bits -= d.bits;
                    op = d.op;
                    if (op & 16) break;
                    if (op & 64) { msg = MSG_BAD_DCODE, mode = MODE_BAD; goto done; }
                    d = dcode[d.val + (hold & ((1u << op) - 1))];
                }
                uint32_t dist = d.val;
                op &= 15;
                if (bits < op) { NEED_BYTE(); if (bits < op) NEED_BYTE(); }
                dist += (uint32_t)hold & ((1u << op) - 1);
                hold >>= op;
                bits -= op;
                uint32_t have = (uint32_t)(out - beg);  // (bytes of this call's output before here)
                if (dist > have) {  // reaches back into the window (the output of earlier calls)
                    uint32_t back = dist - have;
                    if (back > whave) { msg = MSG_BAD_DIST, mode = MODE_BAD; goto done; }
                    const uint8_t *from;
                    if (wnext == 0) {  // the window ends at its end
                        from = window + wsize - back;
                        if (back < len) { len -= back; while (back--) *out++ = *from++; from = out - dist; }
                    } else if (wnext < back) {  // wraps: the window's end part, then its start
                        from = window + wsize + wnext - back;
                        back -= wnext;
                        if (back < len) {
                            len -= back;
                            while (back--) *out++ = *from++;
                            from = window;
                            if (wnext < len) {
                                back = wnext;
                                len -= back;
                                while (back--) *out++ = *from++;
                                from = out - dist;
                            }
                        }
                    } else {  // inside the window's filled part
                        from = window + wnext - back;
                        if (back < len) { len -= back; while (back--) *out++ = *from++; from = out - dist; }
                    }
                    while (len--) *out++ = *from++;
                } else {  // from this call's output (may overlap itself: byte by byte, forwards)
                    const uint8_t *from = out - dist;
                    while (len--) *out++ = *from++;
                }
                break;
            }
            if (op & 64) {
                if (op & 32) mode = MODE_TYPE;  // end of block
                else msg = MSG_BAD_LCODE, mode = MODE_BAD;
                goto done;
            }
            h = lcode[h.val + (hold & ((1u << op) - 1))];  // second-level table
        }
    } while (in < last && out < end);
done:
    {   // whole bytes still in the bit buffer go back to the input
        uint32_t back = bits >> 3;
        in -= back;
        bits -= back << 3;
        hold &= ((uint64_t)1 << bits) - 1;
    }
    if (msg) U64(strm + 0x30) = msg;
    if (mode >= 0) U32(st + 0) = (uint32_t)mode;
    U64(strm + 0) = (uint64_t)(uintptr_t)in;
    U64(strm + 0x18) = (uint64_t)(uintptr_t)out;
    U32(strm + 8) = (uint32_t)(in < last ? 5 + (last - in) : 5 - (in - last));
    U32(strm + 0x20) = (uint32_t)(out < end ? 257 + (end - out) : 257 - (out - end));
    U64(st + 72) = hold;
    U32(st + 80) = bits;
#undef NEED_BYTE
}

// ---- check mode: the native result kept, the stream / state put back, the lifted code runs, then both compared
typedef struct {
    uint64_t strm, in, out, msg, hold;
    uint32_t avail_in, avail_out, bits, mode;
} Snap;
static void snap(uint64_t strm, Snap *s) {
    uint64_t st = U64(strm + 0x38);
    *s = (Snap){strm, U64(strm), U64(strm + 0x18), U64(strm + 0x30), U64(st + 72), U32(strm + 8), U32(strm + 0x20),
                U32(st + 80), U32(st)};
}
static void restore(const Snap *s) {
    uint64_t st = U64(s->strm + 0x38);
    U64(s->strm) = s->in, U64(s->strm + 0x18) = s->out, U64(s->strm + 0x30) = s->msg, U64(st + 72) = s->hold;
    U32(s->strm + 8) = s->avail_in, U32(s->strm + 0x20) = s->avail_out, U32(st + 80) = s->bits, U32(st) = s->mode;
}
static __thread Snap t_before, t_native;
static __thread uint8_t *t_bytes;
static __thread size_t t_nbytes;
static __thread bool t_checking;

static int mode_flag(void) {  // 0 lifted, 1 native, 2 check
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("D3_NATIVE_INFLATE");
        m = e && *e && !atoi(e) ? 0 : getenv("D3_INFLATE_CHECK") ? 2 : 1;
        LOG("[d3] zlib inner loop: %s\n", m == 0 ? "lifted" : m == 1 ? "native" : "native, checked against lifted");
    }
    return m;
}

bool d3_inflate_fast(Cpu *c) {
    int m = mode_flag();
    if (m == 0) return false;
    if (m == 1) { run(c->x[0], (uint32_t)c->x[1]); return true; }
    snap(c->x[0], &t_before);
    run(c->x[0], (uint32_t)c->x[1]);
    snap(c->x[0], &t_native);
    t_nbytes = (size_t)(t_native.out - t_before.out);
    free(t_bytes);
    t_bytes = malloc(t_nbytes ? t_nbytes : 1);
    memcpy(t_bytes, (const void *)(uintptr_t)t_before.out, t_nbytes);
    restore(&t_before);
    t_checking = true;
    return false;  // (the lifted code runs now; d3_inflate_check at its end)
}

void d3_inflate_check(Cpu *c) {
    if (!t_checking) return;
    t_checking = false;
    (void)c;
    Snap lifted;
    snap(t_before.strm, &lifted);
    static uint64_t calls, bad;
    calls++;
    size_t n = (size_t)(lifted.out - t_before.out);
    bool same = !memcmp(&lifted, &t_native, sizeof lifted) && n == t_nbytes &&
                !memcmp(t_bytes, (const void *)(uintptr_t)t_before.out, n);
    if (!same && bad++ < 20)
        LOG("[d3] zlib check: DIFFERENT (call %llu): out %zu/%zu bytes, in %llx/%llx, hold %llx/%llx bits %u/%u mode %u/%u\n",
            (unsigned long long)calls, n, t_nbytes, (unsigned long long)lifted.in, (unsigned long long)t_native.in,
            (unsigned long long)lifted.hold, (unsigned long long)t_native.hold, lifted.bits, t_native.bits, lifted.mode,
            t_native.mode);
    if ((calls & (calls - 1)) == 0 && calls >= 64)
        LOG("[d3] zlib check: %llu calls, %llu different\n", (unsigned long long)calls, (unsigned long long)bad);
}

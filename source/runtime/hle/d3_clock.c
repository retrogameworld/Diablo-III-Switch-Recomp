// Diablo III at 120 fps: the hero and the monsters looked doubled while moving (the tester 2026-11-17: "as if he is a doubled
// silhouette" - only on the monitor, never in a screenshot, never at 60 fps, coming after a while, gone again after a
// long freeze).
//
// WHAT IT WAS. On the screen the hero jumped forth and back against the camera on alternate frames - up to 7.5 px at
// 4K while running - which the eye sees as two overlapping heroes. The characters' drawn positions move every frame;
// the camera (and the rest of what the game moves by "game time") moved unevenly. The cause is the time the game
// hands each frame:
//   1. Its frame function (main+0x4c84f0) keeps a smoothed frame clock: T ([game+0x2398], units of 1/3000 s) goes on
//      by a smoothed frame length D ([game+0x23a0], 50 = 1/60 s to begin with), stands still while it is ahead of the
//      real clock, and starts over when 50 ms behind. Made for 60 fps; at 120 fps it can settle on "50 every second
//      frame, nothing in between".
//   2. Inside the frame (main+0x40810) a game timer ([game+0x10]+0x34) turns T into the game time, counted in 1/60 s
//      steps (50 units). Measured in hidden runs with the frame time made even: the zigzag is 0.3 px at 4K for every
//      unit a frame's game time sits past a half-step boundary - 0.1 px at 0, 0.6 px at 2, 3.6 px at 12, 6.3 px at 21,
//      set on purpose, before and after a pause alike. A pause (the pause menu, or the game's own pause after a frame
//      of 63 ms or more: main+0x404e0) leaves the game time anywhere against the frames - hence "it comes after a
//      while", and a long freeze, being such a pause, could "cure" it by chance.
// THE FIX (d3_frame_time below, d3_gen_patch.py #15): at 120 fps the frame's time is ours - on by exactly half a step
// (25 units) per frame, and nudged after a pause until every frame's game time is a whole number of half steps.
// At 60 fps nothing here runs. D3_FRAME_CLOCK=0: the game's own clock.
//
// The rest of this file is what the hunt left behind, all off unless asked for, to be cleared out once the tester has
// confirmed the fix: test logs (D3_CLOCK_LOG=1..4: [d3clock] settings, [d3c] per present, [d3f] per frame, [d3t]
// world steps, [d3l] lock / event users), the world thread waiting for the frames (d3_sim_wait, D3_SIM_SYNC=1 - made
// no difference once the game time was in place; d3_world_wait, D3_WORLD_SYNC=1 - the wrong thread), and the first,
// wrong idea (the game's clock held back: d3_clock_ns / d3_clock_frame's DRAIN part, D3_CLOCK_SYNC=1).
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../hle.h"

#define BASE_PPM 10000u                // the clock's slowdown above 60 fps (1%)
#define BOOST_PPM 200000u             // ... while making up for a late frame (20%)
#define LATE_NS 8000000ull            // a frame this late for its slot: a display tick may be lost
#define BOOST_MAX_NS 200000000ull     // the most the clock gives back at the fast rate

// ---- the clock's lag: base, plus rate x the time since t0 (rate 0: constant). Read by every thread that asks for
// the time; written by the frame pacer's thread only (a sequence counter around the three values).
static _Atomic uint32_t lag_seq;
static _Atomic uint64_t lag_base, lag_t0;
static _Atomic uint32_t lag_ppm;
static _Atomic bool engaged;          // false until the first calibration: the clock untouched
static _Atomic uint64_t last_ns;      // the latest time handed out (never backwards across a change)

static uint64_t lag_at(uint64_t now) {
    for (;;) {
        uint32_t s = atomic_load(&lag_seq);
        if (s & 1) continue;
        uint64_t b = atomic_load(&lag_base), t = atomic_load(&lag_t0);
        uint32_t r = atomic_load(&lag_ppm);
        if (atomic_load(&lag_seq) != s) continue;
        return b + (r && now > t ? (now - t) / 1000000ull * r + (now - t) % 1000000ull * r / 1000000ull : 0);
    }
}
static void lag_rate(uint64_t now, uint32_t ppm) {
    if (atomic_load(&lag_ppm) == ppm) return;
    uint64_t b = lag_at(now);
    atomic_fetch_add(&lag_seq, 1);
    atomic_store(&lag_base, b);
    atomic_store(&lag_t0, now);
    atomic_store(&lag_ppm, ppm);
    atomic_fetch_add(&lag_seq, 1);
    atomic_store(&engaged, true);
}

// The game's clock for the host time real_ns (rt_ticks)
uint64_t d3_clock_ns(uint64_t real_ns) {
    if (!atomic_load_explicit(&engaged, memory_order_relaxed)) return real_ns;
    uint64_t lag = lag_at(real_ns), v = real_ns > lag ? real_ns - lag : 0;
    uint64_t prev = atomic_load(&last_ns);
    while (v > prev && !atomic_compare_exchange_weak(&last_ns, &prev, v)) {}
    return v > prev ? v : prev;
}

// (hunt, off by default: D3_WORLD_SYNC=1) The loop at main+0x4c6360 (lock, main+0x7b88d0 / 0x7b8770, unlock, sleep
// 16 ms minus that) waits for a fixed point in the frame slots instead (d3_gen_patch.py #12). It turned out not to be
// the thread that steps the world; no effect.
// The frame slots, from the frame pacer (nvn/objects.c) - used by the fix below as well:
static bool clock_log(void);
_Atomic uint64_t g_d3_pace_next, g_d3_pace_period, g_d3_pace_seen;  // the next slot's start, the slot's length, when told
static _Atomic uint64_t steps;  // the world's steps so far (d3_tick_mark, below)
int d3_world_wait(uint32_t ms) {
    static int on = -1;
    static int64_t phase;
    if (on < 0) {
        const char *e = getenv("D3_WORLD_SYNC"), *p = getenv("D3_WORLD_PHASE_US");
        on = e ? atoi(e) != 0 : 0;
        phase = p ? atoll(p) * 1000 : 2000000;
        if (on) LOG("[d3clock] the world thread steps with the frames above 60 fps (%lld us after a slot's start)\n", (long long)(phase / 1000));
    }
    if (!on || ms > 16) return 0;
    extern uint64_t plat_now_ns(void);
    extern void plat_sleep_until_ns(uint64_t t);
    static uint64_t last;  // the latest step's start (the end of the wait before it)
    uint64_t now = plat_now_ns(), period = atomic_load(&g_d3_pace_period), next = atomic_load(&g_d3_pace_next);
    if (!period || period > 10000000ull || now - atomic_load(&g_d3_pace_seen) > 100000000ull) { last = 0; return 0; }
    uint64_t want = last && now - last < 100000000ull ? last + 16666667ull : now;
    if (want < now) want = now;
    int64_t g0 = (int64_t)next + phase, d = (int64_t)want - g0;
    int64_t j = d >= 0 ? (d + (int64_t)period / 2) / (int64_t)period : -((-d + (int64_t)period / 2) / (int64_t)period);
    uint64_t t = (uint64_t)(g0 + j * (int64_t)period);
    while (t < now) t += period;
    plat_sleep_until_ns(t);
    last = plat_now_ns();
    return 1;
}

// (hunt, off by default: D3_SIM_SYNC=1) The world thread (main+0x7c5c70: "16 ms since the last step? then step
// main+0x7c7240", on its own beat a little off 60 Hz) waits for the next frame in which the game counts a step and
// starts D3_SIM_PHASE_US into it (d3_gen_patch.py #16). Tried on the guess that a world step in the wrong frame made
// the zigzag; with the game time in place (d3_frame_time) it made no difference, with it out of place it did not help.
static _Atomic uint64_t step_seq, step_slot_ns;  // frames that counted a step so far; the latest one's slot start
int d3_sim_wait(uint32_t ms) {
    static int on = -1;
    static int64_t phase;
    if (on < 0) {
        on = getenv("D3_SIM_SYNC") && atoi(getenv("D3_SIM_SYNC")) != 0;  // (off by default: no difference measured)
        phase = (getenv("D3_SIM_PHASE_US") ? atoll(getenv("D3_SIM_PHASE_US")) : 3000) * 1000;
    }
    if (!on || ms > 16) return 0;
    extern uint64_t plat_now_ns(void);
    extern void plat_sleep_until_ns(uint64_t t);
    uint64_t now = plat_now_ns(), period = atomic_load(&g_d3_pace_period);
    if (!period || period > 10000000ull || now - atomic_load(&g_d3_pace_seen) > 100000000ull) return 0;
    static uint64_t used;
    uint64_t deadline = now + 17000000ull;
    for (;;) {
        uint64_t s = atomic_load(&step_seq);
        if (s != used) {
            used = s;
            uint64_t t = atomic_load(&step_slot_ns) + (uint64_t)phase;
            if (t > plat_now_ns()) plat_sleep_until_ns(t);
            return 1;
        }
        uint64_t n = plat_now_ns();
        if (n >= deadline) return 1;
        plat_sleep_until_ns(n + 200000 < deadline ? n + 200000 : deadline);
    }
}

// ---- THE FIX (see the top of the file): the time the game hands its frame function, at 120 fps.
// game_t: the game's own smoothed frame time; now: the game's clock (main+0xa49c30, 1/3000 s); game: *(main+0x114a840).
// Where the game time sits on the half-step grid (0..24 units past a grid point). Steered by what the picture shows
// (hle/d3_monitor.c): the best place moves with the world thread's timing. D3_GAME_PHASE=<n>: where it starts (tests).
static _Atomic int phase_target;
static _Atomic int phase_moved;  // (set by d3_phase_shift: the nudging starts afresh)
static _Atomic int phase_poke;   // (d3_phase_poke: frames the game's step is still to be moved later (+) / earlier (-))
void d3_phase_poke(int frames) { atomic_fetch_add(&phase_poke, frames); }
static _Atomic int phase_flip;   // (unused now)
void d3_phase_shift(int units) {
    int t = atomic_load(&phase_target);
    atomic_store(&phase_target, ((t + units) % 25 + 25) % 25);
    atomic_store(&phase_moved, 1);
}
void d3_phase_flip(void) { atomic_fetch_xor(&phase_flip, 1); }
static _Atomic int phase_slip;   // (d3_phase_slip: the frame time takes one extra half step, once)
void d3_phase_slip(void) { atomic_store(&phase_slip, 1); }
uint32_t d3_frame_time(uint32_t game_t, uint32_t now, uint64_t game) {
    static int on = -1;
    if (on < 0) {
        on = !(getenv("D3_FRAME_CLOCK") && atoi(getenv("D3_FRAME_CLOCK")) == 0);
        if (getenv("D3_GAME_PHASE")) atomic_store(&phase_target, ((atoi(getenv("D3_GAME_PHASE")) % 25) + 25) % 25);
    }
    static bool have;
    static uint32_t t;
    static int tries, rest;
    int target = atomic_load(&phase_target), mon_u = -1, mon_k = 1, mon_restart = 0, mon_nudge = 0;
    if (atomic_exchange(&phase_moved, 0)) tries = rest = 0;
    extern uint64_t plat_now_ns(void);
    uint64_t period = atomic_load(&g_d3_pace_period);
    // (only at 120 fps: a frame is exactly 25 of the game's units there, half a step)
    if (!on || !game || period < 8200000ull || period > 8500000ull || plat_now_ns() - atomic_load(&g_d3_pace_seen) > 100000000ull) {
        have = false;
        return game_t;
    }
    int32_t behind = have ? (int32_t)(now - t) : 0;
    if (!have || behind > 150 || behind < -150) {
        if (!have) LOG("[d3clock] 120 fps: the frame's time moves on by half a game step every frame (the game's own smoothing can settle on every second frame)\n");
        have = true; t = now; tries = rest = 0; mon_restart = 1;
    } else {
        // on by one frame (25 units); by two or more when that many frames' time has passed (a frame that came late)
        int k = (behind + 13) / 25;
        if (k < 1) k = 1;
        if (atomic_exchange(&phase_slip, 0)) k++;
        t += 25u * (uint32_t)k;
        mon_k = k;
    }
    {   // The game time's place against the frames. The game's time for this frame will be [game+0x10]+0x58 (a base, in
        // units) plus what its game timer ([game+0x10]+0x34) has gathered since its mark; 50 units are one step.
        // Measured (2026-11-17, hidden 120 fps walks, frame time even): the hero's zigzag against the camera is
        // 0.3 px at 4K for every unit the frames' game times sit past a half-step boundary - 0.1 px at 0, 0.6 px at 2,
        // 6.3 px at 21 - and a pause leaves them anywhere. So the frame time is nudged by a unit or two per frame until
        // every frame's game time is a whole number of half steps (D3_GAME_PHASE=<0..24>: that many units past, tests).
        uint64_t g10 = *(const uint64_t *)(uintptr_t)(game + 0x10);
        if (g10 && !getenv("D3_NO_GAME_PHASE")) {
            const uint32_t *b = (const uint32_t *)(uintptr_t)(g10 + 0x34);
            int32_t d = (int32_t)(t - b[0]);
            // (only while the game time runs: paused, the game sets its marks anew every frame and nothing holds)
            static uint32_t last_base;
            uint32_t base_now = *(const uint32_t *)(uintptr_t)(g10 + 0x58);
            bool running = base_now != last_base;
            last_base = base_now;
            if (rest > 0) rest--;
            else if (running && b[4] == 60 && d > 0 && d < 100) {
                uint32_t gu = *(const uint32_t *)(uintptr_t)(g10 + 0x58) + (b[1] + (uint32_t)d - b[2]);
                int e = (target - (int)(gu % 25) + 25) % 25;  // units to add to get there (0..24)
                if (e > 12) e -= 25;                          // ... or to leave out
                if (e) {
                    int a = e > 2 ? 2 : e < -2 ? -2 : e;
                    if (a < 0 && d + a < 1) a = 1 - d;
                    t += (uint32_t)a;
                    mon_nudge = a;
                    mon_u = (int)((gu + (uint32_t)a) % 25);
                    if (++tries > 40) { tries = 0; rest = 60; }  // (it doesn't follow: left alone a while)
                } else {
                    tries = 0; mon_u = (int)(gu % 25);
                    // In place on the half-step grid. One more thing can be off: WHICH frame of each pair counts the
                    // game's 1/60 s step. Moved a frame later or earlier (the step mark at +0xc by half a step; +8, the
                    // time accounted for, is never before it and less than a step after) the picture goes from clean
                    // to the full 7.5 px zigzag and back: two places next to each other are clean, the ones beside
                    // them are not, and a pause can leave it in a bad one (measured with D3_STEP_POKE, 2026-11-17).
                    // What decides it inside the game is not found; the watchman (hle/d3_monitor.c) sees the zigzag and
                    // asks for the step to move (d3_phase_poke). No frame is dropped or repeated by it.
                    uint32_t *wb = (uint32_t *)(uintptr_t)(g10 + 0x34);
                    uint32_t left = wb[2] - wb[3];  // (0..49: what was left after the last step)
                    extern uint64_t renderer_present_count(void);
                    {   // (tests) D3_STEP_POKE=<frame>:<n>,...: n frames later (+) or earlier (-) from that frame on
                        static uint64_t poked_at;
                        uint64_t fr = renderer_present_count();
                        for (const char *q = getenv("D3_STEP_POKE"); q && *q;) {
                            uint64_t pf = strtoull(q, (char **)&q, 10);
                            int n = *q == ':' ? (int)strtol(q + 1, (char **)&q, 10) : 0;
                            if (pf <= fr && pf > poked_at && fr - pf < 4) { atomic_fetch_add(&phase_poke, n); poked_at = pf; }
                            while (*q == ',') q++;
                        }
                    }
                    int pk = atomic_load(&phase_poke);
                    if (pk > 0 && left >= 25 && left < 50) { wb[3] += 25; atomic_fetch_sub(&phase_poke, 1); }
                    else if (pk < 0 && left < 25) { wb[3] -= 25; atomic_fetch_add(&phase_poke, 1); }
                    if (pk && clock_log()) LOG("[d3clock] frame %llu: the game's step counted a frame %s (%d to go)\n",
                                               (unsigned long long)renderer_present_count(), pk > 0 ? "later" : "earlier", atomic_load(&phase_poke));
                }
            }
        }
    }
    if (clock_log()) {  // (hunt) the game's sync state, when it changes: the flags at [[game+0x10]+0x800], main+0x1767b34, +0x44
        uint64_t g10 = *(const uint64_t *)(uintptr_t)(game + 0x10);
        uint64_t fp = g10 ? *(const uint64_t *)(uintptr_t)(g10 + 0x800) : 0;
        if (fp) {
            static uint32_t lf = ~0u, l44 = ~0u; static uint8_t lb = 0xff;
            static int lines;
            uint32_t f = *(const uint32_t *)(uintptr_t)fp & ~0x80u, v44 = *(const uint32_t *)(uintptr_t)(g10 + 0x44);
            uint8_t b34 = *(const uint8_t *)(uintptr_t)0x7001767b34ull;
            if ((f != lf || v44 != l44 || b34 != lb) && lines++ < 400) {
                extern uint64_t renderer_present_count(void);
                LOG("[d3s] frame %llu: flags %08x, b34 %u, +0x44 %u, +0x54 %u, +0x40 %u, +0x24 %u\n", (unsigned long long)renderer_present_count(),
                    f, b34, v44, *(const uint32_t *)(uintptr_t)(g10 + 0x54), *(const uint32_t *)(uintptr_t)(fp + 0x40), *(const uint32_t *)(uintptr_t)(fp + 0x24));
                lf = f; l44 = v44; lb = b34;
            }
        }
    }
    {   // (the picture's recorder and the steering: hle/d3_monitor.c)
        extern void d3_mon_frame(uint64_t frame, int u, int k, int restart, int nudge);
        extern uint64_t renderer_present_count(void);
        d3_mon_frame(renderer_present_count(), mon_u, mon_k, mon_restart, mon_nudge);
    }
    *(uint64_t *)(uintptr_t)(game + 0x2398) = t;  // (the game's own T and D, for when 60 fps comes back)
    *(uint64_t *)(uintptr_t)(game + 0x23a0) = 25;
    {   // how many 1/60 s steps the game will count for this frame (its game timer at [game+0x10]+0x34: +0 last, +4 sum,
        // +0xc the steps' mark, +0x10 rate): a frame that counts one is where the world thread steps (d3_sim_wait)
        uint64_t g10 = *(const uint64_t *)(uintptr_t)(game + 0x10);
        if (g10) {
            const uint32_t *b = (const uint32_t *)(uintptr_t)(g10 + 0x34);
            int32_t d = (int32_t)(t - b[0]);
            if (d < 0) d = 0;
            if (d > 10000) d = 10000;
            uint32_t sum = b[1] + (uint32_t)d, n = b[4] ? (sum - b[3]) * b[4] / 3000 : 0;
            if (n) {
                atomic_store(&step_slot_ns, atomic_load(&g_d3_pace_next) - period);
                atomic_fetch_add(&step_seq, 1);
            }
            static int every = -1;  // (ghost hunt) D3_CLOCK_LOG=2: this frame's time, the clock, the steps, the world's steps so far
            if (every < 0) every = getenv("D3_CLOCK_LOG") && atoi(getenv("D3_CLOCK_LOG")) >= 2;
            if (every) {
                extern uint64_t renderer_present_count(void);
                const uint32_t *a = (const uint32_t *)(uintptr_t)g10;  // (the other timer there, the real-time one: same fields)
                int32_t da = (int32_t)(t - a[0]);
                if (da < 0) da = 0;
                if (da > 10000) da = 10000;
                uint32_t suma = a[1] + (uint32_t)da, na = a[4] ? (suma - a[3]) * a[4] / 3000 : 0;
                // (and the two clocks the frame before left behind, in seconds: game time at +0x60, real time at +0x2c)
                static float pg, pr;
                float gt = *(const float *)(uintptr_t)(g10 + 0x60), rt = *(const float *)(uintptr_t)(g10 + 0x2c);
                // (... and the world's own step counter, [*(main+0x11509e8)]+0x238, against the game time in steps)
                uint64_t sv = *(const uint64_t *)(uintptr_t)0x70011509e8ull;
                sv = sv ? *(const uint64_t *)(uintptr_t)sv : 0;
                uint32_t svt = sv ? *(const uint32_t *)(uintptr_t)(sv + 0x238) : 0;
                uint32_t gu = *(const uint32_t *)(uintptr_t)(g10 + 0x58) + (sum - b[2]);
                LOG("[d3f] %llu %u %u %u %u %d %llu %u %u %u %.2f %.2f %u %.3f %u\n", (unsigned long long)renderer_present_count(), t, now, n, (sum - b[3]), behind,
                    (unsigned long long)atomic_load(&steps), na, (suma - a[3]), a[4], (gt - pg) * 3000.0f, (rt - pr) * 3000.0f, svt, (double)gt * 60.0, gu);
                pg = gt; pr = rt;
            }
        }
    }
    return t;
}

// (ghost hunt) D3_CLOCK_LOG=4: who takes and gives back the world's lock (*(main+0x1154df8)), and when
void d3_lock_seen(uint64_t m, int unlock, uint64_t lr) {
    static int log4 = -1;
    if (log4 < 0) log4 = getenv("D3_CLOCK_LOG") && atoi(getenv("D3_CLOCK_LOG")) >= 4;
    if (!log4) return;
    // (unlock: 0 lock, 1 unlock - the world's lock; 'S' signal, 'W' wait, 'T' timed wait - the frame event at game+0xa40)
    if (unlock <= 1) { if (m != *(const uint64_t *)(uintptr_t)0x7001154df8ull) return; }
    else {
        uint64_t g = *(const uint64_t *)(uintptr_t)0x700114a840ull;
        g = g ? *(const uint64_t *)(uintptr_t)(g + 0x10) : 0;
        if (!g || m != g + 0xa40) return;
    }
    extern uint64_t plat_now_ns(void), kernel_current_thread_id(void);
    LOG("[d3l] %llu %llu %c %llx\n", (unsigned long long)(plat_now_ns() / 1000), (unsigned long long)kernel_current_thread_id(),
        unlock > 1 ? (char)unlock : unlock ? 'U' : 'L', (unsigned long long)lr);
}

static _Atomic uint64_t steps;
void d3_tick_mark(void) {
    atomic_fetch_add_explicit(&steps, 1, memory_order_relaxed);
    static int log3 = -1;  // (ghost hunt) D3_CLOCK_LOG=3: each step's time and thread
    if (log3 < 0) log3 = getenv("D3_CLOCK_LOG") && atoi(getenv("D3_CLOCK_LOG")) >= 3;
    if (log3) {
        extern uint64_t plat_now_ns(void), kernel_current_thread_id(void);
        LOG("[d3t] %llu %llu\n", (unsigned long long)(plat_now_ns() / 1000), (unsigned long long)kernel_current_thread_id());
    }
}

static bool clock_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("D3_CLOCK_LOG") != NULL;
    return on;
}

// Once per presented frame, after the pacer's wait. period_ns: the frame slot; lost_ns: how late this frame was for
// its slot (0: on time).
void d3_clock_frame(uint64_t now, uint64_t period_ns, uint64_t lost_ns) {
    static int off = -1;
    if (off < 0) off = getenv("D3_CLOCK_SYNC") == NULL;  // (2026-11-17: not reliable yet - on only with D3_CLOCK_SYNC=1)
    if (clock_log()) {  // (ghost hunt) the game's own frame limit settings (its preferences at main+0x181b7c4: +0x74 on, +0x78 fps)
        static uint32_t last_on = ~0u, last_fps = ~0u;
        uint32_t on = *(const uint32_t *)(uintptr_t)0x700181b838ull, fps = *(const uint32_t *)(uintptr_t)0x700181b83cull;
        if (on != last_on || fps != last_fps) {
            extern uint64_t renderer_present_count(void);
            LOG("[d3clock] frame %llu: the game's frame limit %s, %u fps\n", (unsigned long long)renderer_present_count(), on ? "on" : "off", fps);
            last_on = on; last_fps = fps;
        }
    }
    if (clock_log()) {  // (ghost hunt) the game's update rate, *(main+0x114a840) + 0xc: 0 = every frame?, 60 = in 1/60 s steps?
        static uint32_t last_rate = ~0u, last_54 = ~0u;
        uint64_t g = *(const uint64_t *)(uintptr_t)0x700114a840ull;
        uint32_t rate = g ? *(const uint32_t *)(uintptr_t)(g + 0xc) : ~0u - 1;
        uint64_t g10 = g ? *(const uint64_t *)(uintptr_t)(g + 0x10) : 0;
        uint32_t f54 = g10 ? *(const uint32_t *)(uintptr_t)(g10 + 0x54) : ~0u - 1;
        (void)f54; (void)last_54;
        if (rate != last_rate) {
            extern uint64_t renderer_present_count(void);
            LOG("[d3clock] frame %llu: the game's update rate %u (was %u), +0x54 %u\n", (unsigned long long)renderer_present_count(), rate, last_rate, f54);
            last_rate = rate; last_54 = f54;
        }
    }
    {   // (ghost hunt) D3_CLOCK_LOG=2: every frame - host time, the clock's lag, the game's steps so far, how late
        static int every = -1;
        if (every < 0) every = getenv("D3_CLOCK_LOG") && atoi(getenv("D3_CLOCK_LOG")) >= 2;
        if (every) {
            extern uint64_t renderer_present_count(void);
            extern volatile uint64_t g_vsync_count, g_vsync_ns;  // (hle/vi.c: the display's 60 Hz tick)
            uint64_t vn = __atomic_load_n(&g_vsync_ns, __ATOMIC_ACQUIRE);
            extern uint64_t kernel_current_thread_id(void);
            LOG("[d3c] %llu %llu %llu %llu %llu %llu %lld\n", (unsigned long long)renderer_present_count(), (unsigned long long)(now / 1000),
                (unsigned long long)(atomic_load(&engaged) ? lag_at(now) / 1000 : 0), (unsigned long long)atomic_load(&steps), (unsigned long long)(lost_ns / 1000),
                (unsigned long long)__atomic_load_n(&g_vsync_count, __ATOMIC_ACQUIRE), (long long)kernel_current_thread_id());
            (void)vn;
        }
    }
    static bool fast;
    static uint64_t seen, quiet, owed, boost_from, boosts;
    static uint32_t base_ppm = BASE_PPM, boost_ppm = BOOST_PPM;  // (tests: D3_CLOCK_BASE_PPM, D3_CLOCK_BOOST_PPM; 0: none)
    static bool read_env;
    if (!read_env) {
        read_env = true;
        if (getenv("D3_CLOCK_BASE_PPM")) base_ppm = (uint32_t)atoi(getenv("D3_CLOCK_BASE_PPM"));
        if (getenv("D3_CLOCK_BOOST_PPM")) boost_ppm = (uint32_t)atoi(getenv("D3_CLOCK_BOOST_PPM"));
    }

    bool is_fast = !off && period_ns && period_ns <= 10000000ull;
    if (!is_fast) {  // 60 fps (or off): the clock exact
        if (fast) { lag_rate(now, 0); fast = false; }
        return;
    }
    if (!fast) {
        fast = true; quiet = owed = boost_from = 0; seen = atomic_load(&steps);
        LOG("[d3clock] above 60 fps: the game's clock %g%% slow, %g%% after late frames\n", base_ppm / 10000.0, boost_ppm / 10000.0);
    }
    uint64_t s = atomic_load(&steps);
    quiet = s != seen ? 0 : quiet + 1;
    seen = s;
    bool running = quiet < 8;  // the world is stepping (not a menu, a loading screen, a pause)

    if (boost_from) {  // what the clock has given back since the boost began (or was last counted)
        uint64_t got = (now - boost_from) / 1000000ull * boost_ppm;
        owed = owed > got ? owed - got : 0;
        boost_from = now;
    }
    if (lost_ns >= LATE_NS && boost_ppm) {
        owed += lost_ns;
        if (owed > BOOST_MAX_NS) owed = BOOST_MAX_NS;
        if (clock_log() || boosts < 10) LOG("[d3clock] frame late by %llu ms: %llu ms to give back\n", (unsigned long long)(lost_ns / 1000000), (unsigned long long)(owed / 1000000));
        boosts++;
    }
    if (owed && running) { if (!boost_from) boost_from = now; lag_rate(now, boost_ppm); }
    else { boost_from = 0; lag_rate(now, base_ppm); }
}

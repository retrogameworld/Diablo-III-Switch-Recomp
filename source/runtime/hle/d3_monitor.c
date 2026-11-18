// Diablo III at 120 fps, part two of the "doubled hero" fix (part one: hle/d3_clock.c d3_frame_time).
//
// With the frame's time on the game's half-step grid the hero's zigzag against the camera was gone in most hidden
// runs (0.1-0.2 px at 4K) - and at its worst, 7.5 px, in some: once after the pause menu, and for the tester 20 minutes into
// a session, seconds after a 16 ms hitch. The zigzag is 0.3 px for every unit (1/3000 s) the frame's game time is past
// an edge, up to 25 units, then it starts over - and where that edge lies depends on when the game's world thread
// finishes its steps against the frames, which a pause or a hitch moves. Sitting right behind the edge is the best
// picture and one hitch away from the worst.
//
// (Later the same day: the worst case is WHICH frame of each pair counts the game's step; what decides the right one
// inside the game is not found, so it is the watchman below that sees the zigzag and has the step moved.)
// So the picture is measured and the frame clock steered by it (the tester's idea: "reset it subtly, without a frame drop"):
//   - d3_mon_pos (from the hero's draw, nvn/vulkan/exec.c): his place on the screen and in the world every frame.
//     The zigzag is the part of his movement on the screen that changes sign every frame (smooth turns and the camera
//     easing don't); against his speed in the world it tells how many units past the edge the game time is.
//   - d3_mon_frame (from d3_frame_time, every frame): every CTRL_FRAMES frames, when there is movement to measure,
//     and two measurements in a row say the game time is ACT units or more from its place, it is moved there
//     (d3_phase_shift: a unit or two per frame, no frame dropped or repeated); at the zigzag's fullest the game's step
//     is counted a frame later or earlier (d3_phase_poke) until the picture is clean.
// One log line every two seconds ("[d3m] ..."), with what reaches the display as well (d3_mon_present: when each frame
// is handed to Windows, against the display's refresh - in case what the tester sees is there and not in the game's picture).
// D3_NO_MONITOR=1: all off. D3_NO_PHASE_CTRL=1: measured and logged, not steered.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../hle.h"

int d3_vblank_info(uint64_t *vblank_ns, uint64_t *period_ns);  // d3_vblank.c
void d3_phase_shift(int units);                                // d3_clock.c
void d3_phase_poke(int frames);

#define CTRL_FRAMES 30  // a measurement (a quarter of a second)
#define ACT 3.0         // units past its place from which the game time is moved
#define MIN_SPEED 3.0   // px per frame at 4K below which nothing is decided (a run is 7.5)

static bool mon_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("D3_NO_MONITOR") == NULL;
    return on;
}

// ---- what is drawn (the render thread): the hero's origin on the screen (px at 4K) and in the world. Sums in
// hundredths of a px; two sets: [0] for the controller, [1] for the log line.
#define WORLD_PX 41.7f  // px at 4K per world unit along the ground (a run: 0.18 units = 7.5 px a frame)
typedef struct { _Atomic int64_t ax, ay; _Atomic uint64_t v, n; } Acc;
static Acc acc[2];
void d3_mon_pos(uint64_t frame, float x, float y, float wx, float wy) {
    if (!mon_on()) return;
    static uint64_t last;
    static float x1, y1, x2, y2, wx1, wy1;
    static int run;
    if (frame == last + 1) {
        if (run >= 2) {
            float dx = x - 2 * x1 + x2, dy = y - 2 * y1 + y2;  // (a zigzag of +-A: 4A, the sign changing every frame)
            float vx = wx - wx1, vy = wy - wy1;
            if (dx < 60 && dx > -60 && dy < 60 && dy > -60 && vx < 2 && vx > -2 && vy < 2 && vy > -2) {  // (more: a cut, another object)
                int sg = frame & 1 ? -1 : 1;
                float v = __builtin_sqrtf(vx * vx + vy * vy) * WORLD_PX;
                for (int k = 0; k < 2; k++) {
                    atomic_fetch_add(&acc[k].ax, (int64_t)(sg * dx * 100));
                    atomic_fetch_add(&acc[k].ay, (int64_t)(sg * dy * 100));
                    atomic_fetch_add(&acc[k].v, (uint64_t)(v * 100));
                    atomic_fetch_add(&acc[k].n, 1);
                }
            } else run = 0;
        }
        run++;
    } else run = 1;
    x2 = x1; y2 = y1; x1 = x; y1 = y; wx1 = wx; wy1 = wy; last = frame;
}
// One set's result, and the set emptied: the zigzag (as the hunt measured it: half the mean second difference, px),
// the speed (px per frame), the frames it is from
static void acc_take(Acc *a, double *z, double *v, uint64_t *n) {
    int64_t ax = atomic_exchange(&a->ax, 0), ay = atomic_exchange(&a->ay, 0);
    uint64_t vs = atomic_exchange(&a->v, 0);
    *n = atomic_exchange(&a->n, 0);
    *z = *n ? ((ax < 0 ? -ax : ax) + (ay < 0 ? -ay : ay)) / 200.0 / (double)*n : 0;
    *v = *n ? vs / 100.0 / (double)*n : 0;
}

// ---- what reaches the display (the thread that presents)
static _Atomic uint64_t ph_bin[8], ph_period;
void d3_mon_present(uint64_t t_ns) {
    if (!mon_on()) return;
    static uint64_t vb, per;
    static int age;
    if (!per || ++age >= 30) {
        age = 0;
        if (!d3_vblank_info(&vb, &per)) { per = 0; return; }
        atomic_store(&ph_period, per);
    }
    if (!per) return;
    uint64_t ph = t_ns >= vb ? (t_ns - vb) % per : per - 1 - (vb - t_ns - 1) % per;
    atomic_fetch_add(&ph_bin[ph * 8 / per], 1);
}

// ---- the frame pacer started its slots anew (a frame came lost_ns after its slot)
static _Atomic uint64_t regrid_n, regrid_max;
void d3_mon_regrid(uint64_t lost_ns) {
    atomic_fetch_add(&regrid_n, 1);
    uint64_t m = atomic_load(&regrid_max);
    while (lost_ns > m && !atomic_compare_exchange_weak(&regrid_max, &m, lost_ns)) {}
}

// ---- every frame while the 120 fps frame clock runs (the game thread). u: the game time's place on the half-step
// grid (0..24, -1 not known); k: half steps taken; restart; nudge: units the frame time was moved by
void d3_mon_frame(uint64_t frame, int u, int k, int restart, int nudge) {
    if (!mon_on()) return;
    static int ctrl = -1;
    if (ctrl < 0) ctrl = getenv("D3_NO_PHASE_CTRL") == NULL;
    static uint32_t n, uh[26], two, restarts, nudges, moves, cn, settle;
    static int moved_sum;

    // the controller
    if (restart) { cn = 0; settle = 1; }
    if (++cn >= CTRL_FRAMES) {
        cn = 0;
        double z, v;
        uint64_t zn;
        acc_take(&acc[0], &z, &v, &zn);
        if (settle) settle--;  // (the window a move fell into: not judged)
        else {
            static double prev = -1;  // (a middling measurement waits for a second one like it)
            static int tried, hold;
            // The zigzag at its fullest: the game's step is counted in a wrong frame (two frames next to each other are
            // right; a pause or a hitch can leave it one off on either side). From where it is: one frame earlier (what
            // cured it in the hidden runs), else one later, else two earlier, else two later; if nothing helps it is put
            // back and left alone for ten seconds.
            static const int poke[5] = {-1, +2, -3, +4, -2};
            if (hold) hold--;
            if (zn >= CTRL_FRAMES * 3 / 4 && v >= MIN_SPEED) {
                double d = 25.0 * z / v;  // units from its place
                if (d > 25) d = 25;
                bool full = d >= 18;
                if (d <= ACT) { prev = -1; tried = 0; }
                else if (!full && (prev < 0 || d - prev > 4 || prev - d > 4)) prev = d;
                else if (ctrl && !hold) {
                    int by = full ? 0 : -(int)(d + 0.5);
                    if (!full) d3_phase_shift(by);
                    else {
                        d3_phase_poke(poke[tried++]);
                        if (tried == 5) { tried = 0; hold = 40; }
                    }
                    settle = 1; moves++; moved_sum += by; prev = -1;
                    static int logged;
                    if (logged++ < 300)
                        LOG("[d3m] frame %llu: zigzag %.1f px at %.1f px a frame (%.0f units from its place): %s\n",
                            (unsigned long long)frame, z, v, d,
                            !full ? "the game time moved" : hold ? "nothing helped: put back, left alone for 10 s" : "the game's step moved to another frame");
                }
            } else prev = -1;
        }
    }

    // the log line
    n++;
    uh[u >= 0 && u < 25 ? u : 25]++;
    if (k > 1) two++;
    if (restart) restarts++;
    if (nudge) nudges++;
    if (n < 240) return;
    int best = 0;
    for (int i = 1; i < 26; i++) if (uh[i] > uh[best]) best = i;
    double z, v;
    uint64_t zn, bins[8], per = atomic_load(&ph_period), bt = 0;
    acc_take(&acc[1], &z, &v, &zn);
    for (int i = 0; i < 8; i++) { bins[i] = atomic_exchange(&ph_bin[i], 0); bt += bins[i]; }
    if (!bt) bt = 1;
    LOG("[d3m] frame %llu: hero zigzag %.2f px (speed %.1f px a frame, %llu frames) | clock u=%d (%u%%), "
        "2-step frames %u, restarts %u, nudged %u, moved %u times (%+d) | pacer regrids %llu (worst %.1f ms) | handed to the "
        "display, %% per eighth of the refresh (%.3f ms): %llu %llu %llu %llu %llu %llu %llu %llu\n",
        (unsigned long long)frame, z, v, (unsigned long long)zn,
        best == 25 ? -1 : best, uh[best] * 100 / n, two, restarts, nudges, moves, moved_sum,
        (unsigned long long)atomic_exchange(&regrid_n, 0), atomic_exchange(&regrid_max, 0) / 1e6, per / 1e6,
        (unsigned long long)(bins[0] * 100 / bt), (unsigned long long)(bins[1] * 100 / bt), (unsigned long long)(bins[2] * 100 / bt),
        (unsigned long long)(bins[3] * 100 / bt), (unsigned long long)(bins[4] * 100 / bt), (unsigned long long)(bins[5] * 100 / bt),
        (unsigned long long)(bins[6] * 100 / bt), (unsigned long long)(bins[7] * 100 / bt));
    n = two = restarts = nudges = moves = 0;
    moved_sum = 0;
    for (int i = 0; i < 26; i++) uh[i] = 0;
}

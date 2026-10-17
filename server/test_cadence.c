/*
 * Host self-test for the S5 cadence core. Builds and runs on the dev box with
 * mingw64 gcc; no qagame, no Linux, no server.
 *
 * This is the gate. 2.1 refuses to install if footsteps_selftest() fails, and
 * ql_sndtap's build refuses to emit binaries if test_stub.exe fails, both
 * because the failure mode of a wrong cadence or a wrong emitter is silent or
 * fatal rather than an error return. Same rule here: if this does not pass, the
 * .so does not get built.
 *
 * What it proves, per framerate:
 *   1. bobCycle is bit-identical to an independent pure-vanilla simulation fed
 *      the same commands. This is S5's core claim -- if it ever fails, the
 *      client's prediction no longer agrees and the doubles come back.
 *   2. Total footstep rate is never below the 250 fps reference of 1.953/s.
 *   3. Players who do not need help get zero injections.
 *   4. No injection lands within FS_MIN_STEP_GAP_MS of the previous footstep.
 *   5. On stairs, air time is not credited and a landing footstep counts as
 *      the previous footstep: still no injections for a stock client.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs_cadence.h"

#define BOBMOVE_RUN 0.4f
#define REFERENCE_RATE (250.0 / (double)FS_UNITS_PER_STEP)   /* 1.953125 */
#define CLOCK_RATE ((double)FS_CLOCK_UNITS_PER_SEC / (double)FS_UNITS_PER_STEP)

/* Integer usercmd msec values whose running mean is 1000/fps, which is what a
 * real client emits -- 400 fps alternates 3 and 2 rather than sending 2.5. */
static int next_msec(double period, double *carry) {
    double want = period + *carry;
    int m = (int)(want + 0.5);
    if (m < 1) {
        m = 1;
    }
    *carry = want - (double)m;
    return m;
}

static int trunc_add_ref(float add) {
    if (add <= -1024.f || add >= 1024.f) {
        return 0;
    }
    return (int)add;
}

struct sim {
    int vanilla;        /* footsteps qagame's own PM_AddEvent would ring */
    int injected;
    int bob_mismatch;   /* vs the independent vanilla simulation */
    int gap_violations; /* injections too close to the previous footstep */
    int min_inject_gap;
    int inj_pair_violations; /* two injections inside FS_MIN_INJECT_GAP_MS */
    int last_inj, have_inj;
};

/* Two injections closer than this share an entityState value and listeners
 * drop the second: EVENT_VALID_MSEC plus a snapshot each side at sv_fps 40.
 * Fixed here rather than taken from FS_MIN_INJECT_GAP_MS, so a build with the
 * rule turned off (-DFS_MIN_INJECT_GAP_MS=0) fails this check. */
#define LISTENER_PAIR_MS 350

static void note_injection(struct sim *r, int now) {
    if (r->have_inj && now - r->last_inj < LISTENER_PAIR_MS) {
        r->inj_pair_violations++;
    }
    r->have_inj = 1;
    r->last_inj = now;
}

static void run(double fps, int seconds, int allow_replace, struct sim *r) {
    struct fs_slot s;
    struct fs_result res;
    double period = 1000.0 / fps;
    double carry = 0.0;
    int now = 100000;               /* arbitrary non-zero commandTime base */
    int end = now + seconds * 1000;
    int bob = 0;                    /* what the hook believes bobCycle is */
    int ref_bob = 0;                /* independent pure-vanilla bobCycle */
    int last_step = 0;
    int have_step = 0;

    memset(r, 0, sizeof(*r));
    r->min_inject_gap = 1 << 30;
    fs_slot_reset(&s);
    s.last_bob = 0;

    while (now < end) {
        int msec = next_msec(period, &carry);
        float addend = BOBMOVE_RUN * (float)msec;
        int ref_new;

        now += msec;

        /* independent vanilla, the thing the client also computes */
        ref_new = (ref_bob + trunc_add_ref(addend)) & 255;

        fs_step(&s, now, bob, addend, &res, allow_replace);

        if (res.new_bob != ref_new) {
            r->bob_mismatch++;
        }
        bob = res.new_bob;
        s.last_bob = bob;
        ref_bob = ref_new;

        if (res.vanilla_flip && !res.suppress) {
            r->vanilla++;
        }
        if (res.inject) {
            r->injected++;
            note_injection(r, now);
            if (have_step) {
                int gap = now - last_step;
                if (gap < r->min_inject_gap) {
                    r->min_inject_gap = gap;
                }
                if (gap < FS_MIN_STEP_GAP_MS) {
                    r->gap_violations++;
                }
            }
        }
        if ((res.vanilla_flip && !res.suppress) || res.inject) {
            last_step = now;
            have_step = 1;
        }
    }
}

/*
 * Mixed-interval cell: mostly 1 ms commands with a >=4 ms one every `every`th
 * command, which is what a real 1000 fps client sends when its frame time
 * occasionally slips. This exists because of a live regression: the first build
 * reset the reference accumulator on every >=4 ms command, copying 2.1's policy.
 * 2.1's accumulator only has to survive to the next 4 ms tick; this one has to
 * survive 128 ticks, so the reset threw away up to half a second of accrued
 * cadence each time. On our test server 5.3% of commands took that branch and the
 * server delivered 115 footsteps where the reference wanted 153. A uniform-msec
 * sweep cannot see it, which is why this cell is here.
 */
static void run_mixed(int every, int seconds, struct sim *r) {
    struct fs_slot s;
    struct fs_result res;
    int now = 100000;
    int end = now + seconds * 1000;
    int bob = 0, ref_bob = 0, n = 0;
    int last_step = 0, have_step = 0;

    memset(r, 0, sizeof(*r));
    r->min_inject_gap = 1 << 30;
    fs_slot_reset(&s);

    while (now < end) {
        int msec = (++n % every == 0) ? 4 : 1;
        float addend = BOBMOVE_RUN * (float)msec;
        int ref_new = (ref_bob + trunc_add_ref(addend)) & 255;

        now += msec;
        fs_step(&s, now, bob, addend, &res, 0);
        if (res.new_bob != ref_new) {
            r->bob_mismatch++;
        }
        bob = res.new_bob;
        s.last_bob = bob;
        ref_bob = ref_new;

        if (res.vanilla_flip) r->vanilla++;
        if (res.inject) {
            r->injected++;
            note_injection(r, now);
            if (have_step) {
                int gap = now - last_step;
                if (gap < r->min_inject_gap) r->min_inject_gap = gap;
                if (gap < FS_MIN_STEP_GAP_MS) r->gap_violations++;
            }
        }
        if (res.vanilla_flip || res.inject) {
            last_step = now;
            have_step = 1;
        }
    }
}

static int check_mixed(void) {
    static const int everies[] = {5, 10, 19, 40, 100};
    const int n = (int)(sizeof(everies) / sizeof(everies[0]));
    const int seconds = 120;
    int failures = 0;
    int i;

    printf("\nmixed 1ms traffic with a 4ms command every Nth "
           "(the live regression cell)\n");
    printf("%6s  %9s  %9s  %9s  %8s  %s\n",
           "every", "vanilla/s", "inject/s", "total/s", "vs ref", "verdict");
    for (i = 0; i < n; i++) {
        struct sim r;
        double van, inj, total;
        int bad = 0;
        const char *why = "";

        run_mixed(everies[i], seconds, &r);
        van = (double)r.vanilla / seconds;
        inj = (double)r.injected / seconds;
        total = van + inj;

        if (r.bob_mismatch) {
            bad = 1;
            why = "bobCycle DIVERGED from vanilla";
        } else if (total < REFERENCE_RATE - 0.10) {
            bad = 1;
            why = "starved: accumulator is being reset";
        } else if (r.gap_violations) {
            bad = 1;
            why = "injection inside the minimum gap";
        } else if (r.inj_pair_violations) {
            bad = 1;
            why = "two injections inside FS_MIN_INJECT_GAP_MS";
        }
        if (bad) failures++;
        printf("%6d  %9.3f  %9.3f  %9.3f  %7.0f%%  %s%s\n",
               everies[i], van, inj, total,
               100.0 * total / REFERENCE_RATE, bad ? "FAIL: " : "pass", why);
    }
    return failures;
}

/* Above ~390 fps, s2 replaces the listener's vanilla stream with the
 * listener clock. What a listener hears is injections only, at CLOCK_RATE, a
 * real 250 fps walk's tempo. At 333 fps and below the mode stays off. */
static int check_replace(void) {
    static const double fps_list[] = {250, 333, 400, 450, 500, 1000};
    const int n = (int)(sizeof(fps_list) / sizeof(fps_list[0]));
    const int seconds = 120;
    int failures = 0;
    int i;

    printf("\nlistener-replace mode (s2, above ~390 fps), clock %.3f steps/s\n", CLOCK_RATE);
    printf("%6s  %9s  %9s  %9s  %s\n", "fps", "heard_van", "inject/s", "total/s", "verdict");
    for (i = 0; i < n; i++) {
        struct sim r;
        double van, inj, total;
        int bad = 0;
        const char *why = "";

        run(fps_list[i], seconds, 1, &r);
        van = (double)r.vanilla / seconds;
        inj = (double)r.injected / seconds;
        total = van + inj;
        if (r.bob_mismatch) {
            bad = 1;
            why = "bobCycle DIVERGED from vanilla";
        } else if (r.gap_violations) {
            bad = 1;
            why = "injection inside the minimum gap";
        } else if (r.inj_pair_violations) {
            bad = 1;
            why = "two injections inside FS_MIN_INJECT_GAP_MS";
        } else if (total < REFERENCE_RATE - 0.05) {
            bad = 1;
            why = "listener stream below the reference";
        } else if (fps_list[i] >= 400 && (inj < 1.80 || van > 0.15)) {
            bad = 1;
            why = "vanilla still in the listener stream, or the clock is short";
        } else if (fps_list[i] >= 400 && (inj < CLOCK_RATE - 0.05 || inj > CLOCK_RATE + 0.05)) {
            bad = 1;
            why = "the listener clock is off its tempo";
        } else if (fps_list[i] <= 333 && r.injected != 0) {
            bad = 1;
            why = "replaced a client who was already at or above the reference";
        }
        if (bad) failures++;
        printf("%6.0f  %9.3f  %9.3f  %9.3f  %s%s\n",
               fps_list[i], van, inj, total, bad ? "FAIL: " : "pass", why);
    }
    return failures;
}

/*
 * Stairs: running in stretches on the ground with short drops in between, the
 * shape of a staircase taken downwards. Airborne commands never reach the bob
 * site, and each landing rings a PM_CrashLand footstep ahead of PM_Footsteps in
 * the same command. With `note`, the hook reports both, as fsvql_ring_pre and
 * the site-A ring scan do; without it this is the hook before that fix, which
 * credited the air time to the reference on the next grounded command. Live on
 * 2026-10-03, that gave a stock 250 fps client 18 injections in 30 s of stairs.
 */
struct stair_sim {
    struct sim s;
    int landings;
    int ground_ms;
};

/* `flat_every` > 0 makes every flat_every-th ground stretch flat_ms long: a
 * flight of stairs, then running on at the top. That is where a footstep owed
 * on the stairs is paid off 200 ms after the last landing, and the next regular
 * one can follow inside LISTENER_PAIR_MS. Measured live on d6d28bed: 293 ms. */
static void run_stairs_flat(double fps, int seconds, int ground, int flat_every, int flat_ms,
                            int allow_replace, int note, struct stair_sim *r);

static void run_stairs(double fps, int seconds, int ground, int allow_replace, int note,
                       struct stair_sim *r) {
    run_stairs_flat(fps, seconds, ground, 0, 0, allow_replace, note, r);
}

static void run_stairs_flat(double fps, int seconds, int ground, int flat_every, int flat_ms,
                            int allow_replace, int note, struct stair_sim *r) {
    static const int air_ms[] = {40, 80, 120, 160};
    struct fs_slot s;
    struct fs_result res;
    double period = 1000.0 / fps;
    double carry = 0.0;
    int now = 100000;
    int end = now + seconds * 1000;
    int bob = 0, ref_bob = 0;
    int last_step = 0, have_step = 0;
    int ground_until = now + ground, air_until = 0, ai = 0;

    memset(r, 0, sizeof(*r));
    r->s.min_inject_gap = 1 << 30;
    fs_slot_reset(&s);

    while (now < end) {
        int msec = next_msec(period, &carry);
        float addend = BOBMOVE_RUN * (float)msec;
        int ref_new;

        now += msec;
        if (air_until) {
            if (now < air_until) {
                if (note) {
                    fs_note_command(&s, now);
                }
                continue;
            }
            air_until = 0;
            ground_until = now + ((flat_every > 0 && ai % flat_every == 0) ? flat_ms : ground);
            r->landings++;
            if (note) {
                fs_note_footstep(&s, now);
            }
            last_step = now;
            have_step = 1;
        }
        r->ground_ms += msec;

        ref_new = (ref_bob + trunc_add_ref(addend)) & 255;
        fs_step(&s, now, bob, addend, &res, allow_replace);
        if (res.new_bob != ref_new) {
            r->s.bob_mismatch++;
        }
        bob = res.new_bob;
        s.last_bob = bob;
        ref_bob = ref_new;

        if (res.vanilla_flip && !res.suppress) {
            r->s.vanilla++;
        }
        if (res.inject) {
            r->s.injected++;
            note_injection(&r->s, now);
            if (have_step) {
                int gap = now - last_step;
                if (gap < r->s.min_inject_gap) r->s.min_inject_gap = gap;
                if (gap < FS_MIN_STEP_GAP_MS) r->s.gap_violations++;
            }
        }
        if ((res.vanilla_flip && !res.suppress) || res.inject) {
            last_step = now;
            have_step = 1;
        }
        if (now >= ground_until) {
            air_until = now + air_ms[ai++ % (int)(sizeof(air_ms) / sizeof(air_ms[0]))];
        }
    }
}

/* 120 ms stretches are a fast descent: every stretch is shorter than the
 * minimum gap after its landing, so the clock never fits in and replace mode
 * hears the landings alone, which is also all a stock client gets there apart
 * from bob flips within 120 ms of a landing. 400 ms stretches leave room, and
 * there the clock must not be starved. */
static int check_stairs(void) {
    static const double fps_list[] = {250, 333, 400, 1000};
    static const int grounds[] = {120, 400};
    const int n = (int)(sizeof(fps_list) / sizeof(fps_list[0]));
    const int seconds = 120;
    int failures = 0;
    int g, i, replace;

    printf("\nstairs: ground stretches, each followed by a 40/80/120/160 ms drop "
           "and a CrashLand footstep\n");
    printf("%6s  %6s  %7s  %9s  %9s  %9s  %9s  %8s  %s\n", "ground", "fps", "replace",
           "vanilla/s", "landing/s", "inject/s", "old inj/s", "minGap", "verdict");
    for (g = 0; g < 2; g++)
    for (replace = 0; replace <= 1; replace++) {
        for (i = 0; i < n; i++) {
            struct stair_sim r, old;
            double van, land, inj, cap;
            char gapbuf[16];
            int bad = 0;
            const char *why = "";

            run_stairs(fps_list[i], seconds, grounds[g], replace, 1, &r);
            run_stairs(fps_list[i], seconds, grounds[g], replace, 0, &old);
            van = (double)r.s.vanilla / seconds;
            land = (double)r.landings / seconds;
            inj = (double)r.s.injected / seconds;
            /* the clock may only run on ground time */
            cap = (replace ? CLOCK_RATE : REFERENCE_RATE) * r.ground_ms / (seconds * 1000.0);

            if (r.s.bob_mismatch) {
                bad = 1;
                why = "bobCycle DIVERGED from vanilla";
            } else if (r.s.gap_violations) {
                bad = 1;
                why = "injection inside the minimum gap of a footstep or landing";
            } else if (r.s.inj_pair_violations) {
                bad = 1;
                why = "two injections inside FS_MIN_INJECT_GAP_MS";
            } else if (fps_list[i] <= 333 && r.s.injected != 0) {
                bad = 1;
                why = "injected into a stock client on stairs";
            } else if (inj > cap + 0.05) {
                bad = 1;
                why = "air time credited to the clock";
            } else if (replace && fps_list[i] >= 400 && grounds[g] > FS_MIN_STEP_GAP_MS
                       && inj < cap - 0.15) {
                bad = 1;
                why = "landings starve the listener clock";
            }
            if (bad) failures++;
            if (r.s.injected) {
                snprintf(gapbuf, sizeof gapbuf, "%d", r.s.min_inject_gap);
            } else {
                snprintf(gapbuf, sizeof gapbuf, "%s", "-");
            }
            printf("%6d  %6.0f  %7d  %9.3f  %9.3f  %9.3f  %9.3f  %8s  %s%s\n",
                   grounds[g], fps_list[i], replace, van, land, inj,
                   (double)old.s.injected / seconds, gapbuf,
                   bad ? "FAIL: " : "pass", why);
        }
    }
    return failures;
}

/* Stairs, then running on at the top, repeated. The only cell where an owed
 * footstep is released after landings and a regular one follows on flat ground,
 * so it is the one that has to catch two injections inside LISTENER_PAIR_MS. */
static int check_stairs_to_flat(void) {
    static const double fps_list[] = {400, 500, 1000};
    static const int grounds[] = {120, 200};
    const int seconds = 300;
    int failures = 0;
    size_t f, g;
    int replace;

    printf("\nstairs then flat: 6 stair steps (ground stretch, 40-160 ms drop, landing), "
           "then 3 s of running\n");
    printf("%6s  %6s  %7s  %9s  %9s  %11s  %s\n", "ground", "fps", "replace", "landing/s",
           "inject/s", "pairs<350ms", "verdict");
    for (g = 0; g < sizeof(grounds) / sizeof(grounds[0]); g++)
    for (replace = 0; replace <= 1; replace++)
    for (f = 0; f < sizeof(fps_list) / sizeof(fps_list[0]); f++) {
        struct stair_sim r;
        int bad = 0;
        const char *why = "";
        run_stairs_flat(fps_list[f], seconds, grounds[g], 7, 3000, replace, 1, &r);
        if (r.s.bob_mismatch) {
            bad = 1;
            why = "bobCycle DIVERGED from vanilla";
        } else if (r.s.gap_violations) {
            bad = 1;
            why = "injection inside the minimum gap of a footstep or landing";
        } else if (r.s.inj_pair_violations) {
            bad = 1;
            why = "two injections inside LISTENER_PAIR_MS: listeners drop the second";
        } else if (r.s.injected == 0) {
            bad = 1;
            why = "no injections at all: the cell does not exercise the clock";
        }
        if (bad) failures++;
        printf("%6d  %6.0f  %7d  %9.3f  %9.3f  %11d  %s%s\n", grounds[g], fps_list[f], replace,
               (double)r.landings / seconds, (double)r.s.injected / seconds,
               r.s.inj_pair_violations, bad ? "FAIL: " : "pass", why);
    }
    return failures;
}

/*
 * Landings that are events, not footsteps: a bunnyhop or a jump pad lands with
 * EV_FALL_* (or takes off with EV_JUMP / EV_JUMP_PAD), and the hook reports it
 * through fs_note_event. An injection within FS_EVENT_GUARD_MS after one would
 * drain into the same entityState slot before the next snapshot and erase it
 * for listeners. The guard must also stay short enough that the clock still
 * fits into short ground contacts: no more than 10% fewer injections than with
 * the guard off.
 */
#define EVENT_SAFE_MS 50

static void run_event_landings(double fps, int seconds, int ground, int allow_replace, int guard,
                               int *injected, int *violations) {
    static const int air_ms[] = {300, 450, 600, 700};   /* hops and pad flights */
    struct fs_slot s;
    struct fs_result res;
    double period = 1000.0 / fps, carry = 0.0;
    int now = 100000, end = now + seconds * 1000, bob = 0;
    int ground_until = now + ground, air_until = 0, ai = 0, last_evt = -100000;

    fs_slot_reset(&s);
    *injected = *violations = 0;
    while (now < end) {
        int msec = next_msec(period, &carry);
        now += msec;
        if (air_until) {
            if (now < air_until) {
                fs_note_command(&s, now);
                continue;
            }
            air_until = 0;
            ground_until = now + ground;
            last_evt = now;
            if (guard) {
                fs_note_event(&s, now);
            }
        }
        fs_step(&s, now, bob, BOBMOVE_RUN * (float)msec, &res, allow_replace);
        bob = res.new_bob;
        s.last_bob = bob;
        if (res.inject) {
            (*injected)++;
            if (now - last_evt < EVENT_SAFE_MS) {
                (*violations)++;
            }
        }
        if (now >= ground_until) {
            air_until = now + air_ms[ai++ % 4];
        }
    }
}

static int check_event_guard(void) {
    static const double fps_list[] = {400, 1000};
    static const int grounds[] = {60, 120, 400};
    const int seconds = 300;
    int failures = 0;
    size_t f, g;

    printf("\nlandings as events (bunnyhop / jump pad): ground stretch, 300-700 ms in the air, "
           "EV_FALL on landing\n");
    printf("%6s  %6s  %10s  %10s  %12s  %s\n", "ground", "fps", "inj/s", "inj/s off",
           "within 50 ms", "verdict");
    for (g = 0; g < sizeof(grounds) / sizeof(grounds[0]); g++)
    for (f = 0; f < sizeof(fps_list) / sizeof(fps_list[0]); f++) {
        int inj, viol, inj_off, viol_off;
        int bad = 0;
        const char *why = "";
        run_event_landings(fps_list[f], seconds, grounds[g], 1, 1, &inj, &viol);
        run_event_landings(fps_list[f], seconds, grounds[g], 1, 0, &inj_off, &viol_off);
        if (viol) {
            bad = 1;
            why = "an injection inside EVENT_SAFE_MS of a landing event";
        } else if (inj < 0.9 * inj_off) {
            bad = 1;
            why = "the guard starves the clock in short ground contacts";
        }
        if (bad) failures++;
        printf("%6d  %6.0f  %10.3f  %10.3f  %5d (%4d)  %s%s\n", grounds[g], fps_list[f],
               (double)inj / seconds, (double)inj_off / seconds, viol, viol_off,
               bad ? "FAIL: " : "pass", why);
    }
    return failures;
}

int main(void) {
    static const double fps_list[] = {125, 200, 250, 300, 333, 375, 400, 450, 500, 800, 1000};
    const int n = (int)(sizeof(fps_list) / sizeof(fps_list[0]));
    const int seconds = 120;
    int failures = 0;
    int i;

    printf("S5 cadence self-test  -  %d s per cell, reference %.3f steps/s\n\n",
           seconds, REFERENCE_RATE);
    printf("%6s  %9s  %9s  %9s  %8s  %8s  %s\n",
           "fps", "vanilla/s", "inject/s", "total/s", "vs ref", "minGap", "verdict");

    for (i = 0; i < n; i++) {
        struct sim r;
        double van, inj, total;
        const char *why = "";
        char gapbuf[16];
        int bad = 0;

        run(fps_list[i], seconds, 0, &r);
        van = (double)r.vanilla / seconds;
        inj = (double)r.injected / seconds;
        total = van + inj;

        /* 1. bobCycle must be indistinguishable from vanilla. Non-negotiable. */
        if (r.bob_mismatch) {
            bad = 1;
            why = "bobCycle DIVERGED from vanilla";
        }
        /* 2. never below the reference, allowing one step of slack over the run */
        else if (total < REFERENCE_RATE - 0.05) {
            bad = 1;
            why = "below the 250 fps reference";
        }
        /* 3. a player already at or above the reference is left alone */
        else if (van >= REFERENCE_RATE && r.injected != 0) {
            bad = 1;
            why = "injected into a player who did not need it";
        }
        /* 4. the guard held */
        else if (r.gap_violations) {
            bad = 1;
            why = "injection inside the minimum gap";
        }
        /* 5. listeners can tell two injections apart */
        else if (r.inj_pair_violations) {
            bad = 1;
            why = "two injections inside FS_MIN_INJECT_GAP_MS";
        }

        if (bad) {
            failures++;
        }
        if (r.injected) {
            snprintf(gapbuf, sizeof gapbuf, "%d", r.min_inject_gap);
        } else {
            snprintf(gapbuf, sizeof gapbuf, "%s", "-");
        }
        printf("%6.0f  %9.3f  %9.3f  %9.3f  %7.0f%%  %8s  %s%s\n",
               fps_list[i], van, inj, total, 100.0 * total / REFERENCE_RATE,
               gapbuf, bad ? "FAIL: " : "pass", why);
    }

    failures += check_mixed();
    failures += check_replace();
    failures += check_stairs();
    failures += check_stairs_to_flat();
    failures += check_event_guard();

    printf("\n%s\n", failures ? "SELF-TEST FAILED" : "self-test passed");
    return failures ? 1 : 0;
}

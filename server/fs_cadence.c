#include "fs_cadence.h"

/* Mirror of qagame's own truncation, including the guard against a NaN or wild
 * float reaching the cast. 2.1 carries the same guard for the same reason: an
 * out-of-range (int) cast is undefined and this runs on the game thread. */
static int trunc_add(float add) {
    if (add <= -1024.f || add >= 1024.f) {
        return 0;
    }
    return (int)add;
}

void fs_slot_reset(struct fs_slot *s) {
    s->have_prev = 0;
    s->last_cmd_time = 0;
    s->last_bob = 0;
    s->acc_q = 0;
    s->target_cycle = 0;
    s->debt = 0;
    s->have_step = 0;
    s->last_step_time = 0;
    s->have_van = 0;
    s->last_van_time = 0;
    s->van_period_ms = 0;
    s->mean_dt_q8 = 0;
    s->replace = 0;
    s->have_inj = 0;
    s->last_inj_time = 0;
    s->have_evt = 0;
    s->last_evt_time = 0;
}

/* Enter above ~390 fps (mean command under 2.58 ms). A steady 360 stays on
 * its own vanilla, which is already faster than the reference. Leave below
 * ~320 fps so a walk near the line does not flap every window. */
#define FS_REPLACE_ENTER_Q8 660
#define FS_REPLACE_EXIT_Q8  800

static void track_rate(struct fs_slot *s, int dt, int allow_replace) {
    if (!allow_replace) {
        s->replace = 0;
        return;
    }
    if (dt < 1 || dt > 250) {
        return;
    }
    if (s->mean_dt_q8 == 0) {
        s->mean_dt_q8 = dt << 8;
    } else {
        s->mean_dt_q8 += ((dt << 8) - s->mean_dt_q8) >> 4;
    }
    if (!s->replace && s->mean_dt_q8 < FS_REPLACE_ENTER_Q8) {
        s->replace = 1;
    } else if (s->replace && s->mean_dt_q8 > FS_REPLACE_EXIT_Q8) {
        s->replace = 0;
    }
}

/* The measured vanilla beat, or 0 when there is no usable one.
 *
 * Returning 0 once vanilla is overdue is what makes this safe. A player who speeds
 * up stops flipping bobCycle entirely, and a stale period would leave the predictor
 * forever claiming a flip is imminent — which would suppress every injection from
 * then on. One reference step past the predicted flip is treated as "vanilla
 * is gone". */
static int van_period(const struct fs_slot *s, int now) {
    if (!s->have_van || s->van_period_ms <= 0) {
        return 0;
    }
    /* A missed flip means this beat is gone. The old 2x window was as long as
     * the beat itself, so a slow beat (a 3 ms slip at 500 fps, period ~1.7 s)
     * kept claiming a flip was imminent and held every injection. One reference
     * step of lateness is enough to call it dead. */
    if (now - s->last_van_time > s->van_period_ms + FS_STEP_INTERVAL_MS) {
        return 0;
    }
    return s->van_period_ms;
}

/* Milliseconds until vanilla flips on its own, or -1 when it will not.
 *
 * The flip tests bit 7 of (cycle + 64), so the boundaries are at cycle 64 and 192 —
 * 128 units apart. Distance to the next boundary, divided by the measured unit
 * rate, is the estimate. The unit rate comes from the *observed* interval between
 * the last two flips rather than from bobmove, because truncation means the
 * effective rate is lower than bobmove * msec and by a margin that depends on the
 * client's command pacing. */
static int ms_to_vanilla_flip(const struct fs_slot *s, int now, int cycle) {
    int period = van_period(s, now);
    int dist;

    if (!period) {
        return -1;
    }
    dist = ((64 - (cycle & 255)) % FS_UNITS_PER_STEP + FS_UNITS_PER_STEP)
           % FS_UNITS_PER_STEP;
    if (dist == 0) {
        dist = FS_UNITS_PER_STEP;
    }
    return (int)(((long)dist * (long)period) / FS_UNITS_PER_STEP);
}

void fs_note_command(struct fs_slot *s, int now) {
    if (s->have_prev) {
        s->last_cmd_time = now;
    }
}

void fs_note_footstep(struct fs_slot *s, int now) {
    s->have_step = 1;
    s->last_step_time = now;
}

void fs_note_event(struct fs_slot *s, int now) {
    s->have_evt = 1;
    s->last_evt_time = now;
}

void fs_step(struct fs_slot *s, int now, int old_bob, float addend,
             struct fs_result *out, int allow_replace) {
    int dt = s->have_prev ? now - s->last_cmd_time : 0;
    int iadd;
    int neu;

    out->inject = 0;
    out->suppress = 0;
    track_rate(s, dt, allow_replace);

    /* Something outside this hook moved bobCycle: the idle reset when the player
     * stops, a respawn, a teleport. Re-phase rather than carrying stale state,
     * and drop any debt — a player who just stopped does not owe footsteps. */
    if (s->last_bob != old_bob) {
        s->acc_q = 0;
        s->target_cycle = old_bob & 255;    /* phase-lock to vanilla */
        s->debt = 0;
    }

    s->last_cmd_time = now;
    s->have_prev = 1;

    /* ---- vanilla bobCycle, byte for byte. This is the load-bearing line of
     * the whole design: the client runs this same arithmetic on this same
     * command, so whatever we compute here, its prediction computes too. ---- */
    iadd = trunc_add(addend);
    neu = (old_bob + iadd) & 255;
    out->new_bob = neu;
    out->ecx_out = old_bob + 0x40;
    out->vanilla_flip = (((old_bob + 64) ^ (neu + 64)) & 128) != 0;

    /*
     * ---- private reference cadence ----
     *
     * Advance one bob unit per 4 ms of *elapsed* time, for every command,
     * whatever its length, and flip-test it exactly as vanilla does. That is
     * precisely what a 250 fps client's pmove produces: one unit per 4 ms
     * command. In replace mode the listener hears only this cycle, and it runs
     * at FS_CLOCK_UNITS_PER_SEC instead, a real 250 fps walk's tempo.
     *
     * Two earlier revisions of this block were wrong, both caught by
     * measurement rather than by reading:
     *
     *   1. It reset the accumulator on every >=4 ms command, copying 2.1's
     *      policy. 2.1's accumulator only has to survive to the next 4 ms tick;
     *      this one has to survive 128 ticks, about half a second, before it
     *      owes a footstep. On our test server 5.3% of 47149 commands took that
     *      branch and the resets cost about a fifth of the footsteps owed.
     *
     *   2. It then skipped accrual on those commands instead of resetting,
     *      which is just as wrong in a subtler way: their elapsed time stopped
     *      counting toward the reference at all, while their vanilla flips were
     *      still cancelling debt accrued by *other* commands' time. One 4 ms
     *      command in five halved the delivered rate.
     *
     * Accruing from all elapsed time fixes both. The phase-lock in the resync
     * above is what makes it safe at exactly 250 fps, where this cycle and
     * bobCycle advance in lockstep and their flips cancel.
     */
    if (dt >= 1 && dt <= 250) {
        s->acc_q += dt * (s->replace ? FS_CLOCK_UNITS_PER_SEC : FS_REF_UNITS_PER_SEC);
        while (s->acc_q >= 1000) {
            int old_t = s->target_cycle;
            s->acc_q -= 1000;
            s->target_cycle = (old_t + 1) & 255;
            if (((old_t + 64) ^ (s->target_cycle + 64)) & 128) {
                if (s->debt < FS_DEBT_MAX) {
                    s->debt++;
                }
            }
        }
    } else {
        /* No previous command, or a gap too long to read as movement time.
         * With fs_note_command called on every command, dt is this command's
         * own length; without it, air time lands here or, under 250 ms, above.
         * Bank nothing rather than crediting dead air. */
        s->acc_q = 0;
    }

    /* A vanilla flip means qagame is about to call PM_AddEvent itself, so the
     * client both predicted it and will have it confirmed in the ring. It counts
     * against the debt exactly like an injection would. */
    if (out->vanilla_flip) {
        /* Above 375 fps the listener hears the reference clock, not this
         * flip. Leave the debt and the injection clock where they are. The
         * mover's ring still gets the event from qagame; the hook strips it
         * from the entity snapshot. */
        if (s->replace) {
            out->suppress = 1;
            return;
        }
        if (s->debt > FS_DEBT_MIN) {
            s->debt--;
        }
        /* Measure the vanilla beat so injections can be placed against it. */
        if (s->have_van) {
            int period = now - s->last_van_time;
            if (period >= 1 && period <= 4 * FS_STEP_INTERVAL_MS) {
                s->van_period_ms = period;
            }
        }
        s->have_van = 1;
        s->last_van_time = now;
        s->have_step = 1;
        s->last_step_time = now;
        return;
    }

    /*
     * Behind the reference cadence. *When* to emit matters as much as whether:
     *
     *   - With no vanilla stream (>=500 fps, where bobmove * msec truncates to
     *     zero every command) there is nothing to interleave with. Debt becomes
     *     available exactly one reference interval apart, so emitting as soon as
     *     it exists is already evenly spaced — which is why that band measures 88%
     *     of seconds carrying exactly two footsteps. Leave that path alone.
     *
     *   - With a vanilla stream faster than the reference, place the rare
     *     injection at the midpoint of that beat. `since >= ahead` is the
     *     crossing of time-since-last and time-until-next.
     *
     *   - With a vanilla stream slower than the reference, do not wait for it.
     *     A 500 fps client whose frames sometimes slip to 3 ms still produces a
     *     few vanilla flips, and the measured period of that beat is ~1.7 s.
     *     Holding the injection until the midpoint of a beat slower than the
     *     512 ms reference is what opened the 875 ms holes. The injector owns
     *     the clock in that case; a vanilla flip still cancels one unit of debt
     *     when it happens to arrive.
     */
    if (s->debt >= 1) {
        int since = s->have_step ? now - s->last_step_time : FS_STEP_INTERVAL_MS;
        int period = van_period(s, now);
        int ahead = ms_to_vanilla_flip(s, now, neu);
        int place;

        /* Replace mode owns the clock outright. Waiting on a vanilla beat
         * here would put the irregular flips back into the gaps. */
        if (s->replace) {
            place = (!s->have_step || since >= FS_MIN_STEP_GAP_MS);
        } else if (ahead >= 0 && ahead < FS_MIN_STEP_GAP_MS) {
            /* Vanilla is about to fire inside the minimum gap. Emitting now
             * makes a pair, and the vanilla then cancels the next owed step. */
            place = 0;
        } else if (ahead < 0 || period > FS_STEP_INTERVAL_MS) {
            /* Slower than the reference: the injector owns the clock. Waiting
             * for the midpoint of a 1.7 s beat is the 875 ms hole. */
            place = (!s->have_step || since >= FS_MIN_STEP_GAP_MS);
        } else {
            place = (since >= ahead) && (since >= FS_MIN_STEP_GAP_MS);
        }
        if (place && s->have_inj && now - s->last_inj_time < FS_MIN_INJECT_GAP_MS) {
            place = 0;
        }
        if (place && s->have_evt && now - s->last_evt_time < FS_EVENT_GUARD_MS) {
            place = 0;
        }
        if (place) {
            s->debt--;
            s->have_step = 1;
            s->last_step_time = now;
            s->have_inj = 1;
            s->last_inj_time = now;
            out->inject = 1;
        }
    }
}

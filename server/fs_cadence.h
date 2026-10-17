/*
 * S5 footstep cadence core — pure logic, no Linux or qagame dependencies, so it
 * can be compiled and swept on the dev box before any .so exists.
 *
 * S5 in one sentence: leave the networked playerState.bobCycle advancing
 * *exactly* as vanilla so the client's prediction agrees with the server bit
 * for bit, keep the cadence accumulator in private server memory, and inject
 * only the footsteps vanilla failed to produce.
 *
 * Why that is the whole fix: the client de-duplicates a predicted footstep
 * against the server's positionally, via the shared eventSequence, and it can
 * only do that if both sides computed the same bobCycle from the same commands.
 * footsteps250 2.1 quantizes bobCycle, which breaks that agreement — and the
 * replay, the doubles, and the 375-500 fps problem are all downstream of it.
 * Removing the divergence removes all three at once. Evidence and the measured
 * offsets are in notes/fix-options-decision.md.
 *
 * This file therefore writes bobCycle with the vanilla value and never anything
 * else. It performs no writes to any networked playerState_t field.
 */

#ifndef FS_CADENCE_H
#define FS_CADENCE_H

/* One 250 fps pmove step. A 250 fps client adds trunc(0.4 * 4) = 1 bob unit per
 * command, so 1 unit per 4 ms is the reference progression: 250 units/s, which
 * is 250/128 = 1.953 footsteps/s. That, not the un-truncated 3.125/s, is the
 * target — 2.1 already established that true-rate is louder than stock and the
 * stated goal is to match a 250 fps player. */
#define FS_QUANTUM_MS 4
#define FS_REF_UNITS_PER_SEC (1000 / FS_QUANTUM_MS)

/* The listener clock in replace mode, which is all other players hear from
 * anyone above ~355 fps. A real 250 fps client walks faster than the
 * uniform-command model above: no client paces its commands evenly and
 * truncation works in its favour, so measured walks are 2.05-2.15 footsteps/s.
 * The clock matches that walk, not the model: 269 units/s is 2.102 footsteps/s,
 * one every 476 ms. Below the replace line the deficit filler keeps the
 * model's 250 units/s, which is what keeps it phase-locked to, and silent for,
 * a stock 250 or 333 fps player. */
#define FS_CLOCK_UNITS_PER_SEC 269

/* Commands this long or longer are left entirely alone: a 250 fps client is
 * already at the reference rate, so it accrues no debt and gets no injections.
 * Same boundary and same reasoning as 2.1. */
#define FS_VANILLA_MS 4

/* One footstep per 128 bob units — bit 7 of (cycle + 64) flipping. */
#define FS_UNITS_PER_STEP 128

/* An injection is never emitted within this many milliseconds of the previous
 * footstep from *either* path. It exists to stop an injection landing on top of a
 * vanilla flip and producing an audible stutter pair. Note the failure mode of a
 * badly chosen value here is one unevenly spaced footstep, never a doubled sound —
 * unlike every window in the client ledger, this one cannot eat a real event.
 * Default is well under the ~512 ms reference interval and well over a snapshot. */
#define FS_MIN_STEP_GAP_MS 200

/* Two injections with no real event between them reach listeners with the same
 * entityState value, and cgame plays an entity event only when the value
 * changes. The server clears ent->s.event EVENT_VALID_MSEC (300 ms) after it
 * was set, on a server frame, and a snapshot has to show the cleared slot before
 * the second injection, so the second must come at least 300 ms + one
 * snapshot interval each side later at sv_fps 40. Measured live on d6d28bed:
 * coming off stairs at 1000 fps, a footstep owed from the stairs went out 200
 * ms after the last landing and the next regular one 293 ms after it, and
 * listeners never played the second. */
#ifndef FS_MIN_INJECT_GAP_MS
#define FS_MIN_INJECT_GAP_MS 350
#endif

/* A jump, a jump pad or a landing (EV_JUMP, EV_JUMP_PAD, EV_FALL_*) drained into
 * ent->s.event is overwritten if an injection drains into the same slot before
 * the next snapshot, and listeners never hear it. Two snapshots at sv_fps 40.
 * Kept this short on purpose: a bunnyhop's ground contact is often under
 * 100 ms, and the clock must still fit in it. Pain is an externalEvent with
 * its own priority on the slot, and ring_room already sends an injection by
 * the overflow path then. */
#ifndef FS_EVENT_GUARD_MS
#define FS_EVENT_GUARD_MS 50
#endif

/* The reference interval between footsteps: 128 units at one unit per 4 ms. Used
 * to place an injection rather than merely to rate-limit it — see fs_step. */
#define FS_STEP_INTERVAL_MS (FS_UNITS_PER_STEP * FS_QUANTUM_MS)

/* Debt is clamped so a pathological session cannot bank a burst of injections
 * and spend them all at once. */
#define FS_DEBT_MAX 2
#define FS_DEBT_MIN (-2)

struct fs_slot {
    int have_prev;      /* a previous command has been seen for this slot */
    int last_cmd_time;  /* playerState.commandTime of that command */
    int last_bob;       /* bobCycle we wrote last, to notice outside writes */
    int acc_q;          /* elapsed ms x units/s, banked toward the next tick at 1000 */
    /* Private reference cycle, 0..255, advanced one unit per 4 ms of *elapsed*
     * time and flip-tested exactly like bobCycle. It is phase-locked to
     * bobCycle whenever the two are resynced, which is what keeps a genuine
     * 250 fps client from ever accruing debt: there its flips and vanilla's
     * land on the same command, so they cancel instead of drifting in and out
     * of phase and emitting a spurious extra footstep. */
    int target_cycle;
    int debt;           /* footsteps the reference cadence owes */
    int have_step;      /* a footstep has been emitted for this slot */
    int last_step_time; /* commandTime of it, either path */
    /* The vanilla flip beat, tracked so an injection can be *placed* relative to
     * it instead of firing whenever debt happens to become available. Arbitrary
     * placement is what made the 360-500 fps band lumpy: measured 26% of seconds
     * carrying one footstep where the neighbouring bands carry two. */
    int have_van;       /* a vanilla flip has been seen for this slot */
    int last_van_time;  /* commandTime of the most recent one */
    int van_period_ms;  /* interval between the last two, i.e. the vanilla beat */
    /* EMA of command dt in milliseconds * 256. 0 until the first command.
     * Above ~390 fps the listener hears the reference clock instead of the
     * irregular vanilla flips; see fs_step. */
    int mean_dt_q8;
    int replace;
    int have_inj;       /* an injection has been emitted for this slot */
    int last_inj_time;  /* commandTime of it, for FS_MIN_INJECT_GAP_MS */
    int have_evt;       /* a jump / jump pad / landing event has been seen */
    int last_evt_time;  /* commandTime of it, for FS_EVENT_GUARD_MS */
};

struct fs_result {
    int new_bob;      /* what to store in playerState.bobCycle — always vanilla */
    int ecx_out;      /* old + 0x40, the value qagame's next insn expects */
    int vanilla_flip; /* vanilla crossed a boundary: qagame will ring an event */
    int inject;       /* emit an injected footstep this command */
    /* allow_replace was set, this player is above the 375 fps line, and this
     * command's vanilla flip must not be copied out to other players. The
     * mover's ring is left alone so his prediction still confirms. */
    int suppress;
};

void fs_slot_reset(struct fs_slot *s);

/* Run one command. `now` is playerState.commandTime after PmoveSingle set it,
 * `old_bob` is playerState.bobCycle before the update, `addend` is qagame's own
 * bobmove * pml.msec. */
/* `allow_replace` is 1 only for the listener-replace mode (s2). When it is 0
 * the cadence is the original deficit filler and `suppress` stays 0. */
void fs_step(struct fs_slot *s, int now, int old_bob, float addend,
             struct fs_result *out, int allow_replace);

/* A command ran that did not reach the bob site: airborne, or no movement
 * keys. Vanilla bobCycle did not move on it, so its time must not be credited
 * to the reference either, or the next grounded command owes footsteps for air
 * time. Down a staircase that is every step: measured on our test server
 * 2026-10-03, a stock 250 fps client got 18 injections in 30 s of stairs, each
 * landing a snapshot after the CrashLand footstep. Call once per command, after
 * Pmove; on a command that did reach the site it changes nothing. */
void fs_note_command(struct fs_slot *s, int now);

/* A footstep reached listeners from outside the cadence (PM_CrashLand), so the
 * minimum gap counts from it too. */
void fs_note_footstep(struct fs_slot *s, int now);

/* A jump, jump pad or landing event reached the ring: no injection for
 * FS_EVENT_GUARD_MS, so it can't overwrite that event in ent->s.event. */
void fs_note_event(struct fs_slot *s, int now);

#endif /* FS_CADENCE_H */

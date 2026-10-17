#define _GNU_SOURCE
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "fs_cadence.h"

/*
 * footstepsVQL — footstep cadence for qagamex64.so, a replacement for
 * footsteps250 2.1. Only one of the two may hold the bobCycle site; this one
 * refuses (FSVQL_NO_PATTERN) rather than fight 2.1 for it. 2.1's ABI shape,
 * trampoline contract, slot table and map-repatch discipline are deliberately
 * mirrored so the operational habits transfer.
 *
 * What it does differently from 2.1, and why it is the whole fix:
 *
 *   2.1 quantizes playerState.bobCycle so a high-FPS player's *server* cycle
 *   advances at the stock-250 rate. That makes footsteps audible again, but it
 *   also makes the server's bobCycle disagree with what the client's own pmove
 *   computes from the same commands. The client de-duplicates a predicted
 *   footstep against the server's positionally, through the shared
 *   eventSequence, and it can only do that when both sides agree. Breaking the
 *   agreement is what lets an unpredicted footstep slide into the two-slot
 *   predictable ring, shift the drain window, and replay the jump or shot
 *   sitting in the other slot. That replay is the doubled sound.
 *
 *   footstepsVQL therefore writes bobCycle with the *vanilla* value, always, and
 *   keeps the reference cadence in private server memory instead.
 *
 * Two sites, installed together or not at all by fsvql_patch():
 *   A  the bobCycle store in PM_Footsteps: vanilla bobCycle, private cadence.
 *   C  the call to SendPendingPredictableEvents in ClientThink_real: each owed
 *      footstep is written into the mover's ring for the listeners' drain and
 *      taken back out before any snapshot. Above ~390 fps the vanilla bob
 *      footstep is also stripped from the listeners' entityState, so they hear
 *      only the reference clock.
 *
 * No logging, no file descriptors, nothing that can block the game thread —
 * same rule 2.1 adopted. Everything an operator needs comes back through the
 * return codes and the counters.
 */

#define NSLOT 256
#define CORE_LEN 27
#define MAX_OBJ 4
#define MAX_SEG 8
#define QAGAME_NAME "qagamex64.so"
#define NBUCKET 8

enum {
    FSVQL_OK = 1,
    FSVQL_NOT_MAPPED = 0,
    FSVQL_SELFTEST = -1,
    FSVQL_NO_PATTERN = -2,
    FSVQL_AMBIGUOUS = -3,
    FSVQL_MPROTECT = -4,
    FSVQL_NO_RING = -5,         /* site A is there, site C is not in this build */
    FSVQL_NO_GATE = -6,         /* PM_Footsteps' footstep gate is not where expected */
    FSVQL_NO_REACH = -7         /* no page for site C's stub within 2 GB of qagame */
};

/* The bobCycle update inside PM_Footsteps. Verified unique in the live
 * qagamex64.so (sha256 091d568f...) by scripts/_qagame_sigs.py, and it is the
 * same 27 bytes 2.1 keys off, so the two artifacts cannot silently patch
 * different sites. */
static const unsigned char k_core[CORE_LEN] = {
    0x8b, 0x48, 0x08, 0xf3, 0x0f, 0x2a, 0xc1, 0x83, 0xc1, 0x40, 0xf3, 0x0f,
    0x58, 0xc1, 0xf3, 0x0f, 0x2c, 0xd0, 0x81, 0xe2, 0xff, 0x00, 0x00, 0x00,
    0x89, 0x50, 0x08
};

struct slot {
    const void *ps;
    struct fs_slot c;
    /* Ring undo. Everything needed to put the mover's predictable-event ring
     * back exactly as his own prediction left it, recorded at the moment we
     * write into it and consumed a few hundred instructions later in the same
     * ClientThink_real call. undo_seq is eventSequence *before* our write, so
     * "the ring has not moved since" is exactly undo_seq + 1. */
    int undo_pending;
    int undo_slot;
    int undo_event;
    int undo_parm;
    int undo_seq;
    int undo_ev;        /* the raw event id we wrote, for the re-tag */
    /* A vanilla bob footstep that must not reach listeners in replace mode,
     * by its ring sequence. It leaves through whichever listener channel drains
     * it first: ent->s.event (cleared after the drain, matched by its exact
     * tag) or the overflow temp entity (skipped before SendPendingPredictable-
     * Events runs), on this command or a later one if the ring was backed up.
     * The mover's ring is never touched. CrashLand footsteps never set this. */
    int strip_pending;
    int strip_seq;
    /* ent->s.event as this mover's last command left it, and when. A stripped
     * vanilla footstep overwrote it in the drain; putting it back keeps the
     * clock footstep (or shot) it would otherwise erase before any snapshot. */
    int ent_prev_valid;
    int ent_prev_event;
    int ent_prev_parm;
    int ent_prev_time;
    /* eventSequence as the last undo left it. Ring events past it at site A
     * were added earlier in this command's Pmove, which is where PM_CrashLand's
     * landing footstep comes from. */
    int ev_seen_valid;
    int ev_seen;
};

/* Older than this, the entity slot's previous event is not put back: two
 * snapshots at sv_fps 40, so a listener cannot have missed it and nobody new in
 * view hears a stale one. */
#define STRIP_RESTORE_MS 50

typedef int (*footstep_surface_fn)(void);

/*
 * When vanilla plays a footstep at all. PM_Footsteps reaches the bobCycle store
 * from four branches - running, walk key, crouched, and a slow-velocity band -
 * and only the running one sets its local `footstep` flag, held in r13d. After
 * the store it plays a footstep only if that flag is set, pm->noFootsteps is
 * clear and pm->waterlevel is 0. The injector has to obey the same gate, or a
 * walking or crouched player above ~355 fps, silent in vanilla, is heard at the
 * reference clock (measured on our test server 2026-10-03: 1.875/s walking and
 * 1.812/s crouched, through bot snapshots).
 *
 * The trampoline passes r13d and r12 (pm). These bytes are what make that
 * meaningful, so they are checked relative to the site before anything is
 * installed:
 *   site - 16     mov rax,[r12]                    r12 is pm, ps loaded from it
 *   site + 0x2f   mov edi,[r12+0xd4]; test; je G   waterlevel, then to the gate
 *   G             test r13d,r13d; je; mov esi,[r12+0x2c]; test esi,esi
 *                                                  footstep flag, noFootsteps
 * Nothing between the store and G writes r13.
 */
#define GATE_PRE_OFF   (-16)
#define GATE_WL_OFF    0x2F
#define GATE_REL_OFF   0x3B
#define GATE_NEXT_OFF  0x3F
#define PM_NOFOOTSTEPS_OFF 0x2C
#define PM_WATERLEVEL_OFF  0xD4
static const unsigned char k_gate_pre[] = {0x49, 0x8b, 0x04, 0x24};
static const unsigned char k_gate_wl[] = {
    0x41, 0x8b, 0xbc, 0x24, 0xd4, 0x00, 0x00, 0x00, 0x85, 0xff, 0x0f, 0x84
};
static const unsigned char k_gate_head[] = {0x45, 0x85, 0xed, 0x0f, 0x84};
static const unsigned char k_gate_tail[] = {0x41, 0x8b, 0x74, 0x24, 0x2c, 0x85, 0xf6};
#define GATE_TAIL_OFF 9

/* SendPendingPredictableEvents, entry through its G_TempEntity call. Only used
 * to find the function: site C's existing call must target exactly this
 * address. 0 in the mask means "any byte": the two rel32 displacements, which
 * are intra-module and so position independent but would differ in another
 * build. Uniqueness checked by scripts/_qagame_sigs.py against the live binary. */
static const unsigned char k_sendp_sig[] = {
    0x41,0x56,0x41,0x55,0x41,0x54,0x55,0x53,
    0x8b,0x87,0xec,0x01,0x00,0x00,
    0x48,0x89,0xfb,
    0x3b,0x47,0x6c,0x0f,0x8d,0,0,0,0,
    0x41,0x89,0xc4,0x83,0xe0,0x01,
    0x44,0x8b,0xaf,0x80,0x00,0x00,0x00,
    0x48,0x83,0xc0,0x1c,0x41,0x83,0xe4,0x03,0x41,0xc1,0xe4,0x08,
    0x44,0x0b,0x24,0x87,
    0xc7,0x87,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x48,0x8d,0x7f,0x14,
    0x44,0x89,0xe6,
    0x41,0x83,0xc4,0x0d,
    0xe8,0,0,0,0
};
static const unsigned char k_sendp_msk[] = {
    1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,
    1,1,1,
    1,1,1,1,1,0,0,0,0,
    1,1,1,1,1,1,
    1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,
    1,1,1,
    1,1,1,1,
    1,0,0,0,0
};

/* PM_FootstepForSurface: reads pml.groundTrace.surfaceFlags via RIP (wildcarded)
 * then branches on SURF_NOSTEPS / SURF_METALSTEPS. Returns 0, 1, 2, 81 or 82. */
static const unsigned char k_surf_sig[] = {
    0x8b,0x15,0,0,0,0, 0x31,0xc0, 0xf6,0xc6,0x20, 0x75,0,
    0xf6,0xc6,0x10, 0xb0,0x02, 0x74,0, 0xf3,0xc3
};
static const unsigned char k_surf_msk[] = {
    1,1,0,0,0,0, 1,1, 1,1,1, 1,0,
    1,1,1, 1,1, 1,0, 1,1
};

/* ---------- the ring write and undo ----------
 *
 * Listeners hear the full reference cadence, the mover hears nothing extra, and
 * there is no double for anyone, because nothing unpredicted is ever left in the
 * mover's playerState.
 *
 * The trick is that *other* players do not hear the mover through the ring. They
 * hear him through his own entityState: BG_PlayerStateToEntityState drains one
 * ring event into ent->s.event (+0xBC) and that is what ships in their snapshots.
 * The temp entity SendPendingPredictableEvents builds is only the overflow
 * channel for a second event in the same frame. Measured, notes/s1-qagame-offsets.md.
 *
 * And the mover never receives his own entityState at all — the server
 * regenerates it from his playerState instead of sending it. So an event that
 * exists in the ring only long enough to be copied into ent->s.event reaches
 * every listener and never reaches the mover.
 *
 * The whole window therefore fits inside one ClientThink_real call:
 *
 *   oldEventSequence = ps->eventSequence      saved before Pmove
 *   Pmove(...)                                <- our site A writes the ring here
 *   if (ps->eventSequence != oldEventSequence)
 *           ent->eventTime = level.time       <- sees our write, so the 300 ms
 *                                                event-validity clock is armed
 *   BG_PlayerStateToEntityState(ps, &ent->s)  <- listeners get it here
 *   SendPendingPredictableEvents(ps)          <- overflow temp entity
 *   ---- our undo goes here ----              <- before any snapshot is built
 *
 * Writing inside Pmove rather than just before the drain is deliberate: it is the
 * only position that precedes the oldEventSequence comparison, and without that
 * comparison firing, ent->eventTime stays stale and ent->s.event is cleared by
 * the next frame's sweep instead of persisting for EVENT_VALID_MSEC. That
 * persistence is what makes other players hearing the footstep robust against a
 * single dropped snapshot.
 *
 * The cost of writing that early is that something else could add a ring event
 * between our write and our undo. Only PM_WaterEvents can: PM_Footsteps runs
 * after PM_Weapon in PmoveSingle, so jumps and shots are already in. If it
 * happens the ring has moved past our slot and rolling back would eat the other
 * event, so the undo declines and counts it. That leaves one unpredicted footstep
 * in the ring, i.e. the 2.1 symptom, once — g_ring_skipped is how we find out
 * whether it ever actually occurs.
 *
 * No instruction is displaced for this. The rel32 of the existing
 * `call SendPendingPredictableEvents` is repointed at a thunk that calls the
 * original and then undoes the ring: four bytes, and the e8 opcode is never
 * touched. A rel32 reaches only +-2 GB, and qagame is mapped afresh on every
 * map change, so the call lands on a stub mapped near qagame that jumps on to
 * the thunk with an absolute address (see stub_near).
 */
#define SITE_C_CALL_OFF 0x2F       /* the e8, from the start of the signature */

/* playerState_t, as dword indices for an int *. All measured. */
#define PS_EVENTSEQ_I      (0x6c / 4)
#define PS_EVENTS_I        (0x70 / 4)
#define PS_EVENTPARMS_I    (0x78 / 4)
#define PS_ENTITYEVENTSEQ_I (0x1ec / 4)
#define PS_EXTERNALEVENT_I (0x80 / 4)
#define PS_EVENT_SLOTS 2           /* MAX_PS_EVENTS; the `and eax,1` at 0x59a8d */

/* The tail of ClientThink_real. Both rel32s wildcarded; the `74 0b` is not,
 * because a short jump's displacement is intra-region. Uniqueness checked by
 * scripts/_qagame_sigs.py — one hit, at 0x5a1aa. */
static const unsigned char k_ctr_sig[] = {
    0x48,0x8b,0xbd,0x40,0x02,0x00,0x00,
    0x8b,0x44,0x24,0x38,
    0x39,0x47,0x6c,
    0x74,0x0b,
    0x41,0x8b,0x44,0x24,0x24,
    0x89,0x85,0x7c,0x02,0x00,0x00,
    0xba,0x01,0x00,0x00,0x00,
    0x48,0x89,0xee,
    0xe8,0,0,0,0,
    0x48,0x8b,0xbd,0x40,0x02,0x00,0x00,
    0xe8,0,0,0,0
};
static const unsigned char k_ctr_msk[] = {
    1,1,1,1,1,1,1,
    1,1,1,1,
    1,1,1,
    1,1,
    1,1,1,1,1,
    1,1,1,1,1,1,
    1,1,1,1,1,
    1,1,1,
    1,0,0,0,0,
    1,1,1,1,1,1,1,
    1,0,0,0,0
};

static footstep_surface_fn g_surface;

/* Site C: the repointed call. g_patched_c is the address of the e8 opcode, and
 * fsvql_real_sendp is where the thunk forwards to. g_ring is 1 only while that
 * call targets our stub in the current mapping, and gates every ring write. */
void *fsvql_real_sendp __attribute__((visibility("hidden")));
void fsvql_thunk_sendp(void);
void fsvql_ring_pre(int *ps);
void fsvql_ring_undo(int *ps, void *ent);
static uint8_t *g_patched_c;
static size_t g_site_c_off;
static int g_site_c_valid;
static int32_t g_site_c_orig_rel;
static int g_ring;
static int unpatch_c(void);
static int ring_live(void);

static volatile unsigned long g_ring_written;
static volatile unsigned long g_ring_drained;
static volatile unsigned long g_ring_undone;
static volatile unsigned long g_ring_skipped;
static volatile unsigned long g_ring_missed;
static volatile unsigned long g_suppressed;
static volatile unsigned long g_ring_retagged;
static volatile unsigned long g_ring_deferred;
static volatile unsigned long g_quiet;
static volatile unsigned long g_strip_overflow;
static volatile unsigned long g_strip_restored;

static struct slot g_slot[NSLOT];
static int g_status;
static int selftest_done;
static uint8_t *g_patched_at;
static size_t g_site_off;
static int g_site_off_valid;

static volatile unsigned long g_hits;
static volatile unsigned long g_vanilla;
static volatile unsigned long g_due;
static volatile unsigned long g_dt[NBUCKET];
/* Exact sum of command intervals, in ms. The honest denominator for any rate:
 * the hook only runs while grounded and moving, so wall clock is wrong, and
 * reconstructing movement time from g_dt[] is only approximate because the wide
 * buckets (5-8, 9-32, >32 ms) have to be scored at a midpoint. That
 * approximation understated a 200 fps client's measured rate by about 20% in the
 * first live session, because its 5 ms commands were scored as 6 ms. */
static volatile unsigned long g_move_ms;

static int is_bob_footstep(int ev);
static int is_guarded_event(int ev);

void *fsvql_ret __attribute__((visibility("hidden")));
void fsvql_tramp(void);
int fsvql_bob_update(int *ps, float addend, int footstep, const unsigned char *pm);

void fsvql_reset_slots(void) {
    memset(g_slot, 0, sizeof(g_slot));
}

void fsvql_reset_counters(void) {
    int i;
    g_hits = 0;
    g_vanilla = 0;
    g_due = 0;
    g_move_ms = 0;
    g_ring_written = 0;
    g_ring_drained = 0;
    g_ring_undone = 0;
    g_ring_skipped = 0;
    g_ring_missed = 0;
    g_suppressed = 0;
    g_ring_retagged = 0;
    g_ring_deferred = 0;
    g_quiet = 0;
    g_strip_overflow = 0;
    g_strip_restored = 0;
    for (i = 0; i < NBUCKET; i++) {
        g_dt[i] = 0;
    }
}

uint64_t fsvql_hits(void) { return g_hits; }
uint64_t fsvql_vanilla_flips(void) { return g_vanilla; }
uint64_t fsvql_injections_due(void) { return g_due; }
uint64_t fsvql_move_ms(void) { return g_move_ms; }

/* 0: no previous command, 1..4: that many ms, 5: 5-8, 6: 9-32, 7: over 32. */
uint64_t fsvql_dt_bucket(int index) {
    if (index < 0 || index >= NBUCKET) {
        return 0;
    }
    return g_dt[index];
}

/* The ring pipeline: injections_due -> written -> drained -> undone.
 *
 * drained is the one that answers "did other players actually get it": it counts
 * writes that a listener channel consumed. written == drained means every footstep
 * owed was delivered; a gap means some reached nobody.
 *
 * skipped counts the rare case where PM_WaterEvents added an event behind ours and
 * the rollback declined; missed counts a write whose undo never ran at all. Both
 * should be 0, and both mean one unpredicted footstep was left in the mover's
 * ring, i.e. one chance of the 2.1 symptom. */
uint64_t fsvql_ring_written(void) { return g_ring_written; }
uint64_t fsvql_ring_drained(void) { return g_ring_drained; }
uint64_t fsvql_ring_undone(void) { return g_ring_undone; }
uint64_t fsvql_ring_skipped(void) { return g_ring_skipped; }
uint64_t fsvql_ring_missed(void) { return g_ring_missed; }
/* Vanilla bob footsteps stripped from a listener's entityState (replace, above
 * ~390 fps), and injected footsteps whose entityState tag was moved clear of the
 * next real event after the rollback. */
uint64_t fsvql_ring_suppressed(void) { return g_suppressed; }
uint64_t fsvql_ring_retagged(void) { return g_ring_retagged; }
/* Owed footsteps held back a command because the engine had no room left to
 * ship another ring event to listeners in that command. */
uint64_t fsvql_ring_deferred(void) { return g_ring_deferred; }
/* Commands on which vanilla plays no footstep (walk key, crouched, in water,
 * footsteps disabled), where the clock owes and injects nothing. */
uint64_t fsvql_quiet(void) { return g_quiet; }
/* Of fsvql_ring_suppressed(): those taken off the overflow path instead of out
 * of ent->s.event, because another event held the entity slot that command. */
uint64_t fsvql_ring_suppressed_overflow(void) { return g_strip_overflow; }
/* Of those taken out of ent->s.event: how many put back the event the drain had
 * overwritten (usually a clock footstep) instead of leaving the slot empty. */
uint64_t fsvql_ring_suppressed_restored(void) { return g_strip_restored; }

static int dt_bucket(int dt) {
    if (dt <= 0) return 0;
    if (dt <= 4) return dt;
    if (dt <= 8) return 5;
    if (dt <= 32) return 6;
    return 7;
}

static unsigned slot_hash(const void *ps) {
    uint64_t x = (uint64_t)(uintptr_t)ps;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return (unsigned)(uint8_t)x;
}

/* Look up without creating: a playerState never seen at the bob site cannot have
 * anything outstanding, and allocating here would evict a real player's slot. */
static struct slot *slot_find(const void *ps) {
    unsigned idx = slot_hash(ps), i, end = idx + 8;
    for (i = idx; i != end; i++) {
        if (g_slot[(uint8_t)i].ps == ps) {
            return &g_slot[(uint8_t)i];
        }
    }
    return NULL;
}

static struct slot *slot_for(const void *ps) {
    unsigned idx = slot_hash(ps), i, end = idx + 8;
    struct slot *empty = NULL;

    for (i = idx; i != end; i++) {
        struct slot *s = &g_slot[(uint8_t)i];
        if (s->ps == ps) {
            return s;
        }
        if (!empty && s->ps == NULL) {
            empty = s;
        }
    }
    if (!empty) {
        empty = &g_slot[idx];
    }
    empty->ps = ps;
    /* A slot arrives here either evicted from a full probe chain or because a
     * playerState allocation was recycled; anything the previous occupant left
     * would otherwise be applied to whoever owns the slot now. */
    empty->strip_pending = 0;
    empty->ent_prev_valid = 0;
    empty->ev_seen_valid = 0;
    empty->undo_pending = 0;
    fs_slot_reset(&empty->c);
    return empty;
}

/* How many more ring events the engine will ship to listeners this command.
 * BG_PlayerStateToEntityState copies one into ent->s.event unless externalEvent
 * holds that slot, and SendPendingPredictableEvents sends one more as the
 * overflow temp entity (notes/s1-qagame-offsets.md). Ours is written last in the
 * command, so whatever is already pending is ahead of it. */
static int ring_room(const int *ps) {
    int pending = ps[PS_EVENTSEQ_I] - ps[PS_ENTITYEVENTSEQ_I];
    int cap = ps[PS_EXTERNALEVENT_I] ? 1 : 2;
    if (pending < 0) {
        pending = 0;
    }
    return cap - pending;
}

/*
 * Trampoline body. ps is playerState_t*, addend is qagame's bobmove * pml.msec.
 * Returns the *old* bobCycle, because the trampoline tail has to rebuild
 * ecx = old + 0x40 for the instruction that followed the replaced store.
 *
 * ps[0] is commandTime, ps[2] is bobCycle. commandTime is assigned at the top
 * of PmoveSingle, so the delta between two calls for one player is that
 * command's msec — the same pml.msec the client's own PM_Footsteps used for the
 * same command. That identity is what makes the vanilla branch below bit-exact
 * against the client's prediction.
 */
int fsvql_bob_update(int *ps, float addend, int footstep, const unsigned char *pm) {
    struct slot *s = slot_for(ps);
    struct fs_result r;
    int old = ps[2];
    int now = ps[0];
    int dt = s->c.have_prev ? now - s->c.last_cmd_time : 0;
    int had_step = s->c.have_step;
    int last_step = s->c.last_step_time;
    int had_inj = s->c.have_inj;
    int last_inj = s->c.last_inj_time;
    int audible = footstep && pm
                  && *(const int *)(pm + PM_NOFOOTSTEPS_OFF) == 0
                  && *(const int *)(pm + PM_WATERLEVEL_OFF) == 0;

    g_dt[dt_bucket(dt)]++;
    /* Same sanity window fs_step uses to decide a command is readable movement
     * time, so the counter and the cadence never disagree about what counts. */
    if (dt >= 1 && dt <= 250) {
        g_move_ms += (unsigned long)dt;
    }

    /* PM_CrashLand and PM_CheckJump run before PM_Footsteps in PmoveSingle, so
     * a landing footstep, landing or jump from this command is already in the
     * ring, and listeners hear it. Nothing else puts a footstep id there ahead
     * of this site. */
    if (s->ev_seen_valid) {
        int seq = ps[PS_EVENTSEQ_I];
        int k = seq - PS_EVENT_SLOTS > s->ev_seen ? seq - PS_EVENT_SLOTS : s->ev_seen;
        for (; k < seq; k++) {
            int ev = ps[PS_EVENTS_I + (k & (PS_EVENT_SLOTS - 1))];
            if (is_bob_footstep(ev)) {
                fs_note_footstep(&s->c, now);
            } else if (is_guarded_event(ev)) {
                fs_note_event(&s->c, now);
            }
        }
    }

    fs_step(&s->c, now, old, addend, &r, g_ring);

    /* Vanilla is silent on this command, so the clock is too: nothing injected,
     * nothing stripped (a footstep in ent->s.event now can only be CrashLand's),
     * and no debt carried into the next running stretch. */
    if (!audible) {
        if (r.inject) {
            s->c.have_step = had_step;
            s->c.last_step_time = last_step;
            s->c.have_inj = had_inj;
            s->c.last_inj_time = last_inj;
        }
        r.inject = 0;
        r.suppress = 0;
        s->c.debt = 0;
        g_quiet++;
    }

    /* An injection that the engine cannot ship this command would reach nobody:
     * vanilla lets an unshipped event wait in the ring, but ours has to be gone
     * before the mover's snapshot. With two already pending it would also
     * overwrite the older one. So hand it back to the cadence - debt and the
     * last-step time as they were - and it goes out on a later command. */
    if (r.inject && g_ring && g_patched_c && g_surface && ring_room(ps) < 1) {
        s->c.debt++;
        s->c.have_step = had_step;
        s->c.last_step_time = last_step;
        s->c.have_inj = had_inj;
        s->c.last_inj_time = last_inj;
        r.inject = 0;
        g_ring_deferred++;
    }

    ps[2] = r.new_bob;                  /* vanilla, and nothing else, ever */
    s->c.last_bob = r.new_bob;

    g_hits++;
    if (r.vanilla_flip && audible) {
        g_vanilla++;
    }
    /* An undo still outstanding when this player's next command arrives means the
     * write/undo pair broke — ClientThink_real ran Pmove but not our thunk. Do not
     * try to repair the ring here: a snapshot has already gone out and the ring may
     * have legitimately moved on, so touching it now could eat a real event. Just
     * account for it, because a non-zero count invalidates the fix. */
    if (s->undo_pending) {
        s->undo_pending = 0;
        g_ring_missed++;
    }

    /* PM_Footsteps adds this command's footstep straight after this site, so it
     * takes the ring sequence that is current now (nothing is injected on a
     * command that flips). */
    if (r.suppress) {
        s->strip_pending = 1;
        s->strip_seq = ps[PS_EVENTSEQ_I];
    }
    if (r.inject) {
        g_due++;
        /* Resolve the surface here, inside pmove, because that is the only place
         * pml.groundTrace is current. A 0 back means SURF_NOSTEPS, i.e. a
         * surface that is deliberately silent - drop it rather than substituting
         * a default footstep. Written only while the thunk that takes it back
         * out again is installed. */
        if (g_ring && g_patched_c && g_surface) {
            int ev = g_surface();
            if (ev) {
                /* Exactly what PM_AddEvent does. The readers apply the
                 * (seq & 3) << 8 tag, so the raw id goes in. */
                int seq = ps[PS_EVENTSEQ_I];
                int slot = seq & (PS_EVENT_SLOTS - 1);

                s->undo_seq = seq;
                s->undo_ev = ev;
                s->undo_slot = slot;
                s->undo_event = ps[PS_EVENTS_I + slot];
                s->undo_parm = ps[PS_EVENTPARMS_I + slot];
                ps[PS_EVENTS_I + slot] = ev;
                ps[PS_EVENTPARMS_I + slot] = 0;
                ps[PS_EVENTSEQ_I] = seq + 1;
                s->undo_pending = 1;
                g_ring_written++;
            }
        }
    }
    return old;
}

/* Footstep event ids. CrashLand uses the same ids, so stripping goes by the
 * ring sequence the bob flip took, never by id alone. */
static int is_bob_footstep(int ev) {
    ev &= ~0x300; /* EV_EVENT_BITS */
    return ev == 1 || ev == 2 || ev == 81 || ev == 82;
}

/* EV_FALL_SHORT 6, EV_FALL_MEDIUM 7, EV_FALL_FAR 8, EV_JUMP_PAD 9, EV_JUMP 10
 * (notes/event-numbers.md). Not EV_FIRE_WEAPON: under LG it comes every 50 ms
 * and the clock would never fit between shots. */
static int is_guarded_event(int ev) {
    ev &= ~0x300;
    return ev >= 6 && ev <= 10;
}

#define ENT_CLIENT_OFF 0x240
#define ES_EVENT_OFF 0xBC
#define ES_EVENTPARM_OFF 0xC0

/* rbp at site C is the gentity; only touch it when ent->client (+0x240) is the
 * playerState we were called with. */
static int ent_is_ps(const void *ent, const int *ps) {
    return ent && *(void *const *)((const unsigned char *)ent + ENT_CLIENT_OFF)
                  == (const void *)ps;
}

/*
 * Put the ring back exactly as the mover's own prediction left it. Runs from the
 * thunk at site C, after BG_PlayerStateToEntityState has already copied the event
 * into ent->s.event for every listener and after SendPendingPredictableEvents has
 * had its chance at the overflow temp entity — and before any snapshot is built,
 * so the mover never sees that any of it happened.
 *
 * entityEventSequence has to come back too. The drain advanced it to consume our
 * event; leaving it ahead of eventSequence would mean the *next* real jump or shot
 * is never shipped to other players, which is a silent and much worse bug than the
 * one this exists to avoid.
 */
/*
 * Runs from the thunk just before SendPendingPredictableEvents. If the event it
 * is about to ship as the overflow temp entity is the vanilla footstep this
 * mover's listeners must not hear, step entityEventSequence past it, so the
 * overflow ships the next event or none. Only the listeners' drain pointer
 * moves: events[] and eventSequence, which the mover predicts against, stay
 * as they are, and entityEventSequence is never sent to any client.
 */
void fsvql_ring_pre(int *ps) {
    struct slot *s;
    int seq;

    if (!g_ring) {
        return;
    }
    s = slot_find(ps);
    if (!s) {
        return;
    }
    /* Once per ClientThink_real, airborne commands included, so the next
     * site-A dt is that command's own length and not the air time before it. */
    fs_note_command(&s->c, ps[0]);
    if (!s->strip_pending) {
        return;
    }
    seq = s->strip_seq;
    if (ps[PS_ENTITYEVENTSEQ_I] == seq && ps[PS_EVENTSEQ_I] > seq
        && is_bob_footstep(ps[PS_EVENTS_I + (seq & (PS_EVENT_SLOTS - 1))])) {
        ps[PS_ENTITYEVENTSEQ_I] = seq + 1;
        s->strip_pending = 0;
        g_suppressed++;
        g_strip_overflow++;
    }
}

static void ring_undo_core(int *ps, void *ent, struct slot *s) {
    /* Drained by now, through the entity slot this command or an earlier one,
     * or skipped above. If it is what ent->s.event holds, take it back out: its
     * id is a footstep and its tag is its own sequence's. Put back what the
     * drain overwrote if that is recent, otherwise leave the slot empty. */
    if (s && s->strip_pending && ps[PS_ENTITYEVENTSEQ_I] > s->strip_seq) {
        unsigned char *e = ent;
        s->strip_pending = 0;
        if (ent_is_ps(ent, ps)) {
            int ev = *(int *)(e + ES_EVENT_OFF);
            if (is_bob_footstep(ev) && ((ev >> 8) & 3) == (s->strip_seq & 3)) {
                int back = s->ent_prev_valid && s->ent_prev_event
                           && ps[0] - s->ent_prev_time <= STRIP_RESTORE_MS;
                *(int *)(e + ES_EVENT_OFF) = back ? s->ent_prev_event : 0;
                *(int *)(e + ES_EVENTPARM_OFF) = back ? s->ent_prev_parm : 0;
                g_suppressed++;
                if (back) {
                    g_strip_restored++;
                }
            }
        }
    }
    if (!s || !s->undo_pending) {
        return;
    }
    s->undo_pending = 0;

    /*
     * Did a listener channel actually consume our event? entityEventSequence only
     * moves past undo_seq when BG_PlayerStateToEntityState copied the event into
     * ent->s.event — which is how other players hear the mover — or when
     * SendPendingPredictableEvents shipped it as a temp entity instead, which it
     * does when externalEvent held the entityState slot that frame. Either way it
     * went out to other players. A write that is never drained reached nobody, so
     * written minus drained is the honest count of footsteps that failed to go out.
     *
     * Counted before the rollback decision below, because delivery to listeners and
     * hiding it from the mover are two independent things that can fail separately.
     */
    if (ps[PS_ENTITYEVENTSEQ_I] > s->undo_seq) {
        g_ring_drained++;
    }

    /* Has anything been added behind us? Only PM_WaterEvents can get in, since
     * PM_Footsteps runs after PM_Weapon. If it did, the two-slot ring cannot
     * express "remove the older of these", and rolling eventSequence back would
     * discard the other event for the mover. Declining costs one unpredicted
     * footstep; rolling back would cost a lost jump or splash. */
    if (ps[PS_EVENTSEQ_I] != s->undo_seq + 1) {
        g_ring_skipped++;
        return;
    }

    ps[PS_EVENTSEQ_I] = s->undo_seq;
    ps[PS_EVENTS_I + s->undo_slot] = s->undo_event;
    ps[PS_EVENTPARMS_I + s->undo_slot] = s->undo_parm;
    if (ps[PS_ENTITYEVENTSEQ_I] == s->undo_seq + 1) {
        ps[PS_ENTITYEVENTSEQ_I] = s->undo_seq;
        /* The next real event is now drained at undo_seq as well, so it gets the
         * same (seq & 3) << 8 tag ours went out with. Listeners compare the whole
         * value against previousEvent (cgamex86 1004c1cf), so a CrashLand or
         * vanilla footstep there, while ours is still live, would be identical
         * to it and never play. Two tags on differs from both neighbours. */
        if (ent_is_ps(ent, ps)) {
            int *evp = (int *)((unsigned char *)ent + ES_EVENT_OFF);
            if (*evp == (s->undo_ev | ((s->undo_seq & 3) << 8))) {
                *evp = s->undo_ev | (((s->undo_seq + 2) & 3) << 8);
                g_ring_retagged++;
            }
        }
    }
    g_ring_undone++;
}

void fsvql_ring_undo(int *ps, void *ent) {
    struct slot *s;

    if (!g_ring) {
        return;
    }
    s = slot_find(ps);
    ring_undo_core(ps, ent, s);
    if (s) {
        /* Events added after site A this command: BG_TouchJumpPad's EV_JUMP_PAD
         * comes from G_TouchTriggers, after Pmove. Our own write is rolled
         * back by now; one left by a declined undo is a footstep id, which
         * this ignores. */
        if (s->ev_seen_valid) {
            int seq = ps[PS_EVENTSEQ_I];
            int k = seq - PS_EVENT_SLOTS > s->ev_seen ? seq - PS_EVENT_SLOTS : s->ev_seen;
            for (; k < seq; k++) {
                if (is_guarded_event(ps[PS_EVENTS_I + (k & (PS_EVENT_SLOTS - 1))])) {
                    fs_note_event(&s->c, ps[0]);
                }
            }
        }
        s->ev_seen = ps[PS_EVENTSEQ_I];
        s->ev_seen_valid = 1;
    }
    if (s && ent_is_ps(ent, ps)) {
        const unsigned char *e = ent;
        int ev = *(const int *)(e + ES_EVENT_OFF);
        /* Dated by when this value appeared, not by when it was last seen. */
        if (!s->ent_prev_valid || ev != s->ent_prev_event) {
            s->ent_prev_event = ev;
            s->ent_prev_time = ps[0];
        }
        s->ent_prev_parm = *(const int *)(e + ES_EVENTPARM_OFF);
        s->ent_prev_valid = 1;
    }
}

/* Two minutes of running at each of 1, 2, 3 and 4 ms must land on the 250 fps
 * reference rate of about 1.95 steps/s once vanilla flips and owed injections
 * are added together, and the bobCycle written must equal a pure vanilla
 * simulation fed the same commands. A build that fails either refuses to
 * install rather than changing how the server sounds — 2.1's rule, and the
 * bobCycle half is the one that matters most: if it ever diverges, the client's
 * prediction stops agreeing and the doubles come straight back. */
#define SELFTEST_S 120
/* 1.953 steps/s, scaled by 100 and floored the same way the counters below are. */
#define REF_X100 195
/* The replace-mode listener clock, FS_CLOCK_UNITS_PER_SEC / 128 = 2.102 steps/s. */
#define CLOCK_X100 (FS_CLOCK_UNITS_PER_SEC * 100 / FS_UNITS_PER_STEP)

/* Per second x100: vanilla flips a listener hears, vanilla flips stripped from
 * the listener, injections; and commands whose bobCycle left vanilla. */
struct st_cell { int van; int supp; int inj; int bad; };

/* Two minutes of running, cycling through `msecs`. */
static void selftest_run(const int *msecs, int nmsec, int allow_replace,
                         struct st_cell *out) {
    struct fs_slot s;
    struct fs_result r;
    int now = 100000;
    int end = now + SELFTEST_S * 1000;
    int bob = 0, ref = 0, van = 0, supp = 0, inj = 0, bad = 0, k = 0;

    fs_slot_reset(&s);
    while (now < end) {
        int msec = msecs[k++ % nmsec];
        float addend = 0.4f * (float)msec;
        int iadd = (addend <= -1024.f || addend >= 1024.f) ? 0 : (int)addend;
        int refnew = (ref + iadd) & 255;

        now += msec;
        fs_step(&s, now, bob, addend, &r, allow_replace);
        if (r.new_bob != refnew) {
            bad++;
        }
        bob = r.new_bob;
        s.last_bob = bob;
        ref = refnew;
        if (r.vanilla_flip) {
            if (r.suppress) supp++;
            else van++;
        }
        if (r.inject) inj++;
    }
    out->van = van * 100 / SELFTEST_S;
    out->supp = supp * 100 / SELFTEST_S;
    out->inj = inj * 100 / SELFTEST_S;
    out->bad = bad;
}

/*
 * Two minutes of running at every command interval from 1 to 8 ms. Three
 * invariants, and a build that breaks any of them refuses to install rather
 * than changing how the server sounds.
 *
 * Note what is deliberately NOT asserted: a single target rate. The vanilla
 * footstep rate is a sawtooth in framerate, not a constant -- a 333 fps client
 * legitimately emits 2.60/s off 3 ms commands and a 200 fps client 3.125/s,
 * both above the 250 fps reference. footstepsVQL only ever *adds* footsteps, so the
 * correct assertion is "never below the reference", never "equal to it". An
 * earlier revision of this function asserted a 1.80-2.10 band and failed the
 * 3 ms case, which is how this comment came to exist.
 */
int fsvql_selftest(void) {
    static const int alt23[] = {2, 3};
    struct st_cell c;
    int msec;
    for (msec = 1; msec <= 8; msec++) {
        selftest_run(&msec, 1, 0, &c);

        /* 1. bobCycle must be indistinguishable from vanilla. If this ever
         * fails the client's prediction stops agreeing and the doubled sounds
         * come straight back, so it is the one that matters most. */
        if (c.bad) {
            return -1;
        }
        /* 2. never quieter than a 250 fps client */
        if (c.van + c.inj < REF_X100 - 15) {
            return -1;
        }
        /* 3. a player already at or above the reference is left alone */
        if (c.van >= REF_X100 - 5 && c.inj != 0) {
            return -1;
        }

        /* The same three for the listener-replace path, which must also stay
         * out of the way entirely at 3 ms (333 fps) and slower. */
        selftest_run(&msec, 1, 1, &c);
        if (c.bad || c.van + c.inj < REF_X100 - 15) {
            return -1;
        }
        if (msec >= 3 && (c.supp != 0 || c.inj != 0)) {
            return -1;
        }
        /* At 1 and 2 ms replace is on and the listener hears the clock alone. */
        if (msec <= 2 && (c.inj < CLOCK_X100 - 5 || c.inj > CLOCK_X100 + 5)) {
            return -1;
        }
    }
    /* 400 fps sends alternating 2 and 3 ms commands, so vanilla does flip and
     * replace is on: no flip may reach a listener, and the clock carries them. */
    selftest_run(alt23, 2, 1, &c);
    if (c.bad || c.van != 0 || c.supp == 0
        || c.inj < CLOCK_X100 - 5 || c.inj > CLOCK_X100 + 5) {
        return -1;
    }
    return 0;
}

__asm__(
    ".text\n"
    ".globl fsvql_tramp\n"
    ".type fsvql_tramp,@function\n"
    "fsvql_tramp:\n"
    "  .byte 0xf3, 0x0f, 0x1e, 0xfa\n"          /* endbr64, spelled for old as */
    "  lea    -0x80(%rsp), %rsp\n"
    "  push   %rbp\n"
    "  mov    %rsp, %rbp\n"
    "  and    $0xfffffffffffffff0, %rsp\n"
    "  sub    $0x160, %rsp\n"
    "  mov    %rax, 0x100(%rsp)\n"
    "  pushf\n"
    "  pop    %rax\n"
    "  mov    %rax, 0x148(%rsp)\n"
    "  mov    %rcx, 0x108(%rsp)\n"
    "  mov    %rdx, 0x110(%rsp)\n"
    "  mov    %rsi, 0x118(%rsp)\n"
    "  mov    %rdi, 0x120(%rsp)\n"
    "  mov    %r8,  0x128(%rsp)\n"
    "  mov    %r9,  0x130(%rsp)\n"
    "  mov    %r10, 0x138(%rsp)\n"
    "  mov    %r11, 0x140(%rsp)\n"
    "  movups %xmm0, 0x00(%rsp)\n"
    "  movups %xmm1, 0x10(%rsp)\n"
    "  movups %xmm2, 0x20(%rsp)\n"
    "  movups %xmm3, 0x30(%rsp)\n"
    "  movups %xmm4, 0x40(%rsp)\n"
    "  movups %xmm5, 0x50(%rsp)\n"
    "  movups %xmm6, 0x60(%rsp)\n"
    "  movups %xmm7, 0x70(%rsp)\n"
    "  movups %xmm8, 0x80(%rsp)\n"
    "  movups %xmm9, 0x90(%rsp)\n"
    "  movups %xmm10, 0xa0(%rsp)\n"
    "  movups %xmm11, 0xb0(%rsp)\n"
    "  movups %xmm12, 0xc0(%rsp)\n"
    "  movups %xmm13, 0xd0(%rsp)\n"
    "  movups %xmm14, 0xe0(%rsp)\n"
    "  movups %xmm15, 0xf0(%rsp)\n"
    "  mov    0x100(%rsp), %rdi\n"                /* rax at the site = ps */
    "  movss  %xmm1, %xmm0\n"                     /* addend */
    "  mov    %r13d, %esi\n"                      /* PM_Footsteps' footstep flag */
    "  mov    %r12, %rdx\n"                       /* pm */
    "  call   fsvql_bob_update@PLT\n"
    "  mov    %eax, 0x150(%rsp)\n"
    "  movups 0x00(%rsp), %xmm0\n"
    "  movups 0x10(%rsp), %xmm1\n"
    "  movups 0x20(%rsp), %xmm2\n"
    "  movups 0x30(%rsp), %xmm3\n"
    "  movups 0x40(%rsp), %xmm4\n"
    "  movups 0x50(%rsp), %xmm5\n"
    "  movups 0x60(%rsp), %xmm6\n"
    "  movups 0x70(%rsp), %xmm7\n"
    "  movups 0x80(%rsp), %xmm8\n"
    "  movups 0x90(%rsp), %xmm9\n"
    "  movups 0xa0(%rsp), %xmm10\n"
    "  movups 0xb0(%rsp), %xmm11\n"
    "  movups 0xc0(%rsp), %xmm12\n"
    "  movups 0xd0(%rsp), %xmm13\n"
    "  movups 0xe0(%rsp), %xmm14\n"
    "  movups 0xf0(%rsp), %xmm15\n"
    "  mov    0x108(%rsp), %rcx\n"
    "  mov    0x110(%rsp), %rdx\n"
    "  mov    0x118(%rsp), %rsi\n"
    "  mov    0x120(%rsp), %rdi\n"
    "  mov    0x128(%rsp), %r8\n"
    "  mov    0x130(%rsp), %r9\n"
    "  mov    0x138(%rsp), %r10\n"
    "  mov    0x140(%rsp), %r11\n"
    "  mov    0x148(%rsp), %rax\n"
    "  push   %rax\n"
    "  popf\n"
    "  mov    0x150(%rsp), %ecx\n"                /* old bobCycle */
    "  mov    0x100(%rsp), %rax\n"
    "  cvtsi2ss %ecx, %xmm0\n"
    "  add    $0x40, %ecx\n"
    "  addss  %xmm1, %xmm0\n"
    "  mov    0x8(%rax), %edx\n"
    "  and    $0xff, %edx\n"
    "  mov    %rbp, %rsp\n"
    "  pop    %rbp\n"
    "  lea    0x80(%rsp), %rsp\n"
    "  jmp    *fsvql_ret(%rip)\n"
);

/*
 * Site C's thunk. Reached, through the stub, by the existing `call` in ClientThink_real whose rel32
 * we repointed, so nothing is displaced and there is no tail to replay: it calls
 * the overflow skip, the real SendPendingPredictableEvents, and then the undo.
 *
 * Only rdi needs preserving — it is the playerState argument, and all three
 * callees want it; SendPendingPredictableEvents reads no other argument.
 * Everything else volatile is already dead across the original call as far as
 * ClientThink_real is concerned, and rbp and rbx, which the code after the call
 * does use, are callee-saved by every callee.
 *
 * Alignment. At thunk entry rsp == 8 (mod 16), because the caller's `call` pushed
 * a return address. `push %rdi` takes it to 0 (mod 16), which is what the ABI
 * wants immediately before a `call`. rdi is then reloaded from the stack rather
 * than popped, so that every call is made at the same alignment — popping
 * first would leave rsp at 8 (mod 16) and misalign it.
 */
__asm__(
    ".text\n"
    ".globl fsvql_thunk_sendp\n"
    ".type fsvql_thunk_sendp,@function\n"
    "fsvql_thunk_sendp:\n"
    "  .byte 0xf3, 0x0f, 0x1e, 0xfa\n"          /* endbr64 */
    "  push   %rdi\n"
    "  call   fsvql_ring_pre@PLT\n"
    "  mov    (%rsp), %rdi\n"
    "  call   *fsvql_real_sendp(%rip)\n"
    "  mov    (%rsp), %rdi\n"
    "  mov    %rbp, %rsi\n"                     /* ClientThink_real's rbp is the gentity */
    "  call   fsvql_ring_undo@PLT\n"
    "  pop    %rdi\n"
    "  ret\n"
);

struct seg { uint8_t *p; size_t n; };
struct qobj { uintptr_t base; struct seg seg[MAX_SEG]; int nseg; };
struct qmap { struct qobj obj[MAX_OBJ]; int nobj; };

static int collect_cb(struct dl_phdr_info *info, size_t size, void *data) {
    struct qmap *m = data;
    struct qobj *o;
    int i;

    (void)size;
    if (!info->dlpi_name || !strstr(info->dlpi_name, QAGAME_NAME)) {
        return 0;
    }
    if (m->nobj >= MAX_OBJ) {
        return 0;
    }
    o = &m->obj[m->nobj];
    o->base = (uintptr_t)info->dlpi_addr;
    o->nseg = 0;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
        if (ph->p_filesz < CORE_LEN || o->nseg >= MAX_SEG) continue;
        o->seg[o->nseg].p = (uint8_t *)(info->dlpi_addr + ph->p_vaddr);
        o->seg[o->nseg].n = (size_t)ph->p_filesz;
        o->nseg++;
    }
    if (o->nseg > 0) {
        m->nobj++;
    }
    return 0;
}

/* From the loader's link map, never /proc/self/maps: qagamex64.so is dlclose'd
 * and dlopen'ed on every map change, and dereferencing a site remembered from a
 * previous mapping would fault inside the game process. */
static int qagame_map(struct qmap *m) {
    memset(m, 0, sizeof(*m));
    dl_iterate_phdr(collect_cb, m);
    return m->nobj;
}

static int in_obj(const struct qobj *o, const uint8_t *p, size_t need) {
    int i;
    for (i = 0; i < o->nseg; i++) {
        if (p >= o->seg[i].p && (size_t)(p - o->seg[i].p) + need <= o->seg[i].n) {
            return 1;
        }
    }
    return 0;
}

static int in_map(const struct qmap *m, const uint8_t *p, size_t need) {
    int i;
    for (i = 0; i < m->nobj; i++) {
        if (in_obj(&m->obj[i], p, need)) return 1;
    }
    return 0;
}

static int tramp_shaped(const uint8_t *p) {
    int i;
    if (p[0] != 0xff || p[1] != 0x25) return 0;
    for (i = 14; i < CORE_LEN; i++) {
        if (p[i] != 0x90) return 0;
    }
    return 1;
}

/* Shaped like a trampoline *and* jumping into this copy of this library. An
 * older footstepsVQL, or 2.1's trampoline, fails this and is left alone rather than
 * being trusted or stomped. */
static int tramp_is_ours(const uint8_t *p) {
    void *dest;
    if (!tramp_shaped(p)) return 0;
    if (p[2] || p[3] || p[4] || p[5]) return 0;
    memcpy(&dest, p + 6, sizeof(dest));
    return dest == (void *)fsvql_tramp;
}

static uint8_t *find_bytes(uint8_t *base, size_t n, const unsigned char *pat, size_t m) {
    size_t i;
    if (n < m) return NULL;
    for (i = 0; i + m <= n; i++) {
        if (memcmp(base + i, pat, m) == 0) return base + i;
    }
    return NULL;
}

/* Byte search with a mask: 0 in the mask means "any byte". Used for the
 * patterns that have to wildcard rel32 displacements. */
static uint8_t *find_masked(uint8_t *base, size_t n, const unsigned char *pat,
                            const unsigned char *msk, size_t m, int *count) {
    size_t i, k;
    uint8_t *first = NULL;

    *count = 0;
    if (n < m) {
        return NULL;
    }
    for (i = 0; i + m <= n; i++) {
        for (k = 0; k < m; k++) {
            if (msk[k] && base[i + k] != pat[k]) {
                break;
            }
        }
        if (k == m) {
            (*count)++;
            if (!first) {
                first = base + i;
            }
        }
    }
    return first;
}

static int protect(void *p, size_t n, int prot) {
    uintptr_t pg = (uintptr_t)p & ~(uintptr_t)0xfff;
    uintptr_t end = ((uintptr_t)p + n + 0xfff) & ~(uintptr_t)0xfff;
    return mprotect((void *)pg, end - pg, prot);
}

/*
 * Drop every pointer and flag that belongs to a particular qagame mapping.
 *
 * g_ring alone must never mean "the ring write is safe"; it has to mean "and
 * g_surface and site C point into the mapping we are patched into right now".
 * qagame is dlclosed and dlopened on every map change, so the moment a *new*
 * mapping is adopted all of them are stale. Clearing them here, before site A
 * goes live in the new mapping, makes the ring write fail closed until site C
 * has been re-armed in that same mapping.
 *
 * g_site_c_off is deliberately kept: it is an offset from the load base, so it
 * stays correct across a remap.
 */
static void forget_mapping(void) {
    g_ring = 0;
    g_surface = NULL;
    g_patched_c = NULL;
    fsvql_real_sendp = NULL;
}

static int install_at(uint8_t *site, uintptr_t base) {
    unsigned char buf[CORE_LEN];
    void *tramp = (void *)fsvql_tramp;

    if (protect(site, CORE_LEN, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return FSVQL_MPROTECT;
    }
    /* Reached only when site A held vanilla bytes, i.e. this is a mapping we are
     * not already patched into. Anything remembered from the previous one is
     * stale, so it goes before the trampoline goes live. */
    forget_mapping();
    /* Return address first: the moment the jump lands, the tail reads fsvql_ret. */
    fsvql_ret = site + CORE_LEN;
    memset(buf, 0x90, sizeof(buf));
    buf[0] = 0xff;
    buf[1] = 0x25;
    buf[2] = buf[3] = buf[4] = buf[5] = 0;
    memcpy(buf + 6, &tramp, sizeof(tramp));
    memcpy(site, buf, CORE_LEN);
    g_patched_at = site;
    if (base) {
        g_site_off = (size_t)((uintptr_t)site - base);
        g_site_off_valid = 1;
    }
    (void)protect(site, CORE_LEN, PROT_READ | PROT_EXEC);
    return FSVQL_OK;
}

static int restore_at(uint8_t *site) {
    if (protect(site, CORE_LEN, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return FSVQL_MPROTECT;
    }
    memcpy(site, k_core, CORE_LEN);
    (void)protect(site, CORE_LEN, PROT_READ | PROT_EXEC);
    g_patched_at = NULL;
    g_status = FSVQL_NOT_MAPPED;
    forget_mapping();
    fsvql_reset_slots();
    return 1;
}

static void remember(uint8_t *site, uintptr_t base) {
    g_patched_at = site;
    if (base) {
        g_site_off = (size_t)((uintptr_t)site - base);
        g_site_off_valid = 1;
    }
}

uint64_t fsvql_qagame_base(void) {
    struct qmap m;
    if (qagame_map(&m) == 0) return 0;
    return (uint64_t)m.obj[0].base;
}

uint64_t fsvql_site(void) {
    return (uint64_t)(uintptr_t)g_patched_at;
}

/* Is site A ours in this mapping? Refreshes g_patched_at if it is. */
static int site_a_ours(const struct qmap *m) {
    int i;
    if (g_patched_at && in_map(m, g_patched_at, CORE_LEN) && tramp_is_ours(g_patched_at)) {
        return 1;
    }
    if (g_site_off_valid) {
        for (i = 0; i < m->nobj; i++) {
            uint8_t *p = (uint8_t *)(m->obj[i].base + g_site_off);
            if (in_obj(&m->obj[i], p, CORE_LEN) && tramp_is_ours(p)) {
                remember(p, m->obj[i].base);
                return 1;
            }
        }
    }
    return 0;
}

/* The footstep gate around a vanilla bob site in object o (see k_gate_pre). */
static int gate_ok(const struct qobj *o, const uint8_t *site) {
    const uint8_t *g;
    int32_t rel;

    if (!in_obj(o, site + GATE_PRE_OFF, sizeof(k_gate_pre))
        || memcmp(site + GATE_PRE_OFF, k_gate_pre, sizeof(k_gate_pre)) != 0) {
        return 0;
    }
    if (!in_obj(o, site + GATE_WL_OFF, GATE_NEXT_OFF - GATE_WL_OFF)
        || memcmp(site + GATE_WL_OFF, k_gate_wl, sizeof(k_gate_wl)) != 0) {
        return 0;
    }
    memcpy(&rel, site + GATE_REL_OFF, sizeof(rel));
    g = site + GATE_NEXT_OFF + rel;
    if (!in_obj(o, g, GATE_TAIL_OFF + sizeof(k_gate_tail))) {
        return 0;
    }
    return memcmp(g, k_gate_head, sizeof(k_gate_head)) == 0
           && memcmp(g + GATE_TAIL_OFF, k_gate_tail, sizeof(k_gate_tail)) == 0;
}

/* Site A: install the bob trampoline in this mapping, or confirm it is ours. */
static int patch_a(struct qmap *m) {
    uint8_t *vanilla = NULL;
    uintptr_t vanilla_base = 0;
    const struct qobj *vanilla_obj = NULL;
    int vanilla_n = 0;
    int i, j;

    /* Known offset in a live object: O(1), and no write if it is already ours. */
    if (g_site_off_valid) {
        for (i = 0; i < m->nobj; i++) {
            uint8_t *p = (uint8_t *)(m->obj[i].base + g_site_off);
            if (!in_obj(&m->obj[i], p, CORE_LEN)) continue;
            if (tramp_is_ours(p)) {
                remember(p, m->obj[i].base);
                return FSVQL_OK;
            }
            if (memcmp(p, k_core, CORE_LEN) == 0) {
                if (!gate_ok(&m->obj[i], p)) return FSVQL_NO_GATE;
                fsvql_reset_slots();
                return install_at(p, m->obj[i].base);
            }
        }
    }

    for (i = 0; i < m->nobj; i++) {
        for (j = 0; j < m->obj[i].nseg; j++) {
            uint8_t *v = find_bytes(m->obj[i].seg[j].p, m->obj[i].seg[j].n, k_core, CORE_LEN);
            if (v) {
                vanilla_n++;
                if (!vanilla) {
                    vanilla = v;
                    vanilla_base = m->obj[i].base;
                    vanilla_obj = &m->obj[i];
                }
            }
        }
    }

    if (vanilla_n == 1) {
        if (!gate_ok(vanilla_obj, vanilla)) return FSVQL_NO_GATE;
        fsvql_reset_slots();
        return install_at(vanilla, vanilla_base);
    }
    if (vanilla_n > 1) {
        return FSVQL_AMBIGUOUS;
    }
    /* No vanilla bytes and nothing of ours: most likely 2.1 holds this site.
     * Refuse rather than fighting another artifact for it. */
    return FSVQL_NO_PATTERN;
}

/* Locate SendPendingPredictableEvents and PM_FootstepForSurface. A signature
 * matching more than once does not ship. */
static int resolve_targets(struct qmap *m, uint8_t **sendp_out) {
    int i, j;
    uint8_t *sendp = NULL, *surf = NULL;
    int sendp_n = 0, surf_n = 0;

    for (i = 0; i < m->nobj; i++) {
        for (j = 0; j < m->obj[i].nseg; j++) {
            int c;
            uint8_t *p = find_masked(m->obj[i].seg[j].p, m->obj[i].seg[j].n,
                                     k_sendp_sig, k_sendp_msk,
                                     sizeof(k_sendp_sig), &c);
            if (p) {
                sendp_n += c;
                if (!sendp) sendp = p;
            }
            p = find_masked(m->obj[i].seg[j].p, m->obj[i].seg[j].n,
                            k_surf_sig, k_surf_msk, sizeof(k_surf_sig), &c);
            if (p) {
                surf_n += c;
                if (!surf) surf = p;
            }
        }
    }
    if (sendp_n > 1 || surf_n > 1) return FSVQL_AMBIGUOUS;
    if (!sendp || !surf) return FSVQL_NO_PATTERN;
    g_surface = (footstep_surface_fn)(void *)surf;
    *sendp_out = sendp;
    return FSVQL_OK;
}

/*
 * Site C's stub: `jmp [rip+0]` and the thunk's 64-bit address, on its own page
 * within rel32 reach of the call. A jump, not a call, so the thunk is entered
 * with the stack exactly as ClientThink_real's `call` left it (see the
 * alignment note on the thunk). 1.0 pointed the rel32 at the thunk itself,
 * which only works while qagame is mapped within 2 GB of this library; on
 * 2026-10-07 a map change put it 2.26 GB away and the re-patch refused (-5).
 * The call always goes through a stub now, so the one path is the tested one.
 * Pages are never unmapped: qagame is dlclosed without telling us, so there is
 * no moment at which a stub is known to be unused. One stays in use for as
 * long as qagame keeps landing within reach of it, so a day of map changes
 * normally maps one page, and MAX_STUB bounds the total.
 */
#define STUB_LEN 14
#define STUB_PAGE 0x1000
#define MAX_STUB 8
#define STUB_STEP 0x1000000         /* 16 MB between mmap hints */
#define STUB_HINTS 64               /* +-1 GB of hints around the call */
#define REL32_REACH ((intptr_t)0x7fff0000)

static uint8_t *g_stub[MAX_STUB];
static int g_nstub;

static int in_reach(const uint8_t *from, const uint8_t *to) {
    intptr_t d = (intptr_t)to - (intptr_t)from;
    return d > -REL32_REACH && d < REL32_REACH;
}

static int stub_is_ours(const uint8_t *p) {
    int i;
    for (i = 0; i < g_nstub; i++) {
        if (p == g_stub[i]) return 1;
    }
    return 0;
}

/* A stub reachable by a rel32 measured from `from` (the address after the
 * call). FSVQL_OK with *out set, FSVQL_MPROTECT, or FSVQL_NO_REACH. */
static int stub_near(const uint8_t *from, uint8_t **out) {
    void *thunk = (void *)fsvql_thunk_sendp;
    uintptr_t page = (uintptr_t)from & ~(uintptr_t)(STUB_PAGE - 1);
    int i, k;

    for (i = 0; i < g_nstub; i++) {
        if (in_reach(from, g_stub[i])) {
            *out = g_stub[i];
            return FSVQL_OK;
        }
    }
    if (g_nstub >= MAX_STUB) return FSVQL_NO_REACH;

    /* Hint 0 is the kernel's own choice, usually next to the newest mapping;
     * then alternate below and above the call in 16 MB steps. A hint the
     * kernel cannot honour comes back elsewhere, so every result is checked. */
    for (k = 0; k <= 2 * STUB_HINTS; k++) {
        uintptr_t step = (uintptr_t)((k + 1) / 2) * STUB_STEP;
        void *hint;
        uint8_t *p;

        if (k == 0) {
            hint = NULL;
        } else if (k & 1) {
            if (step >= page) continue;
            hint = (void *)(page - step);
        } else {
            hint = (void *)(page + step);
        }
        p = mmap(hint, STUB_PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) continue;
        if (!in_reach(from, p) || !in_reach(from, p + STUB_LEN)) {
            munmap(p, STUB_PAGE);
            continue;
        }
        p[0] = 0xff;
        p[1] = 0x25;
        p[2] = p[3] = p[4] = p[5] = 0;
        memcpy(p + 6, &thunk, sizeof(thunk));
        if (mprotect(p, STUB_PAGE, PROT_READ | PROT_EXEC) != 0) {
            munmap(p, STUB_PAGE);
            return FSVQL_MPROTECT;
        }
        g_stub[g_nstub++] = p;
        *out = p;
        return FSVQL_OK;
    }
    return FSVQL_NO_REACH;
}

/* Does the call at `p` currently target our stub (or, as 1.0 wrote it, our
 * thunk directly)? */
static int call_c_is_ours(const uint8_t *p) {
    int32_t rel;
    const uint8_t *t;
    if (p[0] != 0xe8) return 0;
    memcpy(&rel, p + 1, sizeof(rel));
    t = p + 5 + (intptr_t)rel;
    return t == (const uint8_t *)fsvql_thunk_sendp || stub_is_ours(t);
}

/*
 * Repoint ClientThink_real's `call SendPendingPredictableEvents` at our stub,
 * which jumps to our thunk.
 *
 * `sendp` is the function entry that resolve_targets already found by its own
 * signature, and the call's existing target must equal it. That cross-check is the
 * real validation here: two independent signatures, in two different functions,
 * have to agree about one address before four bytes are written.
 */
static int patch_c(uint8_t *sendp, struct qmap *m) {
    uint8_t *win = NULL, *call, *stub = NULL;
    uintptr_t win_base = 0;
    int win_n = 0, i, j, rc;
    int32_t rel, want;
    intptr_t delta;

    for (i = 0; i < m->nobj; i++) {
        for (j = 0; j < m->obj[i].nseg; j++) {
            int c;
            uint8_t *p = find_masked(m->obj[i].seg[j].p, m->obj[i].seg[j].n,
                                     k_ctr_sig, k_ctr_msk,
                                     sizeof(k_ctr_sig), &c);
            if (p) {
                win_n += c;
                if (!win) { win = p; win_base = m->obj[i].base; }
            }
        }
    }
    if (win_n > 1) return FSVQL_AMBIGUOUS;
    if (!win) return FSVQL_NO_PATTERN;

    call = win + SITE_C_CALL_OFF;
    if (call[0] != 0xe8) return FSVQL_NO_PATTERN;
    if (call_c_is_ours(call)) {
        g_patched_c = call;
        g_site_c_off = (size_t)((uintptr_t)call - win_base);
        g_site_c_valid = 1;
        fsvql_real_sendp = (void *)sendp;
        return FSVQL_OK;
    }
    memcpy(&rel, call + 1, sizeof(rel));
    /* The two signatures must agree, or we are not looking at what we think. */
    if (call + 5 + (intptr_t)rel != sendp) return FSVQL_NO_PATTERN;

    /* A near call reaches +-2 GB, and nothing keeps this library that close to
     * qagame, so the call targets a stub that is. Checked again rather than
     * trusted: a truncated rel32 would send the game somewhere else. */
    rc = stub_near(call + 5, &stub);
    if (rc != FSVQL_OK) return rc;
    delta = stub - (call + 5);
    want = (int32_t)delta;
    if ((intptr_t)want != delta) return FSVQL_NO_REACH;

    if (protect(call, 5, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return FSVQL_MPROTECT;
    }
    /* Forwarding target before the redirect: the moment those four bytes land,
     * the thunk can be entered. */
    fsvql_real_sendp = (void *)sendp;
    g_site_c_orig_rel = rel;
    memcpy(call + 1, &want, sizeof(want));
    g_patched_c = call;
    g_site_c_off = (size_t)((uintptr_t)call - win_base);
    g_site_c_valid = 1;
    (void)protect(call, 5, PROT_READ | PROT_EXEC);
    return FSVQL_OK;
}

static int unpatch_c(void) {
    struct qmap m;
    int i;

    if (!g_site_c_valid) return 0;
    if (qagame_map(&m) == 0) return 0;
    for (i = 0; i < m.nobj; i++) {
        uint8_t *p = (uint8_t *)(m.obj[i].base + g_site_c_off);
        if (!in_obj(&m.obj[i], p, 5) || !call_c_is_ours(p)) {
            continue;
        }
        if (protect(p, 5, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            return FSVQL_MPROTECT;
        }
        memcpy(p + 1, &g_site_c_orig_rel, sizeof(g_site_c_orig_rel));
        (void)protect(p, 5, PROT_READ | PROT_EXEC);
        g_patched_c = NULL;
        fsvql_real_sendp = NULL;
        return 1;
    }
    return 0;
}

/* 1 = the call in the current mapping points at our stub, and the ring is armed. */
static int ring_live(void) {
    struct qmap m;
    int i;

    if (!g_ring) return 0;
    if (qagame_map(&m) == 0) return 0;
    if (g_patched_c && in_map(&m, g_patched_c, 5) && call_c_is_ours(g_patched_c)) {
        return 1;
    }
    if (g_site_c_valid) {
        for (i = 0; i < m.nobj; i++) {
            uint8_t *p = (uint8_t *)(m.obj[i].base + g_site_c_off);
            if (in_obj(&m.obj[i], p, 5) && call_c_is_ours(p)) {
                g_patched_c = p;
                return 1;
            }
        }
    }
    return 0;
}

/* Site C, in the mapping site A is in. FSVQL_OK, FSVQL_MPROTECT, or FSVQL_NO_RING
 * for anything that means "not in this build". */
static int arm_ring(struct qmap *m) {
    uint8_t *sendp = NULL;
    int rc;

    if (ring_live()) return FSVQL_OK;
    rc = resolve_targets(m, &sendp);
    if (rc == FSVQL_OK) rc = patch_c(sendp, m);
    if (rc == FSVQL_OK) {
        g_ring = 1;
        return FSVQL_OK;
    }
    g_surface = NULL;
    if (rc == FSVQL_MPROTECT || rc == FSVQL_NO_REACH) return rc;
    return FSVQL_NO_RING;
}

/*
 * Install both sites in the current qagame mapping, or neither. The bob hook
 * without the ring would only count footsteps, which sounds exactly like no
 * hook at all while reporting itself patched - so a ring that cannot be armed
 * takes site A back out and says why.
 */
int fsvql_patch(void) {
    struct qmap m;
    int rc;

    if (!selftest_done) {
        selftest_done = 1;
        if (fsvql_selftest() != 0) {
            g_status = FSVQL_SELFTEST;
            return FSVQL_SELFTEST;
        }
    }
    if (g_status == FSVQL_SELFTEST) {
        return FSVQL_SELFTEST;
    }
    if (qagame_map(&m) == 0) {
        g_status = FSVQL_NOT_MAPPED;
        return FSVQL_NOT_MAPPED;
    }
    rc = patch_a(&m);
    if (rc == FSVQL_OK) {
        rc = arm_ring(&m);
        if (rc != FSVQL_OK) {
            (void)restore_at(g_patched_at);
        }
    }
    g_status = rc;
    return rc;
}

int fsvql_unpatch(void) {
    struct qmap m;
    int ring = g_ring;

    /* Site C first: its thunk forwards to the real SendPendingPredictableEvents,
     * so leaving it pointing into a library nobody will call again would break the
     * game's own event delivery, not merely ours. */
    g_ring = 0;
    if (unpatch_c() == FSVQL_MPROTECT) {
        /* Site C still calls the thunk, and the thunk forwards through
         * fsvql_real_sendp. Restoring site A would clear that pointer under a
         * live call and crash the next ClientThink, so nothing changes. */
        g_ring = ring;
        return FSVQL_MPROTECT;
    }

    if (qagame_map(&m) == 0) return 0;
    if (site_a_ours(&m)) {
        return restore_at(g_patched_at);
    }
    return 0;
}

/* 1 only when both sites are ours in the current mapping. */
int fsvql_status(void) {
    struct qmap m;

    if (g_status == FSVQL_SELFTEST) return FSVQL_SELFTEST;
    if (qagame_map(&m) == 0) return FSVQL_NOT_MAPPED;
    if (site_a_ours(&m)) {
        return ring_live() ? FSVQL_OK : FSVQL_NO_RING;
    }
    if (g_status == FSVQL_MPROTECT || g_status == FSVQL_NO_RING || g_status == FSVQL_NO_GATE
        || g_status == FSVQL_NO_REACH) {
        return g_status;
    }
    return FSVQL_NOT_MAPPED;
}

uint64_t fsvql_surface_addr(void) { return (uint64_t)(uintptr_t)g_surface; }
uint64_t fsvql_site_c(void) { return (uint64_t)(uintptr_t)g_patched_c; }
uint64_t fsvql_sendp_addr(void) { return (uint64_t)(uintptr_t)fsvql_real_sendp; }
uint64_t fsvql_stub_pages(void) { return (uint64_t)g_nstub; }

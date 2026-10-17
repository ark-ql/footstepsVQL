/* ql_sndtap — instrumentation tap for the QL double-sound investigation.
 *
 * Two hooks, both located and verified in notes/phase1-addresses.md:
 *
 *   cgamex86.dll  + 0x19EB0   CG_EntityEvent(centity_t *cent, vec3_t pos)
 *   quakelive_steam.exe + 0xDA050
 *                             S_StartSound(vec3_t *origin, int entityNum,
 *                                          int entchannel, sfxHandle_t sfx)
 *
 * Purpose: give every event and every audible start a QPC timestamp and a
 * caller address. The retail cg_debugevents / s_show pair can already COUNT
 * these, but their clock (s_show paintedtime) only resolves 10 ms and is not
 * monotonic, so it cannot tell a 2 ms duplicate from a 20 ms one. That
 * distinction is what separates the competing hypotheses, hence this DLL.
 *
 * Also implements the F2 ledger: remember recently played (entity, event)
 * pairs in a wide client-local ring and optionally suppress a repeat. The
 * point of MAX_PS_EVENTS being 2 is that the client only has two slots of
 * memory; this gives it as many as we like without touching any networked
 * struct.
 *
 * Build: native/build.ps1 (i686 MinGW, -Wall -Wextra -Werror).
 * Nothing is written to the game directory.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "ql_sndtap_ipc.h"
#include "ql_sndtap_version.h"

/* ------------------------------------------------------------------ */
/* Target description                                                  */
/* ------------------------------------------------------------------ */

#define CGAME_MODULE   "cgamex86.dll"
#define CG_ENTITYEVENT_RVA 0x19EB0u
#define CG_STEAL_BYTES 6   /* 81 ec 40 04 00 00  sub esp, 0x440 */

#define S_STARTSOUND_RVA 0xDA050u
#define SND_STEAL_BYTES 6  /* 55 / 8b ec / 83 ec 08 */

/* QL's S_StartSound takes FIVE arguments, not Q3's four: the legacy 4-arg
 * entry at 0x4DA350 does `fld1` and forwards here with volume = 1.0. Every
 * cgame sound arrives through that shim, which is why the first captures
 * all showed ret=+0xda36e (the shim's call site), not S_StartLocalSound. */
#define SND_ARGS 5

/* Looping sounds never touch S_StartSound. Without this hook the tap is
 * blind to the rocket flight loop, which is a live suspect for the
 * "distortion" heard on a ground rocket jump. */
#define S_ADDLOOPINGSOUND_RVA 0xDA4C0u
#define LOOP_STEAL_BYTES 6 /* 55 / 8b ec / 83 ec 20 */

/* Signatures with 0xFF-masked wildcards over the embedded absolute
 * addresses. cgame is rebased at load and the engine can be moved by ASLR,
 * so the raw bytes must never be matched directly. */
static const unsigned char SIG_CG_ENTITYEVENT[] = {
    0x81, 0xEC, 0x40, 0x04, 0x00, 0x00,       /* sub esp, 0x440           */
    0xA1, 0x00, 0x00, 0x00, 0x00,             /* mov eax, [__security_ck] */
    0x33, 0xC4,                               /* xor eax, esp             */
    0x89, 0x84, 0x24, 0x3C, 0x04, 0x00, 0x00  /* mov [esp+0x43c], eax     */
};
static const char MASK_CG_ENTITYEVENT[] = "xxxxxx" "x????" "xx" "xxxxxxx";

static const unsigned char SIG_S_STARTSOUND[] = {
    0x55, 0x8B, 0xEC,                         /* push ebp; mov ebp, esp   */
    0x83, 0xEC, 0x08,                         /* sub esp, 8               */
    0x83, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00, /* cmp [s_soundStarted], 0  */
    0x0F, 0x84                                /* je ...                   */
};
static const char MASK_S_STARTSOUND[] = "xxx" "xxx" "xx????x" "xx";

static const unsigned char SIG_S_ADDLOOPINGSOUND[] = {
    0x55, 0x8B, 0xEC,                         /* push ebp; mov ebp, esp   */
    0x83, 0xEC, 0x20,                         /* sub esp, 0x20            */
    0x83, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00, /* cmp [s_soundStarted], 0  */
    0x0F, 0x84                                /* je ...                   */
};
static const char MASK_S_ADDLOOPINGSOUND[] = "xxx" "xxx" "xx????x" "xx";

/* Engine globals, measured. Offsets from the engine module base. */
#define RVA_S_KNOWNSFX 0xE60950u  /* 0x01260950 - 0x00400000 */
#define SFX_T_STRIDE   0x58u

/* Snapshot playerState_t, cgame RVA. Written by the rep movsd of 0x94
 * dwords from snap+0x2C at cgame 0x44719, per independed-render's HANDOFF.
 * MEASURED offsets there: commandTime +0x00, pm_type +0x04, bobCycle +0x08,
 * pm_flags +0x0C, origin +0x14, clientNum +0x88, weapon +0x90,
 * viewangles +0xA0, viewheight +0xAC, PERS_HITS +0x104.
 *
 * Note clientNum is +0x88 and viewangles +0xA0, where Q3 has +0x8C and
 * +0x98. QL's playerState_t is NOT Q3's, so eventSequence / events[] /
 * eventParms[] / externalEvent cannot be taken from Q3 headers. Finding
 * them is what QL_TAP_PS_SCAN is for. */
#define CG_SNAP_PS_RVA 0xA9C210u
#define PS_DWORDS 0x94u

/* entityState_t, measured. Prefixed because winuser.h already defines
 * ES_NUMBER as an edit-control style. */
#define QL_ES_NUMBER 0x00
#define QL_ES_ETYPE  0x04
#define QL_ES_EVENT  0xBC
#define QL_EV_EVENT_BITS 0x300

/* ------------------------------------------------------------------ */
/* Logging: buffered, writer thread, never blocks the game thread      */
/* ------------------------------------------------------------------ */

#define LOG_BUF_BYTES 0x40000u  /* 256 KB per buffer, two of them */

static CRITICAL_SECTION g_log_cs;
static char *g_buf[2];
static size_t g_used[2];
static int g_active;
static HANDLE g_writer_thread;
static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static volatile LONG g_stop;
static volatile LONG g_dropped;
static volatile LONG g_errors;
static double g_qpc_to_us;
static LARGE_INTEGER g_qpc_base;

static QlSndTapShared *g_ipc;
static HANDLE g_ipc_map;

static double now_us(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - g_qpc_base.QuadPart) * g_qpc_to_us;
}

static void log_line(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(tmp) - 2) {
        n = (int)(sizeof(tmp) - 2);
    }
    tmp[n++] = '\n';

    /* Drop rather than stall. At 1000 fps a blocking write would change the
     * frame timing we are trying to measure. */
    if (!TryEnterCriticalSection(&g_log_cs)) {
        InterlockedIncrement(&g_dropped);
        return;
    }
    if (g_used[g_active] + (size_t)n <= LOG_BUF_BYTES) {
        memcpy(g_buf[g_active] + g_used[g_active], tmp, (size_t)n);
        g_used[g_active] += (size_t)n;
    } else {
        InterlockedIncrement(&g_dropped);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void flush_once(void)
{
    int idx;
    size_t len;
    DWORD written;

    EnterCriticalSection(&g_log_cs);
    idx = g_active;
    len = g_used[idx];
    if (len == 0) {
        LeaveCriticalSection(&g_log_cs);
        return;
    }
    g_active ^= 1;
    g_used[g_active] = 0;
    LeaveCriticalSection(&g_log_cs);

    if (g_log_file != INVALID_HANDLE_VALUE) {
        if (!WriteFile(g_log_file, g_buf[idx], (DWORD)len, &written, NULL)) {
            InterlockedIncrement(&g_errors);
        }
    }
    g_used[idx] = 0;
}

static DWORD WINAPI writer_main(LPVOID unused)
{
    (void)unused;
    while (!g_stop) {
        Sleep(100);
        flush_once();
        if (g_ipc) {
            g_ipc->log_dropped = g_dropped;
            g_ipc->log_errors = g_errors;
        }
    }
    flush_once();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ledger (fix F2)                                                     */
/* ------------------------------------------------------------------ */

#define LEDGER_SLOTS 256

typedef struct {
    int ent;
    int ev;
    double t_us;
} LedgerEntry;

static LedgerEntry g_ledger[LEDGER_SLOTS];
static unsigned g_ledger_head;
static volatile LONG g_suppressed;
static volatile LONG g_would_suppress;
static volatile LONG g_spared_by_batch;
static volatile LONG g_spared_by_caller;

/* Time of the previous CG_EntityEvent call, whatever it was. A replay is
 * emitted in the same ring drain as the event that displaced it, so it
 * lands microseconds behind its predecessor; a legitimate repeat does not.
 * MEASURED on our test server 2026-09-19: replays at 9-10 us, closest legitimate
 * event in the same session 990 us. See notes/phase2-replay-confirmed.md. */
static double g_prev_evt_us;

/* Returns 1 if this (ent, ev) was already played inside the window. */
static int ledger_is_replay(int ent, int ev, double t_us, double window_us)
{
    unsigned i;
    for (i = 0; i < LEDGER_SLOTS; ++i) {
        const LedgerEntry *e = &g_ledger[i];
        if (e->t_us == 0.0) {
            continue;
        }
        if (e->ent == ent && e->ev == ev && (t_us - e->t_us) <= window_us) {
            return 1;
        }
    }
    return 0;
}

static void ledger_record(int ent, int ev, double t_us)
{
    LedgerEntry *e = &g_ledger[g_ledger_head % LEDGER_SLOTS];
    e->ent = ent;
    e->ev = ev;
    e->t_us = t_us;
    g_ledger_head++;
}

/* ------------------------------------------------------------------ */
/* sfxHandle -> name                                                   */
/* ------------------------------------------------------------------ */

static unsigned char *g_knownsfx;
static int g_sfx_name_off = -1;   /* discovered, not assumed */

/* QL's sfx_t is 0x58 bytes, which does not match Q3's layout, so the name
 * offset is found empirically: the first NUL-terminated ASCII run inside the
 * struct that looks like a sound path. */
static int discover_sfx_name_offset(const unsigned char *rec)
{
    int off;
    for (off = 0; off < (int)SFX_T_STRIDE - 8; off += 4) {
        const char *s = (const char *)(rec + off);
        int i;
        for (i = 0; i < 48; ++i) {
            char c = s[i];
            if (c == '\0') {
                break;
            }
            if (c < 0x20 || c > 0x7E) {
                i = -1;
                break;
            }
        }
        if (i >= 5 && (strchr(s, '/') != NULL || strstr(s, ".wav") ||
                       strstr(s, ".ogg"))) {
            return off;
        }
    }
    return -1;
}

static const char *sfx_name(int handle)
{
    unsigned char *rec;
    if (!g_knownsfx || handle < 0 || handle > 4096) {
        return "?";
    }
    rec = g_knownsfx + (size_t)handle * SFX_T_STRIDE;
    if (IsBadReadPtr(rec, SFX_T_STRIDE)) {
        return "?";
    }
    if (g_sfx_name_off < 0) {
        g_sfx_name_off = discover_sfx_name_offset(rec);
        if (g_sfx_name_off >= 0) {
            log_line("info sfx_name_offset=0x%x", g_sfx_name_off);
        } else {
            return "?";
        }
    }
    return (const char *)(rec + g_sfx_name_off);
}

/* ------------------------------------------------------------------ */
/* Module bases, filled in at hook install                             */
/* ------------------------------------------------------------------ */

static uintptr_t g_cgame_base;
static uintptr_t g_cgame_size;
static uintptr_t g_engine_base;
static uintptr_t g_engine_size;

/* ------------------------------------------------------------------ */
/* playerState scanner — locate eventSequence and events[]             */
/* ------------------------------------------------------------------ */

/* The server-side fix needs the offsets of eventSequence, events[] and
 * externalEvent inside playerState_t, and QL's layout is not Q3's. Rather
 * than guess, watch the snapshot playerState across real events:
 *
 *   - eventSequence is a dword that steps by a small positive amount and
 *     never goes backwards.
 *   - events[i] holds the event id we are about to play.
 *
 * playerState_t contains no pointers, so the layout is identical on the
 * 64-bit server; offsets found here transfer to qagamex64.so directly.
 */
static unsigned g_ps_prev[PS_DWORDS];
static int g_ps_have_prev;
static unsigned g_ps_scan_n;

static void ps_scan(int ev_about_to_play)
{
    const unsigned *ps;
    unsigned i;
    char buf[400];
    int n = 0;

    if (!g_cgame_base) {
        return;
    }
    ps = (const unsigned *)(g_cgame_base + CG_SNAP_PS_RVA);
    if (IsBadReadPtr(ps, PS_DWORDS * 4)) {
        return;
    }
    if (!g_ps_have_prev) {
        memcpy(g_ps_prev, ps, sizeof(g_ps_prev));
        g_ps_have_prev = 1;
        return;
    }
    /* Report every dword holding this event id — events[] candidates. */
    n = _snprintf(buf, sizeof(buf), "psscan ev=%d hits", ev_about_to_play);
    for (i = 0; i < PS_DWORDS && n > 0 && n < (int)sizeof(buf) - 24; ++i) {
        if (ps[i] == (unsigned)ev_about_to_play && i > 3) {
            n += _snprintf(buf + n, sizeof(buf) - n, " +0x%x", i * 4);
        }
    }
    log_line("%s", buf);

    /* And every dword that changed since the previous event, with old and
     * new values. eventSequence shows up as a small monotonic step. */
    n = _snprintf(buf, sizeof(buf), "psdiff ev=%d", ev_about_to_play);
    for (i = 0; i < PS_DWORDS && n > 0 && n < (int)sizeof(buf) - 40; ++i) {
        if (ps[i] != g_ps_prev[i]) {
            int delta = (int)ps[i] - (int)g_ps_prev[i];
            if (delta > 0 && delta <= 4) {
                n += _snprintf(buf + n, sizeof(buf) - n, " +0x%x:%u->%u(+%d)",
                               i * 4, g_ps_prev[i], ps[i], delta);
            }
        }
    }
    log_line("%s", buf);

    /* The event-ring span verbatim. eventSequence 0x6C and events[] 0x70/74
     * are measured; eventParms and externalEvent are only inferred from
     * clientNum sitting at 0x88, and externalEvent jumps to an arbitrary
     * event id rather than stepping, so the delta filter above cannot see
     * it. Dumping the span is what turns those into measurements. */
    log_line("psring ev=%d  seq6c=%u ev70=%u ev74=%u parm78=%u parm7c=%u"
             " x80=%u x84=%u cn88=%u",
             ev_about_to_play, ps[0x6C / 4], ps[0x70 / 4], ps[0x74 / 4],
             ps[0x78 / 4], ps[0x7C / 4], ps[0x80 / 4], ps[0x84 / 4],
             ps[0x88 / 4]);
    memcpy(g_ps_prev, ps, sizeof(g_ps_prev));
    g_ps_scan_n++;
}

/* ------------------------------------------------------------------ */
/* Hook callbacks (plain cdecl, called from the generated stubs)       */
/* ------------------------------------------------------------------ */

static uintptr_t image_size(HMODULE m)
{
    const unsigned char *p = (const unsigned char *)m;
    LONG e_lfanew;
    if (!m) {
        return 0;
    }
    e_lfanew = *(const LONG *)(p + 0x3C);
    return *(const uintptr_t *)(p + e_lfanew + 0x50); /* SizeOfImage */
}

/* Return addresses come from two different modules — the sound hooks are
 * called from cgame as well as the engine — so a single base cannot be
 * assumed. Tag the module rather than emit a nonsense RVA. */
static void fmt_ret(char *out, size_t n, const void *ret)
{
    uintptr_t a = (uintptr_t)ret;
    if (g_engine_base && a >= g_engine_base && a < g_engine_base + g_engine_size) {
        _snprintf(out, n, "e+0x%x", (unsigned)(a - g_engine_base));
    } else if (g_cgame_base && a >= g_cgame_base &&
               a < g_cgame_base + g_cgame_size) {
        _snprintf(out, n, "c+0x%x", (unsigned)(a - g_cgame_base));
    } else {
        _snprintf(out, n, "?0x%p", ret);
    }
    out[n - 1] = '\0';
}

/* NOTE on parameter order: the generated stub pushes with a constant
 * displacement, which lands the saved return address in the FIRST slot and
 * the original arguments after it. Keep ret_addr first in both callbacks. */

/* Returns non-zero to suppress the original CG_EntityEvent call. */
int __cdecl ql_on_entity_event(void *ret_addr, void *cent)
{
    int ent, ev, etype, raw;
    double t, dprev;
    int suppress = 0;

    if (!cent || IsBadReadPtr(cent, QL_ES_EVENT + 4)) {
        return 0;
    }
    raw = *(int *)((char *)cent + QL_ES_EVENT);
    ev = raw & ~QL_EV_EVENT_BITS;
    ent = *(int *)((char *)cent + QL_ES_NUMBER);
    etype = *(int *)((char *)cent + QL_ES_ETYPE);
    t = now_us();
    /* Snapshot the gap before g_prev_evt_us is advanced below. */
    dprev = g_prev_evt_us > 0.0 ? (t - g_prev_evt_us) / 1000.0 : -1.0;

    if (g_ipc) {
        InterlockedIncrement(&g_ipc->evt_count);
    }

    if (g_ipc && (g_ipc->knob[QL_TAP_LEDGER] ||
                  g_ipc->knob[QL_TAP_LEDGER_DRYRUN])) {
        int candidate =
            ledger_is_replay(ent, ev, t, (double)g_ipc->ledger_window_us);
        /* The batch test is what makes this safe. Without it a 40 ms window
         * sits uncomfortably close to LG's 50 ms cadence and would
         * eventually eat a real shot. */
        int in_batch = (g_prev_evt_us > 0.0) &&
                       ((t - g_prev_evt_us) <= (double)g_ipc->ledger_batch_us);
        /* Only the predictable-playerState path can produce this bug. */
        unsigned caller = (unsigned)((uintptr_t)ret_addr - g_cgame_base);
        int right_caller = (g_ipc->ledger_caller_rva == 0) ||
                           (caller == (unsigned)g_ipc->ledger_caller_rva);

        if (candidate && in_batch && !right_caller) {
            InterlockedIncrement(&g_spared_by_caller);
            g_ipc->ledger_spared_by_caller = g_spared_by_caller;
        } else if (candidate && !in_batch) {
            InterlockedIncrement(&g_spared_by_batch);
            g_ipc->ledger_spared_by_batch = g_spared_by_batch;
        } else if (candidate) {
            if (g_ipc->knob[QL_TAP_LEDGER_DRYRUN]) {
                InterlockedIncrement(&g_would_suppress);
                g_ipc->ledger_would_suppress = g_would_suppress;
            }
            if (g_ipc->knob[QL_TAP_LEDGER]) {
                suppress = 1;
                InterlockedIncrement(&g_suppressed);
                g_ipc->ledger_suppressed = g_suppressed;
            }
        }
    }
    ledger_record(ent, ev, t);
    g_prev_evt_us = t;

    /* Only the local player's predictable events tell us anything about
     * the playerState event ring. */
    if (g_ipc && g_ipc->knob[QL_TAP_PS_SCAN] && ent == 0 &&
        (unsigned)((uintptr_t)ret_addr - g_cgame_base) == 0x43545u) {
        ps_scan(ev);
    }

    if (!g_ipc || g_ipc->knob[QL_TAP_EVT_LOG]) {
        char rs[32];
        fmt_ret(rs, sizeof(rs), ret_addr);
        log_line("evt t=%.3f ent=%d ev=%d bits=0x%x etype=%d ret=%s"
                 " dprev=%.3f sup=%d",
                 t, ent, ev, (unsigned)(raw & QL_EV_EVENT_BITS), etype,
                 rs, dprev, suppress);
    }
    return suppress;
}

/* vol_bits is the 5th argument, a float taken as raw bits so no float ABI
 * assumption is made about how the stub handed it over. Relevant because
 * "distortion" on overlapping sounds is consistent with mix saturation, and
 * the amplitude of each contributor is the first thing to check. */
int __cdecl ql_on_start_sound(void *ret_addr, void *origin, int entity_num,
                              int entchannel, int sfx_handle,
                              unsigned vol_bits)
{
    double t = now_us();
    const float *o = (const float *)origin;
    float vol;
    char org[64];
    char rs[32];

    memcpy(&vol, &vol_bits, sizeof(vol));
    if (g_ipc) {
        InterlockedIncrement(&g_ipc->snd_count);
    }
    if (!g_ipc || g_ipc->knob[QL_TAP_SND_LOG]) {
        if (o && !IsBadReadPtr(o, 12)) {
            _snprintf(org, sizeof(org), "%.1f,%.1f,%.1f", o[0], o[1], o[2]);
        } else {
            _snprintf(org, sizeof(org), "null");
        }
        org[sizeof(org) - 1] = '\0';
        fmt_ret(rs, sizeof(rs), ret_addr);
        log_line("snd t=%.3f ent=%d chan=%d sfx=%d vol=%.3f org=%s name=%s"
                 " ret=%s",
                 t, entity_num, entchannel, sfx_handle, vol, org,
                 sfx_name(sfx_handle), rs);
    }
    return 0; /* never suppress a sound from here */
}

/* ---- looping sounds -------------------------------------------------- */

/* S_AddLoopingSound is called once per frame per active loop, so at 1000 fps
 * logging every call would be thousands of lines a second and would itself
 * change the timing. Only transitions are logged: a pair that has been
 * quiet for LOOP_GAP_US and reappears counts as a start. That is exactly
 * the churn signal — a rocket loop starting and dying within a frame or
 * two — without the flood. */
#define LOOP_SLOTS 64
#define LOOP_GAP_US 100000.0 /* 100 ms of silence ends a loop */

typedef struct {
    int ent;
    int sfx;
    double last_us;
} LoopSeen;

static LoopSeen g_loops[LOOP_SLOTS];
static volatile LONG g_loop_starts;

int __cdecl ql_on_add_looping(void *ret_addr, int entity_num, void *origin,
                              void *velocity, int sfx_handle)
{
    double t = now_us();
    int i, free_slot = -1;
    int is_start = 1;
    (void)origin;
    (void)velocity;

    if (g_ipc) {
        InterlockedIncrement(&g_ipc->loop_calls);
    }
    for (i = 0; i < LOOP_SLOTS; ++i) {
        if (g_loops[i].last_us == 0.0) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (g_loops[i].ent == entity_num && g_loops[i].sfx == sfx_handle) {
            if ((t - g_loops[i].last_us) <= LOOP_GAP_US) {
                is_start = 0;
            }
            g_loops[i].last_us = t;
            break;
        }
        if ((t - g_loops[i].last_us) > LOOP_GAP_US) {
            g_loops[i].last_us = 0.0; /* expire */
            if (free_slot < 0) {
                free_slot = i;
            }
        }
    }
    if (i == LOOP_SLOTS && free_slot >= 0) {
        g_loops[free_slot].ent = entity_num;
        g_loops[free_slot].sfx = sfx_handle;
        g_loops[free_slot].last_us = t;
    }
    if (is_start) {
        InterlockedIncrement(&g_loop_starts);
        if (g_ipc) {
            g_ipc->loop_starts = g_loop_starts;
        }
        if (!g_ipc || g_ipc->knob[QL_TAP_LOOP_LOG]) {
            char rs[32];
            fmt_ret(rs, sizeof(rs), ret_addr);
            log_line("loop t=%.3f ent=%d sfx=%d name=%s ret=%s START",
                     t, entity_num, sfx_handle, sfx_name(sfx_handle), rs);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Trampoline / stub construction                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *target;
    unsigned char *tramp;   /* original bytes + jmp back */
    unsigned char *stub;    /* pushad/call/popad/jmp     */
    unsigned char saved[16];
    size_t stolen;
    int installed;
} Hook;

static int sig_match(const unsigned char *at, const unsigned char *sig,
                     const char *mask, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (mask[i] == 'x' && at[i] != sig[i]) {
            return 0;
        }
    }
    return 1;
}

static void emit(unsigned char **p, const void *bytes, size_t n)
{
    memcpy(*p, bytes, n);
    *p += n;
}

static void emit_u8(unsigned char **p, unsigned char v) { *(*p)++ = v; }

static void emit_rel32(unsigned char **p, const void *to)
{
    int32_t rel = (int32_t)((uintptr_t)to - ((uintptr_t)*p + 4));
    memcpy(*p, &rel, 4);
    *p += 4;
}

/* Build: pushad; pushfd; push args; call cb; add esp,n; test eax,eax;
 * jnz skip; popfd; popad; jmp tramp;  skip: popfd; popad; ret [imm] */
static unsigned char *build_stub(void *cb, int nargs, size_t stolen,
                                 unsigned char *tramp, int allow_skip)
{
    unsigned char *stub = (unsigned char *)VirtualAlloc(
        NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char *p = stub;
    unsigned char *jnz_patch;
    int i;
    (void)stolen;

    if (!stub) {
        return NULL;
    }
    emit_u8(&p, 0x60);                    /* pushad */
    emit_u8(&p, 0x9C);                    /* pushfd */

    /* After pushad+pushfd esp dropped 36. Original [esp+0] = retaddr,
     * [esp+4..] = args. Pushing right-to-left keeps the displacement
     * constant at 36 + 4*nargs for every push. */
    for (i = 0; i < nargs + 1; ++i) {
        unsigned char disp = (unsigned char)(36 + 4 * nargs);
        emit_u8(&p, 0xFF);                /* push dword [esp+disp] */
        emit_u8(&p, 0x74);
        emit_u8(&p, 0x24);
        emit_u8(&p, disp);
    }
    emit_u8(&p, 0xE8);                    /* call cb */
    emit_rel32(&p, cb);
    emit_u8(&p, 0x83);                    /* add esp, 4*(nargs+1) */
    emit_u8(&p, 0xC4);
    emit_u8(&p, (unsigned char)(4 * (nargs + 1)));

    jnz_patch = NULL;
    if (allow_skip) {
        emit_u8(&p, 0x85);                /* test eax, eax */
        emit_u8(&p, 0xC0);
        emit_u8(&p, 0x75);                /* jnz short -> patched below */
        jnz_patch = p;
        emit_u8(&p, 0x00);
    }
    emit_u8(&p, 0x9D);                    /* popfd */
    emit_u8(&p, 0x61);                    /* popad */
    emit_u8(&p, 0xE9);                    /* jmp tramp */
    emit_rel32(&p, tramp);

    if (allow_skip) {
        *jnz_patch = (unsigned char)(p - (jnz_patch + 1));
        emit_u8(&p, 0x9D);                /* popfd */
        emit_u8(&p, 0x61);                /* popad */
        emit_u8(&p, 0xC3);                /* ret (cdecl: caller cleans) */
    }
    return stub;
}

static int hook_install(Hook *h, unsigned char *target, size_t stolen,
                        void *cb, int nargs, int allow_skip)
{
    DWORD old;
    unsigned char *p;

    if (h->installed) {
        return 1;
    }
    memset(h, 0, sizeof(*h));
    h->target = target;
    h->stolen = stolen;
    memcpy(h->saved, target, stolen);

    h->tramp = (unsigned char *)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!h->tramp) {
        return 0;
    }
    p = h->tramp;
    emit(&p, target, stolen);
    emit_u8(&p, 0xE9);
    emit_rel32(&p, target + stolen);

    h->stub = build_stub(cb, nargs, stolen, h->tramp, allow_skip);
    if (!h->stub) {
        return 0;
    }

    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    p = target;
    emit_u8(&p, 0xE9);
    emit_rel32(&p, h->stub);
    while ((size_t)(p - target) < stolen) {
        emit_u8(&p, 0x90);
    }
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    h->installed = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Install / module tracking                                           */
/* ------------------------------------------------------------------ */

static Hook g_hook_cg;
static Hook g_hook_snd;
static Hook g_hook_loop;

static int install_engine(void)
{
    HMODULE m = GetModuleHandleW(NULL);
    unsigned char *at;

    if (!m) {
        return 0;
    }
    g_engine_base = (uintptr_t)m;
    g_engine_size = image_size(m);
    at = (unsigned char *)m + S_STARTSOUND_RVA;
    if (!sig_match(at, SIG_S_STARTSOUND, MASK_S_STARTSOUND,
                   sizeof(SIG_S_STARTSOUND))) {
        log_line("FAIL engine signature mismatch at +0x%x", S_STARTSOUND_RVA);
        return 0;
    }
    g_knownsfx = (unsigned char *)m + RVA_S_KNOWNSFX;
    if (!hook_install(&g_hook_snd, at, SND_STEAL_BYTES,
                      (void *)ql_on_start_sound, SND_ARGS, 0)) {
        log_line("FAIL engine hook install");
        return 0;
    }
    log_line("ok engine base=0x%p S_StartSound=+0x%x knownsfx=0x%p",
             (void *)m, S_STARTSOUND_RVA, (void *)g_knownsfx);

    at = (unsigned char *)m + S_ADDLOOPINGSOUND_RVA;
    if (!sig_match(at, SIG_S_ADDLOOPINGSOUND, MASK_S_ADDLOOPINGSOUND,
                   sizeof(SIG_S_ADDLOOPINGSOUND))) {
        log_line("WARN looping signature mismatch at +0x%x, loops not tapped",
                 S_ADDLOOPINGSOUND_RVA);
    } else if (!hook_install(&g_hook_loop, at, LOOP_STEAL_BYTES,
                             (void *)ql_on_add_looping, 4, 0)) {
        log_line("WARN looping hook install failed");
    } else {
        log_line("ok S_AddLoopingSound=+0x%x", S_ADDLOOPINGSOUND_RVA);
    }

    if (g_ipc) {
        g_ipc->engine_hooked = 1;
    }
    return 1;
}

static int install_cgame(HMODULE m)
{
    unsigned char *at = (unsigned char *)m + CG_ENTITYEVENT_RVA;

    if (!sig_match(at, SIG_CG_ENTITYEVENT, MASK_CG_ENTITYEVENT,
                   sizeof(SIG_CG_ENTITYEVENT))) {
        log_line("FAIL cgame signature mismatch at base=0x%p +0x%x",
                 (void *)m, CG_ENTITYEVENT_RVA);
        return 0;
    }
    g_cgame_base = (uintptr_t)m;
    g_cgame_size = image_size(m);
    g_hook_cg.installed = 0;
    if (!hook_install(&g_hook_cg, at, CG_STEAL_BYTES,
                      (void *)ql_on_entity_event, 1, 1)) {
        log_line("FAIL cgame hook install");
        return 0;
    }
    log_line("ok cgame base=0x%p CG_EntityEvent=+0x%x",
             (void *)m, CG_ENTITYEVENT_RVA);
    if (g_ipc) {
        g_ipc->cgame_hooked = 1;
    }
    return 1;
}

/* cgame is unloaded and reloaded on every map change and vid_restart. A tap
 * that does not re-arm silently stops logging mid-session, which produces a
 * clean-looking but wrong dataset. Poll rather than trust one install. */
static DWORD WINAPI module_watch(LPVOID unused)
{
    HMODULE last = NULL;
    (void)unused;
    while (!g_stop) {
        HMODULE m = GetModuleHandleA(CGAME_MODULE);
        if (m != last) {
            if (m) {
                if (install_cgame(m)) {
                    if (last && g_ipc) {
                        g_ipc->cgame_reloads++;
                    }
                }
            } else {
                g_hook_cg.installed = 0;
                if (g_ipc) {
                    g_ipc->cgame_hooked = 0;
                }
                log_line("info cgame unloaded");
            }
            last = m;
        }
        Sleep(250);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bootstrap                                                           */
/* ------------------------------------------------------------------ */

static void open_log(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    SYSTEMTIME st;
    HMODULE self = NULL;

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)open_log, &self);
    GetModuleFileNameW(self, dir, MAX_PATH);
    {
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) {
            *slash = 0;
        }
    }
    GetLocalTime(&st);
    _snwprintf(path, MAX_PATH, L"%ls\\ql_sndtap_%04d%02d%02d-%02d%02d%02d.log",
               dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    g_log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void open_ipc(void)
{
    wchar_t name[128];
    _snwprintf(name, 128, L"%ls%lu", QL_SNDTAP_PREFIX,
               (unsigned long)GetCurrentProcessId());
    g_ipc_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                   0, sizeof(QlSndTapShared), name);
    if (!g_ipc_map) {
        return;
    }
    g_ipc = (QlSndTapShared *)MapViewOfFile(g_ipc_map, FILE_MAP_ALL_ACCESS,
                                            0, 0, sizeof(QlSndTapShared));
    if (!g_ipc) {
        return;
    }
    memset(g_ipc, 0, sizeof(*g_ipc));
    g_ipc->magic = QL_SNDTAP_MAGIC;
    g_ipc->version = QL_SNDTAP_VERSION;
    g_ipc->struct_bytes = sizeof(QlSndTapShared);
    g_ipc->game_pid = (LONG)GetCurrentProcessId();
    g_ipc->knob[QL_TAP_EVT_LOG] = 1;
    g_ipc->knob[QL_TAP_SND_LOG] = 1;
    g_ipc->knob[QL_TAP_LOOP_LOG] = 1;
    g_ipc->knob[QL_TAP_LEDGER] = 0;
    g_ipc->knob[QL_TAP_LEDGER_DRYRUN] = 0;
    /* Both MEASURED on our test server 2026-09-19, not guessed:
     * observed replays sat 8.3 and 22.1 ms behind their own first play, so
     * 40 ms covers them plus a 25 ms snapshot interval of jitter; and they
     * landed 9-10 us behind the previous event while the closest legitimate
     * event was 990 us, so 100 us is a 10x margin either way. */
    g_ipc->ledger_window_us = 40000;
    g_ipc->ledger_batch_us = 100;
    /* cgame RVA of the predictable-playerState delivery path, MEASURED.
     * The entity path is 0x4C204 and is deliberately excluded. */
    g_ipc->ledger_caller_rva = 0x43545;
    _snprintf(g_ipc->commit, sizeof(g_ipc->commit), "%s", QL_SNDTAP_VERSION_STR);
    g_ipc->ready = 1;
}

__declspec(dllexport) void QLSndTapInitialize(void)
{
    LARGE_INTEGER freq;
    static LONG once = 0;

    if (InterlockedExchange(&once, 1)) {
        return;
    }
    QueryPerformanceFrequency(&freq);
    g_qpc_to_us = 1000000.0 / (double)freq.QuadPart;
    QueryPerformanceCounter(&g_qpc_base);

    InitializeCriticalSection(&g_log_cs);
    g_buf[0] = (char *)VirtualAlloc(NULL, LOG_BUF_BYTES, MEM_COMMIT,
                                    PAGE_READWRITE);
    g_buf[1] = (char *)VirtualAlloc(NULL, LOG_BUF_BYTES, MEM_COMMIT,
                                    PAGE_READWRITE);
    if (!g_buf[0] || !g_buf[1]) {
        return;
    }
    open_log();
    open_ipc();
    log_line("ql_sndtap %s pid=%lu qpc_freq=%lld", QL_SNDTAP_VERSION_STR,
             (unsigned long)GetCurrentProcessId(), (long long)freq.QuadPart);

    g_writer_thread = CreateThread(NULL, 0, writer_main, NULL, 0, NULL);
    install_engine();
    CreateThread(NULL, 0, module_watch, NULL, 0, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

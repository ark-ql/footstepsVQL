/* Shared control page for ql_sndtap.
 *
 * Modelled on independed-render-ql-patch's ql_hood_ipc.h: one page of shared
 * memory, DLL writes status, controller writes knobs. Deliberately a separate
 * mapping name so the two projects can be loaded at the same time without
 * fighting over the hood page.
 */
#ifndef QL_SNDTAP_IPC_H
#define QL_SNDTAP_IPC_H

#include <windows.h>
#include <stdint.h>

#define QL_SNDTAP_MAGIC   0x50415453u /* "STAP" */
#define QL_SNDTAP_VERSION 5u          /* v5: playerState scanner */
#define QL_SNDTAP_PREFIX  L"Local\\ql_sndtap_"
#define QL_SNDTAP_TARGET  L"quakelive_steam.exe"

enum {
    QL_TAP_EVT_LOG = 0,   /* log every CG_EntityEvent call            */
    QL_TAP_SND_LOG,       /* log every S_StartSound call              */
    QL_TAP_LOOP_LOG,      /* log looping-sound starts (not every call) */
    QL_TAP_LEDGER,        /* F2: suppress a replayed event            */
    QL_TAP_LEDGER_DRYRUN, /* count what the ledger WOULD suppress     */
    QL_TAP_PS_SCAN,       /* locate eventSequence/events[] in playerState */
    QL_TAP_KNOB_COUNT
};

typedef struct QlSndTapShared {
    uint32_t magic;
    uint32_t version;
    uint32_t struct_bytes;
    volatile LONG ready;

    /* status: DLL writes, controller reads */
    volatile LONG game_pid;
    volatile LONG engine_hooked;
    volatile LONG cgame_hooked;
    volatile LONG cgame_reloads;
    volatile LONG evt_count;
    volatile LONG snd_count;
    volatile LONG loop_calls;   /* every S_AddLoopingSound call, per frame */
    volatile LONG loop_starts;  /* transitions only: a loop that went quiet */
    volatile LONG ledger_suppressed;
    volatile LONG ledger_would_suppress;
    /* Repeats inside the time window that the batch test spared. These are
     * the legitimate fast repeats — LG at 50 ms — that a naive window-only
     * ledger would have eaten. Watch this: it should be > 0 while firing. */
    volatile LONG ledger_spared_by_batch;
    /* Repeats rejected purely because they came from the entity path. Should
     * stay 0; if it moves, the caller gate is doing real work and the
     * assumption that entity events never replay needs revisiting. */
    volatile LONG ledger_spared_by_caller;
    volatile LONG log_dropped;
    volatile LONG log_errors;
    char commit[16];
    char note[96];

    /* knobs: controller writes, DLL reads */
    volatile LONG knob[QL_TAP_KNOB_COUNT];
    /* Ledger window in microseconds. An event repeating inside this window
     * with the same (entity, masked event id) is a replay CANDIDATE. */
    volatile LONG ledger_window_us;
    /* Second, decisive test. A replay is emitted in the same ring drain as
     * the event that displaced it, so it lands microseconds after the
     * previous event. MEASURED on our test server: replays 9-10 us, while the
     * closest legitimate event in the same session was 990 us. Only a
     * candidate that ALSO lands within this many microseconds of the
     * previous event is suppressed. */
    volatile LONG ledger_batch_us;
    /* Third gate: only suppress events delivered from this cgame call site.
     * The replay is by definition a predictable-playerState phenomenon —
     * eventSequence lives in playerState and only the local player has a
     * predicted one. Entity events reach CG_EntityEvent from a different
     * caller and cannot have this bug, so they must never be suppressed.
     * MEASURED: playerstate path = cgame RVA 0x43545, entity path 0x4C204.
     * Set to 0 to disable the check (not recommended). */
    volatile LONG ledger_caller_rva;
} QlSndTapShared;

#endif /* QL_SNDTAP_IPC_H */

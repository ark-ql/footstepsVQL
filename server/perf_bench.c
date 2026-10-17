/*
 * Server cost of footstepsVQL, per command and at load. Runs in its own process
 * against a private dlopen of qagamex64.so, like selftest_emit.py: the live
 * server is not touched, and no qagame code runs except PM_FootstepForSurface,
 * which reads one static.
 *
 *   gcc -O2 -o perf_bench perf_bench.c -ldl
 *   nice -n 19 ./perf_bench [qagamex64.so] [footstepsVQL_hook.so]
 *
 * Per command it runs what ClientThink_real does around the two sites: site A
 * (fsvql_bob_update), qagame's own PM_AddEvent on a vanilla flip, the entity
 * drain, site C before (fsvql_ring_pre) and after (fsvql_ring_undo). The same
 * loop without the hook calls is the baseline and is subtracted. The assembly
 * trampoline's register save and restore is timed separately as a replica,
 * because only the patched game can call the real one.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define I_SEQ (0x6c / 4)
#define I_EV (0x70 / 4)
#define I_EESEQ (0x1ec / 4)
#define ENT_CLIENT 0x240
#define ES_EVENT 0xBC
#define MAXP 32

typedef int (*bob_fn)(int *, float, int, const unsigned char *);
typedef void (*pre_fn)(int *);
typedef void (*undo_fn)(int *, void *);
typedef int (*int_fn)(void);
typedef uint64_t (*u64_fn)(void);

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int ps[MAXP][1024];
static unsigned char ent[MAXP][0x300];
static unsigned char pm[0x100];

static bob_fn bob;
static pre_fn pre;
static undo_fn undo;

static void reset(int players) {
    int i;
    memset(ps, 0, sizeof(ps));
    memset(ent, 0, sizeof(ent));
    for (i = 0; i < players; i++) {
        void *owner = ps[i];
        ps[i][0] = 100000;
        memcpy(ent[i] + ENT_CLIENT, &owner, sizeof(owner));
    }
}

/* One command for player i: hooked or not. Returns something to keep it live. */
static inline int think(int i, int msec, int hooked) {
    int *p = ps[i];
    int old, neu, seq, e;
    p[0] += msec;
    if (hooked) {
        old = bob(p, 0.4f * (float)msec, 1, pm);
    } else {
        old = p[2];
        p[2] = (old + (int)(0.4f * (float)msec)) & 255;
    }
    neu = p[2];
    if (((old + 64) ^ (neu + 64)) & 128) {          /* qagame's own PM_AddEvent */
        seq = p[I_SEQ];
        p[I_EV + (seq & 1)] = 1;
        p[I_SEQ] = seq + 1;
    }
    if (p[I_EESEQ] < p[I_SEQ]) {                    /* BG_PlayerStateToEntityState */
        e = p[I_EESEQ];
        *(int *)(ent[i] + ES_EVENT) = p[I_EV + (e & 1)] | ((e & 3) << 8);
        p[I_EESEQ] = e + 1;
    }
    if (hooked) pre(p);
    if (p[I_EESEQ] < p[I_SEQ]) p[I_EESEQ] += 1;     /* overflow temp entity */
    if (hooked) undo(p, ent[i]);
    return neu;
}

static double run(int players, int msec, int hooked, long cmds, int *sink) {
    long n;
    double t0;
    int acc = 0;
    reset(players);
    t0 = now_s();
    for (n = 0; n < cmds; n++) {
        acc += think((int)(n % players), msec, hooked);
    }
    *sink += acc;
    return (now_s() - t0) * 1e9 / (double)cmds;
}

/* The register save/restore shape of fsvql_tramp, around an empty body. */
static double tramp_replica(long n) {
    static unsigned char area[0x200] __attribute__((aligned(16)));
    long i;
    double t0 = now_s();
    for (i = 0; i < n; i++) {
        __asm__ volatile(
            "pushf\n pop %%rax\n mov %%rax, 0x148(%0)\n"
            "mov %%rcx, 0x108(%0)\n mov %%rdx, 0x110(%0)\n mov %%rsi, 0x118(%0)\n"
            "mov %%rdi, 0x120(%0)\n mov %%r8, 0x128(%0)\n mov %%r9, 0x130(%0)\n"
            "mov %%r10, 0x138(%0)\n mov %%r11, 0x140(%0)\n"
            "movups %%xmm0, 0x00(%0)\n movups %%xmm1, 0x10(%0)\n movups %%xmm2, 0x20(%0)\n"
            "movups %%xmm3, 0x30(%0)\n movups %%xmm4, 0x40(%0)\n movups %%xmm5, 0x50(%0)\n"
            "movups %%xmm6, 0x60(%0)\n movups %%xmm7, 0x70(%0)\n movups %%xmm8, 0x80(%0)\n"
            "movups %%xmm9, 0x90(%0)\n movups %%xmm10, 0xa0(%0)\n movups %%xmm11, 0xb0(%0)\n"
            "movups %%xmm12, 0xc0(%0)\n movups %%xmm13, 0xd0(%0)\n movups %%xmm14, 0xe0(%0)\n"
            "movups %%xmm15, 0xf0(%0)\n"
            "movups 0x00(%0), %%xmm0\n movups 0x10(%0), %%xmm1\n movups 0x20(%0), %%xmm2\n"
            "movups 0x30(%0), %%xmm3\n movups 0x40(%0), %%xmm4\n movups 0x50(%0), %%xmm5\n"
            "movups 0x60(%0), %%xmm6\n movups 0x70(%0), %%xmm7\n movups 0x80(%0), %%xmm8\n"
            "movups 0x90(%0), %%xmm9\n movups 0xa0(%0), %%xmm10\n movups 0xb0(%0), %%xmm11\n"
            "movups 0xc0(%0), %%xmm12\n movups 0xd0(%0), %%xmm13\n movups 0xe0(%0), %%xmm14\n"
            "movups 0xf0(%0), %%xmm15\n"
            "mov 0x108(%0), %%rcx\n mov 0x110(%0), %%rdx\n mov 0x118(%0), %%rsi\n"
            "mov 0x120(%0), %%rdi\n mov 0x128(%0), %%r8\n mov 0x130(%0), %%r9\n"
            "mov 0x138(%0), %%r10\n mov 0x140(%0), %%r11\n"
            "mov 0x148(%0), %%rax\n push %%rax\n popf\n"
            : : "r"(area)
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc",
              "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
              "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    }
    return (now_s() - t0) * 1e9 / (double)n;
}

int main(int argc, char **argv) {
    const char *qa = argc > 1 ? argv[1] : "./qagamex64.so";
    const char *hk = argc > 2 ? argv[2] : "./footstepsVQL_hook.so";
    static const int msecs[] = {1, 2, 3, 4};
    static const int players[] = {1, 8, 16, 32};
    const long cmds = 4000000L;
    void *hq, *hh;
    int_fn selftest, patch, unpatch;
    u64_fn written, drained, missed, skipped, suppressed;
    double t0, st_ms, patch_ms;
    int sink = 0, rc;
    size_t a, b;

    hq = dlopen(qa, RTLD_LAZY | RTLD_LOCAL);
    if (!hq) { fprintf(stderr, "dlopen qagame: %s\n", dlerror()); return 1; }
    hh = dlopen(hk, RTLD_NOW | RTLD_LOCAL);
    if (!hh) { fprintf(stderr, "dlopen hook: %s\n", dlerror()); return 1; }
    selftest = (int_fn)dlsym(hh, "fsvql_selftest");
    patch = (int_fn)dlsym(hh, "fsvql_patch");
    unpatch = (int_fn)dlsym(hh, "fsvql_unpatch");
    bob = (bob_fn)dlsym(hh, "fsvql_bob_update");
    pre = (pre_fn)dlsym(hh, "fsvql_ring_pre");
    undo = (undo_fn)dlsym(hh, "fsvql_ring_undo");
    written = (u64_fn)dlsym(hh, "fsvql_ring_written");
    drained = (u64_fn)dlsym(hh, "fsvql_ring_drained");
    missed = (u64_fn)dlsym(hh, "fsvql_ring_missed");
    skipped = (u64_fn)dlsym(hh, "fsvql_ring_skipped");
    suppressed = (u64_fn)dlsym(hh, "fsvql_ring_suppressed");
    if (!selftest || !patch || !unpatch || !bob || !pre || !undo || !written) {
        fprintf(stderr, "missing exports\n");
        return 1;
    }

    printf("== load time (game thread, once per library load)\n");
    t0 = now_s();
    rc = selftest();
    st_ms = (now_s() - t0) * 1e3;
    t0 = now_s();
    rc = patch();
    patch_ms = (now_s() - t0) * 1e3;
    printf("  fsvql_selftest()          %.2f ms (returned 0 = pass)\n", st_ms);
    printf("  fsvql_patch() first call  %.2f ms (selftest again + signature scan + install, rc %d)\n",
           patch_ms, rc);
    if (rc != 1) { fprintf(stderr, "patch did not install, ring paths would be skipped\n"); return 1; }

    printf("\n== trampoline register save/restore (replica)\n");
    tramp_replica(2000000L);
    printf("  %.1f ns per call\n", tramp_replica(20000000L));

    printf("\n== per command, hook minus the same loop without it (%ld commands per cell)\n", cmds);
    printf("%8s %8s %10s %10s %10s\n", "cmd_ms", "players", "base_ns", "hooked_ns", "hook_ns");
    run(1, 1, 1, 1000000L, &sink);
    for (b = 0; b < sizeof(players) / sizeof(players[0]); b++) {
        for (a = 0; a < sizeof(msecs) / sizeof(msecs[0]); a++) {
            double base = run(players[b], msecs[a], 0, cmds, &sink);
            double hooked = run(players[b], msecs[a], 1, cmds, &sink);
            printf("%8d %8d %10.1f %10.1f %10.1f\n", msecs[a], players[b], base, hooked, hooked - base);
        }
    }
    printf("\n  ring: written %llu drained %llu stripped %llu skipped %llu missed %llu\n",
           (unsigned long long)written(), (unsigned long long)drained(),
           (unsigned long long)(suppressed ? suppressed() : 0),
           (unsigned long long)(skipped ? skipped() : 0), (unsigned long long)(missed ? missed() : 0));
    printf("  unpatch rc %d; sink %d\n", unpatch(), sink);
    return 0;
}

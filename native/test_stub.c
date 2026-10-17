/* Offline self-test for the trampoline/stub machinery in ql_sndtap.c.
 *
 * The stub pushes arguments with a constant displacement, which is easy to
 * get wrong by one slot and produces a crash inside the game rather than a
 * clear error. This exercises it against a synthetic target before anything
 * is injected into quakelive_steam.exe.
 *
 * Includes the .c directly so the real emit/hook code is under test rather
 * than a copy of it.
 */

#include "ql_sndtap.c"

static int g_body_ran;
static void *g_seen_ret;
static int g_seen[4];
static int g_cb_calls;
static int g_cb_return;

static int __cdecl test_cb(void *ret_addr, int a, int b, int c, int d)
{
    g_cb_calls++;
    g_seen_ret = ret_addr;
    g_seen[0] = a;
    g_seen[1] = b;
    g_seen[2] = c;
    g_seen[3] = d;
    return g_cb_return;
}

typedef void(__cdecl *fake_fn)(int, int, int, int);

/* push ebp; mov ebp,esp; sub esp,8   <- 6 stealable bytes
 * inc [g_body_ran]; mov esp,ebp; pop ebp; ret */
static unsigned char *make_fake(void)
{
    unsigned char *f = (unsigned char *)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char *p = f;
    emit_u8(&p, 0x55);
    emit_u8(&p, 0x8B);
    emit_u8(&p, 0xEC);
    emit_u8(&p, 0x83);
    emit_u8(&p, 0xEC);
    emit_u8(&p, 0x08);
    emit_u8(&p, 0xFF);                       /* inc dword [g_body_ran] */
    emit_u8(&p, 0x05);
    emit(&p, &(void *){ &g_body_ran }, 4);
    emit_u8(&p, 0x8B);                       /* mov esp, ebp */
    emit_u8(&p, 0xE5);
    emit_u8(&p, 0x5D);                       /* pop ebp */
    emit_u8(&p, 0xC3);                       /* ret */
    return f;
}

static int fails;

static void check(const char *what, long long got, long long want)
{
    int ok = (got == want);
    if (!ok) {
        fails++;
    }
    printf("  [%s] %-34s got=%lld want=%lld\n", ok ? "ok" : "FAIL", what, got,
           want);
}

int main(void)
{
    unsigned char *fake;
    Hook h;
    int i;

    InitializeCriticalSection(&g_log_cs);
    g_buf[0] = (char *)VirtualAlloc(NULL, LOG_BUF_BYTES, MEM_COMMIT,
                                    PAGE_READWRITE);
    g_buf[1] = (char *)VirtualAlloc(NULL, LOG_BUF_BYTES, MEM_COMMIT,
                                    PAGE_READWRITE);

    printf("stub self-test\n");

    /* --- 4 args, no skip: the S_StartSound shape ------------------- */
    fake = make_fake();
    memset(&h, 0, sizeof(h));
    if (!hook_install(&h, fake, 6, (void *)test_cb, 4, 0)) {
        printf("  [FAIL] hook_install\n");
        return 1;
    }
    g_body_ran = 0;
    g_cb_calls = 0;
    g_cb_return = 0;
    ((fake_fn)fake)(0x11111111, 22, 33, 44);

    check("callback called", g_cb_calls, 1);
    check("arg0 origin", (unsigned)g_seen[0], 0x11111111u);
    check("arg1 entityNum", g_seen[1], 22);
    check("arg2 entchannel", g_seen[2], 33);
    check("arg3 sfxHandle", g_seen[3], 44);
    check("ret_addr nonzero", g_seen_ret != NULL, 1);
    check("original body ran", g_body_ran, 1);

    /* Stack balance: if the stub leaks or over-pops, this corrupts fast. */
    for (i = 0; i < 2000; ++i) {
        ((fake_fn)fake)(1, 2, 3, 4);
    }
    check("body ran 2001 times", g_body_ran, 2001);
    check("cb ran 2001 times", g_cb_calls, 2001);

    /* --- 1 arg with skip: the CG_EntityEvent shape ------------------ */
    fake = make_fake();
    memset(&h, 0, sizeof(h));
    if (!hook_install(&h, fake, 6, (void *)test_cb, 1, 1)) {
        printf("  [FAIL] hook_install (skip variant)\n");
        return 1;
    }
    g_body_ran = 0;
    g_cb_calls = 0;
    g_cb_return = 0;
    ((fake_fn)fake)(0x22222222, 0, 0, 0);
    check("1-arg: cb called", g_cb_calls, 1);
    check("1-arg: arg0 cent", (unsigned)g_seen[0], 0x22222222u);
    check("1-arg: ret nonzero", g_seen_ret != NULL, 1);
    check("1-arg: body ran (no skip)", g_body_ran, 1);

    g_cb_return = 1; /* ask the stub to suppress */
    ((fake_fn)fake)(0x33333333, 0, 0, 0);
    check("skip: cb called", g_cb_calls, 2);
    check("skip: body did NOT run", g_body_ran, 1);

    g_cb_return = 0;
    for (i = 0; i < 2000; ++i) {
        ((fake_fn)fake)(7, 0, 0, 0);
    }
    check("skip variant stack stable", g_body_ran, 2001);

    printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}

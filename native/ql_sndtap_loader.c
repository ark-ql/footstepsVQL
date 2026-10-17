/* ql_sndtap_loader — console injector.
 *
 * Optional. The normal way to use this project is to run ql_sndtap_ui.exe,
 * which injects on its own. This exists for scripted or headless runs where
 * no window is wanted.
 *
 *   ql_sndtap_loader.exe [--dll <path>] [--wait <seconds>] [--pid <n>]
 *
 * --dll defaults to ql_sndtap.dll beside this executable.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ql_inject.h"

static const wchar_t *TARGET = L"quakelive_steam.exe";
static const char *INIT_EXPORT = "QLSndTapInitialize";

static void logf_(const char *fmt, ...)
{
    va_list ap;
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    char dll_buf[MAX_PATH];
    const char *dll = NULL;
    int wait_s = 90;
    DWORD pid = 0;
    char err[128];
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) {
            dll = argv[++i];
        } else if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            wait_s = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
            pid = (DWORD)atoi(argv[++i]);
        }
    }
    if (!dll) {
        char dir[MAX_PATH];
        ql_module_dir(dir, sizeof(dir));
        _snprintf(dll_buf, sizeof(dll_buf), "%s\\ql_sndtap.dll", dir);
        dll_buf[sizeof(dll_buf) - 1] = '\0';
        dll = dll_buf;
    }

    for (i = 0; i < wait_s * 2 && !pid; ++i) {
        pid = ql_find_pid(TARGET);
        if (!pid) {
            Sleep(500);
        }
    }
    if (!pid) {
        logf_("FAIL target not found after %d s", wait_s);
        return 1;
    }
    logf_("target pid=%lu", (unsigned long)pid);

    /* Let the process finish its own startup before touching it. */
    Sleep(1500);

    if (!ql_inject(pid, dll, INIT_EXPORT, err, sizeof(err))) {
        logf_("FAIL %s", err);
        return 1;
    }
    logf_("%s", err);
    logf_("ok");
    return 0;
}

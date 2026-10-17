/* ql_sndtap_ui.exe — knob panel for the injected tap DLL.
 *
 * Same shape and rationale as independed-render-ql-patch's ql_hood_ui.exe: a
 * separate process with plain Win32 controls, no overlay. An in-game overlay
 * would mean owning a present path and a mouse the game also wants, and this
 * tool must never change the frame it is measuring.
 *
 * Holds no state of its own. The shared page is the state; this reflects it
 * and writes knobs into it. The game runs identically if this never starts.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "ql_sndtap_ipc.h"
#include "ql_sndtap_version.h"
#include "ql_inject.h"

#define ID_TIMER      1
#define ID_KNOB_BASE  1000
#define ID_WIN_EDIT   2000
#define ID_WIN_SET    2001
#define ID_LAUNCH     2003
#define POLL_MS       100u

#define WINDOW_W 460
#define WINDOW_H 560

/* Quake Live on Steam. Going through Steam rather than the exe keeps this
 * independent of which library folder the game sits in. */
#define STEAM_APP_URL L"steam://rungameid/282440"
#define INIT_EXPORT   "QLSndTapInitialize"

typedef struct KnobUi {
    int index;
    const WCHAR *label;
    const WCHAR *hint;
    int confirm_on;   /* changes what the player hears, not just the log */
} KnobUi;

static const KnobUi KNOBS[] = {
    { QL_TAP_EVT_LOG,
      L"log CG_EntityEvent",
      L"every event the client plays, with QPC time and caller", 0 },
    { QL_TAP_SND_LOG,
      L"log S_StartSound",
      L"every audible start: entity, channel, sfx name, volume", 0 },
    { QL_TAP_LOOP_LOG,
      L"log looping-sound starts",
      L"transitions only, not the per-frame calls (rocket flight loop)", 0 },
    { QL_TAP_LEDGER_DRYRUN,
      L"ledger dry run (count only)",
      L"counts what would be suppressed; changes nothing you hear", 0 },
    { QL_TAP_LEDGER,
      L"ledger ACTIVE (suppress replays)",
      L"ADVANCED: actually drops events. Verify with dry run first.", 1 },
    { QL_TAP_PS_SCAN,
      L"playerState scan (server-fix prep)",
      L"locates eventSequence / events[] offsets; read-only, noisy log", 0 }
};

#define KNOB_UI_COUNT ((int)(sizeof(KNOBS) / sizeof(KNOBS[0])))

static HANDLE g_map;
static QlSndTapShared *g_shared;
static HANDLE g_game;
static HWND g_check[KNOB_UI_COUNT];
static HWND g_hint[KNOB_UI_COUNT];
static HWND g_status;
static HWND g_win_edit;
static HWND g_win_set;
static HWND g_launch;
static HWND g_banner;
static HFONT g_font;
static HFONT g_mono;
static int g_mismatch;
static int g_win_edit_dirty;

/* Injection state. This EXE is the whole tool: it waits for the game,
 * injects the DLL itself, then attaches to the page the DLL publishes. No
 * Steam launch option and no wrapper script. */
static DWORD g_injected_pid;
static char g_inject_note[160] = "waiting for quakelive_steam.exe";

static void detach(void)
{
    if (g_shared) {
        UnmapViewOfFile(g_shared);
        g_shared = NULL;
    }
    if (g_map) {
        CloseHandle(g_map);
        g_map = NULL;
    }
    if (g_game) {
        CloseHandle(g_game);
        g_game = NULL;
    }
}

static int try_attach_pid(DWORD pid)
{
    WCHAR name[96];
    HANDLE map;
    QlSndTapShared *view;

    _snwprintf(name, 96, L"%ls%lu", QL_SNDTAP_PREFIX, (unsigned long)pid);
    name[95] = L'\0';
    map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) {
        return 0;
    }
    view = (QlSndTapShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0,
                                           sizeof(QlSndTapShared));
    if (!view) {
        CloseHandle(map);
        return 0;
    }
    if (view->magic != QL_SNDTAP_MAGIC) {
        UnmapViewOfFile(view);
        CloseHandle(map);
        return 0;
    }
    /* A stale UI beside a newer DLL must say so rather than display whatever
     * the fields used to mean. */
    if (view->version != QL_SNDTAP_VERSION ||
        view->struct_bytes != sizeof(QlSndTapShared)) {
        g_mismatch = 1;
        UnmapViewOfFile(view);
        CloseHandle(map);
        return 0;
    }
    g_mismatch = 0;
    g_map = map;
    g_shared = view;
    g_game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    return 1;
}

static int attach(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e;
    int found = 0;

    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e)) {
        do {
            if (_wcsicmp(e.szExeFile, QL_SNDTAP_TARGET) != 0) {
                continue;
            }
            if (try_attach_pid(e.th32ProcessID)) {
                found = 1;
                break;
            }
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return found;
}

/* Inject into the game if it is running and has no tap yet. Idempotent:
 * once a pid has been injected it is remembered, and an already-published
 * shared page short-circuits this entirely. */
static void maybe_inject(void)
{
    DWORD pid = ql_find_pid(QL_SNDTAP_TARGET);
    char dir[MAX_PATH];
    char dll[MAX_PATH];
    char err[128];

    if (!pid) {
        g_injected_pid = 0;
        _snprintf(g_inject_note, sizeof(g_inject_note),
                  "waiting for quakelive_steam.exe");
        return;
    }
    if (pid == g_injected_pid) {
        return; /* already done; waiting for the page to appear */
    }
    ql_module_dir(dir, sizeof(dir));
    _snprintf(dll, sizeof(dll), "%s\\ql_sndtap.dll", dir);

    if (ql_inject(pid, dll, INIT_EXPORT, err, sizeof(err))) {
        g_injected_pid = pid;
        _snprintf(g_inject_note, sizeof(g_inject_note),
                  "injected into pid %lu (%s)", (unsigned long)pid, err);
    } else {
        /* Do not spin: remember the pid so a hard failure is reported once
         * rather than retried every 100 ms. */
        g_injected_pid = pid;
        _snprintf(g_inject_note, sizeof(g_inject_note), "INJECT FAILED: %s",
                  err);
    }
}

static void refresh_status(void)
{
    WCHAR text[2048];
    const QlSndTapShared *s = g_shared;
    int i;

    if (!s) {
        WCHAR msg[1024];
        WCHAR note[192];
        int n;
        for (n = 0; n < (int)sizeof(g_inject_note) - 1 && g_inject_note[n];
             ++n) {
            note[n] = (WCHAR)(unsigned char)g_inject_note[n];
        }
        note[n] = L'\0';
        _snwprintf(msg, 1024,
                   g_mismatch
                       ? L"Version mismatch.\r\n\r\nThis ql_sndtap_ui.exe and "
                         L"the injected DLL are from\r\ndifferent builds. Run "
                         L"native\\build.ps1 and use the pair in bin\\."
                         L"\r\n\r\n%ls"
                       : L"No tap attached yet.\r\n\r\n"
                         L"Start Quake Live (button below, or however you "
                         L"normally do).\r\nThis window injects the DLL on its "
                         L"own as soon as the\r\ngame appears - no Steam "
                         L"launch option needed.\r\n\r\n%ls",
                   note);
        msg[1023] = L'\0';
        SetWindowTextW(g_status, msg);
        return;
    }

    _snwprintf(
        text, 2048,
        L"build %hs   pid %ld\r\n"
        L"engine %ls   cgame %ls   cgame reloads %ld\r\n"
        L"\r\n"
        L"events       %ld\r\n"
        L"sounds       %ld\r\n"
        L"loop starts  %ld   (calls %ld)\r\n"
        L"\r\n"
        L"ledger window   %ld us  (%ld.%02ld ms)\r\n"
        L"ledger batch    %ld us\r\n"
        L"would suppress  %ld\r\n"
        L"suppressed      %ld\r\n"
        L"spared by batch %ld   (legit fast repeats, e.g. LG at 50 ms)\r\n"
        L"spared by caller %ld  (entity-path events; should stay 0)\r\n"
        L"\r\n"
        L"log dropped %ld   errors %ld%ls",
        s->commit, s->game_pid,
        s->engine_hooked ? L"hooked" : L"NOT HOOKED",
        s->cgame_hooked ? L"hooked" : L"not loaded",
        s->cgame_reloads,
        s->evt_count, s->snd_count, s->loop_starts, s->loop_calls,
        s->ledger_window_us, s->ledger_window_us / 1000,
        (s->ledger_window_us % 1000) / 10,
        s->ledger_batch_us,
        s->ledger_would_suppress, s->ledger_suppressed,
        s->ledger_spared_by_batch, s->ledger_spared_by_caller,
        s->log_dropped, s->log_errors,
        (s->log_dropped || s->log_errors) ? L"   <- LINES WERE LOST" : L"");
    text[2047] = L'\0';
    SetWindowTextW(g_status, text);

    for (i = 0; i < KNOB_UI_COUNT; ++i) {
        SendMessageW(g_check[i], BM_SETCHECK,
                     s->knob[KNOBS[i].index] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    /* Do not fight the user while they are typing in the window box. */
    if (!g_win_edit_dirty && GetFocus() != g_win_edit) {
        WCHAR buf[32];
        _snwprintf(buf, 32, L"%ld", s->ledger_window_us);
        SetWindowTextW(g_win_edit, buf);
    }
}

static void set_controls_enabled(int on)
{
    int i;
    for (i = 0; i < KNOB_UI_COUNT; ++i) {
        EnableWindow(g_check[i], on ? TRUE : FALSE);
    }
    EnableWindow(g_win_edit, on ? TRUE : FALSE);
    EnableWindow(g_win_set, on ? TRUE : FALSE);
}

static void on_knob_clicked(int slot)
{
    int index;
    LONG want;

    if (!g_shared || slot < 0 || slot >= KNOB_UI_COUNT) {
        return;
    }
    index = KNOBS[slot].index;
    want = SendMessageW(g_check[slot], BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (want && KNOBS[slot].confirm_on) {
        int answer = MessageBoxW(
            GetParent(g_check[slot]),
            L"Let the ledger actually drop events?\n\n"
            L"This is the candidate fix, not a measurement. It stops the "
            L"client playing an event whose (entity, event id) pair was "
            L"already played inside the window below.\n\n"
            L"If the window is too wide it will eat legitimate repeats — the "
            L"second shot of an LG burst is only 50 ms after the first — and "
            L"that is a worse bug than the one being fixed, and a quieter one.\n\n"
            L"Run the dry run first and set the window from the measured gap "
            L"distribution, not by feel.",
            L"ledger ACTIVE", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDOK) {
            SendMessageW(g_check[slot], BM_SETCHECK, BST_UNCHECKED, 0);
            return;
        }
    }
    InterlockedExchange(&g_shared->knob[index], want);
}

static void apply_window(void)
{
    WCHAR buf[32];
    long v;

    if (!g_shared) {
        return;
    }
    GetWindowTextW(g_win_edit, buf, 32);
    v = wcstol(buf, NULL, 10);
    if (v < 0) {
        v = 0;
    }
    if (v > 1000000) {
        v = 1000000;
    }
    InterlockedExchange(&g_shared->ledger_window_us, (LONG)v);
    g_win_edit_dirty = 0;
}

/* Row geometry is derived from the actual font metrics rather than from
 * hardcoded pixel counts. With a fixed 18 px row a larger system font — any
 * display scaling above 100% — overflows the checkbox into the hint below
 * it, which is the overlap seen in the rega hood panel. */
static int g_row_h;      /* one checkbox line   */
static int g_hint_h;     /* one hint line       */
static int g_status_top; /* y of the status box */

static int measure_line_height(HWND w, HFONT f)
{
    HDC dc = GetDC(w);
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm;
    int h;

    GetTextMetricsW(dc, &tm);
    h = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(w, dc);
    return h;
}

/* Height a wrapped STATIC needs for `text` at `width`. */
static int measure_wrapped(HWND w, HFONT f, const WCHAR *text, int width)
{
    HDC dc = GetDC(w);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r = { 0, 0, width, 0 };

    DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK);
    SelectObject(dc, old);
    ReleaseDC(w, dc);
    return r.bottom - r.top;
}

static const WCHAR *BANNER_TEXT =
    L"Knobs take effect immediately. Nothing here is saved: a relaunch "
    L"starts from the DLL defaults (both logs on, ledger off).";

static void create_controls(HWND w)
{
    int i;
    int y = 8;
    int text_w = WINDOW_W - 40;
    int banner_h;
    int line, ctl_h, edit_w;
    RECT want;

    line = measure_line_height(w, g_font);
    g_row_h = line + 5;
    g_hint_h = line + 1;
    ctl_h = line + 10;              /* edit boxes and buttons */
    banner_h = measure_wrapped(w, g_font, BANNER_TEXT, text_w) + 4;

    g_banner = CreateWindowExW(0, L"STATIC", BANNER_TEXT,
                               WS_CHILD | WS_VISIBLE, 10, y, text_w, banner_h,
                               w, NULL, NULL, NULL);
    y += banner_h + 8;

    for (i = 0; i < KNOB_UI_COUNT; ++i) {
        g_check[i] = CreateWindowExW(
            0, L"BUTTON", KNOBS[i].label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            12, y, WINDOW_W - 44, g_row_h, w,
            (HMENU)(UINT_PTR)(ID_KNOB_BASE + i), NULL, NULL);
        y += g_row_h;
        g_hint[i] = CreateWindowExW(
            0, L"STATIC", KNOBS[i].hint, WS_CHILD | WS_VISIBLE,
            32, y, WINDOW_W - 60, g_hint_h, w, NULL, NULL, NULL);
        y += g_hint_h + 5;
    }

    y += 6;
    edit_w = 100;
    CreateWindowExW(0, L"STATIC", L"ledger window (us):", WS_CHILD | WS_VISIBLE,
                    12, y + 4, 130, line + 2, w, NULL, NULL, NULL);
    g_win_edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"40000",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        146, y, edit_w, ctl_h, w, (HMENU)(UINT_PTR)ID_WIN_EDIT, NULL, NULL);
    g_win_set = CreateWindowExW(
        0, L"BUTTON", L"Set", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        146 + edit_w + 6, y, 60, ctl_h, w, (HMENU)(UINT_PTR)ID_WIN_SET, NULL,
        NULL);
    g_launch = CreateWindowExW(
        0, L"BUTTON", L"Start Quake Live", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        WINDOW_W - 180, y, 150, ctl_h, w, (HMENU)(UINT_PTR)ID_LAUNCH, NULL,
        NULL);
    y += ctl_h + 8;

    g_status_top = y;
    g_status = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        10, y, text_w, 200, w, NULL, NULL, NULL);

    for (i = 0; i < KNOB_UI_COUNT; ++i) {
        SendMessageW(g_check[i], WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_hint[i], WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    SendMessageW(g_banner, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_win_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_win_set, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_launch, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_mono, TRUE);

    /* Size the frame so the client area actually fits the laid-out content
     * plus room for the status box, instead of trusting a constant. */
    want.left = 0;
    want.top = 0;
    want.right = WINDOW_W - 18;
    want.bottom = g_status_top + 230;
    AdjustWindowRect(&want, (DWORD)GetWindowLongPtrW(w, GWL_STYLE), FALSE);
    SetWindowPos(w, NULL, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

static LRESULT CALLBACK window_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        create_controls(w);
        set_controls_enabled(0);
        refresh_status();
        SetTimer(w, ID_TIMER, POLL_MS, NULL);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            int id = LOWORD(wp);
            if (id >= ID_KNOB_BASE && id < ID_KNOB_BASE + KNOB_UI_COUNT) {
                on_knob_clicked(id - ID_KNOB_BASE);
            } else if (id == ID_WIN_SET) {
                apply_window();
            } else if (id == ID_LAUNCH) {
                ShellExecuteW(w, L"open", STEAM_APP_URL, NULL, NULL,
                              SW_SHOWNORMAL);
            }
        } else if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == ID_WIN_EDIT) {
            g_win_edit_dirty = 1;
        }
        return 0;
    case WM_TIMER:
        if (wp == ID_TIMER) {
            if (!g_shared) {
                /* Attach first: the DLL may already be in, from a previous
                 * run of this panel or from the standalone loader. */
                if (attach()) {
                    set_controls_enabled(1);
                } else {
                    maybe_inject();
                }
            } else if (g_game &&
                       WaitForSingleObject(g_game, 0) == WAIT_OBJECT_0) {
                /* Game exited. Stay open and re-arm rather than closing, so
                 * a framerate cell can be re-run without restarting this. */
                detach();
                set_controls_enabled(0);
                g_injected_pid = 0;
            }
            refresh_status();
        }
        return 0;
    case WM_SIZE:
        /* Let the status box take whatever height the window has, so
         * resizing or a DPI change cannot clip or overlap it. */
        if (g_status) {
            int cw = LOWORD(lp), ch = HIWORD(lp);
            MoveWindow(g_status, 10, g_status_top, cw - 20,
                       ch - g_status_top - 10, TRUE);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        /* A read-only multiline EDIT is coloured through this message too,
         * and it must paint an OPAQUE background. With TRANSPARENT it never
         * erases, so every SetWindowText draws on top of the previous text
         * instead of replacing it — which is the garbled status box. Only
         * the real STATIC labels and checkboxes want transparency, so they
         * sit on the window's own grey. */
        if ((HWND)lp == g_status) {
            SetBkMode((HDC)wp, OPAQUE);
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)(UINT_PTR)GetSysColorBrush(COLOR_WINDOW);
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)(UINT_PTR)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        KillTimer(w, ID_TIMER);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show)
{
    WNDCLASSEXW cls;
    HWND w;
    MSG msg;
    WCHAR title[128];

    (void)prev;
    (void)args;

    {
        HANDLE once = CreateMutexW(NULL, TRUE, L"Local\\ql_sndtap_ui_single");
        if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
            return 0;
        }
    }

    /* Scale with the display, and lay out from the resulting metrics. A
     * fixed -11 against a scaled system font is what makes the rows
     * overlap on anything above 100%. */
    {
        HDC screen = GetDC(NULL);
        int dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(NULL, screen);
        g_font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                             FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI");
        g_mono = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                             FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                             L"Consolas");
    }

    ZeroMemory(&cls, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = window_proc;
    cls.hInstance = inst;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    cls.lpszClassName = L"QlSndTapUi";
    if (!RegisterClassExW(&cls)) {
        return 1;
    }

    _snwprintf(title, 128, L"ql sndtap  %hs", QL_SNDTAP_VERSION_STR);
    title[127] = L'\0';

    /* Resizable: create_controls sizes it to fit, and WM_SIZE keeps the
     * status box filling whatever is left. */
    w = CreateWindowExW(0, cls.lpszClassName, title,
                        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                        CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_W, WINDOW_H,
                        NULL, NULL, inst, NULL);
    if (!w) {
        return 1;
    }
    ShowWindow(w, show);
    UpdateWindow(w);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_font) {
        DeleteObject(g_font);
    }
    if (g_mono) {
        DeleteObject(g_mono);
    }
    detach();
    return 0;
}

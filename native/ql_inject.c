#include "ql_inject.h"

#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

DWORD ql_find_pid(const wchar_t *exe_name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    DWORD pid = 0;

    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe_name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

void ql_module_dir(char *out, size_t out_bytes)
{
    char path[MAX_PATH];
    char *slash;
    HMODULE self = NULL;

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)ql_module_dir, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash) {
        *slash = '\0';
    }
    _snprintf(out, out_bytes, "%s", path);
    out[out_bytes - 1] = '\0';
}

static int run_remote(HANDLE proc, LPTHREAD_START_ROUTINE fn, void *arg,
                      DWORD *exit_code)
{
    HANDLE th = CreateRemoteThread(proc, NULL, 0, fn, arg, 0, NULL);
    if (!th) {
        return 0;
    }
    WaitForSingleObject(th, 20000);
    if (exit_code) {
        GetExitCodeThread(th, exit_code);
    }
    CloseHandle(th);
    return 1;
}

int ql_inject(DWORD pid, const char *dll_path, const char *init_export,
              char *err, size_t err_bytes)
{
    HANDLE proc;
    void *remote_path;
    size_t bytes;
    DWORD remote_base = 0;
    HMODULE k32, local;
    FARPROC local_init;
    uintptr_t init_rva;

#define FAILV(...)                                   \
    do {                                             \
        _snprintf(err, err_bytes, __VA_ARGS__);      \
        err[err_bytes - 1] = '\0';                   \
        if (proc) {                                  \
            CloseHandle(proc);                       \
        }                                            \
        return 0;                                    \
    } while (0)

    proc = NULL;
    if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) {
        FAILV("dll not found: %s", dll_path);
    }
    proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                           PROCESS_VM_READ,
                       FALSE, pid);
    if (!proc) {
        FAILV("OpenProcess failed (err %lu) - run as the same user",
              GetLastError());
    }

    bytes = strlen(dll_path) + 1;
    remote_path = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);
    if (!remote_path ||
        !WriteProcessMemory(proc, remote_path, dll_path, bytes, NULL)) {
        FAILV("write remote path failed (err %lu)", GetLastError());
    }

    k32 = GetModuleHandleA("kernel32.dll");
    if (!run_remote(proc,
                    (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA"),
                    remote_path, &remote_base) ||
        remote_base == 0) {
        FAILV("remote LoadLibraryA failed");
    }

    /* The remote base need not equal the local one, so resolve the export by
     * its offset within our own copy rather than by address.
     * The FreeLibrary is load-bearing: without it this process keeps a write
     * lock on the DLL for as long as it lives, which silently breaks every
     * subsequent rebuild. (A datafile mapping would avoid the lock but
     * GetProcAddress does not work on one.) */
    local = LoadLibraryA(dll_path);
    if (!local) {
        FAILV("local LoadLibraryA failed (err %lu)", GetLastError());
    }
    local_init = GetProcAddress(local, init_export);
    if (!local_init) {
        FreeLibrary(local);
        FAILV("export %s missing", init_export);
    }
    init_rva = (uintptr_t)local_init - (uintptr_t)local;
    FreeLibrary(local);

    if (!run_remote(proc,
                    (LPTHREAD_START_ROUTINE)((uintptr_t)remote_base + init_rva),
                    NULL, NULL)) {
        FAILV("remote init failed (err %lu)", GetLastError());
    }
    CloseHandle(proc);
    _snprintf(err, err_bytes, "ok base=0x%lx", (unsigned long)remote_base);
    err[err_bytes - 1] = '\0';
    return 1;
#undef FAILV
}

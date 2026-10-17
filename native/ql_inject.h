/* Remote-thread injection, shared by the standalone loader and the UI. */
#ifndef QL_INJECT_H
#define QL_INJECT_H

#include <windows.h>

/* First process matching `exe_name` (case-insensitive), or 0. */
DWORD ql_find_pid(const wchar_t *exe_name);

/* LoadLibraryA the DLL into `pid`, then call its `init_export`.
 * Returns 1 on success. On failure writes a short reason into `err`. */
int ql_inject(DWORD pid, const char *dll_path, const char *init_export,
              char *err, size_t err_bytes);

/* Directory of the calling module, as an ANSI path without a trailing slash. */
void ql_module_dir(char *out, size_t out_bytes);

#endif /* QL_INJECT_H */

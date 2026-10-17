#!/bin/sh
# Build and gate a release candidate in its own directory. Touches no running
# process, unit, plugin directory or container: the .so is built here, loaded
# only by this script's own python processes, and patched only into
# selftest_emit.py's private dlopen of qagamex64.so.
set -e
cd "$(dirname "$0")"
chmod +x build.sh validate.sh probe_sigs.sh selftest_emit.py
echo "== inputs =="
sha256sum footstepsVQL_hook.c fs_cadence.c fs_cadence.h test_cadence.c \
    footstepsVQL.py selftest_emit.py | sed 's/^/  /'
echo
./probe_sigs.sh
echo
./build.sh
echo
./validate.sh
echo
echo "== glibc versions the .so needs =="
objdump -T footstepsVQL_hook.so | grep -o 'GLIBC_[0-9.]*' | sort -u | sed 's/^/  /'
echo
echo "== release candidate =="
sha256sum footstepsVQL_hook.so footstepsVQL.py | sed 's/^/  /'

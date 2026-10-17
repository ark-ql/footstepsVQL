#!/bin/sh
# Build footstepsVQL_hook.so on the target host.
#
# Must run where the qzeroded process runs: the .so links against the host
# GLIBC, and our test server runs QL natively (no container), so the host gcc is
# the right toolchain there. research.md's "newer GLIBC, .so will not load"
# dead end applied to a containerised ubuntu16 QL, which this host is not.
#
# The host self-test gates the build. If the cadence logic or the bobCycle
# equivalence regresses, no .so is emitted -- 2.1's rule, and the bobCycle half
# is the one that matters: if it diverges the client's prediction stops agreeing
# and the doubled sounds come straight back.

set -e
cd "$(dirname "$0")"

CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror -fPIC"

echo "== host self-test =="
gcc $CFLAGS -o test_cadence fs_cadence.c test_cadence.c -lm
./test_cadence
echo

echo "== building footstepsVQL_hook.so =="
# No -fvisibility=hidden: the plugin dlopen's this and looks the ABI up by name,
# exactly as it does for 2.1. Only fsvql_ret is hidden, in the source.
# No --no-undefined either: libc stays undefined in a shared object.
gcc $CFLAGS -shared -o footstepsVQL_hook.so footstepsVQL_hook.c fs_cadence.c
echo

echo "== exported ABI =="
nm -D --defined-only footstepsVQL_hook.so | grep -E ' T ' | awk '{print "  " $3}'
echo
echo "== artifact =="
ls -l footstepsVQL_hook.so
sha256sum footstepsVQL_hook.so

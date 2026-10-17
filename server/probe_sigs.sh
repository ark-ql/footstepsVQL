#!/bin/sh
# Read-only pre-build gate for stage 2: locate the live qagamex64.so, hash it,
# and uniqueness-check every byte signature the artifact keys off.
#
# Nothing here writes, loads or patches anything. It exists because a signature
# that matches more than once does not ship, and because every stage 2 offset in
# notes/s1-qagame-offsets.md is void if the .so is not sha256 091d568f...
set -e
cd "$(dirname "$0")"

EXPECT=091d568f46a2

echo "== live qagamex64.so =="
QL_DIR=${QL_DIR:-/home/ql/qlds}
SO=$(find "$QL_DIR" -name qagamex64.so -print 2>/dev/null | head -1)
if [ -z "$SO" ]; then
    echo "  NOT FOUND under $QL_DIR (set QL_DIR to the dedicated server directory)"
    exit 1
fi
echo "  path   $SO"
H=$(sha256sum "$SO" | cut -c1-12)
echo "  sha256 ${H}..."
if [ "$H" = "$EXPECT" ]; then
    echo "  MATCHES the binary the offsets were measured against"
else
    echo "  MISMATCH: expected ${EXPECT}... -- every offset in the notes is void"
    exit 1
fi
echo

echo "== signature uniqueness =="
python3 _qagame_sigs.py "$SO"

#!/bin/sh
# Pre-deploy validation for the footstepsVQL pair. Runs on the target and touches
# nothing live: the only process it patches is selftest_emit.py's own, against a
# private dlopen of qagamex64.so.
set -e
cd "$(dirname "$0")"
FSVQL_SO="$(pwd)/footstepsVQL_hook.so"
export FSVQL_SO
QAGAME=${QAGAME:-${QL_DIR:-/home/ql/qlds}/baseq3/qagamex64.so}

echo "== python syntax =="
python3 -m py_compile footstepsVQL.py && echo "  footstepsVQL.py compiles clean"
echo

echo "== ABI cross-check: every symbol the plugin binds must exist =="
nm -D --defined-only footstepsVQL_hook.so | awk '$2=="T"{print $3}' | sort > /tmp/fsvql_have.txt
missing=0
for n in fsvql_patch fsvql_status fsvql_unpatch fsvql_selftest \
         fsvql_hits fsvql_vanilla_flips fsvql_injections_due fsvql_qagame_base \
         fsvql_site fsvql_dt_bucket fsvql_move_ms fsvql_reset_counters fsvql_reset_slots \
         fsvql_ring_written fsvql_ring_drained fsvql_ring_undone \
         fsvql_ring_skipped fsvql_ring_missed fsvql_ring_suppressed fsvql_ring_retagged \
         fsvql_ring_deferred fsvql_quiet fsvql_ring_suppressed_overflow fsvql_ring_pre \
         fsvql_ring_suppressed_restored \
         fsvql_surface_addr fsvql_site_c fsvql_sendp_addr fsvql_stub_pages; do
    if grep -qx "$n" /tmp/fsvql_have.txt; then
        echo "  ok       $n"
    else
        echo "  MISSING  $n"
        missing=$((missing + 1))
    fi
done
# The emitter and the mode switch are gone; an export surviving would mean an
# old source was built.
for n in fsvql_set_mode fsvql_set_emit fsvql_emit_state fsvql_mode fsvql_tramp_b \
         fsvql_send_pending fsvql_temp_entity_addr fsvql_site_b; do
    if grep -qx "$n" /tmp/fsvql_have.txt; then
        echo "  STALE    $n"
        missing=$((missing + 1))
    fi
done
echo
if [ "$missing" -ne 0 ]; then
    echo "ABI MISMATCH: $missing symbol(s) missing or stale"
    exit 1
fi

echo "== cadence selftest through ctypes, exactly as the plugin will call it =="
python3 - <<'PY'
import ctypes
import os
lib = ctypes.CDLL(os.environ["FSVQL_SO"])
for name in ("fsvql_selftest", "fsvql_status", "fsvql_patch"):
    getattr(lib, name).restype = ctypes.c_int
for name in ("fsvql_surface_addr", "fsvql_site_c", "fsvql_sendp_addr"):
    getattr(lib, name).restype = ctypes.c_uint64

st = lib.fsvql_selftest()
print("  fsvql_selftest()   =", st, "(0 = pass)")
print("  fsvql_status()     =", lib.fsvql_status(),
      "(0 expected: qagame is not mapped in this process)")
patched = lib.fsvql_patch()
print("  fsvql_patch()      =", patched, "(0 expected: nothing to patch here)")
addrs = (lib.fsvql_surface_addr(), lib.fsvql_site_c(), lib.fsvql_sendp_addr())
print("  resolved addresses = surface 0x{:x}, site_c 0x{:x}, sendp 0x{:x}".format(*addrs),
      "(all 0 expected)")
raise SystemExit(1 if (st != 0 or patched != 0 or any(addrs)) else 0)
PY
echo

echo "== structural selftest against a private copy of the real qagame =="
# Patches, reads back and unpatches both sites in *this* process, on its own
# dlopen of qagamex64.so. The live server is not involved and no patched code is
# ever executed. This is the only gate that can see whether the signatures still
# find their targets in a real mapping.
python3 ./selftest_emit.py "$QAGAME" "$FSVQL_SO"
echo

echo "== is the site currently free? =="
if grep -q footsteps250_hook /proc/*/maps 2>/dev/null; then
    echo "  WARNING: footsteps250_hook.so is mapped into a running process."
    echo "  footstepsVQL will report -2 (pattern not found) until the next map."
else
    echo "  no footsteps250_hook mapping found"
fi
echo
echo "validation passed"

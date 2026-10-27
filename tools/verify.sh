#!/bin/sh
# verify.sh -- checks the port tree without the toolchain.
#
#  1. the import table is complete and up to date (runtime/tools/gen_imports.py --check)
#  2. every symbol the runtime requires from a port is defined, exactly once
#  3. no file still references a game name from another port
#
# Run from the port folder: ./tools/verify.sh
set -e
cd "$(dirname "$0")/.."

echo "== import table"
if [ -d runtime ]; then
  python3 runtime/tools/gen_imports.py --check
else
  echo "runtime/ is missing (git submodule update --init, or unzip android32 into runtime/)"
fi

echo "== required port symbols"
fail=0
for sym in port_load port_run port_setup_plan jni_method_defs jni_field_defs \
           jni_class_supers jni_missing_classes dcr_config_load dcr_boot_frames; do
  n=$(grep -h "^[A-Za-z_].*\b$sym\b" source/*.c | grep -c . || true)
  if [ "$n" -eq 0 ]; then echo "  MISSING  $sym"; fail=1
  elif [ "$n" -gt 1 ]; then echo "  DUPLICATE $sym ($n)"; fail=1
  else echo "  ok       $sym"; fi
done

echo "== no foreign game names"
if grep -nE "(spiderman|GloftSMHP|SM_LIB|labyrinth|abspace)" source/*.c source/*.h 2>/dev/null; then
  echo "  ^ remove these"; fail=1
else
  echo "  ok"
fi

[ "$fail" -eq 0 ] && echo "== PASS" || { echo "== FAIL"; exit 1; }

#!/bin/bash
# Build the example in the working directory, then optionally require or
# reject a wdt_hal_init reference in sota_wdt.c's object file.
set -euo pipefail
mode="${1:-build}"

if [ "$mode" = "absent" ]; then
  idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.no_wdt" build
else
  idf.py build
fi

if [ "$mode" = "build" ]; then
  exit 0
fi

obj=$(find build -type f \( -name 'sota_wdt.c.obj' -o -name 'sota_wdt.c.o' \) -print -quit)
if [ -z "$obj" ]; then
  echo "sota_wdt object not found" >&2
  find build -name 'sota_wdt*' >&2 || true
  exit 1
fi
echo "watchdog object: $obj"

nm_bin=""
old_ifs=$IFS
IFS=':'
for dir in $PATH; do
  [ -d "$dir" ] || continue
  for cand in "$dir"/*esp*-nm "$dir"/*-nm; do
    if [ -x "$cand" ]; then
      nm_bin=$cand
      break
    fi
  done
  [ -n "$nm_bin" ] && break
done
IFS=$old_ifs
if [ -z "$nm_bin" ]; then
  echo "toolchain nm not on PATH" >&2
  exit 1
fi
echo "nm: $nm_bin"
"$nm_bin" "$obj" | tee /tmp/sota-wdt-nm.txt

if [ "$mode" = "require" ]; then
  grep -E '(^|[[:space:]])U[[:space:]]+wdt_hal_init$' /tmp/sota-wdt-nm.txt
elif [ "$mode" = "absent" ]; then
  if grep -E '(^|[[:space:]])U[[:space:]]+wdt_hal_init$' /tmp/sota-wdt-nm.txt; then
    echo "wdt_hal_init is referenced with the trial watchdog opted out" >&2
    exit 1
  fi
  echo "wdt_hal_init is absent with the trial watchdog opted out"
else
  echo "unknown mode: $mode" >&2
  exit 1
fi

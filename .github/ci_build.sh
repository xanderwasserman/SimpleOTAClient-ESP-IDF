#!/bin/bash
# Build the example in the working directory, check that the image leaves
# headroom in its app slot, then optionally require or reject a wdt_hal_init
# reference in the trial watchdog's object file.
set -euo pipefail
mode="${1:-build}"

if [ "$mode" = "absent" ]; then
  idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.no_wdt" build
else
  idf.py build
fi

# The example image must leave at least 20 percent of its smallest app slot
# free, so growth in the component shows up here before it fills a user's
# slot.
python - <<'PY'
import json, os, subprocess, sys

desc = json.load(open("build/project_description.json"))
image = os.path.getsize(os.path.join(desc["build_dir"], desc["app_bin"]))
gen = os.path.join(os.environ["IDF_PATH"], "components", "partition_table", "gen_esp32part.py")
table = subprocess.run([sys.executable, gen, "build/partition_table/partition-table.bin"],
                       check=True, capture_output=True, text=True).stdout

def size_bytes(text):
    for suffix, mult in (("M", 1024 * 1024), ("K", 1024)):
        if text.upper().endswith(suffix):
            return int(text[:-1], 0) * mult
    return int(text, 0)

slots = []
for line in table.splitlines():
    fields = [f.strip() for f in line.split(",")]
    if len(fields) >= 5 and not fields[0].startswith("#") and fields[1] == "app":
        slots.append(size_bytes(fields[4]))
if not slots:
    sys.exit("no app partition in build/partition_table/partition-table.bin")
slot = min(slots)
free = slot - image
print(f"app image {image} bytes, smallest app slot {slot} bytes, "
      f"{free} bytes ({100 * free / slot:.1f}%) free")
if free * 100 < slot * 20:
    sys.exit("the example leaves less than 20 percent of its app slot free")
PY

if [ "$mode" = "build" ]; then
  exit 0
fi

obj=$(find build -type f \( -name 'sota_wdt_esp32.c.obj' -o -name 'sota_wdt_esp32.c.o' \) -print -quit)
if [ -z "$obj" ]; then
  echo "sota_wdt object not found" >&2
  find build -name 'sota_wdt*' >&2 || true
  exit 1
fi
echo "watchdog object: $obj"

nm_bin=$(grep -E '^CMAKE_NM:' build/CMakeCache.txt | cut -d= -f2)
if [ -z "$nm_bin" ] || [ ! -x "$nm_bin" ]; then
  echo "toolchain nm not found in build/CMakeCache.txt" >&2
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

#!/bin/bash
# Builds and runs the host simulation of the firmware.
#   ./run.sh                 all variants (TOKEN, SECRET, NOSECRETS)
#   ./run.sh TOKEN           one variant
#   DUMP=1 ./run.sh TOKEN    also write the generated web pages as *.html
# Needs: g++ (C++17), zlib development files, the PlatformIO libraries of the
# main environment (installed with "pio pkg install -e twitchDisplay").
set -u
cd "$(dirname "$0")"
REPO="$(cd ../.. && pwd)"
JSON="$REPO/.pio/libdeps/twitchDisplay/ArduinoJson/src"
if [ ! -d "$JSON" ]; then
  echo "ArduinoJson not found, installing the PlatformIO libraries..."
  (cd "$REPO" && pio pkg install -e twitchDisplay) || { echo "Run 'pio pkg install -e twitchDisplay' first."; exit 1; }
fi
CXX="${CXX:-g++}"
FLAGS="-std=gnu++17 -O1 -w -Df_str=c_str -Istubs -I$REPO/include -I$REPO/lib/Filters -I$REPO/lib/LogoImage -I$JSON"
mkdir -p build
$CXX $FLAGS -c "$REPO/lib/LogoImage/LogoImage.cpp" -o build/LogoImage.o || exit 1
status=0
for v in ${@:-TOKEN SECRET NOSECRETS}; do
  rm -f build/test_$v
  if ! $CXX $FLAGS -DVARIANT_$v test.cpp build/LogoImage.o -lz -o build/test_$v; then
    echo "$v: BUILD FAILED"; status=1; continue
  fi
  out=$(build/test_$v 2>&1); rc=$?
  if [ $rc -gt 1 ]; then echo "$v: CRASHED (exit code $rc)"; echo "$out" | tail -5; status=1; continue; fi
  echo "$v: $(echo "$out" | tail -1)"
  echo "$out" | grep "FAIL" | head -20
  echo "$out" | tail -1 | grep -q "all checks passed" || status=1
done
exit $status

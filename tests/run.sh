#!/usr/bin/env bash
# Builds and runs the host tests. No toolchain beyond a C compiler: the firmware logic
# under test is compiled against the stubs in tests/stubs rather than ESP-IDF.
#
#   ./tests/run.sh
set -u
cd "$(dirname "$0")"
CC=${CC:-gcc}
CXX=${CXX:-g++}
mkdir -p out
fail=0

echo "building..."
# The firmware's own headers, so link.c and store.cpp compile unmodified. Keep this list
# the same as the one in run.ps1.
INC="-I. -Istubs -I../components/link/include -I../components/storage/include"
INC="$INC -I../components/net/include -I../components/engine/include -I../components/tat_api/include"

$CC  -std=gnu11  -Wall -Wextra -Wno-unused-parameter $INC -o out/test_link  test_link.c  || exit 1
$CXX -std=gnu++17 -Wall -Wextra -Wno-unused-parameter $INC -o out/test_store test_store.cpp || exit 1

echo
./out/test_link  || fail=1
./out/test_store || fail=1

echo
python3 test_protocol.py || fail=1

echo
if [ $fail -eq 0 ]; then echo "all tests passed"; else echo "TESTS FAILED"; fi
exit $fail

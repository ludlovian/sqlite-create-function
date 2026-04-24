#!/bin/bash
IFS=$'\t\n'; set -euo pipefail

cd "${0%/*}"
CFLAGS="-DDEBUG_TRACE -DDEBUG_ASSERT" make clean all || { echo "rc=$?"; exit 1; }
sqlite3 <test_script.sql &>test.actual || true
diff -C 5 test.{actual,expected} || exit 1
echo "-----------"
echo "Test passed"
echo "-----------"
rm test.actual
make clean all

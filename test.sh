#!/bin/bash
IFS=$'\t\n'; set -euo pipefail

cd "${0%/*}"
make clean
CFLAGS=-DTEST_TRACE make || echo "rc=$?"
sqlite3 <test_script.sql &>test.actual || true
diff -C 5 test.{actual,expected} || exit 1
echo "-----------"
echo "Test passed"
echo "-----------"
rm test.actual
make clean
make

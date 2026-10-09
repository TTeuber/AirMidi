#!/bin/bash
# Run locally what CI runs: host tests (ASan + UBSan), the clang-format check, and the firmware for both boards.
#   tools/check.sh [--fix]     --fix reformats instead of only checking
set -e
cd "$(dirname "$0")/.."
echo "== host tests"
cmake -S test -B test/build >/dev/null
cmake --build test/build -j >/dev/null
ctest --test-dir test/build --output-on-failure | tail -3

echo "== format (clang-format 18.1.8)"
files=$(git ls-files -co --exclude-standard '*.c' '*.h' | grep -v -e '^test/unity/' -e '^spikes/')
cf=(uv tool run --quiet --from clang-format==18.1.8 clang-format)
if [ "$1" = --fix ]; then "${cf[@]}" -i $files; fi
"${cf[@]}" --dry-run --Werror $files && echo "clean"

for board in c6 s3 s3kit; do
    echo "== firmware $board"
    firmware/build.sh $board 2>&1 | grep -E "error|warning:|binary size|build complete" || true
done

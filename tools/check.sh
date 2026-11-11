#!/bin/bash
# Run locally what CI runs: host tests (ASan + UBSan), the clang-format check, the firmware for every board, and the
# app's analyzer and tests (the app's release builds are left to CI: they take minutes).
#   tools/check.sh [--fix]     --fix reformats instead of only checking
set -e
cd "$(dirname "$0")/.."
echo "== host tests"
cmake -S test -B test/build >/dev/null
cmake --build test/build -j >/dev/null
ctest --test-dir test/build --output-on-failure | tail -3

echo "== format (clang-format 18.1.8)"
files=$(git ls-files -co --exclude-standard '*.c' '*.h' | grep -v -e '^test/unity/' -e '^spikes/' -e '/third_party/')
cf=(uv tool run --quiet --from clang-format==18.1.8 clang-format)
if [ "$1" = --fix ]; then "${cf[@]}" -i $files; fi
"${cf[@]}" --dry-run --Werror $files && echo "clean"

for board in c6 s3 s3b s3kit; do
    echo "== firmware $board"
    firmware/build.sh $board 2>&1 | grep -E "error|warning:|binary size|build complete" || true
done

echo "== app (Flutter from app/.fvmrc)"
(cd app && fvm flutter analyze | tail -1 && fvm flutter test 2>&1 | tail -1 | grep -oE '[+-][0-9]+.*(passed|failed).*')
